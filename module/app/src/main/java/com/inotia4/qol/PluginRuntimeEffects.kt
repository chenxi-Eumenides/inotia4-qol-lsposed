package com.inotia4.qol

import org.json.JSONObject
import java.util.LinkedHashSet
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

internal data class ApiListenerConfig(
    val listenAddress: String,
    val listenPort: Int,
)

internal enum class RuntimeEffectBootstrapState {
    SUBSCRIPTION_NOT_ESTABLISHED,
    INITIAL_SNAPSHOT_PENDING,
    READY,
}

internal fun runtimeEffectBootstrapState(
    apiSubscriptionEstablished: Boolean,
    initialSnapshotReceived: Boolean,
): RuntimeEffectBootstrapState = when {
    !apiSubscriptionEstablished -> RuntimeEffectBootstrapState.SUBSCRIPTION_NOT_ESTABLISHED
    initialSnapshotReceived -> RuntimeEffectBootstrapState.READY
    else -> RuntimeEffectBootstrapState.INITIAL_SNAPSHOT_PENDING
}

/** Retains a successful observer token and permits another registration attempt after rejection. */
internal class RetryableObserverToken {
    private var tokenId = 0L

    @Synchronized
    fun currentId(): Long = tokenId

    @Synchronized
    fun isEstablished(): Boolean = tokenId > 0L

    @Synchronized
    fun ensure(register: () -> Long): Boolean {
        if (tokenId > 0L) return true
        val candidate = register()
        if (candidate > 0L) tokenId = candidate
        return tokenId > 0L
    }

    @Synchronized
    fun clear(expectedToken: Long): Boolean {
        if (tokenId != expectedToken || tokenId <= 0L) return false
        tokenId = 0L
        return true
    }
}

/** Serial consumer for Host-trusted plugin runtime effects. */
object PluginRuntimeEffects {
    private const val API_PLUGIN_ID = "core.api"
    private const val SIMPLE_MODE_PLUGIN_ID = "simple_mode"
    private const val DEFAULT_LISTEN_ADDRESS = "0.0.0.0"
    private const val DEFAULT_LISTEN_PORT = 8088
    private const val FALLBACK_LISTEN_PORT_JSON = "8088"

    private val executor = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "plugin-runtime-effects").apply { isDaemon = true }
    }
    private val apiSubscription = RetryableObserverToken()
    private val simpleModeSubscription = RetryableObserverToken()
    private var simpleModeSubscriptionAttempted = false

    @Volatile
    private var initialSnapshotReceived = false

    @Volatile
    private var initialSnapshot = CountDownLatch(1)

    private val latestEventRevisions = mutableMapOf<String, Long>()
    private var latestEffect: PluginEffectState? = null
    private val handledBindFailures = LinkedHashSet<Pair<Long, Int>>()
    private val pendingReceiptSubscriptions = mutableMapOf<Long, Long>()
    private val handledReceiptCallbacks = LinkedHashSet<Long>()
    private val receiptCallbackLock = Any()
    private val receivedReceiptCallbacks = mutableSetOf<Long>()

    /** Subscribe once; a timeout leaves the event subscription active for a late replay. */
    @Synchronized
    internal fun subscribeAndAwaitInitialSnapshot(): RuntimeEffectBootstrapState {
        val apiEstablished = apiSubscription.ensure { subscribe(API_PLUGIN_ID) ?: 0L }
        if (!apiEstablished) {
            return runtimeEffectBootstrapState(
                apiSubscriptionEstablished = false,
                initialSnapshotReceived = initialSnapshotReceived,
            )
        }
        if (!simpleModeSubscriptionAttempted) {
            simpleModeSubscriptionAttempted = true
            simpleModeSubscription.ensure { subscribe(SIMPLE_MODE_PLUGIN_ID) ?: 0L }
        }
        if (!initialSnapshotReceived) awaitInitialSnapshot()
        return runtimeEffectBootstrapState(
            apiSubscriptionEstablished = apiSubscription.isEstablished(),
            initialSnapshotReceived = initialSnapshotReceived,
        )
    }

    private fun subscribe(pluginId: String): Long? {
        val token = try {
            NativeBridge.nativeRuntimeEffectSubscribe(pluginId, PluginRuntimeEffects::class.java)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.CONFIG, "$pluginId runtime-effect subscription failed", t)
            return null
        }
        if (token <= 0L) {
            LogFile.warn(LogDomain.CONFIG, "$pluginId runtime-effect subscription was rejected")
            return null
        }
        return token
    }

    @Synchronized
    fun unsubscribe(): Boolean {
        executor.execute(::unsubscribePendingReceipts)
        val apiToken = apiSubscription.currentId()
        val simpleModeToken = simpleModeSubscription.currentId()
        if (apiToken == 0L && simpleModeToken == 0L) return true
        var allRemoved = true
        var apiRemoved = false
        if (apiToken != 0L) {
            val removed = unsubscribe(apiToken, API_PLUGIN_ID)
            if (removed) {
                apiSubscription.clear(apiToken)
                apiRemoved = true
            } else {
                allRemoved = false
            }
        }
        if (simpleModeToken != 0L) {
            if (unsubscribe(simpleModeToken, SIMPLE_MODE_PLUGIN_ID)) {
                simpleModeSubscription.clear(simpleModeToken)
                simpleModeSubscriptionAttempted = false
            } else {
                allRemoved = false
            }
        }
        if (apiRemoved) {
            initialSnapshotReceived = false
            initialSnapshot = CountDownLatch(1)
            executor.execute {
                latestEventRevisions.clear()
                latestEffect = null
                handledBindFailures.clear()
                ApiServer.clearRuntimeEffect()
            }
        }
        return allRemoved
    }

    private fun unsubscribe(token: Long, pluginId: String): Boolean = try {
        NativeBridge.nativeRuntimeEffectUnsubscribe(token)
    } catch (t: Throwable) {
        LogFile.error(LogDomain.CONFIG, "$pluginId runtime-effect unsubscription failed", t)
        false
    }

    private fun awaitInitialSnapshot(): Boolean = try {
        initialSnapshot.await(5, TimeUnit.SECONDS)
    } catch (e: InterruptedException) {
        Thread.currentThread().interrupt()
        false
    }

    @JvmStatic
    fun onRuntimeEffect(
        pluginId: String,
        eventRevision: Long,
        configRevision: Long,
        actualEnabled: Boolean,
        desiredEnabled: Boolean,
        configJson: String,
    ) {
        val event = PluginEffect(
            pluginId = String(pluginId.toCharArray()),
            eventRevision = eventRevision,
            configRevision = configRevision,
            actualEnabled = actualEnabled,
            desiredEnabled = desiredEnabled,
            configJson = String(configJson.toCharArray()),
        )
        executor.execute { applyEffect(event) }
    }

    /** Called from the AndServer callback; all config I/O is moved to a worker. */
    fun onBindFailure(failedPort: Int) {
        executor.execute { requestDefaultPortIfCurrent(failedPort) }
    }

    private fun applyEffect(event: PluginEffect) {
        if (event.pluginId != API_PLUGIN_ID && event.pluginId != SIMPLE_MODE_PLUGIN_ID) {
            LogFile.warn(LogDomain.CONFIG, "ignored runtime effect for unexpected plugin ${event.pluginId}")
            return
        }
        val lastRevision = latestEventRevisions[event.pluginId]
        if (lastRevision != null && event.eventRevision <= lastRevision) return
        latestEventRevisions[event.pluginId] = event.eventRevision

        if (event.pluginId == SIMPLE_MODE_PLUGIN_ID) {
            // simple_mode has one registered feature, so plugin desired state is that toggle.
            ConfigPlatformEffects.onSimpleModeRuntimeEffect(event.eventRevision, event.desiredEnabled)
            return
        }

        val parsedConfig = parseListenerConfig(event.configJson)
        val effect = PluginEffectState(
            eventRevision = event.eventRevision,
            configRevision = event.configRevision,
            actualEnabled = event.actualEnabled,
            desiredEnabled = event.desiredEnabled,
            config = parsedConfig.config,
            configError = parsedConfig.error,
        )
        latestEffect = effect
        ApiServer.applyRuntimeEffect(
            eventRevision = effect.eventRevision,
            actualEnabled = effect.actualEnabled,
            desiredEnabled = effect.desiredEnabled,
            config = effect.config,
            configError = effect.configError,
        )
        if (!initialSnapshotReceived) {
            initialSnapshotReceived = true
            initialSnapshot.countDown()
        }
    }

    private fun requestDefaultPortIfCurrent(failedPort: Int) {
        val effect = latestEffect ?: return
        if (!effect.actualEnabled || !effect.desiredEnabled || effect.config == null) return
        if (failedPort == DEFAULT_LISTEN_PORT || effect.config.listenPort != failedPort) return

        val key = effect.eventRevision to failedPort
        if (!handledBindFailures.add(key)) return

        val admission = try {
            NativeBridge.nativePluginConfigRequestIfRevision(
                API_PLUGIN_ID,
                "listen_port",
                FALLBACK_LISTEN_PORT_JSON,
                effect.configRevision,
            )
        } catch (t: Throwable) {
            LogFile.error(LogDomain.CONFIG, "core.api listener fallback request failed", t)
            return
        }
        val requestId = parseAdmissionRequestId(admission)
        if (requestId == null) {
            val errorCode = parseErrorCode(admission)
            LogFile.warn(
                LogDomain.CONFIG,
                "core.api listener fallback was not admitted${errorCode?.let { " (error_code=$it)" } ?: ""}; no retry",
            )
            return
        }
        subscribeReceiptOnce(requestId)
    }

    private fun subscribeReceiptOnce(requestId: Long) {
        if (pendingReceiptSubscriptions.containsKey(requestId)) {
            LogFile.warn(LogDomain.CONFIG, "core.api config receipt $requestId is already being observed")
            return
        }
        pendingReceiptSubscriptions[requestId] = 0L
        val token = try {
            NativeBridge.nativePluginConfigSubscribeReceipt(requestId, PluginRuntimeEffects::class.java)
        } catch (t: Throwable) {
            pendingReceiptSubscriptions.remove(requestId)
            LogFile.error(LogDomain.CONFIG, "core.api config receipt $requestId subscription failed", t)
            return
        }
        if (token <= 0L) {
            pendingReceiptSubscriptions.remove(requestId)
            LogFile.warn(LogDomain.CONFIG, "core.api config receipt $requestId subscription was rejected; no retry")
            return
        }
        pendingReceiptSubscriptions[requestId] = token
    }

    private fun parseAdmissionRequestId(json: String): Long? {
        return try {
            val response = JSONObject(json)
            if (response.get("ok") != true) {
                null
            } else {
                val requestId = response.get("request_id")
                (requestId as? Number)?.toLong()?.takeIf { it > 0L }
            }
        } catch (t: Throwable) {
            LogFile.error(LogDomain.CONFIG, "invalid core.api config admission response", t)
            null
        }
    }

    private fun parseErrorCode(json: String): Int? = try {
        JSONObject(json).optInt("error_code").takeIf { it != 0 }
    } catch (_: Throwable) {
        null
    }

    @JvmStatic
    fun onConfigReceipt(requestId: Long, receiptJson: String) {
        synchronized(receiptCallbackLock) {
            receivedReceiptCallbacks += requestId
        }
        val event = ConfigReceiptEvent(requestId, String(receiptJson.toCharArray()))
        executor.execute { applyReceipt(event) }
    }

    private fun applyReceipt(event: ConfigReceiptEvent) {
        synchronized(receiptCallbackLock) {
            receivedReceiptCallbacks.remove(event.requestId)
        }
        if (!handledReceiptCallbacks.add(event.requestId)) return
        if (handledReceiptCallbacks.size > 64) {
            val oldest = handledReceiptCallbacks.firstOrNull()
            if (oldest != null) handledReceiptCallbacks.remove(oldest)
        }
        if (pendingReceiptSubscriptions.remove(event.requestId) == null) {
            LogFile.warn(LogDomain.CONFIG, "ignored late or unknown core.api config receipt ${event.requestId}")
            return
        }
        val receipt = try {
            JSONObject(event.receiptJson)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.CONFIG, "invalid core.api config receipt ${event.requestId}", t)
            return
        }
        if (receipt.opt("ok") != true) {
            LogFile.warn(
                LogDomain.CONFIG,
                "core.api config receipt ${event.requestId} returned error_code=${receipt.optInt("error_code")}",
            )
            return
        }
        when (receipt.optString("persistence")) {
            "pending" -> LogFile.warn(
                LogDomain.CONFIG,
                "core.api config receipt ${event.requestId} was not terminal; no retry",
            )
            "persisted" -> LogFile.info(
                LogDomain.CONFIG,
                "core.api listener port fallback persisted; waiting for a fresh runtime effect",
            )
            "failed" -> LogFile.warn(
                LogDomain.CONFIG,
                "core.api listener port fallback persistence failed (error_code=${receipt.optInt("error_code")})",
            )
            else -> LogFile.warn(
                LogDomain.CONFIG,
                "core.api config receipt ${event.requestId} had an unknown state",
            )
        }
    }

    private fun unsubscribePendingReceipts() {
        if (pendingReceiptSubscriptions.isEmpty()) return
        val pending = pendingReceiptSubscriptions.toMap()
        pending.forEach { (requestId, token) ->
            if (hasReceivedReceiptCallback(requestId)) return@forEach
            try {
                val removed = NativeBridge.nativePluginConfigUnsubscribeReceipt(token)
                if (removed && !hasReceivedReceiptCallback(requestId)) {
                    pendingReceiptSubscriptions.remove(requestId)
                } else if (!removed && !hasReceivedReceiptCallback(requestId)) {
                    LogFile.warn(
                        LogDomain.CONFIG,
                        "core.api config receipt $requestId unsubscription was rejected",
                    )
                    pendingReceiptSubscriptions.remove(requestId)
                }
            } catch (t: Throwable) {
                LogFile.error(LogDomain.CONFIG, "core.api config receipt $requestId unsubscription failed", t)
                if (!hasReceivedReceiptCallback(requestId)) pendingReceiptSubscriptions.remove(requestId)
            }
        }
    }

    private fun hasReceivedReceiptCallback(requestId: Long): Boolean = synchronized(receiptCallbackLock) {
        requestId in receivedReceiptCallbacks
    }

    private fun parseListenerConfig(json: String): ListenerConfigParse {
        return try {
            val config = JSONObject(json)
            val address = if (config.has("listen_address")) {
                config.get("listen_address") as? String
                    ?: return ListenerConfigParse(null, "listen_address must be a string")
            } else {
                DEFAULT_LISTEN_ADDRESS
            }
            if (address.isBlank()) return ListenerConfigParse(null, "listen_address must not be blank")

            val port = if (config.has("listen_port")) {
                val rawPort = config.get("listen_port") as? Number
                    ?: return ListenerConfigParse(null, "listen_port must be an integer")
                val numericPort = rawPort.toDouble()
                if (!numericPort.isFinite() || numericPort % 1.0 != 0.0 || numericPort !in 1.0..65535.0) {
                    return ListenerConfigParse(null, "listen_port must be an integer from 1 to 65535")
                }
                numericPort.toInt()
            } else {
                DEFAULT_LISTEN_PORT
            }
            ListenerConfigParse(ApiListenerConfig(address, port), null)
        } catch (t: Throwable) {
            ListenerConfigParse(
                null,
                "config_json is not a valid listener config: ${t.message ?: t.javaClass.simpleName}",
            )
        }
    }

    private data class PluginEffect(
        val pluginId: String,
        val eventRevision: Long,
        val configRevision: Long,
        val actualEnabled: Boolean,
        val desiredEnabled: Boolean,
        val configJson: String,
    )

    private data class ConfigReceiptEvent(
        val requestId: Long,
        val receiptJson: String,
    )

    private data class PluginEffectState(
        val eventRevision: Long,
        val configRevision: Long,
        val actualEnabled: Boolean,
        val desiredEnabled: Boolean,
        val config: ApiListenerConfig?,
        val configError: String?,
    )

    private data class ListenerConfigParse(
        val config: ApiListenerConfig?,
        val error: String?,
    )
}

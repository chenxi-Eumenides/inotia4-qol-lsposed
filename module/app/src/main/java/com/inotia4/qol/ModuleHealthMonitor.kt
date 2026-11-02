package com.inotia4.qol

import com.inotia4.qol.patch.WatermarkOverlay
import org.json.JSONObject
import java.util.ArrayList
import java.util.Collections
import java.util.concurrent.Executors

internal data class ModuleHealthSnapshot(
    val revision: Long,
    val knownMask: Long,
    val failedMask: Long,
    val mainMenuSeen: Boolean,
)

internal data class ModuleHealthPresentation(
    val unavailableNames: List<String>,
    val visible: Boolean,
)

internal data class ModuleHealthPresentationUpdate(
    val revision: Long,
    val presentation: ModuleHealthPresentation,
)

internal class ModuleHealthSnapshotState {
    var snapshot: ModuleHealthSnapshot? = null
        private set

    var mainMenuSeenSticky = false
        private set

    fun accept(candidate: ModuleHealthSnapshot): Boolean {
        if (snapshot?.let { candidate.revision <= it.revision } == true) return false
        snapshot = candidate
        mainMenuSeenSticky = mainMenuSeenSticky || candidate.mainMenuSeen
        return true
    }
}

internal class ModuleHealthPresentationSequence {
    private var revision = 0L
    private var lastPresentation: ModuleHealthPresentation? = null

    fun next(presentation: ModuleHealthPresentation): ModuleHealthPresentationUpdate? {
        val names = Collections.unmodifiableList(
            ArrayList(presentation.unavailableNames.filter { it.isNotBlank() }.distinct()),
        )
        val normalized = ModuleHealthPresentation(
            unavailableNames = names,
            visible = presentation.visible && names.isNotEmpty(),
        )
        if (normalized == lastPresentation) return null
        lastPresentation = normalized
        revision += 1L
        return ModuleHealthPresentationUpdate(revision, normalized)
    }
}

/** Pure policy for the four Host-owned module-health facts. */
internal object ModuleHealthDecision {
    const val NATIVE_BIT = 1L shl 0
    const val FRAME_SOURCE_BIT = 1L shl 1
    const val ASSEMBLY_BIT = 1L shl 2
    const val CONFIG_STORE_BIT = 1L shl 3

    fun decide(
        snapshot: ModuleHealthSnapshot?,
        nativeLibraryStateKnown: Boolean,
        nativeLibraryLoaded: Boolean,
        mainMenuSeenSticky: Boolean,
        activityVisible: Boolean,
    ): ModuleHealthPresentation {
        val knownMask = snapshot?.knownMask ?: 0L
        val failedMask = snapshot?.failedMask ?: 0L
        fun failed(bit: Long): Boolean = knownMask and bit != 0L && failedMask and bit != 0L

        val nativeFailed = (nativeLibraryStateKnown && !nativeLibraryLoaded) || failed(NATIVE_BIT)
        val frameFailed = failed(FRAME_SOURCE_BIT)
        val mainMenuSeen = mainMenuSeenSticky || snapshot?.mainMenuSeen == true
        val unavailable = ArrayList<String>(4)
        if (nativeFailed) unavailable += "Native"
        if (frameFailed) unavailable += "帧钩子"
        if (mainMenuSeen && failed(ASSEMBLY_BIT)) unavailable += "模块初始化"
        if (mainMenuSeen && failed(CONFIG_STORE_BIT)) unavailable += "配置存储"

        return ModuleHealthPresentation(
            unavailableNames = unavailable.toList(),
            visible = activityVisible && unavailable.isNotEmpty(),
        )
    }
}

/** Consumes Host-owned health events and projects their whitelist to the watermark. */
object ModuleHealthMonitor {
    private const val UINT32_MAX = 0xFFFF_FFFFL

    private val lock = Any()
    private val executor = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "module-health-events").apply { isDaemon = true }
    }

    @Volatile
    private var started = false

    private val healthSubscription = RetryableObserverToken()
    private var activityVisible = false
    private var nativeLibraryStateKnown = false
    private var nativeLibraryLoaded = false
    private val healthFacts = ModuleHealthSnapshotState()
    private val presentationSequence = ModuleHealthPresentationSequence()
    private var lastReadFailure: String? = null

    @Synchronized
    fun start() {
        if (started) return
        started = true

        val libraryLoaded = NativeBridge.libraryLoaded
        val update = synchronized(lock) {
            nativeLibraryStateKnown = true
            nativeLibraryLoaded = libraryLoaded
            nextUiUpdateLocked()
        }
        publish(update)
        if (libraryLoaded) executor.execute(::subscribeOnce)
    }

    fun onMainActivityVisibility(visible: Boolean) {
        val update = synchronized(lock) {
            if (!started) return
            nativeLibraryStateKnown = true
            nativeLibraryLoaded = NativeBridge.libraryLoaded
            if (activityVisible == visible) null else {
                activityVisible = visible
                nextUiUpdateLocked()
            }
        }
        publish(update)
        if (visible) executor.execute(::subscribeOnce)
    }

    /** An actual native init attempt is another event at which a rejected Host observer may retry. */
    fun onNativeInitializationAttemptCompleted() {
        if (started && NativeBridge.libraryLoaded) executor.execute(::subscribeOnce)
    }

    /** JNI may synchronously replay the current snapshot; its callback only queues work. */
    @JvmStatic
    fun onModuleHealthChanged(snapshotJson: String) {
        val immutableJson = String(snapshotJson.toCharArray())
        executor.execute {
            try {
                consumeSnapshot(immutableJson)
            } catch (t: Throwable) {
                LogFile.error(LogDomain.PLATFORM, "module health event processing failed", t)
            }
        }
    }

    /** Called only at Host/module teardown; plugin health disable does not own this subscription. */
    @Synchronized
    fun unsubscribeAtModuleTeardown(): Boolean {
        val token = healthSubscription.currentId()
        if (token == 0L) return true
        val removed = try {
            NativeBridge.nativeModuleHealthUnsubscribe(token)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "module health unsubscription failed", t)
            false
        }
        if (removed) healthSubscription.clear(token)
        return removed
    }

    private fun subscribeOnce() {
        if (!NativeBridge.libraryLoaded) return
        val established = healthSubscription.ensure {
            try {
                NativeBridge.nativeModuleHealthSubscribe(ModuleHealthMonitor::class.java)
            } catch (t: Throwable) {
                LogFile.error(LogDomain.PLATFORM, "module health subscription failed", t)
                0L
            }
        }
        if (!established) {
            LogFile.warn(LogDomain.PLATFORM, "module health subscription was rejected; facts remain unknown")
        }
    }

    private fun consumeSnapshot(raw: String) {
        val parsed = parseSnapshot(raw) ?: return
        val update = synchronized(lock) {
            if (!healthFacts.accept(parsed)) return
            nextUiUpdateLocked()
        }
        publish(update)
    }

    private fun parseSnapshot(raw: String): ModuleHealthSnapshot? = try {
        val value = JSONObject(raw)
        val revision = value.getLong("revision")
        val knownMask = value.getLong("known_mask")
        val failedMask = value.getLong("failed_mask")
        require(revision >= 0L) { "revision must not be negative" }
        require(knownMask in 0L..UINT32_MAX) { "known_mask is outside uint32 range" }
        require(failedMask in 0L..UINT32_MAX) { "failed_mask is outside uint32 range" }
        ModuleHealthSnapshot(
            revision = revision,
            knownMask = knownMask,
            failedMask = failedMask,
            mainMenuSeen = readBoolean(value, "main_menu_seen"),
        ).also {
            lastReadFailure = null
        }
    } catch (t: Throwable) {
        val failureKind = t.javaClass.simpleName
        if (failureKind != lastReadFailure) {
            LogFile.warn(LogDomain.PLATFORM, "module health event invalid ($failureKind); facts remain unknown")
            lastReadFailure = failureKind
        }
        null
    }

    private fun readBoolean(value: JSONObject, key: String): Boolean = when (val raw = value.get(key)) {
        is Boolean -> raw
        is Number -> when (raw.toDouble()) {
            0.0 -> false
            1.0 -> true
            else -> error("$key must be boolean or 0/1")
        }
        else -> error("$key must be boolean or 0/1")
    }

    private fun nextUiUpdateLocked(): ModuleHealthPresentationUpdate? {
        val presentation = ModuleHealthDecision.decide(
            snapshot = healthFacts.snapshot,
            nativeLibraryStateKnown = nativeLibraryStateKnown,
            nativeLibraryLoaded = nativeLibraryLoaded,
            mainMenuSeenSticky = healthFacts.mainMenuSeenSticky,
            activityVisible = activityVisible,
        )
        return presentationSequence.next(presentation)
    }

    private fun publish(update: ModuleHealthPresentationUpdate?) {
        if (update == null) return
        WatermarkOverlay.updateModuleHealth(
            update.revision,
            update.presentation.unavailableNames,
            update.presentation.visible,
        )
    }
}

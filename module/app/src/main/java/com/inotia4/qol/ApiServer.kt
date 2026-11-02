package com.inotia4.qol

import android.content.Context
import com.yanzhenjie.andserver.AndServer
import com.yanzhenjie.andserver.Server
import java.net.BindException
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import javax.net.ServerSocketFactory

object ApiServer {
    private const val DEFAULT_LISTEN_ADDRESS = "0.0.0.0"
    private const val DEFAULT_LISTEN_PORT = 8088

    private val controlExecutor = Executors.newSingleThreadScheduledExecutor { runnable ->
        Thread(runnable, "api-server-control").apply { isDaemon = true }
    }

    private var server: Server? = null
    private var serverStarting = false
    private var activeStartAttempt = 0L
    private var runningConfig: ApiListenerConfig? = null
    private var startContext: Context? = null
    private var startModuleApkPath: String? = null
    private var hasRuntimeEffect = false
    private var actualEnabled = false
    private var desiredEnabled = false
    private var listenerConfig: ApiListenerConfig? = null
    private var lastEventRevision = Long.MIN_VALUE
    private var controlGeneration = 0L
    private var pendingControl: ScheduledFuture<*>? = null

    /** Captures context after the runtime replay and reconciles the latest core.api effect. */
    @Synchronized
    fun bootstrap(context: Context, moduleApkPath: String?) {
        startContext = context
        startModuleApkPath = moduleApkPath
        if (BuildConfig.KERNEL_ONLY) {
            LogFile.info(LogDomain.HTTP, "kernel-only mode: starting loader health server")
            ModuleRuntimeBootstrap.injectModuleAssets(context, moduleApkPath)
            startKernelHealthServerLocked()
            return
        }
        reconcileLocked()
    }

    @Synchronized
    internal fun applyRuntimeEffect(
        eventRevision: Long,
        actualEnabled: Boolean,
        desiredEnabled: Boolean,
        config: ApiListenerConfig?,
        configError: String?,
    ) {
        if (BuildConfig.KERNEL_ONLY || (hasRuntimeEffect && eventRevision <= lastEventRevision)) return

        val wasEnabled = this.actualEnabled && this.desiredEnabled
        val previousConfig = listenerConfig
        hasRuntimeEffect = true
        lastEventRevision = eventRevision
        this.actualEnabled = actualEnabled
        this.desiredEnabled = desiredEnabled
        listenerConfig = config

        when {
            !desiredEnabled || !actualEnabled -> {
                cancelPendingControlLocked()
                stopServerLocked()
            }
            config == null -> {
                cancelPendingControlLocked()
                LogFile.error(
                    LogDomain.HTTP,
                    "core.api listener config rejected; listener remains stopped: ${configError ?: "invalid config"}",
                )
                stopServerLocked()
            }
            !wasEnabled || previousConfig == null -> {
                cancelPendingControlLocked()
                reconcileLocked()
            }
            previousConfig != config && server != null -> scheduleReconcileLocked(500L)
            else -> {
                cancelPendingControlLocked()
                reconcileLocked()
            }
        }
    }

    @Synchronized
    fun clearRuntimeEffect() {
        if (BuildConfig.KERNEL_ONLY) return
        hasRuntimeEffect = false
        actualEnabled = false
        desiredEnabled = false
        listenerConfig = null
        lastEventRevision = Long.MIN_VALUE
        cancelPendingControlLocked()
        stopServerLocked()
    }

    private fun startKernelHealthServerLocked() {
        if (server != null || serverStarting) return
        val context = startContext ?: return
        startServerLocked(context, ApiListenerConfig(DEFAULT_LISTEN_ADDRESS, DEFAULT_LISTEN_PORT))
    }

    private fun reconcileLocked() {
        if (BuildConfig.KERNEL_ONLY || !hasRuntimeEffect) return
        if (!actualEnabled || !desiredEnabled) {
            stopServerLocked()
            return
        }
        val config = listenerConfig
        if (config == null) {
            stopServerLocked()
            return
        }
        if (server != null) {
            if (runningConfig != config) {
                stopServerLocked()
                startContext?.let { startServerLocked(it, config) }
            }
            return
        }
        if (!serverStarting) startContext?.let { startServerLocked(it, config) }
    }

    private fun startServerLocked(context: Context, config: ApiListenerConfig) {
        if (server != null || serverStarting) return
        val attempt = ++activeStartAttempt
        val started = AtomicBoolean(false)
        val failureReported = AtomicBoolean(false)
        serverStarting = true
        ModuleRuntimeBootstrap.injectModuleAssets(context, startModuleApkPath)
        try {
            val address = InetAddress.getByName(config.listenAddress)
            val builder = AndServer.webServer(context)
                .port(config.listenPort)
                .timeout(10, TimeUnit.SECONDS)
                .serverSocketFactory(GracefulCloseServerSocketFactory)
                .listener(object : Server.ServerListener {
                    override fun onStarted() {
                        started.set(true)
                        synchronized(this@ApiServer) {
                            if (activeStartAttempt == attempt) serverStarting = false
                        }
                        LogFile.info(
                            LogDomain.HTTP,
                            "AndServer started on ${config.listenAddress}:${config.listenPort}",
                        )
                    }

                    override fun onStopped() {
                        LogFile.info(LogDomain.HTTP, "AndServer stopped")
                    }

                    override fun onException(e: Exception) {
                        LogFile.error(LogDomain.HTTP, "AndServer error", e)
                        if (!started.get() && failureReported.compareAndSet(false, true)) {
                            handleStartFailure(attempt, config, e)
                        }
                    }
                })
            builder.inetAddress(address)
            val newServer = builder.build()
            server = newServer
            runningConfig = config
            newServer.startup()
        } catch (t: Throwable) {
            if (failureReported.compareAndSet(false, true)) {
                handleStartFailure(attempt, config, t)
            } else {
                LogFile.error(LogDomain.HTTP, "ApiServer start failed", t)
            }
        }
    }

    private fun handleStartFailure(attempt: Long, config: ApiListenerConfig, failure: Throwable) {
        synchronized(this) {
            if (activeStartAttempt == attempt) {
                serverStarting = false
                server = null
                runningConfig = null
            }
        }
        LogFile.error(
            LogDomain.HTTP,
            "ApiServer start failed on ${config.listenAddress}:${config.listenPort}",
            failure,
        )
        if (isBindFailure(failure)) PluginRuntimeEffects.onBindFailure(config.listenPort)
    }

    private fun isBindFailure(failure: Throwable): Boolean {
        var current: Throwable? = failure
        var depth = 0
        while (current != null && depth++ < 16) {
            if (current is BindException) return true
            current = current.cause
        }
        return false
    }

    @Synchronized
    fun stop() {
        cancelPendingControlLocked()
        stopServerLocked()
    }

    private fun stopServerLocked() {
        val previous = server
        activeStartAttempt++
        server = null
        serverStarting = false
        runningConfig = null
        try {
            previous?.shutdown()
        } catch (t: Throwable) {
            LogFile.error(LogDomain.HTTP, "AndServer shutdown failed", t)
        }
    }

    private fun scheduleReconcileLocked(delayMillis: Long) {
        val generation = ++controlGeneration
        pendingControl?.cancel(false)
        pendingControl = controlExecutor.schedule({
            synchronized(this@ApiServer) {
                if (generation != controlGeneration) return@synchronized
                pendingControl = null
                reconcileLocked()
            }
        }, delayMillis, TimeUnit.MILLISECONDS)
    }

    private fun cancelPendingControlLocked() {
        controlGeneration++
        pendingControl?.cancel(false)
        pendingControl = null
    }
}

/**
 * 大响应间歇性 "Connection reset by peer" 的根因修复。
 *
 * 根因（基于 AndServer 2.1.12 + com.yanzhenjie.apache:httpcore:4.4.16 反编译证据）：
 * 1. AndServer 的 `BasicServer$1.run()` 对每个 accept 的连接硬编码了
 *    `SocketConfig.custom().setSoLinger(0).build()`，即 `Socket.setSoLinger(true, 0)`。
 *    SO_LINGER=0 表示「close() 时丢弃未发送完的内核发送缓冲并立即发 TCP RST」，
 *    而非优雅 FIN。多 MB 的大 JSON 响应（如 /api/system/events ≈ 11MB、tables/text ≈ 800KB）
 *    在 WiFi 上单次阻塞 write 排空较慢，若服务端 close()（客户端 `Connection: close`、
 *    keep-alive 空闲超时、或复用策略判定不可复用）恰好发生在客户端尚未 ACK 完最后一窗
 *    数据时，SO_LINGER=0 会把这次关闭变成 RST，客户端读到一半就报 reset。
 *    小响应能瞬间排空进缓冲，永不触发该竞态 —— 与「小端点从不失败、大端点偶发、单发重试成功」吻合。
 * 2. `.timeout(10, SECONDS)` 只映射到 `SocketConfig.setSoTimeout` → `Socket.setSoTimeout`，
 *    在 Java 语义下是**读超时**（仅约束 InputStream.read），对响应写出路径（sendResponseHeader/
 *    sendResponseEntity/flush 走裸 SocketOutputStream）完全无效 —— AndServer 2.1.12 没有写超时。
 * 3. `Server.Builder` 公共接口仅暴露 inetAddress/port/timeout/serverSocketFactory/sslContext/
 *    sslSocketInitializer/listener/build，**无法**配置 SO_LINGER、keep-alive、发送缓冲、写超时。
 *    因此唯一能中和 SO_LINGER=0 的入口就是 `serverSocketFactory(...)`。
 *
 * 修复：注入自定义 ServerSocketFactory，让 accept 出来的 Socket 忽略 AndServer 硬编码的
 * setSoLinger(true, 0)，恢复 JVM 默认的优雅关闭（SO_LINGER 关闭 → FIN），消除 RST 竞态。
 * 不改路由、不改响应 JSON、不做分页。
 */
private object GracefulCloseServerSocketFactory : ServerSocketFactory() {

    override fun createServerSocket(): ServerSocket = GracefulCloseServerSocket()

    override fun createServerSocket(port: Int): ServerSocket =
        throw UnsupportedOperationException("AndServer 自行 bind，不走带参工厂方法")

    override fun createServerSocket(port: Int, backlog: Int): ServerSocket =
        throw UnsupportedOperationException("AndServer 自行 bind，不走带参工厂方法")

    override fun createServerSocket(port: Int, backlog: Int, ifAddress: InetAddress?): ServerSocket =
        throw UnsupportedOperationException("AndServer 自行 bind，不走带参工厂方法")
}

/**
 * 仅重写 [accept]：httpcore 的 RequestListener 会在 accept 之后对每个连接调用
 * `socket.setSoLinger(true, 0)`，这里用重写后的 setSoLinger 把 (true,0) 拦截为 (false,0)，
 * 即关闭 SO_LINGER、退回系统默认的优雅关闭（FIN），避免大响应在 close 时被 RST 截断。
 */
private class GracefulCloseServerSocket : ServerSocket() {

    override fun accept(): Socket {
        val socket = object : Socket() {
            @Throws(SocketException::class)
            override fun setSoLinger(on: Boolean, linger: Int) {
                // 忽略 AndServer 硬编码的 setSoLinger(true, 0)，强制优雅关闭。
                super.setSoLinger(false, 0)
            }
        }
        implAccept(socket)
        return socket
    }
}

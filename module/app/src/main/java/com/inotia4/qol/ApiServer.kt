package com.inotia4.qol

import android.content.Context
import com.inotia4.qol.StaticData
import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.store.ModuleSaveStore
import com.yanzhenjie.andserver.AndServer
import com.yanzhenjie.andserver.Server
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.util.concurrent.TimeUnit
import javax.net.ServerSocketFactory

object ApiServer {

    private var server: Server? = null

    @Volatile
    private var startContext: Context? = null

    @Volatile
    private var startModuleApkPath: String? = null

    /** feature 初始化幂等门禁（bootstrap 只执行一次 feature 初始化） */
    @Volatile
    private var bootstrapped = false

    /** 模块 assets 注入幂等门禁（addAssetPath 不重复执行） */
    @Volatile
    private var assetsInjected = false

    /**
     * 启动入口（HookMain 无条件调用）：**先执行 feature 初始化（仅一次）**，再按
     * `ModuleConfig.apiEnabled` 决定是否启动 HTTP 服务器。
     * `apiEnabled=false` 时 feature 照常初始化（native 预取线程由 applyToNative 内的
     * nativeSetApiEnabled 置为停止），仅不启动 HTTP。
     */
    @Synchronized
    fun bootstrap(context: Context, moduleApkPath: String?) {
        startContext = context
        startModuleApkPath = moduleApkPath
        initFeatures(context, moduleApkPath)
        if (!ModuleConfig.apiEnabled) {
            LogFile.info(LogDomain.HTTP, "ApiServer HTTP disabled by config (apiEnabled=false); features initialized")
            return
        }
        start(context, moduleApkPath)
    }

    /**
     * feature 初始化（仅执行一次，幂等）：静态数据、模块 assets 注入、配置加载、sidecar 存储、
     * 存档备份 native/UI、扩展背包 UI，以及功能开关下发 native（含 nativeSetApiEnabled）。
     * 与 HTTP 服务器无关，`apiEnabled=false` 时同样执行。仅在 [bootstrap] 的同步块内调用。
     */
    private fun initFeatures(context: Context, moduleApkPath: String?) {
        if (bootstrapped) return
        bootstrapped = true
        StaticData.attach(context)
        ensureAssetsInjected(context, moduleApkPath)
        // 模块配置组件：外部 config.json 为唯一来源（v0.5.21 起不再读 assets；缺失用默认值并写入外部存储）
        ModuleConfig.load(context)
        ModuleSaveStore.initialize(context)
        // 存档管理器备份底层（bundle 导出/导入/列表/删除）收口 native feature/save_backup
        NativeBridge.nativeSaveBackupInit(
            context.applicationInfo.dataDir,
            context.getExternalFilesDir(null)?.absolutePath ?: "",
        )
        // 启动期下发 map_id → 中文名表（MAPINFOBASE），native 自存内存表供 entry_json 输出
        // （v0.7.x 给游戏内备份面板的列表项展示地图名）。
        try {
            NativeBridge.nativeSaveBackupSetMapNames(StaticData.buildMapNamesJson())
        } catch (t: Throwable) {
            LogFile.error(LogDomain.HTTP, "savebackup set map names failed", t)
        }
        // 启动游戏内备份面板的懒注入线程（注入 INAP_GOODS 死条目 + 设置页存档备份按钮）
        try {
            val r = NativeBridge.nativeSaveBackupUiInject()
            LogFile.info(LogDomain.HTTP, "savebackup ui inject: $r")
            val sr = NativeBridge.nativeSaveBackupUiSelfCheck()
            LogFile.info(LogDomain.HTTP, "savebackup self check: $sr")
        } catch (t: Throwable) {
            LogFile.error(LogDomain.HTTP, "savebackup ui init failed", t)
        }
        ExtensionBagUiBridge.initialize(context)
        // 功能开关通知 native 生效（含 nativeSetApiEnabled：控制缓存预取线程启停）+ 静态瓦片矩阵加载
        ApiServices.config.applyToNative()
    }

    /**
     * AndServer 通过 context.getAssets() 扫描 .andserver 文件定位注册类。
     * LSPosed 注入场景下 context 是游戏进程的，assets 为游戏 APK；需把模块 APK 加入 AssetManager。
     * 幂等：成功注入一次后不再重复（失败保留重试机会）。
     */
    private fun ensureAssetsInjected(context: Context, moduleApkPath: String?) {
        if (assetsInjected) return
        if (moduleApkPath == null) {
            assetsInjected = true
            return
        }
        try {
            val m = android.content.res.AssetManager::class.java
                .getMethod("addAssetPath", String::class.java)
            m.invoke(context.assets, moduleApkPath)
            assetsInjected = true
            LogFile.info(LogDomain.HTTP, "module assets added: $moduleApkPath")
        } catch (t: Throwable) {
            LogFile.error(LogDomain.HTTP, "addAssetPath failed", t)
        }
    }

    /**
     * 只负责 HTTP 服务器启动（含 assets 注入与端口/地址 fallback）。feature 初始化由
     * [bootstrap] 完成；`apiEnabled=false` 时不会被 bootstrap 调用。
     */
    @Synchronized
    fun start(context: Context, moduleApkPath: String?) {
        startContext = context
        startModuleApkPath = moduleApkPath
        if (server?.isRunning == true) return
        ensureAssetsInjected(context, moduleApkPath)
        startServerWithFallback(context)
    }

    /** 启动 server；端口被占用等失败时回退默认端口重建（v0.5.22） */
    private fun startServerWithFallback(context: Context) {
        if (startServer(context)) return
        if (ModuleConfig.listenPort != ModuleConfig.DEFAULT_LISTEN_PORT) {
            LogFile.info(LogDomain.HTTP, "listenPort=${ModuleConfig.listenPort} start failed, fallback to default ${ModuleConfig.DEFAULT_LISTEN_PORT}")
            ModuleConfig.fallbackListenPortToDefault()
            startServer(context)
        }
    }

    /** 按当前 ModuleConfig 配置构建并启动 AndServer；返回是否启动成功 */
    private fun startServer(context: Context): Boolean = try {
        val builder = AndServer.webServer(context)
            .port(ModuleConfig.listenPort)
            .timeout(10, TimeUnit.SECONDS)
            .serverSocketFactory(GracefulCloseServerSocketFactory)
            .listener(object : Server.ServerListener {
                override fun onStarted() {
                    LogFile.info(LogDomain.HTTP, "AndServer started on ${ModuleConfig.listenAddress}:${ModuleConfig.listenPort}")
                }

                override fun onStopped() {
                    LogFile.info(LogDomain.HTTP, "AndServer stopped")
                }

                override fun onException(e: Exception) {
                    LogFile.error(LogDomain.HTTP, "AndServer error", e)
                }
            })
        // 按配置绑定监听地址；非法地址回退默认绑定（0.0.0.0 通配）
        try {
            builder.inetAddress(InetAddress.getByName(ModuleConfig.listenAddress))
        } catch (e: Exception) {
            LogFile.error(LogDomain.HTTP, "invalid listenAddress=${ModuleConfig.listenAddress}, fallback to wildcard bind", e)
        }
        server = builder.build()
        server?.startup()
        true
    } catch (t: Throwable) {
        LogFile.error(LogDomain.HTTP, "ApiServer start failed", t)
        server = null
        false
    }

    @Synchronized
    fun stop() {
        server?.shutdown()
        server = null
    }

    /** 配置由 false→true 时的启动入口：仅启动 HTTP（feature 已 bootstrap，不重复初始化） */
    @Synchronized
    fun startFromConfig() {
        if (!bootstrapped) {
            LogFile.info(LogDomain.HTTP, "ApiServer startFromConfig skipped: features not bootstrapped")
            return
        }
        if (!ModuleConfig.apiEnabled) return
        if (server?.isRunning == true) return
        val ctx = startContext
        if (ctx == null) {
            LogFile.info(LogDomain.HTTP, "ApiServer startFromConfig skipped: start context unavailable")
            return
        }
        start(ctx, startModuleApkPath)
    }

    /** 按当前 ModuleConfig 监听配置重启服务（配置端点修改监听地址/端口后调用） */
    @Synchronized
    fun restart() {
        val ctx = startContext ?: return
        stop()
        start(ctx, startModuleApkPath)
    }

    /** 延迟重启：先让配置写响应送达客户端，再重启（避免改端口后响应丢失） */
    fun restartDelayed() {
        Thread {
            try {
                Thread.sleep(500)
            } catch (e: InterruptedException) {
                Thread.currentThread().interrupt()
            }
            restart()
        }.start()
    }

    /** 延迟停止：先让配置写响应送达客户端，再停止 HTTP 服务（apiEnabled true→false 时调用） */
    fun stopDelayed() {
        Thread {
            try {
                Thread.sleep(500)
            } catch (e: InterruptedException) {
                Thread.currentThread().interrupt()
            }
            stop()
        }.start()
    }
}

/**
 * 大响应间歇性 "Connection reset by peer" 的根因修复（2026-08-14）。
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

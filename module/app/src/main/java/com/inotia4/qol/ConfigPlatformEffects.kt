package com.inotia4.qol

import com.inotia4.qol.patch.WatermarkOverlay
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/** Native-owned configuration projection used only for platform effects. */
object ConfigPlatformEffects {
    private val executor = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "config-platform-effects").apply { isDaemon = true }
    }
    @Volatile private var latestRevision = 0L
    @Volatile private var initialSnapshot: CountDownLatch? = null

    fun subscribeAndAwaitInitialSnapshot(): Boolean {
        val latch = CountDownLatch(1)
        initialSnapshot = latch
        val subscribed = try {
            NativeBridge.nativeConfigPlatformSubscribe(ConfigPlatformEffects::class.java)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.CONFIG, "native platform config subscription failed", t)
            false
        }
        if (!subscribed) {
            initialSnapshot = null
            return false
        }
        return try {
            latch.await(5, TimeUnit.SECONDS)
        } catch (e: InterruptedException) {
            Thread.currentThread().interrupt()
            false
        } finally {
            initialSnapshot = null
        }
    }

    @JvmStatic
    fun onConfigPlatformUpdate(
        revision: Long,
        listenAddress: String,
        listenPort: Int,
        apiEnabled: Boolean,
        simpleModeEnabled: Boolean,
    ) {
        executor.execute {
            if (revision <= latestRevision) return@execute
            latestRevision = revision
            ApiServer.applyPlatformUpdate(revision, listenAddress, listenPort, apiEnabled)
            WatermarkOverlay.updateSimpleMode(revision, simpleModeEnabled)
            initialSnapshot?.countDown()
        }
    }
}

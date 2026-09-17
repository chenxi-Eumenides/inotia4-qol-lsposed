package com.inotia4.qol.store

import android.content.Context
import com.inotia4.qol.LogDomain
import com.inotia4.qol.LogFile
import com.inotia4.qol.NativeBridge
import java.io.File

/**
 * 每个原版存档槽对应一个模块 sidecar 容器。
 *
 * 容器只保存模块数据，不读取、写入或关联原版 save*.dat 的字节内容。功能以稳定的
 * section 名注册各自不透明的 payload，更新单个 section 时会保留其余未知 section。
 *
 * 本对象是 JNI 委托壳：MSAV 编解码、原子写、last-good 回退与坏容器 quarantine 的
 * 唯一实现在 native `infra/store`（单一全局锁，module-saves/ 唯一文件读写者）；
 * 此处只做参数/返回值转换并保持原公开 API 与语义。进程内只允许 native 侧读写
 * `module-saves/`，Kotlin 侧不再持有任何存储实现。
 */
object ModuleSaveStore {

    data class Section(val version: Int, val payload: ByteArray)

    private const val SLOT_COUNT = 3
    private const val MAX_SECTION_NAME_BYTES = 64
    private const val MAX_SECTION_PAYLOAD_BYTES = 1024 * 1024
    private const val DIRECTORY_NAME = "module-saves"
    private const val FILE_SUFFIX = ".module-save"

    // native infra/store::Status 的稳定取值（见 infra/store.h）；只用到 ok / not_found。
    private const val STORE_OK = 0
    private const val STORE_NOT_FOUND = 1

    private val sectionNamePattern = Regex("[a-z0-9._-]{1,$MAX_SECTION_NAME_BYTES}")

    @Volatile
    private var appContext: Context? = null

    /**
     * 幂等初始化；ApiServer 重启时可安全重复调用。仅记录 appContext 供
     * [primarySnapshotFile] 计算路径；native store 由 nativeSaveBackupInit 注入
     * externalFilesDir 后可用。
     */
    fun initialize(context: Context) {
        appContext = context.applicationContext
    }

    /** 是否已初始化且 native store 可用（module-saves 目录就绪）。桥在启动早期可据此判定存储尚不可用。 */
    fun isInitialized(): Boolean = appContext != null && nativeStoreAvailable()

    /** 确保指定槽的 sidecar 存在且可读取。 */
    fun ensureSlot(slot: Int): Boolean {
        requireSlot(slot)
        return NativeBridge.nativeStoreEnsureSlot(slot) == STORE_OK
    }

    /** 返回 section 的防御性副本；不存在时为 null。 */
    fun readSection(slot: Int, name: String): Section? {
        requireSlot(slot)
        requireSectionName(name)
        val versionOut = IntArray(1)
        val payload = NativeBridge.nativeStoreReadSection(slot, name, versionOut) ?: return null
        return Section(versionOut[0], payload)
    }

    /** 原子替换一个 section，其他（包含未知）section 会原样保留。 */
    fun writeSection(slot: Int, name: String, version: Int, payload: ByteArray): Boolean {
        requireSlot(slot)
        requireSectionName(name)
        require(version > 0) { "section version must be positive" }
        require(payload.size <= MAX_SECTION_PAYLOAD_BYTES) { "section payload too large" }
        return NativeBridge.nativeStoreWriteSection(slot, name, version, payload) == STORE_OK
    }

    /** 删除一个 section；未知 section 仅在调用方明确指定时才会删除。section 本就不存在时返回 true。 */
    fun removeSection(slot: Int, name: String): Boolean {
        requireSlot(slot)
        requireSectionName(name)
        val status = NativeBridge.nativeStoreRemoveSection(slot, name)
        return status == STORE_OK || status == STORE_NOT_FOUND
    }

    /** 原子替换多个 section，并按需删除 section；用于一次保存协调提交。 */
    internal fun replaceSections(
        slot: Int,
        replacements: Map<String, Section>,
        removals: Set<String>,
    ): Boolean {
        requireSlot(slot)
        replacements.keys.forEach(::requireSectionName)
        removals.forEach(::requireSectionName)
        replacements.values.forEach { section ->
            require(section.version > 0) { "section version must be positive" }
            require(section.payload.size <= MAX_SECTION_PAYLOAD_BYTES) { "section payload too large" }
        }
        val names = replacements.keys.toTypedArray()
        val versions = IntArray(names.size) { replacements.getValue(names[it]).version }
        val payloads = Array(names.size) { replacements.getValue(names[it]).payload }
        val status = NativeBridge.nativeStoreReplaceSections(
            slot,
            names,
            versions,
            payloads,
            removals.toTypedArray(),
        )
        return status == STORE_OK
    }

    /** 新建原版存档槽时调用：永久清除该槽旧模块数据后建立空容器。 */
    fun resetSlot(slot: Int): Boolean {
        requireSlot(slot)
        return NativeBridge.nativeStoreResetSlot(slot) == STORE_OK
    }

    /** 返回槽的主容器原始字节（可按该槽解码时；否则 null）。备份导出用。 */
    fun readContainer(slot: Int): ByteArray? {
        requireSlot(slot)
        return NativeBridge.nativeStoreReadContainer(slot)
    }

    /**
     * 从外部容器字节导入一个槽（备份导入用）：先按源容器头第 6 字节存储的槽号校验，
     * 再重编码为目标槽，结果同时原子写入 primary 与 last-good。失败返回 false（目标容器保持原状）。
     */
    fun importContainer(slot: Int, sourceBytes: ByteArray): Boolean {
        requireSlot(slot)
        return NativeBridge.nativeStoreImportContainer(slot, sourceBytes) == STORE_OK
    }

    /**
     * 备份回滚用：槽 primary sidecar 文件路径（仅同模块内部使用）。
     * 内容由 native `infra/store` 写入，本方法只计算路径。
     */
    internal fun primarySnapshotFile(slot: Int): File? {
        requireSlot(slot)
        val base = appContext?.getExternalFilesDir(null) ?: return null
        return File(base, "$DIRECTORY_NAME/slot-$slot$FILE_SUFFIX")
    }

    /** native store 可用性；native 未加载（UnsatisfiedLinkError）时按不可用处理，不崩。 */
    private fun nativeStoreAvailable(): Boolean = try {
        NativeBridge.nativeStoreAvailable()
    } catch (t: Throwable) {
        LogFile.error(LogDomain.SAVE, "module save availability check failed", t)
        false
    }

    private fun requireSlot(slot: Int) {
        require(slot in 0 until SLOT_COUNT) { "slot must be 0-${SLOT_COUNT - 1}" }
    }

    private fun requireSectionName(name: String) {
        require(sectionNamePattern.matches(name)) { "invalid section name" }
    }
}

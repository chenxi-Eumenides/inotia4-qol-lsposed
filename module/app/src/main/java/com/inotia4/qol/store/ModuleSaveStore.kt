package com.inotia4.qol.store

import android.content.Context
import android.util.AtomicFile
import com.inotia4.qol.LogDomain
import com.inotia4.qol.LogFile
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.nio.charset.StandardCharsets
import java.util.LinkedHashMap
import java.util.zip.CRC32

/**
 * 每个原版存档槽对应一个模块 sidecar 容器。
 *
 * 容器只保存模块数据，不读取、写入或关联原版 save*.dat 的字节内容。功能以稳定的
 * section 名注册各自不透明的 payload，更新单个 section 时会保留其余未知 section。
 */
object ModuleSaveStore {

    data class Section(val version: Int, val payload: ByteArray)

    private data class SlotData(
        val generation: Long,
        val sections: LinkedHashMap<String, Section>,
    )

    private data class LoadedSlot(val data: SlotData, val primaryValid: Boolean)

    private const val MAGIC = 0x4D534156 // "MSAV"
    private const val FORMAT_VERSION = 1
    private const val SLOT_COUNT = 3
    private const val MAX_SECTION_NAME_BYTES = 64
    private const val MAX_SECTION_PAYLOAD_BYTES = 1024 * 1024
    private const val MAX_CONTAINER_BYTES = 4 * 1024 * 1024
    private const val DIRECTORY_NAME = "module-saves"
    private const val FILE_SUFFIX = ".module-save"
    private const val LAST_GOOD_SUFFIX = ".last-good"
    // 容器头内存储槽号偏移：u32 magic + u16 formatVersion 之后。
    private const val CONTAINER_SLOT_OFFSET = 6
    private val sectionNamePattern = Regex("[a-z0-9._-]{1,$MAX_SECTION_NAME_BYTES}")
    private val lock = Any()

    @Volatile
    private var appContext: Context? = null

    /** 幂等初始化；ApiServer 重启时可安全重复调用。 */
    fun initialize(context: Context) {
        appContext = context.applicationContext
    }

    /** 是否已初始化（appContext 已设置）。桥在启动早期可据此判定存储尚不可用。 */
    fun isInitialized(): Boolean = appContext != null

    /** 确保指定槽的 sidecar 存在且可读取。 */
    fun ensureSlot(slot: Int): Boolean = synchronized(lock) {
        requireSlot(slot)
        val loaded = loadSlot(slot) ?: return false
        if (loaded.primaryValid) return true
        writeSlot(slot, loaded.data, preserveCurrent = false)
    }

    /** 返回 section 的防御性副本；不存在时为 null。 */
    fun readSection(slot: Int, name: String): Section? = synchronized(lock) {
        requireSlot(slot)
        requireSectionName(name)
        val section = loadSlot(slot)?.data?.sections?.get(name) ?: return null
        Section(section.version, section.payload.copyOf())
    }

    /** 原子替换一个 section，其他（包含未知）section 会原样保留。 */
    fun writeSection(slot: Int, name: String, version: Int, payload: ByteArray): Boolean = synchronized(lock) {
        requireSlot(slot)
        requireSectionName(name)
        require(version > 0) { "section version must be positive" }
        require(payload.size <= MAX_SECTION_PAYLOAD_BYTES) { "section payload too large" }
        val loaded = loadSlot(slot) ?: return false
        val sections = LinkedHashMap(loaded.data.sections)
        sections[name] = Section(version, payload.copyOf())
        writeSlot(slot, SlotData(nextGeneration(loaded.data.generation), sections), preserveCurrent = true)
    }

    /** 删除一个 section；未知 section 仅在调用方明确指定时才会删除。 */
    fun removeSection(slot: Int, name: String): Boolean = synchronized(lock) {
        requireSlot(slot)
        requireSectionName(name)
        val loaded = loadSlot(slot) ?: return false
        if (!loaded.data.sections.containsKey(name)) return true
        val sections = LinkedHashMap(loaded.data.sections)
        sections.remove(name)
        writeSlot(slot, SlotData(nextGeneration(loaded.data.generation), sections), preserveCurrent = true)
    }

    /** 原子替换多个 section，并按需删除 section；用于一次保存协调提交。 */
    internal fun replaceSections(
        slot: Int,
        replacements: Map<String, Section>,
        removals: Set<String>,
    ): Boolean = synchronized(lock) {
        requireSlot(slot)
        replacements.keys.forEach(::requireSectionName)
        removals.forEach(::requireSectionName)
        replacements.values.forEach { section ->
            require(section.version > 0) { "section version must be positive" }
            require(section.payload.size <= MAX_SECTION_PAYLOAD_BYTES) { "section payload too large" }
        }
        val loaded = loadSlot(slot) ?: return false
        val sections = LinkedHashMap(loaded.data.sections)
        removals.forEach(sections::remove)
        replacements.forEach { (name, section) ->
            sections[name] = Section(section.version, section.payload.copyOf())
        }
        writeSlot(slot, SlotData(nextGeneration(loaded.data.generation), sections), preserveCurrent = true)
    }

    /** 新建原版存档槽时调用：永久清除该槽旧模块数据后建立空容器。 */
    fun resetSlot(slot: Int): Boolean = synchronized(lock) {
        requireSlot(slot)
        val primary = primaryFile(slot) ?: return false
        val lastGood = lastGoodFile(slot) ?: return false
        AtomicFile(primary).delete()
        AtomicFile(lastGood).delete()
        writeSlot(slot, SlotData(generation = 1, sections = LinkedHashMap()), preserveCurrent = false)
    }

    /** 返回槽的主容器原始字节（可按该槽解码时；否则 null）。备份导出用。 */
    fun readContainer(slot: Int): ByteArray? = synchronized(lock) {
        requireSlot(slot)
        val bytes = readAtomically(primaryFile(slot) ?: return null)
        if (bytes != null && decode(bytes, slot) != null) bytes else null
    }

    /**
     * 从外部容器字节导入一个槽（备份导入用）：先按源容器头第 6 字节存储的槽号校验，
     * 再重编码为目标槽，结果同时原子写入 primary 与 last-good。失败返回 false（目标容器保持原状）。
     */
    fun importContainer(slot: Int, sourceBytes: ByteArray): Boolean = synchronized(lock) {
        requireSlot(slot)
        val storedSlot = if (sourceBytes.size > CONTAINER_SLOT_OFFSET) {
            sourceBytes[CONTAINER_SLOT_OFFSET].toInt() and 0xFF
        } else {
            -1
        }
        val data = if (storedSlot in 0 until SLOT_COUNT) decode(sourceBytes, storedSlot) else null
        if (data == null) {
            LogFile.info(LogDomain.SAVE, "module save import slot=$slot rejected: source container invalid (storedSlot=$storedSlot)")
            return false
        }
        val encoded = try {
            encode(slot, SlotData(nextGeneration(data.generation), data.sections))
        } catch (t: Throwable) {
            LogFile.error(LogDomain.SAVE, "module save import slot=$slot encode failed", t)
            return false
        }
        val primary = primaryFile(slot) ?: return false
        val lastGood = lastGoodFile(slot) ?: return false
        if (!writeAtomically(primary, encoded)) return false
        if (!writeAtomically(lastGood, encoded)) {
            // primary 已提交且有效；last-good 滞后不阻塞导入，但记录异常供排查。
            LogFile.info(LogDomain.SAVE, "module save import slot=$slot: last-good write failed after primary commit")
            return false
        }
        true
    }

    /** 备份回滚用：槽 primary sidecar 文件路径（仅同模块内部使用）。 */
    internal fun primarySnapshotFile(slot: Int): File? = synchronized(lock) {
        requireSlot(slot)
        primaryFile(slot)
    }

    private fun loadSlot(slot: Int): LoadedSlot? {
        val primary = primaryFile(slot) ?: return null
        decode(readAtomically(primary), slot)?.let { return LoadedSlot(it, primaryValid = true) }
        if (primary.exists()) quarantine(primary, "primary")

        val lastGood = lastGoodFile(slot) ?: return null
        decode(readAtomically(lastGood), slot)?.let {
            LogFile.info(LogDomain.SAVE, "module save slot=$slot recovered from last-good copy")
            return LoadedSlot(it, primaryValid = false)
        }
        if (lastGood.exists()) quarantine(lastGood, "last-good")
        return LoadedSlot(SlotData(generation = 0, sections = LinkedHashMap()), primaryValid = false)
    }

    private fun writeSlot(slot: Int, data: SlotData, preserveCurrent: Boolean): Boolean {
        val primary = primaryFile(slot) ?: return false
        val lastGood = lastGoodFile(slot) ?: return false
        val encoded = try {
            encode(slot, data)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.SAVE, "module save slot=$slot encode failed", t)
            return false
        }

        if (preserveCurrent) {
            val current = readAtomically(primary)
            if (current != null) {
                if (decode(current, slot) != null) {
                    if (!writeAtomically(lastGood, current)) return false
                } else {
                    quarantine(primary, "primary")
                }
            }
        }
        return writeAtomically(primary, encoded)
    }

    private fun encode(slot: Int, data: SlotData): ByteArray {
        require(data.sections.size <= 0xFFFF) { "too many sections" }
        val bytes = ByteArrayOutputStream()
        DataOutputStream(bytes).use { out ->
            out.writeInt(MAGIC)
            out.writeShort(FORMAT_VERSION)
            out.writeByte(slot)
            out.writeLong(data.generation)
            out.writeShort(data.sections.size)
            for ((name, section) in data.sections) {
                requireSectionName(name)
                require(section.version > 0) { "section version must be positive" }
                require(section.payload.size <= MAX_SECTION_PAYLOAD_BYTES) { "section payload too large" }
                val nameBytes = name.toByteArray(StandardCharsets.US_ASCII)
                out.writeByte(nameBytes.size)
                out.write(nameBytes)
                out.writeShort(section.version)
                out.writeInt(section.payload.size)
                out.write(section.payload)
                out.writeInt(crc32(section.payload))
            }
            out.flush()
        }
        val body = bytes.toByteArray()
        require(body.size + Int.SIZE_BYTES <= MAX_CONTAINER_BYTES) { "module save too large" }
        return ByteArrayOutputStream(body.size + Int.SIZE_BYTES).use { full ->
            full.write(body)
            DataOutputStream(full).use { it.writeInt(crc32(body)) }
            full.toByteArray()
        }
    }

    private fun decode(bytes: ByteArray?, expectedSlot: Int): SlotData? {
        if (bytes == null || bytes.size < 4 + 2 + 1 + 8 + 2 + 4 || bytes.size > MAX_CONTAINER_BYTES) return null
        val bodySize = bytes.size - Int.SIZE_BYTES
        val body = bytes.copyOfRange(0, bodySize)
        return try {
            DataInputStream(ByteArrayInputStream(bytes)).use { input ->
                if (input.readInt() != MAGIC) return null
                if (input.readUnsignedShort() != FORMAT_VERSION) return null
                if (input.readUnsignedByte() != expectedSlot) return null
                val generation = input.readLong()
                val count = input.readUnsignedShort()
                val sections = LinkedHashMap<String, Section>(count)
                repeat(count) {
                    val nameLength = input.readUnsignedByte()
                    if (nameLength == 0 || nameLength > MAX_SECTION_NAME_BYTES) return null
                    val nameBytes = ByteArray(nameLength)
                    input.readFully(nameBytes)
                    val name = String(nameBytes, StandardCharsets.US_ASCII)
                    if (!sectionNamePattern.matches(name) || sections.containsKey(name)) return null
                    val version = input.readUnsignedShort()
                    if (version == 0) return null
                    val payloadLength = input.readInt()
                    if (payloadLength < 0 || payloadLength > MAX_SECTION_PAYLOAD_BYTES) return null
                    val payload = ByteArray(payloadLength)
                    input.readFully(payload)
                    if (input.readInt() != crc32(payload)) return null
                    sections[name] = Section(version, payload)
                }
                if (input.readInt() != crc32(body) || input.available() != 0) return null
                SlotData(generation, sections)
            }
        } catch (_: Exception) {
            null
        }
    }

    private fun readAtomically(file: File): ByteArray? = try {
        AtomicFile(file).openRead().use(FileInputStream::readBytes)
    } catch (_: Exception) {
        null
    }

    private fun writeAtomically(file: File, bytes: ByteArray): Boolean {
        val atomicFile = AtomicFile(file)
        val output = try {
            atomicFile.startWrite()
        } catch (t: Throwable) {
            LogFile.error(LogDomain.SAVE, "module save open failed: ${file.name}", t)
            return false
        }
        return try {
            output.write(bytes)
            atomicFile.finishWrite(output)
            true
        } catch (t: Throwable) {
            atomicFile.failWrite(output)
            LogFile.error(LogDomain.SAVE, "module save write failed: ${file.name}", t)
            false
        }
    }

    private fun primaryFile(slot: Int): File? = storageDirectory()?.let { File(it, "slot-$slot$FILE_SUFFIX") }

    private fun lastGoodFile(slot: Int): File? = storageDirectory()?.let {
        File(it, "slot-$slot$FILE_SUFFIX$LAST_GOOD_SUFFIX")
    }

    private fun storageDirectory(): File? {
        val context = appContext
        if (context == null) {
            LogFile.info(LogDomain.SAVE, "module save unavailable: store not initialized")
            return null
        }
        val base = context.getExternalFilesDir(null)
        if (base == null) {
            LogFile.info(LogDomain.SAVE, "module save unavailable: external files directory unavailable")
            return null
        }
        val directory = File(base, DIRECTORY_NAME)
        if (!directory.exists() && !directory.mkdirs()) {
            LogFile.info(LogDomain.SAVE, "module save unavailable: mkdir failed: ${directory.absolutePath}")
            return null
        }
        return directory
    }

    private fun quarantine(file: File, kind: String) {
        val quarantined = File(file.parentFile, "${file.name}.corrupt.${System.currentTimeMillis()}")
        if (file.renameTo(quarantined)) {
            LogFile.info(LogDomain.SAVE, "module save $kind quarantined: ${quarantined.name}")
        } else {
            LogFile.info(LogDomain.SAVE, "module save $kind invalid and could not be quarantined: ${file.name}")
        }
    }

    private fun nextGeneration(generation: Long): Long = if (generation == Long.MAX_VALUE) 1 else generation + 1

    private fun crc32(bytes: ByteArray): Int = CRC32().apply { update(bytes) }.value.toInt()

    private fun requireSlot(slot: Int) {
        require(slot in 0 until SLOT_COUNT) { "slot must be 0-${SLOT_COUNT - 1}" }
    }

    private fun requireSectionName(name: String) {
        require(sectionNamePattern.matches(name)) { "invalid section name" }
    }
}

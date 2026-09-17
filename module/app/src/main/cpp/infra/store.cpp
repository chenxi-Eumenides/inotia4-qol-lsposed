#include "infra/store.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <sys/stat.h>
#include <unistd.h>

// infra/store 实现（见 store.h 的接口契约）。
// MSAV 字节格式与 Kotlin ModuleSaveStore.kt 逐字节对齐；本文件不依赖游戏层。

namespace qol::infra::store {
namespace {

constexpr uint32_t kMagic = 0x4D534156u;  // "MSAV"
constexpr uint16_t kFormatVersion = 1;
constexpr int kSlotCount = 3;
constexpr std::size_t kMaxNameBytes = 64;
constexpr std::size_t kMaxPayloadBytes = 1024 * 1024;
constexpr std::size_t kMaxContainerBytes = 4 * 1024 * 1024;
constexpr int kMinContainerBytes = 4 + 2 + 1 + 8 + 2 + 4;
constexpr int kContainerSlotOffset = 6;  // u32 magic + u16 version 之后

std::mutex g_store_mtx;
std::string g_dir;  // 仅 g_store_mtx 访问；<external>/module-saves
std::atomic<bool> g_available{false};

struct SlotData {
    int64_t generation = 0;
    std::vector<std::pair<std::string, Section>> sections;  // 插入序
};

// ---- CRC32（IEEE 802.3，与 java.util.zip.CRC32 一致）----

uint32_t crc32_table_entry(uint32_t index) {
    uint32_t crc = index;
    for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1u) != 0u ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
    return crc;
}

uint32_t crc32(const uint8_t* data, std::size_t len) {
    static const auto* table = [] {
        auto* entries = new uint32_t[256];
        for (uint32_t i = 0; i < 256; ++i) entries[i] = crc32_table_entry(i);
        return entries;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ---- 字节序工具（DataOutputStream 默认大端）----

void append_u16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
}

void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
}

void append_u64(std::vector<uint8_t>& out, uint64_t value) {
    append_u32(out, static_cast<uint32_t>((value >> 32) & 0xFFFFFFFFu));
    append_u32(out, static_cast<uint32_t>(value & 0xFFFFFFFFu));
}

// ---- section 名与查找 ----

bool valid_name(const char* name) {
    if (name == nullptr) return false;
    const std::size_t len = std::strlen(name);
    if (len == 0 || len > kMaxNameBytes) return false;
    for (std::size_t i = 0; i < len; ++i) {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::vector<std::pair<std::string, Section>>::iterator find_section(
    std::vector<std::pair<std::string, Section>>& sections, const char* name) {
    for (auto it = sections.begin(); it != sections.end(); ++it) {
        if (it->first == name) return it;
    }
    return sections.end();
}

// LinkedHashMap.put 语义：已存在则原位覆盖（不改插入序），否则追加。
void put_section(std::vector<std::pair<std::string, Section>>& sections, const std::string& name,
                 Section section) {
    auto it = find_section(sections, name.c_str());
    if (it != sections.end()) {
        it->second = std::move(section);
        return;
    }
    sections.emplace_back(name, std::move(section));
}

int64_t next_generation(int64_t generation) {
    return generation == INT64_MAX ? 1 : generation + 1;
}

// ---- 编码/解码 ----

bool encode(int slot, const SlotData& data, std::vector<uint8_t>* out) {
    if (data.sections.size() > 0xFFFFu) return false;
    std::vector<uint8_t> body;
    body.reserve(64);
    append_u32(body, kMagic);
    append_u16(body, kFormatVersion);
    body.push_back(static_cast<uint8_t>(slot & 0xFF));
    append_u64(body, static_cast<uint64_t>(data.generation));
    append_u16(body, static_cast<uint16_t>(data.sections.size()));
    for (const auto& entry : data.sections) {
        const std::string& name = entry.first;
        const Section& section = entry.second;
        if (!valid_name(name.c_str()) || section.version == 0 ||
            section.payload.size() > kMaxPayloadBytes) {
            return false;
        }
        body.push_back(static_cast<uint8_t>(name.size()));
        body.insert(body.end(), name.begin(), name.end());
        append_u16(body, section.version);
        append_u32(body, static_cast<uint32_t>(section.payload.size()));
        body.insert(body.end(), section.payload.begin(), section.payload.end());
        append_u32(body, crc32(section.payload.data(), section.payload.size()));
    }
    if (body.size() + 4 > kMaxContainerBytes) return false;
    std::vector<uint8_t> full;
    full.reserve(body.size() + 4);
    full.insert(full.end(), body.begin(), body.end());
    append_u32(full, crc32(body.data(), body.size()));
    *out = std::move(full);
    return true;
}

struct Reader {
    const uint8_t* data;
    std::size_t size;
    std::size_t pos = 0;

    bool u8(uint8_t* out) {
        if (pos + 1 > size) return false;
        *out = data[pos++];
        return true;
    }
    bool u16(uint16_t* out) {
        if (pos + 2 > size) return false;
        *out = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
        pos += 2;
        return true;
    }
    bool u32(uint32_t* out) {
        if (pos + 4 > size) return false;
        *out = (static_cast<uint32_t>(data[pos]) << 24) |
               (static_cast<uint32_t>(data[pos + 1]) << 16) |
               (static_cast<uint32_t>(data[pos + 2]) << 8) |
               static_cast<uint32_t>(data[pos + 3]);
        pos += 4;
        return true;
    }
    bool u64(uint64_t* out) {
        uint32_t hi = 0;
        uint32_t lo = 0;
        if (!u32(&hi) || !u32(&lo)) return false;
        *out = (static_cast<uint64_t>(hi) << 32) | lo;
        return true;
    }
    bool skip(std::size_t n) {
        if (pos + n > size) return false;
        pos += n;
        return true;
    }
};

// 与 Kotlin decode 对齐：magic/version/槽号/计数/名字/CRC/尾 bodyCRC 全部核对。
bool decode(const uint8_t* bytes, std::size_t size, int expected_slot, SlotData* out) {
    if (bytes == nullptr || size < static_cast<std::size_t>(kMinContainerBytes) ||
        size > kMaxContainerBytes) {
        return false;
    }
    const std::size_t body_size = size - 4;
    const uint32_t body_crc = (static_cast<uint32_t>(bytes[body_size]) << 24) |
                              (static_cast<uint32_t>(bytes[body_size + 1]) << 16) |
                              (static_cast<uint32_t>(bytes[body_size + 2]) << 8) |
                              static_cast<uint32_t>(bytes[body_size + 3]);
    if (body_crc != crc32(bytes, body_size)) return false;

    Reader reader{bytes, body_size};
    uint32_t magic = 0;
    uint16_t version = 0;
    uint8_t slot = 0;
    uint64_t generation = 0;
    uint16_t count = 0;
    if (!reader.u32(&magic) || magic != kMagic) return false;
    if (!reader.u16(&version) || version != kFormatVersion) return false;
    if (!reader.u8(&slot) || slot != static_cast<uint8_t>(expected_slot)) return false;
    if (!reader.u64(&generation)) return false;
    if (!reader.u16(&count)) return false;

    SlotData data;
    data.generation = static_cast<int64_t>(generation);
    data.sections.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        uint8_t name_len = 0;
        if (!reader.u8(&name_len) || name_len == 0 || name_len > kMaxNameBytes) return false;
        std::string name(static_cast<std::size_t>(name_len), '\0');
        if (reader.pos + name_len > reader.size) return false;
        std::memcpy(name.data(), bytes + reader.pos, name_len);
        reader.pos += name_len;
        if (!valid_name(name.c_str())) return false;
        if (find_section(data.sections, name.c_str()) != data.sections.end()) return false;
        uint16_t section_version = 0;
        if (!reader.u16(&section_version) || section_version == 0) return false;
        uint32_t payload_len = 0;
        if (!reader.u32(&payload_len)) return false;
        if (payload_len > kMaxPayloadBytes) return false;
        if (reader.pos + payload_len > reader.size) return false;
        Section section;
        section.version = section_version;
        section.payload.assign(bytes + reader.pos, bytes + reader.pos + payload_len);
        reader.pos += payload_len;
        uint32_t payload_crc = 0;
        if (!reader.u32(&payload_crc) || payload_crc != crc32(section.payload.data(),
                                                              section.payload.size())) {
            return false;
        }
        data.sections.emplace_back(std::move(name), std::move(section));
    }
    if (reader.pos != body_size) return false;
    *out = std::move(data);
    return true;
}

// ---- 文件原语 ----

bool path_exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool ensure_dir(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return ::mkdir(path.c_str(), 0755) == 0;
}

// AtomicFile 式原子写：写 <path>.tmp 后 rename；失败清理临时文件。
bool write_atomic(const std::string& path, const std::vector<uint8_t>& bytes) {
    const std::string tmp = path + ".tmp";
    FILE* file = std::fopen(tmp.c_str(), "wb");
    if (file == nullptr) return false;
    bool ok = bytes.empty() ||
              std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    if (ok && std::fflush(file) != 0) ok = false;
    if (ok && ::fsync(::fileno(file)) != 0) ok = false;
    if (std::fclose(file) != 0) ok = false;
    if (!ok) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

// 读取整个文件；文件不存在/读失败返回 false（out 清空）。
bool read_atomic(const std::string& path, std::vector<uint8_t>* out) {
    out->clear();
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    uint8_t buffer[8192];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out->insert(out->end(), buffer, buffer + n);
    }
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    if (!ok) out->clear();
    return ok;
}

std::string primary_path_locked(int slot) {
    return g_dir + "/slot-" + std::to_string(slot) + ".module-save";
}

std::string last_good_path_locked(int slot) {
    return primary_path_locked(slot) + ".last-good";
}

// 坏容器 quarantine 为 <name>.corrupt.<ms>，不删除。
void quarantine_locked(const std::string& path) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const std::string target = path + ".corrupt." + std::to_string(now);
    ::rename(path.c_str(), target.c_str());
}

bool valid_slot(int slot) { return slot >= 0 && slot < kSlotCount; }

// 读取 primary；无效 → 尝试 last-good（并 quarantine 无效文件）。
// 返回 SlotData 与 primary 是否有效；两者皆无 → 空数据 generation=0。
struct LoadResult {
    SlotData data;
    bool primary_valid = false;
};

LoadResult load_slot_locked(int slot) {
    LoadResult result;
    const std::string primary = primary_path_locked(slot);
    std::vector<uint8_t> bytes;
    if (read_atomic(primary, &bytes) && decode(bytes.data(), bytes.size(), slot, &result.data)) {
        result.primary_valid = true;
        return result;
    }
    if (path_exists(primary)) quarantine_locked(primary);

    const std::string last_good = last_good_path_locked(slot);
    if (read_atomic(last_good, &bytes) && decode(bytes.data(), bytes.size(), slot, &result.data)) {
        result.primary_valid = false;
        return result;
    }
    if (path_exists(last_good)) quarantine_locked(last_good);
    result.data = SlotData{};
    result.primary_valid = false;
    return result;
}

Status write_slot_locked(int slot, const SlotData& data, bool preserve_current) {
    const std::string primary = primary_path_locked(slot);
    const std::string last_good = last_good_path_locked(slot);
    std::vector<uint8_t> encoded;
    if (!encode(slot, data, &encoded)) return Status::kTooLarge;

    if (preserve_current) {
        std::vector<uint8_t> current;
        if (read_atomic(primary, &current)) {
            SlotData ignored;
            if (decode(current.data(), current.size(), slot, &ignored)) {
                if (!write_atomic(last_good, current)) return Status::kIoError;
            } else {
                quarantine_locked(primary);
            }
        }
    }
    return write_atomic(primary, encoded) ? Status::kOk : Status::kIoError;
}

Status require_ready(int slot) {
    if (!g_available.load(std::memory_order_acquire)) return Status::kUnavailable;
    if (!valid_slot(slot)) return Status::kBadSlot;
    return Status::kOk;
}

}  // namespace

const char* status_name(Status status) {
    switch (status) {
        case Status::kOk: return "ok";
        case Status::kNotFound: return "not_found";
        case Status::kCorrupt: return "corrupt";
        case Status::kIoError: return "io_error";
        case Status::kUnavailable: return "unavailable";
        case Status::kBadSlot: return "bad_slot";
        case Status::kBadVersion: return "bad_version";
        case Status::kTooLarge: return "too_large";
        case Status::kBadName: return "bad_name";
    }
    return "unknown";
}

void init(const char* external_files_dir) {
    std::lock_guard<std::mutex> lock(g_store_mtx);
    g_available.store(false, std::memory_order_release);
    g_dir.clear();
    if (external_files_dir == nullptr || external_files_dir[0] == '\0') return;
    const std::string dir = std::string(external_files_dir) + "/module-saves";
    if (!ensure_dir(dir)) return;
    g_dir = dir;
    g_available.store(true, std::memory_order_release);
}

bool available() {
    return g_available.load(std::memory_order_acquire);
}

Status ensure_slot(int slot) {
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const Status ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    const LoadResult loaded = load_slot_locked(slot);
    if (loaded.primary_valid) return Status::kOk;
    return write_slot_locked(slot, loaded.data, /*preserve_current=*/false);
}

Status reset_slot(int slot) {
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const Status ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    ::unlink(primary_path_locked(slot).c_str());
    ::unlink(last_good_path_locked(slot).c_str());
    SlotData empty;
    empty.generation = 1;
    return write_slot_locked(slot, empty, /*preserve_current=*/false);
}

Status delete_slot(int slot) {
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const Status ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    ::unlink(primary_path_locked(slot).c_str());
    ::unlink(last_good_path_locked(slot).c_str());
    return Status::kOk;
}

Status read_container(int slot, std::vector<uint8_t>* out) {
    if (out == nullptr) return Status::kBadSlot;
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const Status ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    std::vector<uint8_t> bytes;
    if (!read_atomic(primary_path_locked(slot), &bytes)) return Status::kNotFound;
    SlotData ignored;
    if (!decode(bytes.data(), bytes.size(), slot, &ignored)) return Status::kCorrupt;
    *out = std::move(bytes);
    return Status::kOk;
}

Status import_container(int slot, const uint8_t* data, std::size_t len) {
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const Status ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    if (data == nullptr) return Status::kCorrupt;
    int stored_slot = -1;
    if (len > static_cast<std::size_t>(kContainerSlotOffset)) {
        stored_slot = static_cast<int>(data[kContainerSlotOffset]);
    }
    if (stored_slot < 0 || stored_slot >= kSlotCount) return Status::kCorrupt;
    SlotData data_slot;
    if (!decode(data, len, stored_slot, &data_slot)) return Status::kCorrupt;

    SlotData target;
    target.generation = next_generation(data_slot.generation);
    target.sections = data_slot.sections;
    std::vector<uint8_t> encoded;
    if (!encode(slot, target, &encoded)) return Status::kTooLarge;
    if (!write_atomic(primary_path_locked(slot), encoded)) return Status::kIoError;
    if (!write_atomic(last_good_path_locked(slot), encoded)) {
        // primary 已提交且有效；last-good 滞后不阻塞导入（与 Kotlin 语义一致）。
        return Status::kIoError;
    }
    return Status::kOk;
}

Status read_section(int slot, const char* name, std::vector<uint8_t>* out, uint16_t* version) {
    if (out == nullptr || version == nullptr || !valid_name(name)) return Status::kBadName;
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const Status ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    SlotData data_slot = load_slot_locked(slot).data;
    auto it = find_section(data_slot.sections, name);
    if (it == data_slot.sections.end()) return Status::kNotFound;
    *out = it->second.payload;
    *version = it->second.version;
    return Status::kOk;
}

Status write_section(int slot, const char* name, uint16_t version, const uint8_t* data,
                     std::size_t len) {
    if (!valid_name(name)) return Status::kBadName;
    if (version == 0) return Status::kBadVersion;
    if (len > kMaxPayloadBytes) return Status::kTooLarge;
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const Status ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    SlotData data_slot = load_slot_locked(slot).data;
    Section section;
    section.version = version;
    if (len > 0) section.payload.assign(data, data + len);
    put_section(data_slot.sections, name, std::move(section));
    data_slot.generation = next_generation(data_slot.generation);
    return write_slot_locked(slot, data_slot, /*preserve_current=*/true);
}

Status remove_section(int slot, const char* name) {
    if (!valid_name(name)) return Status::kBadName;
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const Status ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    SlotData data_slot = load_slot_locked(slot).data;
    auto it = find_section(data_slot.sections, name);
    if (it == data_slot.sections.end()) return Status::kNotFound;
    data_slot.sections.erase(it);
    data_slot.generation = next_generation(data_slot.generation);
    return write_slot_locked(slot, data_slot, /*preserve_current=*/true);
}

Status replace_sections(int slot, const ReplaceMap& replacements, const RemoveSet& removals) {
    for (const auto& entry : replacements) {
        if (!valid_name(entry.first.c_str())) return Status::kBadName;
        if (entry.second.version == 0) return Status::kBadVersion;
        if (entry.second.payload.size() > kMaxPayloadBytes) return Status::kTooLarge;
    }
    for (const std::string& name : removals) {
        if (!valid_name(name.c_str())) return Status::kBadName;
    }
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const Status ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    SlotData data_slot = load_slot_locked(slot).data;
    for (const std::string& name : removals) {
        auto it = find_section(data_slot.sections, name.c_str());
        if (it != data_slot.sections.end()) data_slot.sections.erase(it);
    }
    for (const auto& entry : replacements) {
        put_section(data_slot.sections, entry.first, entry.second);
    }
    data_slot.generation = next_generation(data_slot.generation);
    return write_slot_locked(slot, data_slot, /*preserve_current=*/true);
}

}  // namespace qol::infra::store

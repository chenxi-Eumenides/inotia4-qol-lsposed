#include "infra/store.h"
#include "infra/slot_operation_gate.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <array>
#include <cerrno>
#include <algorithm>
#include <limits>

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

std::mutex& g_store_mtx = slot_operation::mutex();
std::string g_dir;  // 仅 g_store_mtx 访问；<external>/module-saves
std::atomic<bool> g_available{false};
std::array<contracts::QolOwnerStoreReservation, kSlotCount> g_reservations{};
uint64_t g_next_reservation = 1;
#if defined(QOL_STORE_TEST_SEAMS)
int (*g_unlink_for_test)(const char*) = nullptr;
#endif

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
        if (body.size() > kMaxContainerBytes - 4 ||
            11 + name.size() > kMaxContainerBytes - 4 - body.size() ||
            section.payload.size() > kMaxContainerBytes - 4 - body.size() - 11 - name.size()) return false;
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
        if (!valid_name(name.c_str()) || std::strlen(name.c_str()) != name.size()) return false;
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
Status read_strict(const std::string& path, std::vector<uint8_t>* out) {
    out->clear();
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return errno == ENOENT ? Status::kNotFound : Status::kIoError;
    struct stat info{};
    if (::fstat(::fileno(file), &info) != 0 || !S_ISREG(info.st_mode)) {
        std::fclose(file);
        return Status::kIoError;
    }
    if (info.st_size < 0 || static_cast<uint64_t>(info.st_size) > kMaxContainerBytes) {
        std::fclose(file);
        return Status::kCorrupt;
    }
    uint8_t buffer[8192];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (n > kMaxContainerBytes - out->size()) {
            std::fclose(file);
            out->clear();
            return Status::kCorrupt;
        }
        out->insert(out->end(), buffer, buffer + n);
    }
    bool ok = std::ferror(file) == 0;
    if (std::fclose(file) != 0) ok = false;
    if (!ok) out->clear();
    return ok ? Status::kOk : Status::kIoError;
}
bool read_atomic(const std::string& path, std::vector<uint8_t>* out) {
    return read_strict(path, out) == Status::kOk;
}

std::string primary_path_locked(int slot) {
    return g_dir + "/slot-" + std::to_string(slot) + ".module-save";
}

std::string last_good_path_locked(int slot) {
    return primary_path_locked(slot) + ".last-good";
}

// 坏容器 quarantine 为 <name>.corrupt.<ms>，不删除。
bool quarantine_locked(const std::string& path) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    std::string target = path + ".corrupt." + std::to_string(now);
    while (path_exists(target)) target = path + ".corrupt." + std::to_string(++now);
    return ::rename(path.c_str(), target.c_str()) == 0;
}

bool valid_slot(int slot) { return slot >= 0 && slot < kSlotCount; }

bool unlink_checked_locked(const std::string& path) {
#if defined(QOL_STORE_TEST_SEAMS)
    const int result = g_unlink_for_test != nullptr ? g_unlink_for_test(path.c_str()) : ::unlink(path.c_str());
#else
    const int result = ::unlink(path.c_str());
#endif
    return result == 0 || errno == ENOENT;
}

// 读取 primary；无效 → 尝试 last-good（并 quarantine 无效文件）。
// 返回 SlotData 与 primary 是否有效；两者皆无 → 空数据 generation=0。
struct LoadResult {
    SlotData data;
    bool primary_valid = false;
    Status status = Status::kNotFound;
    contracts::QolOwnerStoreState state = contracts::QolOwnerStoreState::kAbsent;
};

LoadResult load_slot_locked(int slot) {
    LoadResult result;
    const std::string primary = primary_path_locked(slot);
    std::vector<uint8_t> bytes;
    auto primary_status = read_strict(primary, &bytes);
    if (primary_status == Status::kOk && decode(bytes.data(), bytes.size(), slot, &result.data)) {
        result.primary_valid = true;
        result.status = Status::kOk;
        result.state = contracts::QolOwnerStoreState::kPrimaryValid;
        return result;
    }
    if (primary_status == Status::kOk) primary_status = Status::kCorrupt;

    const std::string last_good = last_good_path_locked(slot);
    auto fallback_status = read_strict(last_good, &bytes);
    if (fallback_status == Status::kOk && decode(bytes.data(), bytes.size(), slot, &result.data)) {
        if (primary_status == Status::kCorrupt && !quarantine_locked(primary)) {
            result.status = Status::kIoError;
            result.state = contracts::QolOwnerStoreState::kIoFailed;
            return result;
        }
        result.primary_valid = false;
        result.status = Status::kOk;
        result.state = contracts::QolOwnerStoreState::kFallbackUsed;
        return result;
    }
    if (fallback_status == Status::kOk) fallback_status = Status::kCorrupt;
    // quarantine 会移走原路径，持久标记防止后续调用或重启把双损坏误判为新槽。
    // 显式 reset/delete 才消除未决标记；损坏证据文件不删除。
    const auto marker = primary + ".unresolved-corrupt";
    const bool corrupt = primary_status == Status::kCorrupt || fallback_status == Status::kCorrupt;
    bool evidence_ok = true;
    if (corrupt) {
        evidence_ok = write_atomic(marker, {1});
        if (evidence_ok && primary_status == Status::kCorrupt) evidence_ok = quarantine_locked(primary);
        if (evidence_ok && fallback_status == Status::kCorrupt) evidence_ok = quarantine_locked(last_good);
    }
    result.data = SlotData{};
    result.primary_valid = false;
    if (!evidence_ok || primary_status == Status::kIoError || fallback_status == Status::kIoError) {
        result.status = Status::kIoError;
        result.state = contracts::QolOwnerStoreState::kIoFailed;
    } else if (corrupt || path_exists(marker)) {
        result.status = Status::kCorrupt;
        result.state = contracts::QolOwnerStoreState::kCorrupt;
    }
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

Status gate_admission_locked(uint64_t owner, const contracts::QolSlotOperationLease* lease, int slot) {
    if (lease == nullptr) return slot_operation::busy_locked(slot) ? Status::kBusy : Status::kOk;
    const auto result = slot_operation::validate_locked(owner, lease, slot);
    if (contracts::succeeded(result)) return Status::kOk;
    return result.code == kQolErrBusy ? Status::kBusy : Status::kBadSlot;
}

}  // namespace

const char* status_name(Status status) {
    switch (status) {
        case Status::kBusy: return "busy";
        case Status::kBlocked: return "blocked";
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
    if ((std::any_of(g_reservations.begin(), g_reservations.end(), [](const auto& held) { return held.id != 0; }) ||
         slot_operation::busy_locked(0) || slot_operation::busy_locked(1) || slot_operation::busy_locked(2)) &&
        (external_files_dir == nullptr || g_dir != std::string(external_files_dir) + "/module-saves")) return;
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

namespace {
Status ensure_common(int slot, uint64_t owner, const contracts::QolSlotOperationLease* lease) {
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const Status ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    const auto gate = gate_admission_locked(owner, lease, slot);
    if (gate != Status::kOk) return gate;
    if (g_reservations[slot].id != 0) return Status::kBusy;
    const LoadResult loaded = load_slot_locked(slot);
    if (loaded.status != Status::kOk && loaded.status != Status::kNotFound) return loaded.status;
    if (loaded.primary_valid) return Status::kOk;
    return write_slot_locked(slot, loaded.data, /*preserve_current=*/false);
}

Status reset_common(int slot, uint64_t owner, const contracts::QolSlotOperationLease* lease) {
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const Status ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    const auto gate = gate_admission_locked(owner, lease, slot);
    if (gate != Status::kOk) return gate;
    if (g_reservations[slot].id != 0) return Status::kBusy;
    // 先确认旧回退文件删除，再替换主容器，避免失败时留下可复活旧槽的 last-good。
    if (!unlink_checked_locked(last_good_path_locked(slot)) ||
        !unlink_checked_locked(primary_path_locked(slot))) return Status::kIoError;
    SlotData empty;
    empty.generation = 1;
    const auto result = write_slot_locked(slot, empty, /*preserve_current=*/false);
    if (result == Status::kOk &&
        !unlink_checked_locked(primary_path_locked(slot) + ".unresolved-corrupt")) return Status::kIoError;
    return result;
}

Status delete_common(int slot, uint64_t owner, const contracts::QolSlotOperationLease* lease) {
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const Status ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    const auto gate = gate_admission_locked(owner, lease, slot);
    if (gate != Status::kOk) return gate;
    if (g_reservations[slot].id != 0) return Status::kBusy;
    if (!unlink_checked_locked(last_good_path_locked(slot)) ||
        !unlink_checked_locked(primary_path_locked(slot))) return Status::kIoError;
    if (!unlink_checked_locked(primary_path_locked(slot) + ".unresolved-corrupt")) return Status::kIoError;
    return Status::kOk;
}
}
Status ensure_slot(int slot) { return ensure_common(slot, 0, nullptr); }
Status reset_slot(int slot) { return reset_common(slot, 0, nullptr); }
Status delete_slot(int slot) { return delete_common(slot, 0, nullptr); }
Status ensure_in_operation(uint64_t owner, const contracts::QolSlotOperationLease* lease, int slot) {
    if (lease == nullptr) return Status::kBadSlot;
    return ensure_common(slot, owner, lease);
}
Status reset_in_operation(uint64_t owner, const contracts::QolSlotOperationLease* lease, int slot) {
    if (lease == nullptr) return Status::kBadSlot;
    return reset_common(slot, owner, lease);
}
Status delete_in_operation(uint64_t owner, const contracts::QolSlotOperationLease* lease, int slot) {
    if (lease == nullptr) return Status::kBadSlot;
    return delete_common(slot, owner, lease);
}

Status read_container(int slot, std::vector<uint8_t>* out) {
    if (out == nullptr) return Status::kBadSlot;
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const Status ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    std::vector<uint8_t> bytes;
    const auto read = read_strict(primary_path_locked(slot), &bytes);
    if (read != Status::kOk) {
        if (read == Status::kNotFound && path_exists(primary_path_locked(slot) + ".unresolved-corrupt")) return Status::kCorrupt;
        return read;
    }
    SlotData ignored;
    if (!decode(bytes.data(), bytes.size(), slot, &ignored)) return Status::kCorrupt;
    *out = std::move(bytes);
    return Status::kOk;
}

namespace {
Status import_common(int slot, const uint8_t* data, std::size_t len, uint64_t owner,
                     const contracts::QolSlotOperationLease* lease, contracts::QolSlotOperationApply* apply) {
    if (apply != nullptr) apply->apply = static_cast<uint32_t>(contracts::QolSlotOperationApplyState::kUnchanged);
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const Status ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    const auto gate = gate_admission_locked(owner, lease, slot);
    if (gate != Status::kOk) return gate;
    if (g_reservations[slot].id != 0) return Status::kBusy;
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
    if (apply != nullptr) apply->apply = static_cast<uint32_t>(contracts::QolSlotOperationApplyState::kPrimaryApplied);
    if (!write_atomic(last_good_path_locked(slot), encoded)) {
        // primary 已提交且有效；last-good 滞后不阻塞导入（与 Kotlin 语义一致）。
        return Status::kIoError;
    }
    if (apply != nullptr) apply->apply = static_cast<uint32_t>(contracts::QolSlotOperationApplyState::kFullyApplied);
    return Status::kOk;
}
}
Status import_container(int slot, const uint8_t* bytes, std::size_t size) {
    return import_common(slot, bytes, size, 0, nullptr, nullptr);
}
Status import_in_operation(uint64_t owner, const contracts::QolSlotOperationLease* lease, int slot,
                           const uint8_t* bytes, std::size_t size, contracts::QolSlotOperationApply* apply) {
    if (apply == nullptr || apply->size < sizeof(*apply) || lease == nullptr) return Status::kBadSlot;
    return import_common(slot, bytes, size, owner, lease, apply);
}

Status read_section(int slot, const char* name, std::vector<uint8_t>* out, uint16_t* version) {
    if (out == nullptr || version == nullptr || !valid_name(name)) return Status::kBadName;
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const Status ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    const auto loaded = load_slot_locked(slot);
    if (loaded.status != Status::kOk) return loaded.status;
    SlotData data_slot = loaded.data;
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
    if (len != 0 && data == nullptr) return Status::kBadName;
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const Status ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    if (slot_operation::busy_locked(slot)) return Status::kBusy;
    if (g_reservations[slot].id != 0) return Status::kBusy;
    const auto loaded = load_slot_locked(slot);
    if (loaded.status != Status::kOk && loaded.status != Status::kNotFound) return loaded.status;
    SlotData data_slot = loaded.data;
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
    if (slot_operation::busy_locked(slot)) return Status::kBusy;
    if (g_reservations[slot].id != 0) return Status::kBusy;
    const auto loaded = load_slot_locked(slot);
    if (loaded.status != Status::kOk) return loaded.status;
    SlotData data_slot = loaded.data;
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
    if (slot_operation::busy_locked(slot)) return Status::kBusy;
    if (g_reservations[slot].id != 0) return Status::kBusy;
    const auto loaded = load_slot_locked(slot);
    if (loaded.status != Status::kOk && loaded.status != Status::kNotFound) return loaded.status;
    SlotData data_slot = loaded.data;
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

namespace {
bool credential_matches(const contracts::QolOwnerStoreReservation& held,
                        const contracts::QolOwnerStoreReservation& credential) {
    return held.id != 0 && held.id == credential.id && held.slot == credential.slot &&
           held.owner == credential.owner && held.generation == credential.generation &&
           held.revision == credential.revision && held.encoded_bytes == credential.encoded_bytes &&
           held.operation == credential.operation;
}
Status apply_candidate(const SlotData& current, const contracts::QolOwnerStoreCandidate& candidate,
                       SlotData* target) {
    if (candidate.size < sizeof(candidate) ||
        (candidate.replacement_count != 0 && candidate.replacements == nullptr) ||
        (candidate.removal_count != 0 && candidate.removals == nullptr)) return Status::kBadName;
    *target = current;
    for (uint32_t i = 0; i < candidate.removal_count; ++i) {
        const auto name = candidate.removals[i];
        if (!valid_name(name)) return Status::kBadName;
        auto it = find_section(target->sections, name);
        if (it != target->sections.end()) target->sections.erase(it);
    }
    const auto add = [&](const contracts::QolStoreSection& section) {
        if (!valid_name(section.name) || (section.size != 0 && section.data == nullptr)) return Status::kBadName;
        if (section.version == 0) return Status::kBadVersion;
        if (section.size > kMaxPayloadBytes) return Status::kTooLarge;
        Section copy;
        copy.version = section.version;
        if (section.size != 0) copy.payload.assign(section.data, section.data + section.size);
        put_section(target->sections, section.name, std::move(copy));
        return Status::kOk;
    };
    for (uint32_t i = 0; i < candidate.replacement_count; ++i) {
        const auto status = add(candidate.replacements[i]);
        if (status != Status::kOk) return status;
    }
    if (candidate.journal != nullptr) {
        if (candidate.journal->name == nullptr ||
            std::strcmp(candidate.journal->name, "module.save.journal") != 0) return Status::kBadName;
        const auto status = add(*candidate.journal);
        if (status != Status::kOk) return status;
    }
    target->generation = next_generation(current.generation);
    return Status::kOk;
}
Status preflight_locked(int slot, const contracts::QolOwnerStoreCandidate* candidates,
                        uint32_t count, uint64_t* lengths, LoadResult* current) {
    if (count == 0 || count > 3 || candidates == nullptr || lengths == nullptr) return Status::kBadName;
    *current = load_slot_locked(slot);
    if (current->status != Status::kOk && current->status != Status::kNotFound) return current->status;
    for (uint32_t i = 0; i < count; ++i) {
        SlotData candidate;
        auto status = apply_candidate(current->data, candidates[i], &candidate);
        if (status != Status::kOk) return status;
        std::vector<uint8_t> encoded;
        if (!encode(slot, candidate, &encoded)) return Status::kTooLarge;
        lengths[i] = encoded.size();
    }
    return Status::kOk;
}
}

Status inspect_slot(int slot, contracts::QolOwnerStoreSlotState* out) {
    if (out == nullptr || out->size < sizeof(*out)) return Status::kBadName;
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const auto ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    const auto loaded = load_slot_locked(slot);
    out->state = static_cast<uint32_t>(loaded.state);
    out->generation = static_cast<uint64_t>(loaded.data.generation);
    out->encoded_bytes = 0;
    out->reservation = g_reservations[slot];
    if (loaded.status == Status::kOk) {
        std::vector<uint8_t> encoded;
        if (!encode(slot, loaded.data, &encoded)) return Status::kCorrupt;
        out->encoded_bytes = encoded.size();
    }
    // 状态本身是 inspect 的输出；损坏/IO 不可用不会伪装为 absent。
    return Status::kOk;
}
Status preflight(int slot, const contracts::QolOwnerStoreCandidate* candidates,
                 uint32_t count, uint64_t* lengths) {
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const auto ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    LoadResult current;
    return preflight_locked(slot, candidates, count, lengths, &current);
}
namespace {
Status reserve_common(uint64_t owner, int slot, const contracts::QolOwnerStoreCandidate* candidates,
                       uint32_t count, contracts::QolOwnerStoreReservation* out,
                       const contracts::QolSlotOperationLease* lease) {
    if (out == nullptr || out->size < sizeof(*out) || owner == 0) return Status::kBadName;
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const auto ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    const auto gate = gate_admission_locked(owner, lease, slot);
    if (gate != Status::kOk) return gate;
    if (g_reservations[slot].id != 0) return Status::kBusy;
    uint64_t lengths[3]{};
    LoadResult loaded;
    const auto status = preflight_locked(slot, candidates, count, lengths, &loaded);
    if (status != Status::kOk) return status;
    if (g_next_reservation == std::numeric_limits<uint64_t>::max()) return Status::kBusy;
    g_reservations[slot] = {sizeof(*out), slot, g_next_reservation++,
        static_cast<uint64_t>(loaded.data.generation), owner,
        *std::max_element(lengths, lengths + count), 0, 0, 1, lease == nullptr ? 0 : lease->id};
    *out = g_reservations[slot];
    return Status::kOk;
}
}
Status reserve(uint64_t owner, int slot, const contracts::QolOwnerStoreCandidate* candidates,
               uint32_t count, contracts::QolOwnerStoreReservation* out) {
    return reserve_common(owner, slot, candidates, count, out, nullptr);
}
Status reserve_in_operation(uint64_t owner, const contracts::QolSlotOperationLease* lease, int slot,
    const contracts::QolOwnerStoreCandidate* candidates, uint32_t count, contracts::QolOwnerStoreReservation* out) {
    if (lease == nullptr) return Status::kBadSlot;
    return reserve_common(owner, slot, candidates, count, out, lease);
}
Status release(uint64_t owner, const contracts::QolOwnerStoreReservation& reservation) {
    if (reservation.size < sizeof(reservation) || !valid_slot(reservation.slot) ||
        reservation.owner != owner || owner == 0) return Status::kBadSlot;
    std::lock_guard<std::mutex> lock(g_store_mtx);
    auto& held = g_reservations[reservation.slot];
    if (held.id == 0 || held.id != reservation.id) return Status::kNotFound;
    if (!credential_matches(held, reservation)) return Status::kBusy;
    if (!slot_operation::reservation_admitted_locked(owner, held.operation, held.slot)) return Status::kBusy;
    held = {};
    return Status::kOk;
}
Status replace_reserved(uint64_t owner, int slot, contracts::QolOwnerStoreReservation* credential,
                        const ReplaceMap& replacements, const RemoveSet& removals) {
    if (credential == nullptr || credential->size < sizeof(*credential) || credential->slot != slot ||
        credential->owner != owner || owner == 0) return Status::kBadSlot;
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const auto ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    auto& held = g_reservations[slot];
    if (!credential_matches(held, *credential) || held.revision == UINT64_MAX)
        return Status::kBusy;
    if (!slot_operation::reservation_admitted_locked(owner, held.operation, slot)) return Status::kBusy;
    const auto loaded = load_slot_locked(slot);
    if (loaded.status != Status::kOk && loaded.status != Status::kNotFound) return loaded.status;
    if (static_cast<uint64_t>(loaded.data.generation) != held.generation) return Status::kBusy;
    SlotData target = loaded.data;
    for (const auto& name : removals) {
        if (!valid_name(name.c_str())) return Status::kBadName;
        auto it = find_section(target.sections, name.c_str());
        if (it != target.sections.end()) target.sections.erase(it);
    }
    for (const auto& item : replacements) {
        if (!valid_name(item.first.c_str())) return Status::kBadName;
        if (item.second.version == 0) return Status::kBadVersion;
        if (item.second.payload.size() > kMaxPayloadBytes) return Status::kTooLarge;
        put_section(target.sections, item.first, item.second);
    }
    target.generation = next_generation(target.generation);
    std::vector<uint8_t> encoded;
    if (!encode(slot, target, &encoded) || encoded.size() > held.encoded_bytes) return Status::kTooLarge;
    // 引用状态只由 set_referenced 修改；可复制凭据中的 retain 不能撤销在途保护。
    const auto status = write_slot_locked(slot, target, true);
    if (status == Status::kOk) {
        held.generation = static_cast<uint64_t>(target.generation);
        ++held.revision;
        *credential = held;
    } else if (held.retain == 0) {
        // 原版尚未开始的 prepared 写失败，不应留下永久占用；pending 必须保留。
        held = {};
    }
    return status;
}
void owner_stopped(uint64_t owner) {
    std::lock_guard<std::mutex> lock(g_store_mtx);
    for (auto& held : g_reservations)
        if (held.owner == owner && held.retain == 0 && !slot_operation::referenced_locked(held.operation)) held = {};
    slot_operation::owner_stopped_locked(owner);
}
Status set_referenced(uint64_t owner, int slot, contracts::QolOwnerStoreReservation* credential,
                      uint32_t referenced) {
    if (credential == nullptr || credential->size < sizeof(*credential) || credential->slot != slot ||
        credential->owner != owner || referenced > 1) return Status::kBadSlot;
    std::lock_guard<std::mutex> lock(g_store_mtx);
    const auto ready = require_ready(slot);
    if (ready != Status::kOk) return ready;
    auto& held = g_reservations[slot];
    if (!credential_matches(held, *credential)) return Status::kBusy;
    if (!slot_operation::reservation_admitted_locked(owner, held.operation, slot)) return Status::kBusy;
    if (held.retain != referenced) {
        if (held.revision == UINT64_MAX) return Status::kBusy;
        held.retain = referenced;
        ++held.revision; // 引用变化不写盘，不改变槽 generation。
    }
    *credential = held;
    return Status::kOk;
}
void owner_rebound(uint64_t old_owner, uint64_t new_owner) {
    std::lock_guard<std::mutex> lock(g_store_mtx);
    slot_operation::owner_rebound_locked(old_owner, new_owner);
    for (auto& held : g_reservations)
        if (held.id != 0 && (held.retain != 0 || held.operation != 0) && held.owner == old_owner && held.revision != UINT64_MAX) {
            held.owner = new_owner;
            ++held.revision;
        }
}

bool reservation_busy_locked(int slot) { return slot >= 0 && slot < 3 && g_reservations[slot].id != 0; }
bool operation_has_reservation_locked(uint64_t operation) {
    if (operation == 0) return false;
    return std::any_of(g_reservations.begin(), g_reservations.end(), [operation](const auto& r) {
        return r.id != 0 && r.operation == operation;
    });
}

#if defined(QOL_STORE_TEST_SEAMS)
void set_unlink_for_test(int (*unlink_fn)(const char*)) {
    std::lock_guard<std::mutex> lock(g_store_mtx);
    g_unlink_for_test = unlink_fn;
}
#endif

}  // namespace qol::infra::store

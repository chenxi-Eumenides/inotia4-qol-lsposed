// save_backup 槽操作前置门集成测试：真实 infra gate/store + fake Host SDK 面。
// 覆盖：首次写前拒绝（gate 被占时 import/delete 失败且文件不变）、acquire 后放行、
// sidecar 写失败回滚完整后 release、parsed.module 非空经 lease 显式入口。
#include "plugins/save_backup/feature.h"
#include "contracts/http.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "host/owner_identity.h"
#include "infra/slot_operation_gate.h"
#include "infra/store.h"
#include "plugins/save_backup/api/commands.h"
#include "plugins/save_backup/bundle.h"
#include "plugins/save_backup/backup_files.h"
#include "plugins/save_backup/persistence/persistence.h"

namespace {
int g_pass = 0, g_fail = 0;
#define CHECK(c) \
    do { \
        if (c) ++g_pass; \
        else { \
            ++g_fail; \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        } \
    } while (0)
namespace contracts = qol::contracts;
using namespace qol::contracts;

#define QOL_HTTP_ROUTE(method, path, owner, op_gated, lease_required) \
    {contracts::HttpMethod::method, path, owner, op_gated, lease_required},
constexpr contracts::HttpRoute kSaveBackupRouteFacts[] = {
#include "plugins/save_backup/routes.inc"
};
#undef QOL_HTTP_ROUTE

int identity = 0, occupier = 0;
QolOwnerToken g_owner = 0, g_occupier = 0;
std::string g_test_series = "original";
std::string g_test_version = "v1";
uint8_t g_test_identity_known = 1;
int32_t g_test_c18_candidate = 0;

int32_t fake_has_capability(uint64_t bit) {
    return bit == kQolTargetCapabilityC18NativeBagLayoutCandidate ? g_test_c18_candidate : 0;
}

QolResult fake_game_identity(QolGameIdentity* out) {
    if (out == nullptr) return contracts::error(kQolErrInvalidArgument, "bad", 3);
    *out = QolGameIdentity{};
    out->series_name = g_test_series.c_str();
    out->version_label = g_test_version.c_str();
    out->known = g_test_identity_known;
    return contracts::ok();
}

QolGameHost& fake_game_host() {
    static QolGameHost game{};
    game.game_identity.read = &fake_game_identity;
    game.target.has_capability = &fake_has_capability;
    return game;
}

// ---- slot_operation 表：authorize（owner_identity）+ infra gate/store（与 host_sdk 同构）----
QolResult op_acquire(void*, QolOwnerToken owner, uint32_t mask, QolSlotOperationLease* out) {
    if (!contracts::succeeded(qol::host::owner_authorize_slot_operation(owner)))
        return contracts::error(kQolErrBlocked, "not authorized", 13);
    return qol::infra::slot_operation::acquire(owner, mask, out);
}
QolResult op_set_referenced(void*, QolOwnerToken owner, QolSlotOperationLease* lease,
                            uint32_t referenced) {
    if (!contracts::succeeded(qol::host::owner_authorize_slot_operation(owner)))
        return contracts::error(kQolErrBlocked, "not authorized", 13);
    return qol::infra::slot_operation::set_referenced(owner, lease, referenced);
}
QolResult op_refresh(void*, QolOwnerToken owner, uint64_t id, QolSlotOperationLease* out) {
    if (!contracts::succeeded(qol::host::owner_authorize_slot_operation(owner)))
        return contracts::error(kQolErrBlocked, "not authorized", 13);
    return qol::infra::slot_operation::refresh(owner, id, out);
}
QolResult op_release(void*, QolOwnerToken owner, const QolSlotOperationLease* lease) {
    if (!contracts::succeeded(qol::host::owner_authorize_slot_operation(owner)))
        return contracts::error(kQolErrBlocked, "not authorized", 13);
    return qol::infra::slot_operation::release(owner, lease);
}
QolResult op_import(void*, QolOwnerToken owner, const QolSlotOperationLease* lease, int32_t slot,
                    const uint8_t* bytes, uint64_t count, QolSlotOperationApply* apply) {
    if (!contracts::succeeded(qol::host::owner_authorize_slot_operation(owner)))
        return contracts::error(kQolErrBlocked, "not authorized", 13);
    if (apply == nullptr || apply->size < sizeof(*apply))
        return contracts::error(kQolErrInvalidArgument, "invalid apply", 12);
    apply->apply = static_cast<uint32_t>(QolSlotOperationApplyState::kUnchanged);
    const auto status = qol::infra::store::import_in_operation(
        owner, lease, slot, bytes, static_cast<std::size_t>(count), apply);
    switch (status) {
        case qol::infra::store::Status::kOk: return contracts::ok();
        case qol::infra::store::Status::kBusy:
            return contracts::error(kQolErrBusy, "slot reserved", 13);
        case qol::infra::store::Status::kNotFound:
            return contracts::error(kQolErrNotFound, "not found", 9);
        default:
            return contracts::error(kQolErrInternal, "store error", 11);
    }
}
QolResult op_delete(void*, QolOwnerToken owner, const QolSlotOperationLease* lease, int32_t slot) {
    if (!contracts::succeeded(qol::host::owner_authorize_slot_operation(owner)))
        return contracts::error(kQolErrBlocked, "not authorized", 13);
    return qol::infra::store::delete_in_operation(owner, lease, slot) ==
                   qol::infra::store::Status::kOk
               ? contracts::ok()
               : contracts::error(kQolErrInternal, "store error", 11);
}
QolResult op_delete_to_trash(void*, QolOwnerToken owner, const QolSlotOperationLease* lease,
                             int32_t slot, const char* trash_dir) {
    if (!contracts::succeeded(qol::host::owner_authorize_slot_operation(owner)))
        return contracts::error(kQolErrBlocked, "not authorized", 13);
    return qol::infra::store::delete_slot_to_trash(owner, lease, slot, trash_dir) ==
                   qol::infra::store::Status::kOk
               ? contracts::ok()
               : contracts::error(kQolErrInternal, "store error", 11);
}

// ---- store 面（真实 infra，两段式 read_container 转发）----
bool g_force_read_corrupt = false;
QolResult store_read_container(int32_t slot, uint8_t* out, uint32_t capacity, uint32_t* out_size) {
    if (g_force_read_corrupt) {
        *out_size = 0;
        return contracts::error(kQolErrInternal, "store error", 11);
    }
    static std::vector<std::vector<uint8_t>> keep;
    std::vector<uint8_t> probe;
    const auto status = qol::infra::store::read_container(slot, &probe);
    if (status != qol::infra::store::Status::kOk) {
        *out_size = 0;
        return status == qol::infra::store::Status::kNotFound
                   ? contracts::error(kQolErrNotFound, "not found", 9)
                   : contracts::error(kQolErrInternal, "store error", 11);
    }
    keep.push_back(std::move(probe));
    const auto& held = keep.back();
    if (held.size() > capacity) {
        *out_size = static_cast<uint32_t>(held.size());
        return contracts::error(kQolErrInvalidArgument, "size query", 10);
    }
    std::memcpy(out, held.data(), held.size());
    *out_size = static_cast<uint32_t>(held.size());
    return contracts::ok();
}
QolResult store_import_container(int32_t slot, const uint8_t* data, uint32_t size) {
    return qol::infra::store::import_container(slot, data, size) ==
                   qol::infra::store::Status::kOk
               ? contracts::ok()
               : contracts::error(kQolErrInternal, "store error", 11);
}
QolResult store_delete_slot(int32_t slot) {
    return qol::infra::store::delete_slot(slot) == qol::infra::store::Status::kOk
               ? contracts::ok()
               : contracts::error(kQolErrInternal, "store error", 11);
}

// ---- result 写入器 ----
QolResult write_ok(char* out, uint32_t capacity, uint32_t* out_size) {
    const char* body = "{\"ok\":true}";
    const uint32_t length = static_cast<uint32_t>(std::strlen(body));
    if (capacity < length) return contracts::error(kQolErrInternal, "small", 5);
    std::memcpy(out, body, length);
    *out_size = length;
    return contracts::ok();
}
QolResult write_error(const char* message, char* out, uint32_t capacity, uint32_t* out_size) {
    const std::string body = std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
    const uint32_t length = static_cast<uint32_t>(body.size());
    if (capacity < length) return contracts::error(kQolErrInternal, "small", 5);
    std::memcpy(out, body.data(), length);
    *out_size = length;
    return contracts::ok();
}

// ---- save 面注入（明文直通加密：cipher = plain + 3 零尾）----
QolResult encrypt_passthrough(void*, int32_t, int32_t, const char*, int32_t* out_result) {
    if (out_result == nullptr) return contracts::error(kQolErrInvalidArgument, "bad", 3);
    *out_result = 1;
    return contracts::ok();
}
QolResult fake_free_data(void*) { return contracts::ok(); }
QolResult fake_mark_stale() { return contracts::ok(); }
QolResult hub_key_fn(const char** out_key) {
    *out_key = "1234567";
    return contracts::ok();
}
uint8_t g_plain_variant = 0;
QolResult load_data_fn(int32_t, void** out_data, int32_t* out_size) {
    // 最小可导出的明文头：块表 rel=4 → 块0 从 +12 开始，长度覆盖块0 的 33 字节字段。
    // +6 仅用于让不同用例产生不同 checksum，避免导出被内容去重。
    static std::vector<uint8_t> plain = [] {
        std::vector<uint8_t> bytes(64, 0);
        bytes[8] = 4;  // u16 块表项低字节：block0 = 8 + 4 = 12
        return bytes;
    }();
    plain[6] = g_plain_variant;
    *out_data = plain.data();
    *out_size = static_cast<int32_t>(plain.size());
    return contracts::ok();
}
int32_t g_current_slot = -1;
bool g_current_slot_fails = false;
bool g_current_slot_not_ready = false;
int32_t g_world_state = 4;
bool g_world_state_fails = false;
int32_t g_slot_exists[3] = {0, 0, 0};
int32_t g_slot_hero_index[3] = {-1, -1, -1};
int32_t g_slot_hero_level[3] = {-1, -1, -1};
int32_t g_slot_hero_class[3] = {-1, -1, -1};
QolResult slot_exists_fn(int32_t slot, int32_t* out) {
    *out = g_slot_exists[slot];
    return contracts::ok();
}
QolResult world_state_fn(int32_t* out) {
    if (g_world_state_fails) return contracts::error(kQolErrNotReady, "not ready", 9);
    *out = g_world_state;
    return contracts::ok();
}
const WorldFactsApi kWorldFacts{&world_state_fn, nullptr, nullptr, nullptr,
                                nullptr,           nullptr, nullptr};
QolResult slot_hero_index_fn(int32_t slot, int32_t* out) {
    *out = g_slot_hero_index[slot];
    return contracts::ok();
}
QolResult hero_level_fn(int32_t slot, int32_t* out) {
    *out = g_slot_hero_level[slot];
    return contracts::ok();
}
QolResult hero_class_fn(int32_t slot, int32_t* out) {
    *out = g_slot_hero_class[slot];
    return contracts::ok();
}
QolResult current_slot_fn(int32_t* out_slot) {
    if (g_current_slot_fails) return contracts::error(kQolErrInternal, "internal", 8);
    *out_slot = g_current_slot;
    // 生产 Adapter 在主菜单返回 NotReady + out=-1（game/adapter_ports/save_port.cpp:104-108）。
    if (g_current_slot_not_ready) return contracts::error(kQolErrNotReady, "not ready", 9);
    return contracts::ok();
}

QolHostSdk& fake_sdk() {
    static QolHostSdk sdk{};
    static const contracts::QolSlotOperationApi operation_api{
        {sizeof(contracts::QolSlotOperationApi), 1, 1}, nullptr, &op_acquire,
        &op_set_referenced, &op_refresh, &op_release, &op_import, &op_delete,
        &op_delete_to_trash};
    sdk.result.write_ok = &write_ok;
    sdk.result.write_error = &write_error;
    sdk.store.read_container = &store_read_container;
    sdk.store.import_container = &store_import_container;
    sdk.store.delete_slot = &store_delete_slot;
    sdk.slot_operation = &operation_api;
    return sdk;
}

SaveFactsApi& fake_save_facts() {
    static SaveFactsApi api{};
    api.hub_key = &hub_key_fn;
    api.current_slot = &current_slot_fn;
    api.load_data = &load_data_fn;
    api.slot_exists = &slot_exists_fn;
    api.slot_hero_index = &slot_hero_index_fn;
    api.hero_level = &hero_level_fn;
    api.hero_class = &hero_class_fn;
    return api;
}

QolSaveCallPort& fake_save_call() {
    static QolSaveCallPort port{};
    port.write.free_data = &fake_free_data;
    port.write.encrypt = &encrypt_passthrough;
    port.write.mark_stale = &fake_mark_stale;
    return port;
}

bool body_ok(const std::string& body) { return body.find("\"ok\":true") != std::string::npos; }

bool has_entry_prefix(const std::string& dir, const std::string& prefix) {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return false;
    bool found = false;
    while (dirent* ent = ::readdir(d)) {
        if (std::strncmp(ent->d_name, prefix.c_str(), prefix.size()) == 0) {
            found = true;
            break;
        }
    }
    ::closedir(d);
    return found;
}

bool has_entry_affix(const std::string& dir, const std::string& prefix,
                     const std::string& suffix) {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return false;
    bool found = false;
    while (dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (name.size() < prefix.size() + suffix.size()) continue;
        if (name.compare(0, prefix.size(), prefix) == 0 &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            found = true;
            break;
        }
    }
    ::closedir(d);
    return found;
}

int count_entries(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return 0;
    int count = 0;
    while (dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (name != "." && name != "..") ++count;
    }
    ::closedir(d);
    return count;
}

int failing_unlink(const char*) {
    errno = EACCES;
    return -1;
}

// 测试临时树清理：mkdtemp 根在成功/失败路径均递归删除（目录只含本测试产物）。
void remove_tree(const std::string& path) {
    DIR* dir = ::opendir(path.c_str());
    if (dir != nullptr) {
        while (dirent* ent = ::readdir(dir)) {
            const std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            const std::string full = path + "/" + name;
            struct stat st{};
            if (::lstat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(full);
            else ::unlink(full.c_str());
        }
        ::closedir(dir);
    }
    ::rmdir(path.c_str());
}

std::string read_file(const std::string& path) {    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return std::string();
    std::string bytes;
    char buffer[4096];
    size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) bytes.append(buffer, n);
    std::fclose(file);
    return bytes;
}

bool write_file(const std::string& path, const std::string& bytes) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    return true;
}

std::vector<uint8_t> read_file_vector(const std::string& path) {
    const std::string bytes = read_file(path);
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

void append_be16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void append_be32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void append_le32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

std::vector<uint8_t> make_c18_file(uint32_t count, int slot, uint8_t fill,
                                   uint8_t reserved0 = 0, uint8_t reserved1 = 0,
                                   uint8_t reserved2 = 0) {
    std::vector<uint8_t> bytes;
    const char magic[] = "C18BAG01";
    bytes.insert(bytes.end(), magic, magic + 8);
    append_le32(bytes, 1);
    append_le32(bytes, count);
    bytes.insert(bytes.end(), {reserved0, reserved1, reserved2});
    bytes.push_back(static_cast<uint8_t>(slot + 1));
    bytes.insert(bytes.end(), static_cast<size_t>(count) * save_backup::kC18NativeBagRecordBytes,
                  fill);
    return bytes;
}

std::string write_v3_bundle(const std::string& root) {
    std::vector<uint8_t> plain(16, 0);
    plain[8] = 4;
    const std::string checksum = save_backup::compute_checksum(
        save_backup::kBundleVersionV3, plain, {}, {});
    const std::string meta = "{\"checksum\":\"" + checksum + "\",\"module_state\":0}";
    std::vector<uint8_t> bytes;
    append_be32(bytes, save_backup::kBundleMagic);
    append_be16(bytes, save_backup::kBundleVersionV3);
    bytes.insert(bytes.end(), {0, 0, 0, 0});
    bytes.insert(bytes.end(), 8, 0);
    append_be32(bytes, static_cast<uint32_t>(plain.size()));
    bytes.insert(bytes.end(), plain.begin(), plain.end());
    append_be32(bytes, 0);  // module
    append_be32(bytes, 0);  // v3 warehouse
    append_be16(bytes, static_cast<uint16_t>(meta.size()));
    bytes.insert(bytes.end(), meta.begin(), meta.end());
    append_be32(bytes, save_backup::crc32_ieee(bytes.data(), bytes.size()));
    const std::string dir = root + "/backups";
    const std::string path = dir + "/legacy-v3-" + checksum + ".qol.bak";
    return write_file(path, std::string(bytes.begin(), bytes.end())) ? checksum : std::string();
}

std::string write_c18_bundle(const std::string& root,
                             const save_backup::C18NativeBagImage& image,
                             const std::string& filename) {
    std::vector<uint8_t> plain(16, 0);
    plain[8] = 4;
    const std::vector<uint8_t> c18_blob = save_backup::encode_c18_native_bag_image(image);
    const std::string checksum = save_backup::compute_checksum(
        save_backup::kBundleVersion, plain, {}, {}, c18_blob);
    const std::string meta = "{\"checksum\":\"" + checksum + "\",\"module_state\":0}";
    std::vector<uint8_t> bytes;
    if (!save_backup::build_bundle(0, 123, plain, {}, {}, meta, bytes, &image)) {
        return std::string();
    }
    const std::string path = root + "/backups/" + filename;
    return write_file(path, std::string(bytes.begin(), bytes.end())) ? checksum : std::string();
}

// 构造 bundle：orig_plain 满足块表布局（block0=8+rel），module 为给定容器；
// checksum 由内容计算（v3 覆盖 plain‖module），写入 external 根的 backups/ 目录。
std::string make_bundle(const std::string& external_root, const std::string&,
                        const std::vector<uint8_t>& module) {
    std::vector<uint8_t> plain(16, 0);
    plain[8] = 4;   // u16 块表 rel=4（低字节）：block0 = 8 + 4 = 12
    plain[12] = 0;  // slot 字节占位
    const std::string checksum =
        save_backup::compute_checksum(save_backup::kBundleVersion, plain, module, {});
    const std::string meta =
        "{\"checksum\":\"" + checksum + "\",\"module_state\":" +
        (module.empty() ? "0" : "1") + "}";
    std::vector<uint8_t> bundle;
    save_backup::build_bundle(0, 12345, plain, module, {}, meta, bundle);
    const std::string dir = external_root + "/backups";
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return std::string();
    const std::string path = dir + "/backup_s0_" + checksum + ".qol_save";
    if (!write_file(path, std::string(bundle.begin(), bundle.end()))) return std::string();
    return checksum;
}
}  // namespace

int main() {
    // ---- L6：typed command payload / 票据 ACK 契约 ----
    {
        CHECK(save_backup::api::ticket_json(7) == "{\"ok\":true,\"ticket\":7}");
        CHECK(sizeof(save_backup::api::ExportPayload) == sizeof(int32_t));
        CHECK(sizeof(save_backup::api::ImportPayload) >= sizeof(int32_t) + 13);
        CHECK(sizeof(save_backup::api::DeletePayload) == 13);
        save_backup::api::ImportPayload import{};
        import.slot = 2;
        std::memcpy(import.checksum, "89abcdef0123", 12);
        CHECK(import.checksum[12] == '\0');
        CHECK(std::string(import.checksum) == "89abcdef0123");
        char out[16] = {};
        uint32_t out_size = 0;
        CHECK(save_backup::api::copy_result("{\"ok\":true}", out, sizeof(out), &out_size));
        CHECK(std::string(out, out_size) == "{\"ok\":true}");
        CHECK(!save_backup::api::copy_result("0123456789abcdefg", out, sizeof(out), &out_size));
    }
    static_assert(sizeof(kSaveBackupRouteFacts) / sizeof(kSaveBackupRouteFacts[0]) == 4);
    const char* const route_paths[] = {
        "/api/system/backup/export", "/api/system/backup/list",
        "/api/system/backup/import", "/api/system/backup/delete",
    };
    for (uint32_t index = 0; index < 4; ++index) {
        CHECK(std::strcmp(kSaveBackupRouteFacts[index].path, route_paths[index]) == 0);
        CHECK(std::strcmp(kSaveBackupRouteFacts[index].owner, "save_backup") == 0);
        CHECK(kSaveBackupRouteFacts[index].op_gated == 0);
        CHECK(kSaveBackupRouteFacts[index].lease_required == 1);
    }
    CHECK(kSaveBackupRouteFacts[0].method == HttpMethod::kPost);
    CHECK(kSaveBackupRouteFacts[1].method == HttpMethod::kGet);
    CHECK(kSaveBackupRouteFacts[2].method == HttpMethod::kPost);
    CHECK(kSaveBackupRouteFacts[3].method == HttpMethod::kPost);

    char dir[] = "save_backup_gate_XXXXXX";
    CHECK(::mkdtemp(dir) != nullptr);
    const std::string root = dir;
    const std::string data_dir = root + "/data";
    const std::string save_dir = data_dir + "/game-files";
    CHECK(::mkdir(data_dir.c_str(), 0755) == 0);
    CHECK(::mkdir(save_dir.c_str(), 0755) == 0);
    const std::string dat_path = save_dir + "/save0.dat";
    const std::string original_dat = "ORIGINAL-DAT-CONTENT";
    CHECK(write_file(dat_path, original_dat));

    CHECK(qol::host::owner_register(&identity));
    CHECK(qol::host::owner_register(&occupier));
    CHECK(qol::host::owner_bind_store(&identity, "module-saves"));
    CHECK(qol::host::owner_bind_slot_operation(&identity));
    CHECK(qol::host::owner_bind_slot_operation(&occupier));
    uint64_t generation = 0;
    CHECK(qol::host::owner_identity_api().query(&identity, &g_owner, &generation).ok == kQolOk);
    CHECK(qol::host::owner_identity_api().query(&occupier, &g_occupier, &generation).ok == kQolOk);

    qol::infra::store::init(root.c_str());
    save_backup_init(data_dir.c_str(), root.c_str());
    save_backup_bind_context(&fake_sdk(), &fake_save_facts(), &fake_save_call(),
                             &fake_game_host(), g_owner);
    save_backup_bind_world_facts(&kWorldFacts);

    // 预置 slot0 sidecar 容器（module 来源），bundle 携带它使 sidecar 步骤真实执行。
    CHECK(qol::infra::store::reset_slot(0) == qol::infra::store::Status::kOk);
    uint8_t payload = 9;
    CHECK(qol::infra::store::write_section(0, "m", 1, &payload, 1) ==
          qol::infra::store::Status::kOk);
    std::vector<uint8_t> module_container;
    CHECK(qol::infra::store::read_container(0, &module_container) ==
          qol::infra::store::Status::kOk);

    const std::string checksum = make_bundle(root, "aaaaaaaaaaaa", module_container);
    CHECK(!checksum.empty());

    // ---- B1：文件类别注册表 ----
    {
        size_t class_count = 0;
        const auto* classes = qol::features::save_backup::backup_file_classes(&class_count);
        CHECK(class_count == 3 && classes != nullptr);
        CHECK(classes[0].kind == qol::features::save_backup::BackupFileKind::kOriginalDat);
        CHECK(std::strcmp(classes[0].id, "original_dat") == 0);
        CHECK(classes[0].meta_state_field == nullptr);
        CHECK(classes[0].checksum_participates && classes[0].delete_participates);
        CHECK(classes[1].kind == qol::features::save_backup::BackupFileKind::kModuleSidecar);
        CHECK(std::strcmp(classes[1].id, "module_sidecar") == 0);
        CHECK(std::strcmp(classes[1].meta_state_field, "module_state") == 0);
        CHECK(classes[1].checksum_participates && classes[1].delete_participates);
        CHECK(classes[2].kind == qol::features::save_backup::BackupFileKind::kWarehouseCompanion);
        CHECK(std::strcmp(classes[2].id, "warehouse_companion") == 0);
        CHECK(classes[2].meta_state_field == nullptr);
        CHECK(classes[2].checksum_participates && classes[2].delete_participates);
    }

    // ---- B1：snapshot 收集与旧 checksum 口径一致（dat → module → warehouse）----
    {
        qol::features::save_backup::SlotSnapshot snapshot;
        std::string snapshot_error;
        CHECK(qol::features::save_backup::collect_slot_snapshot(0, snapshot, snapshot_error));
        CHECK(snapshot.module.state ==
              qol::features::save_backup::CollectedSidecar::State::kPresent);
        CHECK(snapshot.module.bytes == module_container);
        CHECK(snapshot.warehouse.files.empty());
        const std::vector<uint8_t> wh_blob =
            save_backup::encode_warehouse(snapshot.warehouse.files);
        const std::string expected_checksum = save_backup::compute_checksum(
            save_backup::kBundleVersion, snapshot.dat.plain, snapshot.module.bytes, wh_blob);
        const std::string snapshot_export = save_backup_export_json(0);
        std::string actual_checksum;
        CHECK(save_backup::json_find_string(snapshot_export, "checksum", actual_checksum));
        CHECK(actual_checksum == expected_checksum);
        std::string exported_name;
        CHECK(save_backup::json_find_string(snapshot_export, "file_name", exported_name));
        CHECK(exported_name.size() > 8 &&
              exported_name.compare(exported_name.size() - 8, 8, ".qol.bak") == 0);
    }

    // ---- B1：wh4 收集失败沿用原错误串 ----
    {
        const std::string too_big = save_dir + "/save0.dat.wh4-0000000000000002";
        CHECK(write_file(too_big, std::string(4 * 1024 * 1024 + 1, 'X')));
        qol::features::save_backup::SlotSnapshot snapshot;
        std::string error;
        CHECK(!qol::features::save_backup::collect_slot_snapshot(0, snapshot, error));
        CHECK(error == "warehouse file too large");
        CHECK(::unlink(too_big.c_str()) == 0);

        // 读失败仅在非 root 下可稳定构造（root 忽略 000 权限）。
        if (::geteuid() != 0) {
            const std::string unreadable = save_dir + "/save0.dat.wh4-0000000000000003";
            CHECK(write_file(unreadable, "X"));
            CHECK(::chmod(unreadable.c_str(), 0000) == 0);
            qol::features::save_backup::SlotSnapshot failed;
            std::string read_error;
            CHECK(!qol::features::save_backup::collect_slot_snapshot(0, failed, read_error));
            CHECK(read_error == "warehouse read failed");
            (void)::chmod(unreadable.c_str(), 0644);
            CHECK(::unlink(unreadable.c_str()) == 0);
        }
    }

    // ---- SB-01：缓存未刷新时导出仍成功，hero 字段为 -1（不调用游戏槽读取）----
    g_plain_variant = 0;
    const std::string cold_export = save_backup_export_json(0);
    CHECK(body_ok(cold_export));
    CHECK(cold_export.find("\"hero_level\":-1") != std::string::npos);
    CHECK(cold_export.find("\"hero_index\":-1") != std::string::npos);
    CHECK(cold_export.find("\"class_idx\":-1") != std::string::npos);

    // ---- SB-01：主线程刷新缓存后导出使用缓存值 ----
    g_slot_exists[0] = 1;
    g_slot_hero_index[0] = 2;
    g_slot_hero_level[0] = 42;
    g_slot_hero_class[0] = 3;
    save_backup_refresh_slot_meta_cache();
    g_plain_variant = 1;
    const std::string warm_export = save_backup_export_json(0);
    CHECK(body_ok(warm_export));
    CHECK(warm_export.find("\"hero_level\":42") != std::string::npos);
    CHECK(warm_export.find("\"hero_index\":2") != std::string::npos);
    CHECK(warm_export.find("\"class_idx\":3") != std::string::npos);

    // ---- SB-08：lease 被占 → 导出失败，dat/备份目录无新增 ----
    const int backups_before = count_entries(root + "/backups");
    QolSlotOperationLease export_blocker{};
    export_blocker.size = sizeof(export_blocker);
    CHECK(qol::infra::slot_operation::acquire(g_occupier, 1, &export_blocker).ok == kQolOk);
    const std::string export_denied = save_backup_export_json(0);
    CHECK(export_denied.find("slot operation unavailable") != std::string::npos);
    CHECK(qol::infra::slot_operation::release(g_occupier, &export_blocker).ok == kQolOk);
    CHECK(count_entries(root + "/backups") == backups_before);
    CHECK(read_file(dat_path) == original_dat);

    // ---- SB-12：payload 被改写但 meta checksum 未变 → 导入/删除拒绝 ----
    {
        std::vector<uint8_t> plain_a(16, 0x11);
        std::vector<uint8_t> plain_b = plain_a;
        plain_b[5] ^= 0xFF;
        const std::string tampered_ck =
            save_backup::compute_checksum(save_backup::kBundleVersion, plain_a, {}, {});
        std::vector<uint8_t> tampered;
        save_backup::build_bundle(0, 1, plain_b, {}, {},
                                  "{\"checksum\":\"" + tampered_ck + "\",\"module_state\":0}",
                                  tampered);
        const std::string tampered_path =
            root + "/backups/tampered_" + tampered_ck + ".qol_save";
        CHECK(write_file(tampered_path, std::string(tampered.begin(), tampered.end())));
        const std::string tampered_import =
            save_backup_import_json(tampered_ck.c_str(), 1);
        CHECK(tampered_import.find("backup checksum mismatch") != std::string::npos);
        const std::string tampered_delete =
            save_backup_delete_json(tampered_ck.c_str());
        CHECK(tampered_delete.find("backup checksum mismatch") != std::string::npos);
        CHECK(::access(tampered_path.c_str(), F_OK) == 0);
    }

    // ---- SB-12：v2 旧公式（warehouse 不参与 checksum）仍可导入 ----
    {
        save_backup::WarehouseFile wf;
        wf.suffix = ".wh4-000001a043a2bba1";
        wf.data = {0xAA, 0xBB, 0xCC, 0xDD};
        const std::vector<uint8_t> wh_blob = save_backup::encode_warehouse_v2({wf});
        std::vector<uint8_t> plain_v2(16, 0);
        plain_v2[8] = 4;
        const std::string ck_v2 =
            save_backup::compute_checksum(save_backup::kBundleVersionV2, plain_v2, {}, wh_blob);
        std::vector<uint8_t> v2;
        const std::string meta_v2 = "{\"checksum\":\"" + ck_v2 + "\",\"module_state\":0}";
        append_be32(v2, save_backup::kBundleMagic);
        append_be16(v2, save_backup::kBundleVersionV2);
        v2.insert(v2.end(), {0, 0, 0, 0});  // source_slot + reserved
        v2.insert(v2.end(), 8, 0);           // export_time_ms
        append_be32(v2, static_cast<uint32_t>(plain_v2.size()));
        v2.insert(v2.end(), plain_v2.begin(), plain_v2.end());
        append_be32(v2, 0);  // module
        append_be32(v2, static_cast<uint32_t>(wh_blob.size()));
        v2.insert(v2.end(), wh_blob.begin(), wh_blob.end());
        append_be16(v2, static_cast<uint16_t>(meta_v2.size()));
        v2.insert(v2.end(), meta_v2.begin(), meta_v2.end());
        append_be32(v2, save_backup::crc32_ieee(v2.data(), v2.size()));
        const std::string v2_path = root + "/backups/v2_" + ck_v2 + ".qol_save";
        CHECK(write_file(v2_path, std::string(v2.begin(), v2.end())));
        // additive 语义：bundle 外的现存 wh4 后缀必须保留。
        const std::string keep_wh = save_dir + "/save2.dat.wh4-keep000000000001";
        CHECK(write_file(keep_wh, "KEEP-WH4"));
        const std::string v2_import = save_backup_import_json(ck_v2.c_str(), 2);
        CHECK(body_ok(v2_import));
        CHECK(::access((save_dir + "/save2.dat.wh4-000001a043a2bba1").c_str(), F_OK) == 0);
        CHECK(read_file(keep_wh) == "KEEP-WH4");
    }

    // ---- 导入门禁：游戏运行中（world state != 4）拒绝，dat/sidecar 均不变 ----
    g_world_state = 5;
    g_current_slot = 0;
    {
        const std::string in_game = save_backup_import_json(checksum.c_str(), 0);
        CHECK(in_game.find("import requires main menu") != std::string::npos);
        CHECK(read_file(dat_path) == original_dat);
        std::vector<uint8_t> gate_container;
        CHECK(qol::infra::store::read_container(0, &gate_container) ==
              qol::infra::store::Status::kOk);
        CHECK(gate_container == module_container);
    }
    // ---- world.facts 读取失败 → 回退 current_slot；失败 fail-closed ----
    g_world_state_fails = true;
    g_current_slot_fails = true;
    {
        const std::string unavailable = save_backup_import_json(checksum.c_str(), 0);
        CHECK(unavailable.find("save state unavailable") != std::string::npos);
        CHECK(read_file(dat_path) == original_dat);
        std::vector<uint8_t> gate_container;
        CHECK(qol::infra::store::read_container(0, &gate_container) ==
              qol::infra::store::Status::kOk);
        CHECK(gate_container == module_container);
    }
    g_world_state_fails = false;
    g_current_slot_fails = false;
    g_current_slot = -1;
    g_world_state = 4;

    // ---- 门禁判定矩阵：world.facts 优先，失败回退 current_slot ----
    {
        // 主菜单（state=4）即使 current_slot 仍保留旧槽也允许；用不存在 checksum 证明越过了门禁。
        g_world_state = 4;
        g_current_slot = 0;
        CHECK(save_backup_import_json("ffffffffffff", 0).find("backup not found") !=
              std::string::npos);
        // 非主菜单拒绝。
        g_world_state = 5;
        g_current_slot = -1;
        CHECK(save_backup_import_json("ffffffffffff", 0).find("import requires main menu") !=
              std::string::npos);
        // world.facts 失败：回退 current_slot=-1 允许。
        g_world_state_fails = true;
        g_current_slot = -1;
        CHECK(save_backup_import_json("ffffffffffff", 0).find("backup not found") !=
              std::string::npos);
        // world.facts 失败：回退 current_slot=0 拒绝。
        g_current_slot = 0;
        CHECK(save_backup_import_json("ffffffffffff", 0).find("import requires main menu") !=
              std::string::npos);
        g_world_state_fails = false;
        g_current_slot = -1;
        g_world_state = 4;
    }

    // ---- 导出 fail-closed：单个 wh4 超过 4 MiB → 整次导出失败，不组装 bundle ----
    const std::string export_ok = save_backup_export_json(0);
    CHECK(body_ok(export_ok));
    // ---- SB-05：sidecar corrupt → 整次导出失败 ----
    g_force_read_corrupt = true;
    const std::string export_corrupt = save_backup_export_json(0);
    g_force_read_corrupt = false;
    CHECK(export_corrupt.find("module container unreadable") != std::string::npos);
    const std::string big_wh = save_dir + "/save0.dat.wh4-0000000000000001";
    CHECK(write_file(big_wh, std::string(4 * 1024 * 1024 + 1, 'X')));
    const std::string too_big = save_backup_export_json(0);
    CHECK(too_big.find("warehouse file too large") != std::string::npos);
    CHECK(::unlink(big_wh.c_str()) == 0);

    // ---- gate 被占：import/删槽在首次写删前拒绝，dat 未被改 ----
    QolSlotOperationLease blocker{};
    blocker.size = sizeof(blocker);
    CHECK(qol::infra::slot_operation::acquire(g_occupier, 1, &blocker).ok == kQolOk);
    const std::string denied = save_backup_import_json(checksum.c_str(), 0);
    CHECK(denied.find("slot operation unavailable") != std::string::npos);
    CHECK(read_file(dat_path) == original_dat);
    const std::string delete_denied = save_backup_delete_slot_json(0);
    CHECK(delete_denied.find("slot operation unavailable") != std::string::npos);
    CHECK(read_file(dat_path) == original_dat);
    CHECK(qol::infra::slot_operation::release(g_occupier, &blocker).ok == kQolOk);

    // ---- 无占用：import 放行（module 非空经 lease 显式入口），成功后 lease 释放 ----
    // 主菜单真实形态：world.facts 不可用且 current_slot 返回 NotReady/out=-1，回退门禁放行。
    g_world_state_fails = true;
    g_current_slot_not_ready = true;
    const std::string imported = save_backup_import_json(checksum.c_str(), 0);
    g_current_slot_not_ready = false;
    g_world_state_fails = false;
    CHECK(body_ok(imported));
    CHECK(read_file(dat_path) != original_dat);  // dat 已被 bundle 明文覆盖
    // lease 已释放：occupier 现在能 acquire 同槽。
    QolSlotOperationLease verify{};
    verify.size = sizeof(verify);
    CHECK(qol::infra::slot_operation::acquire(g_occupier, 1, &verify).ok == kQolOk);
    CHECK(qol::infra::slot_operation::release(g_occupier, &verify).ok == kQolOk);

    // ---- 删槽：无占用时放行；dat/sidecar/wh4/trash/index 全部清理 ----
    const std::string delete_wh = save_dir + "/save0.dat.wh4-0000000000000004";
    CHECK(write_file(delete_wh, "WH4-DELETE"));
    const std::string deleted = save_backup_delete_slot_json(0);
    CHECK(body_ok(deleted));
    CHECK(::access(dat_path.c_str(), F_OK) != 0);
    CHECK(::access(delete_wh.c_str(), F_OK) != 0);
    CHECK(!has_entry_prefix(save_dir, ".sb-trash-"));
    CHECK(!has_entry_prefix(root + "/saves", ".sb-trash-"));
    CHECK(!has_entry_prefix(root + "/backups", ".trash-"));

    // ---- 删槽 unlink 失败：dat/sidecar/wh4 全部移回，内容不变 ----
    CHECK(write_file(dat_path, original_dat));
    CHECK(qol::infra::store::write_section(0, "m", 1, &payload, 1) ==
          qol::infra::store::Status::kOk);
    const std::string rollback_wh = save_dir + "/save0.dat.wh4-0000000000000005";
    CHECK(write_file(rollback_wh, "WH4-ROLLBACK"));
    std::vector<uint8_t> before_failed_delete;
    CHECK(qol::infra::store::read_container(0, &before_failed_delete) ==
          qol::infra::store::Status::kOk);
    qol::infra::store::set_unlink_for_test(&failing_unlink);
    const std::string delete_failed = save_backup_delete_slot_json(0);
    qol::infra::store::set_unlink_for_test(nullptr);
    CHECK(delete_failed.find("delete incomplete") != std::string::npos);
    CHECK(read_file(dat_path) == original_dat);
    std::vector<uint8_t> after_failed_delete;
    CHECK(qol::infra::store::read_container(0, &after_failed_delete) ==
          qol::infra::store::Status::kOk);
    CHECK(after_failed_delete == before_failed_delete);
    CHECK(read_file(rollback_wh) == "WH4-ROLLBACK");
    CHECK(::unlink(rollback_wh.c_str()) == 0);
    CHECK(!has_entry_prefix(save_dir, ".sb-trash-"));
    CHECK(!has_entry_prefix(root + "/saves", ".sb-trash-"));
    CHECK(!has_entry_prefix(root + "/backups", ".trash-"));

    // ---- C：自动备份条目与自动导入 ----
    {
        save_backup_autobackup_pre_save(0);
        const std::string auto_path = root + "/backups/autobackup/autosave_s0.qol.bak";
        const std::string auto_entry = save_backup_auto_entry_json(0);
        CHECK(body_ok(auto_entry));
        CHECK(auto_entry.find("\"backup\":null") == std::string::npos);
        std::string entry_checksum;
        CHECK(save_backup::json_find_string(auto_entry, "checksum", entry_checksum));
        std::vector<uint8_t> auto_bytes = read_file_vector(auto_path);
        save_backup::ParsedBundle auto_parsed;
        CHECK(!auto_bytes.empty());
        CHECK(save_backup::parse_bundle(auto_bytes.data(), auto_bytes.size(), auto_parsed));
        CHECK(entry_checksum ==
              save_backup::compute_checksum(auto_parsed.bundle_version, auto_parsed.orig_plain,
                                            auto_parsed.module, auto_parsed.warehouse_blob));
        long long auto_map_id = -1;
        long long auto_hero_level = -999;
        long long auto_hero_index = -999;
        long long auto_class_idx = -999;
        CHECK(save_backup::json_find_int(auto_entry, "map_id", auto_map_id));
        CHECK(auto_map_id == 4);
        CHECK(save_backup::json_find_int(auto_entry, "hero_level", auto_hero_level));
        CHECK(auto_hero_level == 42);
        CHECK(save_backup::json_find_int(auto_entry, "hero_index", auto_hero_index));
        CHECK(auto_hero_index == 2);
        CHECK(save_backup::json_find_int(auto_entry, "class_idx", auto_class_idx));
        CHECK(auto_class_idx == 3);
        CHECK(save_backup_auto_entry_json(2) == "{\"ok\":true,\"backup\":null}");

        // 自动备份不进入手动列表。
        CHECK(save_backup_list_json().find("autosave_s0") == std::string::npos);

        // 导入回原槽。
        const int manual_files_before = count_entries(root + "/backups");
        const std::string import_same = save_backup_import_auto_json(0, 0);
        CHECK(body_ok(import_same));
        CHECK(read_file(dat_path) != original_dat);
        CHECK(count_entries(root + "/backups") == manual_files_before);

        // 导入到另一槽。
        const std::string import_other = save_backup_import_auto_json(0, 1);
        CHECK(body_ok(import_other));
        CHECK(::access((save_dir + "/save1.dat").c_str(), F_OK) == 0);

        // 非主菜单：沿用现有门禁文案。
        g_world_state = 5;
        g_current_slot = 0;
        const std::string gated = save_backup_import_auto_json(0, 0);
        g_current_slot = -1;
        g_world_state = 4;
        CHECK(gated.find("import requires main menu") != std::string::npos);

        // 自动备份损坏：沿用现有读取错误文案。
        CHECK(write_file(auto_path, "CORRUPT"));
        const std::string corrupt = save_backup_import_auto_json(0, 0);
        CHECK(corrupt.find("backup unreadable or corrupt") != std::string::npos);
        // 让后续自动备份指纹用例重新导出（避免跳过损坏文件）。
        CHECK(::unlink((root + "/backups/autobackup/autosave_s0.fingerprint").c_str()) == 0);
    }

    // ---- sidecar 回滚完整 → lease 释放：primary 写入被占位目录阻断（store 面未变更）----
    const std::string module_saves = root + "/saves";
    CHECK(::mkdir(module_saves.c_str(), 0755) == 0 || errno == EEXIST);
    // store 的 primary 路径为 slot-0.qol-save，write_atomic 的临时文件为同名 + .tmp。
    const std::string blocked_primary_tmp = module_saves + "/slot-0.qol-save.tmp";
    CHECK(write_file(dat_path, original_dat));
    // 删槽段已清掉 sidecar：重建（generation 会推进，比较基线用重建后的快照）。
    CHECK(qol::infra::store::write_section(0, "m", 1, &payload, 1) ==
          qol::infra::store::Status::kOk);
    std::vector<uint8_t> rebuilt_container;
    CHECK(qol::infra::store::read_container(0, &rebuilt_container) ==
          qol::infra::store::Status::kOk);
    const std::string checksum2 = make_bundle(root, "bbbbbbbbbbbb", rebuilt_container);
    CHECK(::mkdir(blocked_primary_tmp.c_str(), 0755) == 0);
    const std::string failed_primary = save_backup_import_json(checksum2.c_str(), 0);
    CHECK(failed_primary.find("module container import failed") != std::string::npos);
    CHECK(failed_primary.find("rollback failed") == std::string::npos);  // 回滚完整
    CHECK(read_file(dat_path) == original_dat);  // dat 已回滚
    std::vector<uint8_t> untouched_container;
    CHECK(qol::infra::store::read_container(0, &untouched_container) ==
          qol::infra::store::Status::kOk);
    CHECK(untouched_container == rebuilt_container);  // store 面未被改动
    // 回滚完整 → lease 释放（occupier 可 acquire）。
    CHECK(qol::infra::slot_operation::acquire(g_occupier, 1, &verify).ok == kQolOk);
    CHECK(qol::infra::slot_operation::release(g_occupier, &verify).ok == kQolOk);
    CHECK(::rmdir(blocked_primary_tmp.c_str()) == 0);

    // ---- primary 已提交而 last-good 失败：恢复被阻断 → 回滚失败保留 referenced lease
    // 与证据，不伪装收尾成功 ----
    const std::string blocked_last_good_tmp = module_saves + "/slot-0.qol-save.last-good.tmp";
    CHECK(write_file(dat_path, original_dat));
    const std::string checksum3 = make_bundle(root, "cccccccccccc", rebuilt_container);
    CHECK(::mkdir(blocked_last_good_tmp.c_str(), 0755) == 0);
    const std::string failed_last_good = save_backup_import_json(checksum3.c_str(), 0);
    CHECK(failed_last_good.find("rollback failed") != std::string::npos);
    CHECK(read_file(dat_path) == original_dat);  // dat 已回滚
    // sidecar 恢复被阻断：primary 已被 import 改写，回滚失败证据保留。
    std::vector<uint8_t> degraded_container;
    CHECK(qol::infra::store::read_container(0, &degraded_container) ==
          qol::infra::store::Status::kOk);
    CHECK(has_entry_affix(root + "/backups/.rollback", "save0.dat.", ".qol-save"));
    // referenced lease 保留：新 gate 申请被拒（occupier 不能接管）。
    CHECK(qol::infra::slot_operation::acquire(g_occupier, 1, &verify).code == kQolErrBusy);
    CHECK(::rmdir(blocked_last_good_tmp.c_str()) == 0);

    // ---- B3：指定目录导出核心（原子覆盖、无 tmp 残留）----
    const std::string auto_dir = root + "/backups/autobackup";
    std::string core_error;
    CHECK(save_backup_export_slot_to_dir(0, auto_dir.c_str(), "manual_test.qol_save",
                                         &core_error));
    CHECK(::access((auto_dir + "/manual_test.qol_save").c_str(), F_OK) == 0);
    CHECK(!has_entry_prefix(auto_dir, "manual_test.qol_save.tmp-"));
    CHECK(save_backup_export_slot_to_dir(0, auto_dir.c_str(), "manual_test.qol_save",
                                         &core_error));
    CHECK(!has_entry_prefix(auto_dir, "manual_test.qol_save.tmp-"));
    {
        std::vector<uint8_t> manual_bytes;
        save_backup::ParsedBundle manual;
        manual_bytes = read_file_vector(auto_dir + "/manual_test.qol_save"); CHECK(!manual_bytes.empty());
        CHECK(save_backup::parse_bundle(manual_bytes.data(), manual_bytes.size(), manual));
    }

    // ---- C：自动备份（首导出、指纹跳过、源变化重导、失败保留旧文件）----
    {
        const std::string auto_bundle = auto_dir + "/autosave_s0.qol.bak";
        const std::string auto_fp = auto_dir + "/autosave_s0.fingerprint";
        g_plain_variant = 3;
        save_backup_autobackup_pre_save(0);
        CHECK(::access(auto_bundle.c_str(), F_OK) == 0);
        std::vector<uint8_t> bytes1;
        save_backup::ParsedBundle parsed1;
        bytes1 = read_file_vector(auto_bundle); CHECK(!bytes1.empty());
        CHECK(save_backup::parse_bundle(bytes1.data(), bytes1.size(), parsed1));
        std::string meta_ck;
        CHECK(save_backup::json_find_string(parsed1.meta_json, "checksum", meta_ck));
        CHECK(meta_ck == save_backup::compute_checksum(
                             parsed1.bundle_version, parsed1.orig_plain, parsed1.module,
                             parsed1.warehouse_blob));
        CHECK(parsed1.orig_plain.size() == 64 && parsed1.orig_plain[12] == 0xFF);
        CHECK(parsed1.orig_plain[6] == 3);
        const std::string fp1 = read_file(auto_fp);
        struct stat st1 {};
        CHECK(::stat(auto_bundle.c_str(), &st1) == 0);

        // 未变化：指纹一致，目标文件不重写。
        ::usleep(2000);
        save_backup_autobackup_pre_save(0);
        struct stat st2 {};
        CHECK(::stat(auto_bundle.c_str(), &st2) == 0);
        CHECK(st1.st_ino == st2.st_ino);
        CHECK(st1.st_mtim.tv_sec == st2.st_mtim.tv_sec);
        CHECK(st1.st_mtim.tv_nsec == st2.st_mtim.tv_nsec);
        CHECK(read_file(auto_fp) == fp1);

        // dat 变化：重新导出，内容随 fake load_data 变化。
        ::usleep(2000);
        CHECK(write_file(dat_path, "AUTOBACKUP-DAT-1"));
        g_plain_variant = 4;
        save_backup_autobackup_pre_save(0);
        struct stat st3 {};
        CHECK(::stat(auto_bundle.c_str(), &st3) == 0);
        CHECK(st3.st_mtim.tv_nsec != st2.st_mtim.tv_nsec || st3.st_ino != st2.st_ino);
        std::vector<uint8_t> bytes3;
        save_backup::ParsedBundle parsed3;
        bytes3 = read_file_vector(auto_bundle); CHECK(!bytes3.empty());
        CHECK(save_backup::parse_bundle(bytes3.data(), bytes3.size(), parsed3));
        CHECK(parsed3.orig_plain[6] == 4);
        CHECK(read_file(auto_fp) != fp1);

        // wh4 变化：指纹变化并重新导出。
        ::usleep(2000);
        const std::string auto_wh = save_dir + "/save0.dat.wh4-autobk000001";
        CHECK(write_file(auto_wh, "WH4-AUTOBACKUP"));
        save_backup_autobackup_pre_save(0);
        struct stat st4 {};
        CHECK(::stat(auto_bundle.c_str(), &st4) == 0);
        CHECK(st4.st_mtim.tv_nsec != st3.st_mtim.tv_nsec || st4.st_ino != st3.st_ino);
        std::vector<uint8_t> bytes4;
        save_backup::ParsedBundle parsed4;
        bytes4 = read_file_vector(auto_bundle); CHECK(!bytes4.empty());
        CHECK(save_backup::parse_bundle(bytes4.data(), bytes4.size(), parsed4));
        CHECK(parsed4.warehouse.size() == 1);
        CHECK(::unlink(auto_wh.c_str()) == 0);

        // 写目录不可写：不崩溃，旧文件保留。
        if (::geteuid() != 0) {
            struct stat before {};
            CHECK(::stat(auto_bundle.c_str(), &before) == 0);
            CHECK(::chmod(auto_dir.c_str(), 0500) == 0);
            ::usleep(2000);
            save_backup_autobackup_pre_save(0);
            struct stat after {};
            CHECK(::stat(auto_bundle.c_str(), &after) == 0);
            CHECK(before.st_ino == after.st_ino);
            CHECK(before.st_mtim.tv_nsec == after.st_mtim.tv_nsec);
            CHECK(::chmod(auto_dir.c_str(), 0755) == 0);
        }
        // 旧版自动备份 .qol_save 仍可读取。
        const std::string legacy_auto = auto_dir + "/autosave_s0.qol_save";
        CHECK(::rename(auto_bundle.c_str(), legacy_auto.c_str()) == 0);
        const std::string legacy_entry = save_backup_auto_entry_json(0);
        CHECK(body_ok(legacy_entry));
        CHECK(legacy_entry.find("\"backup\":null") == std::string::npos);
        CHECK(::rename(legacy_auto.c_str(), auto_bundle.c_str()) == 0);
    }
    {
        CHECK(qol::infra::store::write_section(1, "m", 1, &payload, 1) ==
              qol::infra::store::Status::kOk);
        std::vector<uint8_t> existing;
        CHECK(qol::infra::store::read_container(1, &existing) ==
              qol::infra::store::Status::kOk);
        const std::string absent_ck = make_bundle(root, "", {});
        const std::string imported = save_backup_import_json(absent_ck.c_str(), 1);
        CHECK(body_ok(imported));
        std::vector<uint8_t> gone;
        CHECK(qol::infra::store::read_container(1, &gone) ==
              qol::infra::store::Status::kNotFound);
    }

    // ---- SB-05：present 备份覆盖目标槽旧 sidecar ----
    {
        uint8_t module_value = 7;
        CHECK(qol::infra::store::write_section(1, "m", 1, &module_value, 1) ==
              qol::infra::store::Status::kOk);
        std::vector<uint8_t> incoming;
        CHECK(qol::infra::store::read_container(1, &incoming) ==
              qol::infra::store::Status::kOk);
        uint8_t old_value = 2;
        CHECK(qol::infra::store::write_section(1, "old", 1, &old_value, 1) ==
              qol::infra::store::Status::kOk);
        const std::string present_ck = make_bundle(root, "", incoming);
        const std::string imported = save_backup_import_json(present_ck.c_str(), 1);
        CHECK(body_ok(imported));
        std::vector<uint8_t> old_out;
        std::vector<uint8_t> module_out;
        uint16_t version = 0;
        CHECK(qol::infra::store::read_section(1, "old", &old_out, &version) ==
              qol::infra::store::Status::kNotFound);
        CHECK(qol::infra::store::read_section(1, "m", &module_out, &version) ==
              qol::infra::store::Status::kOk);
        CHECK(module_out == std::vector<uint8_t>({7}));
    }

    // ---- UI 逐帧解析：文件名枚举 + 单条解析 ----
    {
        const std::string backups_dir = root + "/backups";
        std::vector<uint8_t> plain_a(16, 0);
        plain_a[8] = 4;
        const std::string ck_a =
            save_backup::compute_checksum(save_backup::kBundleVersion, plain_a, {}, {});
        const std::string meta_a = save_backup::build_meta_json(
            0, 1000, 37, 5, 0, 5, 10, std::string(64, 'a'), std::string(), ck_a, 2, "黑魔导");
        std::vector<uint8_t> bundle_a;
        save_backup::build_bundle(0, 1000, plain_a, {}, {}, meta_a, bundle_a);
        const std::string name_a = "zz_ui_a.qol_save";
        CHECK(write_file(backups_dir + "/" + name_a,
                         std::string(bundle_a.begin(), bundle_a.end())));

        std::vector<uint8_t> plain_b(16, 1);
        plain_b[8] = 4;
        const std::string ck_b =
            save_backup::compute_checksum(save_backup::kBundleVersion, plain_b, {}, {});
        const std::string meta_b = save_backup::build_meta_json(
            0, 900, 30, 3, 1, 5, 9, std::string(64, 'b'), std::string(), ck_b);
        std::vector<uint8_t> bundle_b;
        save_backup::build_bundle(0, 900, plain_b, {}, {}, meta_b, bundle_b);
        CHECK(write_file(backups_dir + "/aa_ui_b.qol_save",
                         std::string(bundle_b.begin(), bundle_b.end())));
        CHECK(write_file(backups_dir + "/mm_ui_corrupt.qol_save", "NOT-A-BUNDLE"));

        const std::string files = save_backup_list_files_json();
        CHECK(body_ok(files));
        CHECK(files.find("\"name\":\"" + name_a + "\"") != std::string::npos);
        CHECK(files.find("\"name\":\"aa_ui_b.qol_save\"") != std::string::npos);
        CHECK(files.find("\"name\":\"mm_ui_corrupt.qol_save\"") != std::string::npos);
        CHECK(files.find("autosave_s0") == std::string::npos);  // 只扫 backups 顶层
        const size_t pos_a = files.find("\"name\":\"" + name_a + "\"");
        const size_t pos_c = files.find("\"name\":\"mm_ui_corrupt.qol_save\"");
        const size_t pos_b = files.find("\"name\":\"aa_ui_b.qol_save\"");
        CHECK(pos_a < pos_c && pos_c < pos_b);  // 文件名降序
        const std::string segment_a =
            files.substr(pos_a, files.find('}', pos_a) - pos_a);
        long long listed_size = -1;
        long long listed_mtime = -1;
        CHECK(save_backup::json_find_int(segment_a, "size", listed_size));
        CHECK(listed_size == static_cast<long long>(bundle_a.size()));
        CHECK(save_backup::json_find_int(segment_a, "mtime_ms", listed_mtime));
        CHECK(listed_mtime > 0);

        const std::string entry = save_backup_entry_json(name_a.c_str());
        CHECK(body_ok(entry));
        std::string entry_checksum;
        CHECK(save_backup::json_find_string(entry, "checksum", entry_checksum));
        CHECK(entry_checksum == ck_a);
        long long value = 0;
        CHECK(save_backup::json_find_int(entry, "map_id", value) && value == 37);
        CHECK(save_backup::json_find_int(entry, "hero_level", value) && value == 5);
        CHECK(save_backup::json_find_int(entry, "class_idx", value) && value == 2);
        CHECK(entry.find("\"class_name\":\"黑魔导\"") != std::string::npos);

        // 新后缀 .qol.bak 同样被列表与单条解析接受。
        const std::string name_bak = "zz_ui_bak.qol.bak";
        CHECK(write_file(backups_dir + "/" + name_bak,
                         std::string(bundle_a.begin(), bundle_a.end())));
        const std::string files_bak = save_backup_list_files_json();
        CHECK(files_bak.find("\"name\":\"" + name_bak + "\"") != std::string::npos);
        const std::string entry_bak = save_backup_entry_json(name_bak.c_str());
        CHECK(body_ok(entry_bak));
        CHECK(save_backup::json_find_int(entry_bak, "hero_level", value) && value == 5);
        CHECK(save_backup::json_find_int(entry_bak, "class_idx", value) && value == 2);
        CHECK(::unlink((backups_dir + "/" + name_bak).c_str()) == 0);

        // 旧备份 meta 缺职业/等级：解析明文后写回 meta（checksum/版本不变，CRC 重算）。
        std::vector<uint8_t> plain_bf(86, 0);
        auto set16 = [&](size_t off, uint16_t v) {
            plain_bf[off] = static_cast<uint8_t>(v);
            plain_bf[off + 1] = static_cast<uint8_t>(v >> 8);
        };
        set16(8, 28);
        set16(10, 33);
        set16(12, 61);
        set16(14, 10);
        set16(16, 71);
        set16(18, 7);
        plain_bf[69] = 20;  // map_id
        plain_bf[79] = 1;   // CharacterInfo count
        set16(80, 5);       // record 0 offset
        set16(82, 2);       // record 0 len
        plain_bf[84] = 7;   // level
        plain_bf[85] = 4;   // class
        const std::string ck_bf =
            save_backup::compute_checksum(save_backup::kBundleVersion, plain_bf, {}, {});
        const std::string meta_bf = save_backup::build_meta_json(
            0, 2000, 20, -1, 0, 5, 11, std::string(64, 'c'), std::string(), ck_bf, -1,
            std::string());
        std::vector<uint8_t> bundle_bf;
        save_backup::build_bundle(0, 2000, plain_bf, {}, {}, meta_bf, bundle_bf);
        const std::string name_bf = "bf_ui.qol_save";
        CHECK(write_file(backups_dir + "/" + name_bf,
                         std::string(bundle_bf.begin(), bundle_bf.end())));
        const std::string entry_bf = save_backup_entry_json(name_bf.c_str());
        CHECK(body_ok(entry_bf));
        CHECK(save_backup::json_find_int(entry_bf, "hero_level", value) && value == 7);
        CHECK(save_backup::json_find_int(entry_bf, "class_idx", value) && value == 4);
        CHECK(entry_bf.find("\"class_name\":\"暗影猎手\"") != std::string::npos);
        const std::vector<uint8_t> rewritten = read_file_vector(backups_dir + "/" + name_bf);
        save_backup::ParsedBundle rewritten_parsed;
        CHECK(save_backup::parse_bundle(rewritten.data(), rewritten.size(), rewritten_parsed));
        CHECK(save_backup::json_find_int(rewritten_parsed.meta_json, "hero_level", value) &&
              value == 7);
        CHECK(save_backup::json_find_int(rewritten_parsed.meta_json, "class_idx", value) &&
              value == 4);
        std::string rewritten_checksum;
        CHECK(save_backup::json_find_string(rewritten_parsed.meta_json, "checksum",
                                            rewritten_checksum) &&
              rewritten_checksum == ck_bf);

        // 篡改 checksum（内容与 meta 不一致）与损坏文件。
        const std::string wrong_checksum =
            save_backup::compute_checksum(save_backup::kBundleVersion, plain_b, {}, {});
        std::vector<uint8_t> tampered;
        save_backup::build_bundle(0, 1000, plain_a, {}, {},
                                  "{\"checksum\":\"" + wrong_checksum + "\"}", tampered);
        CHECK(write_file(backups_dir + "/tt_ui_tampered.qol_save",
                         std::string(tampered.begin(), tampered.end())));
        CHECK(save_backup_entry_json("tt_ui_tampered.qol_save").find(
                  "backup checksum mismatch") != std::string::npos);
        CHECK(save_backup_entry_json("mm_ui_corrupt.qol_save").find(
                  "backup unreadable or corrupt") != std::string::npos);
        CHECK(save_backup_entry_json("missing_ui.qol_save").find("backup not found") !=
              std::string::npos);
        CHECK(save_backup_entry_json(nullptr).find("bad request") != std::string::npos);
        CHECK(save_backup_entry_json("").find("bad request") != std::string::npos);
        CHECK(save_backup_entry_json("../x.qol_save").find("bad request") != std::string::npos);
        CHECK(save_backup_entry_json("sub/x.qol_save").find("bad request") != std::string::npos);
        CHECK(save_backup_entry_json("x.txt").find("bad request") != std::string::npos);
    }

    // ---- C18 normalized image：共享 fallback 只读收集、跨版 materialize、版本门禁与回滚 ----
    {
        const std::string c18_backups_dir = root + "/backups";
        g_test_series = "unknown";
        g_test_version.clear();
        g_test_identity_known = 0;
        g_test_c18_candidate = 1;
        qol::features::save_backup::SlotSnapshot unknown_c18_snapshot;
        std::string unknown_c18_error;
        CHECK(!qol::features::save_backup::collect_slot_snapshot(
            0, unknown_c18_snapshot, unknown_c18_error));
        CHECK(unknown_c18_error == "C18 game version unsupported");

        g_test_series = "c18";
        g_test_version = "v586";
        g_test_identity_known = 1;
        g_test_c18_candidate = 0;
        const std::string shared_path = data_dir + "/C18Lbag.dat";
        const std::vector<uint8_t> shared_before =
            make_c18_file(save_backup::kC18NativeBagModernRecordCount, 0, 0x2A,
                          0xA1, 0xB2, 0xC3);
        CHECK(write_file(shared_path, std::string(shared_before.begin(), shared_before.end())));

        std::string export_error;
        CHECK(save_backup_export_slot_to_dir(0, c18_backups_dir.c_str(), "c18-fallback.qol.bak",
                                             &export_error));
        const std::vector<uint8_t> exported =
            read_file_vector(c18_backups_dir + "/c18-fallback.qol.bak");
        save_backup::ParsedBundle exported_parsed;
        CHECK(save_backup::parse_bundle(exported.data(), exported.size(), exported_parsed));
        CHECK(exported_parsed.has_c18_native_bag);
        CHECK(exported_parsed.c18_native_bag.source_version == "v586");
        CHECK(exported_parsed.c18_native_bag.source_slot == 0);
        CHECK(exported_parsed.c18_native_bag.record_count ==
              save_backup::kC18NativeBagModernRecordCount);
        std::string export_checksum;
        CHECK(save_backup::json_find_string(exported_parsed.meta_json, "checksum",
                                            export_checksum));
        CHECK(read_file_vector(shared_path) == shared_before);

        // C18 primary families live in dataDir root and use 1-based filename suffixes.
        const std::string modern_slot1_path = data_dir + "/C18Lwh2.dat";
        const std::vector<uint8_t> modern_slot1 =
            make_c18_file(save_backup::kC18NativeBagModernRecordCount, 1, 0x51,
                          0x11, 0x22, 0x33);
        CHECK(write_file(modern_slot1_path,
                         std::string(modern_slot1.begin(), modern_slot1.end())));
        qol::features::save_backup::SlotSnapshot modern_slot1_snapshot;
        std::string c18_snapshot_error;
        CHECK(qol::features::save_backup::collect_slot_snapshot(
            1, modern_slot1_snapshot, c18_snapshot_error));
        CHECK(modern_slot1_snapshot.c18_native_bag.present);
        CHECK(modern_slot1_snapshot.c18_native_bag.image.record_count ==
              save_backup::kC18NativeBagModernRecordCount);
        const std::array<uint8_t, 3> modern_reserved{0x11, 0x22, 0x33};
        CHECK(modern_slot1_snapshot.c18_native_bag.image.header_reserved ==
              modern_reserved);
        CHECK(::unlink(modern_slot1_path.c_str()) == 0);

        const std::string modern_slot2_path = data_dir + "/C18Lwh3.dat";
        const std::vector<uint8_t> modern_slot2 =
            make_c18_file(save_backup::kC18NativeBagModernRecordCount, 2, 0x61);
        CHECK(write_file(modern_slot2_path,
                         std::string(modern_slot2.begin(), modern_slot2.end())));
        qol::features::save_backup::SlotSnapshot modern_slot2_snapshot;
        CHECK(qol::features::save_backup::collect_slot_snapshot(
            2, modern_slot2_snapshot, c18_snapshot_error));
        CHECK(modern_slot2_snapshot.c18_native_bag.present);
        CHECK(modern_slot2_snapshot.c18_native_bag.image.records.front() == 0x61);
        CHECK(::unlink(modern_slot2_path.c_str()) == 0);

        // A shared source's opaque reserved header bytes survive bundle encoding and target rewrite.
        const std::string modern_import_result =
            save_backup_import_json(export_checksum.c_str(), 1);
        if (!body_ok(modern_import_result)) std::printf("C18 modern import: %s\n", modern_import_result.c_str());
        CHECK(body_ok(modern_import_result));
        const std::string modern_target_path = data_dir + "/C18Lwh2.dat";
        const std::vector<uint8_t> modern_target = read_file_vector(modern_target_path);
        CHECK(modern_target.size() == shared_before.size());
        CHECK(modern_target[0x10] == 0xA1 && modern_target[0x11] == 0xB2 &&
              modern_target[0x12] == 0xC3);
        CHECK(modern_target[0x13] == 2);
        CHECK(::unlink(modern_target_path.c_str()) == 0);
        CHECK(::unlink(shared_path.c_str()) == 0);
        qol::features::save_backup::SlotSnapshot missing_c18_snapshot;
        CHECK(!qol::features::save_backup::collect_slot_snapshot(
            0, missing_c18_snapshot, c18_snapshot_error));
        CHECK(c18_snapshot_error == "C18 bag file not found");

        // v3 没有 C18 段：目标 C18 必须在 dat/sidecar 修改前拒绝。
        g_test_version = "v520";
        const std::string old_slot1_primary = data_dir + "/C18Lbag2.dat";
        const std::vector<uint8_t> old_slot1 =
            make_c18_file(save_backup::kC18NativeBagLegacyRecordCount, 1, 0x72,
                          0x44, 0x55, 0x66);
        CHECK(write_file(old_slot1_primary, std::string(old_slot1.begin(), old_slot1.end())));
        qol::features::save_backup::SlotSnapshot old_slot1_snapshot;
        CHECK(qol::features::save_backup::collect_slot_snapshot(
            1, old_slot1_snapshot, c18_snapshot_error));
        CHECK(old_slot1_snapshot.c18_native_bag.present);
        const std::array<uint8_t, 3> old_reserved{0x44, 0x55, 0x66};
        CHECK(old_slot1_snapshot.c18_native_bag.image.header_reserved ==
              old_reserved);

        const std::string legacy_checksum = write_v3_bundle(root);
        const std::string slot1_dat = save_dir + "/save1.dat";
        const std::string slot1_dat_before = "slot1-before-old-bundle";
        CHECK(write_file(slot1_dat, slot1_dat_before));
        const std::string old_bundle_result =
            save_backup_import_json(legacy_checksum.c_str(), 1);
        CHECK(old_bundle_result.find("backup has no C18 bag image") != std::string::npos);
        CHECK(read_file(slot1_dat) == slot1_dat_before);

        // shared 96 条导入到旧版 per-slot bag 文件：尾部补零，shared 文件保持不变。
        CHECK(body_ok(save_backup_import_json(export_checksum.c_str(), 2)));
        const std::string slot2_bag_path = data_dir + "/C18Lbag3.dat";
        const std::vector<uint8_t> slot2_bag = read_file_vector(slot2_bag_path);
        uint32_t materialized_count = 0;
        std::array<uint8_t, 3> materialized_reserved{};
        std::vector<uint8_t> materialized_records;
        CHECK(save_backup::parse_c18_native_bag_file(
            slot2_bag.data(), slot2_bag.size(), 2,
            save_backup::kC18NativeBagLegacyRecordCount, false, materialized_count,
            materialized_reserved, materialized_records));
        CHECK(materialized_count == save_backup::kC18NativeBagLegacyRecordCount);
        CHECK(materialized_reserved == exported_parsed.c18_native_bag.header_reserved);
        CHECK(std::equal(exported_parsed.c18_native_bag.records.begin(),
                         exported_parsed.c18_native_bag.records.end(),
                         materialized_records.begin()));
        const size_t padded_begin = exported_parsed.c18_native_bag.records.size();
        CHECK(std::all_of(materialized_records.begin() +
                              static_cast<std::ptrdiff_t>(padded_begin),
                          materialized_records.end(),
                          [](uint8_t value) { return value == 0; }));
        CHECK(::access(shared_path.c_str(), F_OK) != 0);

        // 让 C18 per-slot 原子写失败，验证逆序恢复 dat、sidecar 与 C18 旧字节。
        CHECK(qol::infra::store::reset_slot(1) == qol::infra::store::Status::kOk);
        const uint8_t sidecar_payload = 0x71;
        CHECK(qol::infra::store::write_section(1, "c18", 3, &sidecar_payload, 1) ==
              qol::infra::store::Status::kOk);
        std::vector<uint8_t> sidecar_before;
        CHECK(qol::infra::store::read_container(1, &sidecar_before) ==
              qol::infra::store::Status::kOk);
        const std::string slot1_dat_before_rollback = "slot1-before-c18-write-failure";
        CHECK(write_file(slot1_dat, slot1_dat_before_rollback));
        const std::string slot1_bag_path = data_dir + "/C18Lbag2.dat";
        const std::vector<uint8_t> slot1_bag_before =
            make_c18_file(save_backup::kC18NativeBagLegacyRecordCount, 1, 0x77,
                          0xD4, 0xE5, 0xF6);
        CHECK(write_file(slot1_bag_path,
                         std::string(slot1_bag_before.begin(), slot1_bag_before.end())));
        save_backup::C18NativeBagImage rollback_image;
        rollback_image.source_version = "v520";
        rollback_image.source_slot = 0;
        rollback_image.record_count = save_backup::kC18NativeBagLegacyRecordCount;
        rollback_image.records.assign(
            static_cast<size_t>(rollback_image.record_count) *
                save_backup::kC18NativeBagRecordBytes,
            0x44);
        const std::string rollback_checksum =
            write_c18_bundle(root, rollback_image, "c18-rollback.qol.bak");
        save_backup::persistence::fail_next_atomic_write_for_test(slot1_bag_path);
        const std::string rollback_result =
            save_backup_import_json(rollback_checksum.c_str(), 1);
        CHECK(rollback_result.find("C18 bag file write failed") != std::string::npos);
        CHECK(rollback_result.find("rollback failed") == std::string::npos);
        CHECK(read_file(slot1_dat) == slot1_dat_before_rollback);
        CHECK(read_file_vector(slot1_bag_path) == slot1_bag_before);
        std::vector<uint8_t> sidecar_after;
        uint16_t sidecar_version_after = 0;
        CHECK(qol::infra::store::read_section(1, "c18", &sidecar_after,
                                               &sidecar_version_after) ==
              qol::infra::store::Status::kOk);
        CHECK(sidecar_after == std::vector<uint8_t>({sidecar_payload}));
        CHECK(sidecar_version_after == 3);
    }
    g_test_series = "original";
    g_test_version = "v1";

    // ---- token 清零（停用）后前置门不可用 → 拒绝而非越权 ----
    save_backup_bind_context(&fake_sdk(), &fake_save_facts(), &fake_save_call(),
                             &fake_game_host(), 0);
    CHECK(write_file(dat_path, original_dat));
    const std::string no_token = save_backup_import_json(checksum.c_str(), 0);
    CHECK(no_token.find("slot operation unavailable") != std::string::npos);
    CHECK(read_file(dat_path) == original_dat);

    // ---- 启动恢复：index 指向的 trash 文件移回原位，index/trash 清理 ----
    {
        const std::string saves = root + "/saves";
        CHECK(::mkdir(saves.c_str(), 0755) == 0 || errno == EEXIST);
        const std::string trash = saves + "/.sb-trash-recovertest";
        CHECK(::mkdir(trash.c_str(), 0755) == 0);
        const std::string primary = saves + "/slot-1.qol-save";
        const std::string last_good = primary + ".last-good";
        (void)::unlink(primary.c_str());
        (void)::unlink(last_good.c_str());
        CHECK(write_file(trash + "/slot-1.qol-save", "RESTORED-PRIMARY"));
        CHECK(write_file(trash + "/slot-1.qol-save.last-good", "RESTORED-GOOD"));
        const std::string wh_trash = save_dir + "/.sb-trash-recoverwh";
        CHECK(::mkdir(wh_trash.c_str(), 0755) == 0);
        const std::string wh_orig = save_dir + "/save0.dat.wh4-recover001";
        (void)::unlink(wh_orig.c_str());
        CHECK(write_file(wh_trash + "/save0.dat.wh4-recover001", "RESTORED-WH4"));
        const std::string index = root + "/backups/.trash-recovertest.index";
        const std::string index_body = trash + "\t" + primary + "\n" + trash + "\t" + last_good +
                                       "\n" + wh_trash + "\t" + wh_orig + "\n";
        CHECK(write_file(index, index_body));
        save_backup_recover_deletes();
        CHECK(read_file(primary) == "RESTORED-PRIMARY");
        CHECK(read_file(last_good) == "RESTORED-GOOD");
        CHECK(read_file(wh_orig) == "RESTORED-WH4");
        CHECK(::access(trash.c_str(), F_OK) != 0);
        CHECK(::access(wh_trash.c_str(), F_OK) != 0);
        CHECK(::access(index.c_str(), F_OK) != 0);
    }

    save_backup_bind_context(nullptr, nullptr, nullptr, nullptr, 0);
    qol::host::owner_identity_reset_for_test();
    remove_tree(root);

    std::printf("save_backup_gate_tests: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

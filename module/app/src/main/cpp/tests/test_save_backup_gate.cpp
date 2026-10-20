// save_backup 槽操作前置门集成测试：真实 infra gate/store + fake Host SDK 面。
// 覆盖：首次写前拒绝（gate 被占时 import/delete 失败且文件不变）、acquire 后放行、
// sidecar 写失败回滚完整后 release、parsed.module 非空经 lease 显式入口。
#include "plugins/save_backup/feature.h"
#include "contracts/http.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "host/owner_identity.h"
#include "infra/slot_operation_gate.h"
#include "infra/store.h"
#include "plugins/save_backup/bundle.h"

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
int32_t g_slot_exists[3] = {0, 0, 0};
int32_t g_slot_hero_index[3] = {-1, -1, -1};
int32_t g_slot_hero_level[3] = {-1, -1, -1};
int32_t g_slot_hero_class[3] = {-1, -1, -1};
QolResult slot_exists_fn(int32_t slot, int32_t* out) {
    *out = g_slot_exists[slot];
    return contracts::ok();
}
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
    save_backup_bind_context(&fake_sdk(), &fake_save_facts(), &fake_save_call(), nullptr, g_owner);

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
        save_backup::build_bundle(0, 1, plain_v2, {}, wh_blob,
                                  "{\"checksum\":\"" + ck_v2 + "\",\"module_state\":0}", v2);
        v2[4] = 0;  // version = 2（大端）
        v2[5] = 2;
        const size_t body = v2.size() - 4;
        const uint32_t crc = save_backup::crc32_ieee(v2.data(), body);
        v2[body] = static_cast<uint8_t>(crc >> 24);
        v2[body + 1] = static_cast<uint8_t>(crc >> 16);
        v2[body + 2] = static_cast<uint8_t>(crc >> 8);
        v2[body + 3] = static_cast<uint8_t>(crc);
        const std::string v2_path = root + "/backups/v2_" + ck_v2 + ".qol_save";
        CHECK(write_file(v2_path, std::string(v2.begin(), v2.end())));
        const std::string v2_import = save_backup_import_json(ck_v2.c_str(), 2);
        CHECK(body_ok(v2_import));
        CHECK(::access((save_dir + "/save2.dat.wh4-000001a043a2bba1").c_str(), F_OK) == 0);
    }

    // ---- 导入门禁：游戏运行中（current_slot >= 0）拒绝，dat/sidecar 均不变 ----
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
    // ---- current_slot 读取失败 → fail-closed，不写任何文件 ----
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
    g_current_slot_fails = false;
    g_current_slot = -1;

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
    // 主菜单真实形态：Adapter 返回 NotReady 且 out=-1，门禁必须放行而不是 fail-closed。
    g_current_slot_not_ready = true;
    const std::string imported = save_backup_import_json(checksum.c_str(), 0);
    g_current_slot_not_ready = false;
    CHECK(body_ok(imported));
    CHECK(read_file(dat_path) != original_dat);  // dat 已被 bundle 明文覆盖
    // lease 已释放：occupier 现在能 acquire 同槽。
    QolSlotOperationLease verify{};
    verify.size = sizeof(verify);
    CHECK(qol::infra::slot_operation::acquire(g_occupier, 1, &verify).ok == kQolOk);
    CHECK(qol::infra::slot_operation::release(g_occupier, &verify).ok == kQolOk);

    // ---- 删槽：无占用时放行；dat/sidecar/trash/index 全部清理 ----
    const std::string deleted = save_backup_delete_slot_json(0);
    CHECK(body_ok(deleted));
    CHECK(::access(dat_path.c_str(), F_OK) != 0);
    CHECK(!has_entry_prefix(save_dir, ".sb-trash-"));
    CHECK(!has_entry_prefix(root + "/saves", ".sb-trash-"));
    CHECK(!has_entry_prefix(root + "/backups", ".trash-"));

    // ---- 删槽 unlink 失败：已移动文件移回，dat/sidecar 逐字节不变 ----
    CHECK(write_file(dat_path, original_dat));
    CHECK(qol::infra::store::write_section(0, "m", 1, &payload, 1) ==
          qol::infra::store::Status::kOk);
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
    CHECK(!has_entry_prefix(save_dir, ".sb-trash-"));
    CHECK(!has_entry_prefix(root + "/saves", ".sb-trash-"));
    CHECK(!has_entry_prefix(root + "/backups", ".trash-"));

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

    // ---- SB-05：absent 备份导入 → 清除目标槽现存 sidecar ----
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

    // ---- token 清零（停用）后前置门不可用 → 拒绝而非越权 ----
    save_backup_bind_context(&fake_sdk(), &fake_save_facts(), &fake_save_call(), nullptr, 0);
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
        const std::string index = root + "/backups/.trash-recovertest.index";
        CHECK(write_file(index, trash + "/slot-1.qol-save\t" + primary + "\n" +
                                    trash + "/slot-1.qol-save.last-good\t" + last_good + "\n"));
        save_backup_recover_deletes();
        CHECK(read_file(primary) == "RESTORED-PRIMARY");
        CHECK(read_file(last_good) == "RESTORED-GOOD");
        CHECK(::access(trash.c_str(), F_OK) != 0);
        CHECK(::access(index.c_str(), F_OK) != 0);
    }

    save_backup_bind_context(nullptr, nullptr, nullptr, nullptr, 0);
    qol::host::owner_identity_reset_for_test();
    remove_tree(root);

    std::printf("save_backup_gate_tests: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

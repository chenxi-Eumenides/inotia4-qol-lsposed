// save_backup 槽操作前置门集成测试：真实 infra gate/store + fake Host SDK 面。
// 覆盖：首次写前拒绝（gate 被占时 import/delete 失败且文件不变）、acquire 后放行、
// sidecar 写失败回滚完整后 release、parsed.module 非空经 lease 显式入口。
#include "plugins/save_backup/feature.h"

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

// ---- store 面（真实 infra，两段式 read_container 转发）----
QolResult store_read_container(int32_t slot, uint8_t* out, uint32_t capacity, uint32_t* out_size) {
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

QolHostSdk& fake_sdk() {
    static QolHostSdk sdk{};
    static const contracts::QolSlotOperationApi operation_api{
        {sizeof(contracts::QolSlotOperationApi), 1, 0}, nullptr, &op_acquire,
        &op_set_referenced, &op_refresh, &op_release, &op_import, &op_delete};
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
// 写入 external 根的 save_backup/ 目录（feature 的 backup_dir 扫描位置）。
std::string make_bundle(const std::string& external_root, const std::string& checksum,
                        const std::vector<uint8_t>& module) {
    std::vector<uint8_t> plain(16, 0);
    plain[8] = 4;   // u16 块表 rel=4（低字节）：block0 = 8 + 4 = 12
    plain[12] = 0;  // slot 字节占位
    std::vector<uint8_t> bundle;
    save_backup::build_bundle(0, 12345, plain, module, {},
                              "{\"checksum\":\"" + checksum + "\"}", bundle);
    const std::string dir = external_root + "/save_backup";
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return std::string();
    const std::string path = dir + "/backup_s0_" + checksum + ".qol_save";
    if (!write_file(path, std::string(bundle.begin(), bundle.end()))) return std::string();
    return checksum;
}
}  // namespace

int main() {
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

    qol::infra::store::init(data_dir.c_str());
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
    const std::string imported = save_backup_import_json(checksum.c_str(), 0);
    CHECK(body_ok(imported));
    CHECK(read_file(dat_path) != original_dat);  // dat 已被 bundle 明文覆盖
    // lease 已释放：occupier 现在能 acquire 同槽。
    QolSlotOperationLease verify{};
    verify.size = sizeof(verify);
    CHECK(qol::infra::slot_operation::acquire(g_occupier, 1, &verify).ok == kQolOk);
    CHECK(qol::infra::slot_operation::release(g_occupier, &verify).ok == kQolOk);

    // ---- 删槽：无占用时放行 ----
    const std::string deleted = save_backup_delete_slot_json(0);
    CHECK(body_ok(deleted));
    CHECK(::access(dat_path.c_str(), F_OK) != 0);

    // ---- sidecar 回滚完整 → lease 释放：primary 写入被占位目录阻断（store 面未变更）----
    const std::string module_saves = data_dir + "/module-saves";
    CHECK(::mkdir(module_saves.c_str(), 0755) == 0 || errno == EEXIST);
    // store 的 primary 路径为 slot-0.module-save，write_atomic 的临时文件为同名 + .tmp。
    const std::string blocked_primary_tmp = module_saves + "/slot-0.module-save.tmp";
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
    const std::string blocked_last_good_tmp = module_saves + "/slot-0.module-save.last-good.tmp";
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
    CHECK(::access((root + "/save_backup/.rollback/save0.dat.module-save").c_str(), F_OK) == 0);
    // referenced lease 保留：新 gate 申请被拒（occupier 不能接管）。
    CHECK(qol::infra::slot_operation::acquire(g_occupier, 1, &verify).code == kQolErrBusy);
    CHECK(::rmdir(blocked_last_good_tmp.c_str()) == 0);

    // ---- token 清零（停用）后前置门不可用 → 拒绝而非越权 ----
    save_backup_bind_context(&fake_sdk(), &fake_save_facts(), &fake_save_call(), nullptr, 0);
    CHECK(write_file(dat_path, original_dat));
    const std::string no_token = save_backup_import_json(checksum.c_str(), 0);
    CHECK(no_token.find("slot operation unavailable") != std::string::npos);
    CHECK(read_file(dat_path) == original_dat);

    save_backup_bind_context(nullptr, nullptr, nullptr, nullptr, 0);
    qol::host::owner_identity_reset_for_test();
    remove_tree(root);

    std::printf("save_backup_gate_tests: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

#include "transactions.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <cerrno>

#include "contracts/result.h"
#include "plugins/save_backup/backup_files.h"
#include "plugins/save_backup/bundle.h"
#include "plugins/save_backup/feature.h"
#include "plugins/save_backup/log.h"
#include "plugins/save_backup/persistence/persistence.h"
#include "plugins/save_backup/port/save_port.h"
#include "plugins/save_backup/rollback.h"
#include "plugins/save_backup/state/state.h"
#include "plugins/save_backup/api/json_codec.h"
#include "plugins/save_backup/usecases/meta_backfill.h"

#if defined(QOL_STORE_TEST_SEAMS)
namespace qol::infra::store {
int unlink_with_test_seam(const char* path);
}  // namespace qol::infra::store
#endif

namespace save_backup {
namespace usecases {
namespace transaction {
namespace {

#define SB_LOG(...) SAVE_BACKUP_LOG_INFO(__VA_ARGS__)
#define SB_LOG_WARN(...) SAVE_BACKUP_LOG_WARN(__VA_ARGS__)

using save_backup::persistence::copy_file;
using save_backup::persistence::copy_file_replacing;
using save_backup::persistence::ends_with;
using save_backup::persistence::ensure_dir;
using save_backup::persistence::file_exists;
using save_backup::persistence::file_name_of;
using save_backup::persistence::find_by_checksum;
using save_backup::persistence::local_stamp;
using save_backup::persistence::next_transaction_id;
using save_backup::persistence::read_file_bytes;
using save_backup::persistence::remove_file;
using save_backup::persistence::write_file_bytes_atomic;

constexpr const char* kRollbackDirName = ".rollback";

std::string result_error(const char* message) {
    return save_backup::api::result_error(save_backup::state::snapshot().sdk, message);
}

bool backup_dir(std::string& out) {
    return save_backup::persistence::backup_dir(save_backup::state::snapshot().external_dir, out);
}

int locate_save_directory_at(const std::string& data_dir, std::string& out) {
    if (data_dir.empty()) return 1;
    const char* key = save_backup::port::hub_key();
    return save_backup::persistence::locate_save_directory(data_dir, key != nullptr ? key : "", out)
               ? 0
               : 1;
}

int locate_save_directory(std::string& out) {
    return locate_save_directory_at(save_backup::state::snapshot().data_dir, out);
}

bool read_target_c18_layout(save_backup::C18NativeBagLayout& out, std::string& version_label,
                            std::string& err) {
    out = save_backup::C18NativeBagLayout::kNotC18;
    version_label.clear();
    const auto ctx = save_backup::state::snapshot();
    qol::contracts::QolGameIdentity identity{};
    if (ctx.game == nullptr || ctx.game->game_identity.read == nullptr ||
        !qol::contracts::succeeded(ctx.game->game_identity.read(&identity)) ||
        identity.series_name == nullptr || identity.version_label == nullptr) {
        err = "game identity unavailable";
        return false;
    }
    const bool identity_is_c18 = std::strcmp(identity.series_name, "c18") == 0;
    const bool c18_candidate =
        identity_is_c18 ||
        (ctx.game->target.has_capability != nullptr &&
         ctx.game->target.has_capability(
             qol::contracts::kQolTargetCapabilityC18NativeBagLayoutCandidate) != 0);
    if (!c18_candidate) return true;
    if (!identity_is_c18 || identity.known == 0) {
        err = "C18 game version unsupported";
        return false;
    }
    out = save_backup::c18_native_bag_layout(identity.version_label);
    if (out == save_backup::C18NativeBagLayout::kUnknown) {
        err = "C18 game version unsupported";
        return false;
    }
    version_label.assign(identity.version_label);
    return true;
}

// 目标 C18 需要原生背包镜像、但旧备份（v1-v3）没有镜像：按目标布局生成空背包文件。
// record 全零，loader 以「前五个 u64 全零」判定空记录；导入后扩展背包为空。
bool build_empty_c18_native_bag(const std::string& version_label, int slot,
                                save_backup::C18NativeBagLayout layout,
                                save_backup::C18NativeBagImage& out) {
    out = save_backup::C18NativeBagImage{};
    out.source_version = version_label;
    out.source_slot = slot;
    out.record_count = layout == save_backup::C18NativeBagLayout::kLegacy192
                           ? save_backup::kC18NativeBagLegacyRecordCount
                           : save_backup::kC18NativeBagModernRecordCount;
    out.records.assign(
        static_cast<size_t>(out.record_count) * save_backup::kC18NativeBagRecordBytes, 0);
    return save_backup::validate_c18_native_bag_image(out);
}

bool snapshot_c18_target(const std::string& path, bool& existed,
                         std::vector<uint8_t>& previous) {
    existed = false;
    previous.clear();
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0) return errno == ENOENT;
    if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) > save_backup::kC18NativeBagMaxImageBytes ||
        !read_file_bytes(path, previous)) {
        return false;
    }
    existed = true;
    return true;
}

int unlink_file(const std::string& path) {
#if defined(QOL_STORE_TEST_SEAMS)
    return qol::infra::store::unlink_with_test_seam(path.c_str());
#else
    return ::unlink(path.c_str());
#endif
}

}  // namespace

// ---- 槽操作前置门（P3b-2）----
// import/删槽在首次 dat/warehouse 写删之前 acquire 覆盖受影响槽的 operation lease，
// 跨整个操作与回滚持有；完整回滚结束才 release，回滚失败置 referenced 保留证据。
// 生产无并发 gate 持有者时此门惰性（acquire 即成功即释放，行为不变）。
bool slot_operation_acquire(int slot, SlotOperationGuard* guard) {
    guard->api = nullptr;
    guard->held = false;
    guard->lease = qol::contracts::QolSlotOperationLease{};
    guard->lease.size = sizeof(guard->lease);
    if (save_backup::state::snapshot().sdk == nullptr || save_backup::state::snapshot().sdk->slot_operation == nullptr || save_backup::state::snapshot().owner == 0) return false;
    const auto* api = save_backup::state::snapshot().sdk->slot_operation;
    const QolResult result = api->acquire(api->host_ctx, save_backup::state::snapshot().owner, 1u << slot, &guard->lease);
    if (!qol::contracts::succeeded(result)) return false;
    guard->api = api;
    guard->held = true;
    return true;
}

void slot_operation_release(SlotOperationGuard* guard) {
    if (guard == nullptr || !guard->held) return;
    (void)guard->api->release(guard->api->host_ctx, save_backup::state::snapshot().owner, &guard->lease);
    guard->held = false;
}

void slot_operation_hold_referenced(SlotOperationGuard* guard) {
    // 回滚失败保留证据：置 referenced 后 owner 停用也不释放 lease。
    if (guard == nullptr || !guard->held) return;
    (void)guard->api->set_referenced(guard->api->host_ctx, save_backup::state::snapshot().owner, &guard->lease, 1);
}

// sidecar 容器写入的 lease 显式入口。失败时 *store_touched 指出 primary 是否已提交：
// primary_applied（primary 已写、last-good 失败）为 true，需要回滚恢复；
// unchanged（store 面未写任何文件）为 false，跳过 sidecar 恢复避免误报回滚失败。
bool store_import_container_locked_gate(const SlotOperationGuard& guard, int slot,
                                        const uint8_t* data, size_t size, bool* store_touched) {
    *store_touched = false;
    if (!guard.held) return false;
    qol::contracts::QolSlotOperationApply apply{};
    apply.size = sizeof(apply);
    const QolResult result = guard.api->import_container(guard.api->host_ctx, save_backup::state::snapshot().owner,
                                                         &guard.lease, slot, data,
                                                         static_cast<uint64_t>(size), &apply);
    if (qol::contracts::succeeded(result)) {
        *store_touched = true;
        return true;
    }
    *store_touched =
        apply.apply == static_cast<uint32_t>(qol::contracts::QolSlotOperationApplyState::kPrimaryApplied);
    return false;
}

bool store_delete_slot_locked_gate(const SlotOperationGuard& guard, int slot) {
    if (!guard.held) return false;
    return qol::contracts::succeeded(
        guard.api->delete_container(guard.api->host_ctx, save_backup::state::snapshot().owner, &guard.lease, slot));
}


ContainerStatus classify_container_result(const QolResult& result) {
    if (qol::contracts::succeeded(result)) return ContainerStatus::kOk;
    if (result.code == kQolErrNotFound) return ContainerStatus::kNotFound;
    if (result.code == kQolErrInternal) return ContainerStatus::kCorrupt;
    return ContainerStatus::kOther;
}

ContainerStatus store_read_container(int slot, std::vector<uint8_t>& out) {
    out.clear();
    if (save_backup::state::snapshot().sdk == nullptr || save_backup::state::snapshot().sdk->store.read_container == nullptr) {
        return ContainerStatus::kOther;
    }
    uint32_t size = 0;
    QolResult result = save_backup::state::snapshot().sdk->store.read_container(slot, nullptr, 0, &size);
    if (qol::contracts::succeeded(result)) return ContainerStatus::kOk;
    if (result.code != kQolErrInvalidArgument || size == 0) {
        return classify_container_result(result);
    }
    out.resize(size);
    result = save_backup::state::snapshot().sdk->store.read_container(slot, out.data(), size, &size);
    if (!qol::contracts::succeeded(result)) {
        out.clear();
        return classify_container_result(result);
    }
    out.resize(size);
    return ContainerStatus::kOk;
}

bool store_import_container(int slot, const uint8_t* data, size_t size) {
    return save_backup::state::snapshot().sdk != nullptr && save_backup::state::snapshot().sdk->store.import_container != nullptr &&
           size <= UINT32_MAX &&
           qol::contracts::succeeded(save_backup::state::snapshot().sdk->store.import_container(
               slot, data, static_cast<uint32_t>(size)));
}

ContainerStatus store_delete_slot(int slot) {
    if (save_backup::state::snapshot().sdk == nullptr || save_backup::state::snapshot().sdk->store.delete_slot == nullptr) return ContainerStatus::kOther;
    return classify_container_result(save_backup::state::snapshot().sdk->store.delete_slot(slot));
}



// 导入失败还原：dat 按 .rollback 副本还原；sidecar 仅在本次已改动时经 store 还原为
// 导入前快照（prev_container）。导入前无可用 primary（prev_container_exists=false）时删除该槽 sidecar。
bool preserve_rollback_record(const std::string& path, const uint8_t* data, size_t size) {
    return write_file_bytes_atomic(path, data, size);
}

save_backup::RollbackResult restore_from_rollback(
    int slot, const std::string& dat_path, const std::string& rb_dat, bool dat_existed,
    const std::vector<uint8_t>& prev_container, bool prev_container_exists, bool sidecar_touched,
    const SlotOperationGuard* guard) {
    save_backup::RollbackResult result;
    if (dat_existed) {
        result.dat_restored = file_exists(rb_dat) && copy_file_replacing(rb_dat, dat_path);
    } else {
        result.dat_restored = remove_file(dat_path);
    }
    if (sidecar_touched) {
        // 持有 operation lease 时回滚必须走 lease 显式入口：旧无凭据入口在有 operation
        // 期间一律 Busy，回滚会被自己的门挡住。
        bool restore_touched = false;
        const bool sidecar_restored =
            guard != nullptr && guard->held
                ? (prev_container_exists
                       ? store_import_container_locked_gate(*guard, slot, prev_container.data(),
                                                            prev_container.size(), &restore_touched)
                       : store_delete_slot_locked_gate(*guard, slot))
                : (prev_container_exists
                       ? store_import_container(slot, prev_container.data(), prev_container.size())
                       : store_delete_slot(slot) == ContainerStatus::kOk);
        result.sidecar_restored = sidecar_restored;
    }
    if (result.complete()) {
        remove_file(rb_dat);
        return result;
    }

    // 还原不完整时保留已有副本；sidecar 原状只在内存快照中，另写一份记录供人工恢复。
    if (dat_existed && !file_exists(rb_dat)) result.evidence_preserved = false;
    if (!dat_existed && !result.dat_restored) {
        result.evidence_preserved = preserve_rollback_record(rb_dat + ".dat-missing", nullptr, 0) &&
                                    result.evidence_preserved;
    }
    if (sidecar_touched && !result.sidecar_restored) {
        const std::string sidecar_record = rb_dat + ".qol-save";
        if (prev_container_exists) {
            result.evidence_preserved =
                preserve_rollback_record(sidecar_record, prev_container.data(), prev_container.size()) &&
                result.evidence_preserved;
        } else {
            static constexpr char kMissingSidecar[] = "missing\n";
            result.evidence_preserved = preserve_rollback_record(
                                            sidecar_record,
                                            reinterpret_cast<const uint8_t*>(kMissingSidecar),
                                            sizeof(kMissingSidecar) - 1) &&
                                        result.evidence_preserved;
        }
    }
    return result;
}

// ---- 删除事务（trash + index）----

// trash 条目：文件当前所在的临时路径与原始路径。
struct TrashEntry {
    std::string trash_path;
    std::string orig_path;
};

bool write_trash_index(const std::string& index_path, const std::vector<TrashEntry>& entries) {
    std::string text;
    for (const TrashEntry& entry : entries) {
        if (entry.trash_path.empty() || entry.orig_path.empty()) continue;
        text += entry.trash_path;
        text += '\t';
        text += entry.orig_path;
        text += '\n';
    }
    return write_file_bytes_atomic(index_path,
                                   reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

bool parse_trash_index(const std::string& index_path, std::vector<TrashEntry>& out) {
    out.clear();
    std::vector<uint8_t> bytes;
    if (!read_file_bytes(index_path, bytes)) return false;
    const std::string text(bytes.begin(), bytes.end());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty()) continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) {
            SB_LOG_WARN("delete recovery: invalid index entry in %s", index_path.c_str());
            return false;
        }
        const std::string trash_dir = line.substr(0, tab);
        const std::string orig_path = line.substr(tab + 1);
        // 索引由本模块写入；仍校验形状，避免异常内容把文件 rename 到任意路径。
        const auto traverses = [](const std::string& path) {
            return path == ".." || path.rfind("../", 0) == 0 ||
                   path.find("/../") != std::string::npos ||
                   (path.size() >= 3 && path.compare(path.size() - 3, 3, "/..") == 0);
        };
        if (trash_dir.empty() || orig_path.empty() || traverses(trash_dir) ||
            traverses(orig_path) || trash_dir.find("/.sb-trash-") == std::string::npos ||
            file_name_of(orig_path).empty()) {
            SB_LOG_WARN("delete recovery: rejected index entry in %s", index_path.c_str());
            return false;
        }
        out.push_back(TrashEntry{trash_dir + "/" + file_name_of(orig_path), orig_path});
    }
    return true;
}

// 把 trash 中仍存在的文件移回原位（逆序，与移动顺序相反）；目标已存在时跳过并保留 trash。
bool restore_trash_entries(const std::vector<TrashEntry>& entries) {
    bool complete = true;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        const TrashEntry& entry = *it;
        if (!file_exists(entry.trash_path)) continue;  // 已删除或已移回
        if (file_exists(entry.orig_path)) {
            SB_LOG_WARN("delete recovery: target exists, keep trash file: %s",
                        entry.orig_path.c_str());
            complete = false;
            continue;
        }
        if (::rename(entry.trash_path.c_str(), entry.orig_path.c_str()) != 0) {
            SB_LOG_WARN("delete recovery: rename back failed: %s", entry.trash_path.c_str());
            complete = false;
        }
    }
    return complete;
}

void remove_dir_if_empty(const std::string& dir) {
    if (dir.empty()) return;
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return;
    bool empty = true;
    while (dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (name != "." && name != "..") {
            empty = false;
            break;
        }
    }
    ::closedir(d);
    if (empty) (void)::rmdir(dir.c_str());
}

// 无 index 的 sidecar trash（崩溃在写 index 之前/之后均可识别）：按文件名还原到 saves/。
void recover_orphan_sidecar_trash(const std::string& saves_dir) {
    DIR* d = ::opendir(saves_dir.c_str());
    if (d == nullptr) return;
    std::vector<std::string> dirs;
    while (dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (name.rfind(".sb-trash-", 0) != 0) continue;
        dirs.push_back(saves_dir + "/" + name);
    }
    ::closedir(d);
    for (const std::string& trash_dir : dirs) {
        DIR* td = ::opendir(trash_dir.c_str());
        if (td == nullptr) continue;
        std::vector<std::string> names;
        while (dirent* ent = ::readdir(td)) {
            const std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            if (!file_exists(trash_dir + "/" + name)) continue;
            names.push_back(name);
        }
        ::closedir(td);
        bool complete = true;
        for (const std::string& name : names) {
            const std::string orig = saves_dir + "/" + name;
            if (file_exists(orig)) {
                SB_LOG_WARN("delete recovery: target exists, keep trash file: %s", orig.c_str());
                complete = false;
                continue;
            }
            if (::rename((trash_dir + "/" + name).c_str(), orig.c_str()) != 0) {
                SB_LOG_WARN("delete recovery: rename back failed: %s", orig.c_str());
                complete = false;
            }
        }
        if (complete) remove_dir_if_empty(trash_dir);
    }
}


void recover_deletes() {
    std::lock_guard<std::mutex> lk(save_backup::persistence::file_mutex());
    if (save_backup::state::snapshot().external_dir.empty()) return;
    const std::string backups = save_backup::state::snapshot().external_dir + "/" +
                                save_backup::persistence::kBackupDirName;
    DIR* d = ::opendir(backups.c_str());
    if (d != nullptr) {
        std::vector<std::string> indexes;
        while (dirent* ent = ::readdir(d)) {
            const std::string name = ent->d_name;
            if (name.rfind(".trash-", 0) == 0 && ends_with(name, ".index")) {
                indexes.push_back(backups + "/" + name);
            }
        }
        ::closedir(d);
        std::sort(indexes.begin(), indexes.end());
        for (const std::string& index_path : indexes) {
            std::vector<TrashEntry> entries;
            if (!parse_trash_index(index_path, entries)) {
                SB_LOG_WARN("delete recovery: unreadable index: %s", index_path.c_str());
                continue;
            }
            if (!restore_trash_entries(entries)) {
                SB_LOG_WARN("delete recovery: incomplete, keep index: %s", index_path.c_str());
                continue;
            }
            for (const TrashEntry& entry : entries) {
                const size_t slash = entry.trash_path.find_last_of('/');
                if (slash != std::string::npos) {
                    remove_dir_if_empty(entry.trash_path.substr(0, slash));
                }
            }
            remove_file(index_path);
            SB_LOG("delete recovery: restored %zu file(s) from %s", entries.size(),
                   index_path.c_str());
        }
    }
    // 无 index 的 sidecar trash：按目录内文件名还原到 saves/。
    recover_orphan_sidecar_trash(save_backup::state::snapshot().external_dir + "/saves");
}


void slot_deleted_sync(int32_t slot) {
    if (slot < 0 || slot > 2) return;
    if (save_backup::state::snapshot().external_dir.empty()) {
        SB_LOG_WARN("slot delete hook: external dir unavailable, sidecar untouched slot=%d", slot);
        save_backup::port::mark_stale();
        return;
    }
    std::lock_guard<std::mutex> lk(save_backup::persistence::file_mutex());
    // dat 已被游戏原函数删除，这里只处理 sidecar；两阶段（trash → unlink）保证失败可退回。
    SlotOperationGuard guard;
    if (!slot_operation_acquire(slot, &guard)) {
        SB_LOG_WARN("slot delete hook: slot operation unavailable slot=%d", slot);
        save_backup::port::mark_stale();
        return;
    }
    const std::string saves_dir = save_backup::state::snapshot().external_dir + "/saves";
    if (!ensure_dir(saves_dir)) {
        SB_LOG_WARN("slot delete hook: saves dir unavailable slot=%d", slot);
        slot_operation_release(&guard);
        save_backup::port::mark_stale();
        return;
    }
    std::string backups_dir;
    if (!backup_dir(backups_dir)) {
        SB_LOG_WARN("slot delete hook: backup dir unavailable slot=%d", slot);
        slot_operation_release(&guard);
        save_backup::port::mark_stale();
        return;
    }
    const std::string id = next_transaction_id();
    const std::string trash_dir = saves_dir + "/.sb-trash-" + id;
    const std::string index_path = backups_dir + "/.trash-" + id + ".index";
    std::string save_dir;
    std::vector<std::string> wh_names;
    if (locate_save_directory_at(save_backup::state::snapshot().data_dir, save_dir) == 0) {
        wh_names = qol::features::save_backup::list_warehouse_companions(save_dir, slot);
    }
    const bool data_trash_needed = !wh_names.empty();
    const std::string data_trash =
        data_trash_needed ? save_dir + "/.sb-trash-" + id : std::string();
    const std::string primary = saves_dir + "/slot-" + std::to_string(slot) + ".qol-save";
    const std::string last_good = primary + ".last-good";
    const std::string corrupt_marker = primary + ".unresolved-corrupt";
    std::vector<TrashEntry> entries = {
        {trash_dir + "/" + file_name_of(primary), primary},
        {trash_dir + "/" + file_name_of(last_good), last_good},
        {trash_dir + "/" + file_name_of(corrupt_marker), corrupt_marker},
    };
    for (const std::string& wh_name : wh_names) {
        entries.push_back({data_trash + "/" + wh_name, save_dir + "/" + wh_name});
    }
    auto cleanup_trash_dirs = [&]() {
        remove_dir_if_empty(trash_dir);
        if (data_trash_needed) remove_dir_if_empty(data_trash);
    };

    if (::mkdir(trash_dir.c_str(), 0755) != 0) {
        SB_LOG_WARN("slot delete hook: trash mkdir failed slot=%d", slot);
        slot_operation_release(&guard);
        save_backup::port::mark_stale();
        return;
    }
    if (data_trash_needed && ::mkdir(data_trash.c_str(), 0755) != 0) {
        (void)::rmdir(trash_dir.c_str());
        SB_LOG_WARN("slot delete hook: data trash mkdir failed slot=%d", slot);
        slot_operation_release(&guard);
        save_backup::port::mark_stale();
        return;
    }
    if (!write_trash_index(index_path, entries)) {
        (void)::rmdir(trash_dir.c_str());
        if (data_trash_needed) (void)::rmdir(data_trash.c_str());
        SB_LOG_WARN("slot delete hook: index write failed slot=%d", slot);
        slot_operation_release(&guard);
        save_backup::port::mark_stale();
        return;
    }
    if (guard.api->abi.minor < 1 || guard.api->delete_container_to_trash == nullptr) {
        SB_LOG_WARN("slot delete hook: trash api unavailable slot=%d", slot);
        (void)restore_trash_entries(entries);
        remove_file(index_path);
        cleanup_trash_dirs();
        slot_operation_release(&guard);
        save_backup::port::mark_stale();
        return;
    }
    const QolResult status = guard.api->delete_container_to_trash(
        guard.api->host_ctx, save_backup::state::snapshot().owner, &guard.lease, slot, trash_dir.c_str());
    if (!qol::contracts::succeeded(status)) {
        (void)restore_trash_entries(entries);
        remove_file(index_path);
        cleanup_trash_dirs();
        SB_LOG_WARN("slot delete hook: sidecar move failed slot=%d", slot);
        slot_operation_release(&guard);
        save_backup::port::mark_stale();
        return;
    }
    for (const std::string& wh_name : wh_names) {
        const std::string source = save_dir + "/" + wh_name;
        const std::string target = data_trash + "/" + wh_name;
        if (::rename(source.c_str(), target.c_str()) != 0) {
            SB_LOG_WARN("slot delete hook: wh4 move failed slot=%d file=%s", slot,
                        source.c_str());
            (void)restore_trash_entries(entries);
            remove_file(index_path);
            cleanup_trash_dirs();
            slot_operation_release(&guard);
            save_backup::port::mark_stale();
            return;
        }
    }
    bool unlink_failed = false;
    for (const TrashEntry& entry : entries) {
        if (!file_exists(entry.trash_path)) continue;
        if (unlink_file(entry.trash_path) != 0 && errno != ENOENT) {
            unlink_failed = true;
            break;
        }
    }
    if (unlink_failed) {
        if (restore_trash_entries(entries)) {
            remove_file(index_path);
            cleanup_trash_dirs();
        } else {
            SB_LOG_WARN("slot delete hook: rollback incomplete, keep %s", index_path.c_str());
        }
        slot_operation_release(&guard);
        save_backup::port::mark_stale();
        return;
    }
    remove_file(index_path);
    cleanup_trash_dirs();
    slot_operation_release(&guard);
    SB_LOG("slot delete hook: removed sidecar/wh4 slot=%d wh4=%zu", slot, wh_names.size());
    // 游戏删档后槽文件已变：槽区结构不再代表磁盘状态，下一次读取必须重载。
    save_backup::port::mark_stale();
}


std::string import_bundle_file(const std::string& path, int slot) {
    if (slot < 0 || slot > 2) return result_error("bad slot");
    const std::string name = file_name_of(path);
    std::vector<uint8_t> bytes;
    save_backup::ParsedBundle parsed;
    bool module_present = false;
    save_backup::C18NativeBagLayout target_c18_layout =
        save_backup::C18NativeBagLayout::kNotC18;
    {
        std::lock_guard<std::mutex> lk(save_backup::persistence::file_mutex());
        if (!read_file_bytes(path, bytes) ||
            !save_backup::parse_bundle(bytes.data(), bytes.size(), parsed)) {
            return result_error("backup unreadable or corrupt");
        }
        // checksum 必须覆盖全部可还原内容（v4 还含 C18 normalized image），不能只信 meta 声明。
        std::string meta_checksum;
        if (!save_backup::json_find_string(parsed.meta_json, "checksum", meta_checksum) ||
            !save_backup::valid_checksum(meta_checksum) ||
            meta_checksum != save_backup::compute_checksum(parsed.bundle_version, parsed.orig_plain,
                                                           parsed.module, parsed.warehouse_blob,
                                                           parsed.c18_native_bag_blob)) {
            return result_error("backup checksum mismatch");
        }
        // module_state：新备份显式 0/1；旧备份无字段时按 module 是否为空推断。
        long long module_state_raw = -1;
        const bool has_module_state =
            save_backup::json_find_int(parsed.meta_json, "module_state", module_state_raw);
        if (has_module_state && module_state_raw != 0 && module_state_raw != 1) {
            return result_error("backup unreadable or corrupt");
        }
        module_present = has_module_state ? module_state_raw == 1 : !parsed.module.empty();
        if (module_present != !parsed.module.empty()) {
            return result_error("backup unreadable or corrupt");
        }
        std::string identity_error;
        std::string target_c18_version;
        if (!read_target_c18_layout(target_c18_layout, target_c18_version, identity_error)) {
            return result_error(identity_error.c_str());
        }
        const bool target_has_c18_bag =
            target_c18_layout == save_backup::C18NativeBagLayout::kLegacy192 ||
            target_c18_layout == save_backup::C18NativeBagLayout::kModern96;
        if (parsed.has_c18_native_bag && !target_has_c18_bag) {
            return result_error("C18 bag image unsupported on target");
        }
        if (!parsed.has_c18_native_bag && target_has_c18_bag) {
            // 旧备份没有 C18 背包镜像：生成空背包继续导入（扩展背包内容为空），
            // 而不是整次拒绝。镜像在 checksum 校验之后合成，不参与旧 bundle 的校验。
            if (!build_empty_c18_native_bag(target_c18_version, slot, target_c18_layout,
                                            parsed.c18_native_bag)) {
                return result_error("C18 bag image invalid");
            }
            parsed.has_c18_native_bag = true;
            SB_LOG("save backup import: slot%d missing C18 bag image, materialized empty image (%s)",
                   slot, target_c18_version.c_str());
        }
    }
    const bool c18_write = target_c18_layout == save_backup::C18NativeBagLayout::kLegacy192 ||
                           target_c18_layout == save_backup::C18NativeBagLayout::kModern96;
    std::vector<uint8_t> c18_target_file;
    if (c18_write) {
        if (save_backup::state::snapshot().data_dir.empty()) {
            return result_error("C18 data directory unavailable");
        }
        if (!save_backup::build_c18_native_bag_file(parsed.c18_native_bag,
                                                    target_c18_layout, slot,
                                                    c18_target_file)) {
            return result_error("C18 bag image invalid");
        }
    }
    // 旧备份缺职业/等级：导入前先从明文补全并写回源 bundle（不改 checksum/版本）。
    (void)save_backup::usecases::meta_backfill::backfill_entry_meta_from_plain(path, bytes, parsed);

    // 前置门：首次写删之前 acquire；parsed.module 为空也必须 acquire（dat 写入本身
    // 就是不可逆变更）。
    SlotOperationGuard guard;
    if (!slot_operation_acquire(slot, &guard)) return result_error("slot operation unavailable");

    // 1) 原版明文加密到目标槽（块0 slot 字节改写目标槽并追加校验尾）。
    std::vector<uint8_t> cipher;
    if (!save_backup::port::encrypt_plain_to_slot(slot, parsed.orig_plain, cipher) ||
        cipher.empty()) {
        return result_error("encrypt failed");
    }

    // 2) 定位存档目录：优先 dataDir 下含 save*.dat 的子目录，退回 md5(密钥) 目录。
    std::string save_dir;
    switch (locate_save_directory(save_dir)) {
        case 0:
            break;
        case 2:
            return result_error("save directory mkdir failed");
        default:
            return result_error("save directory not found");
    }

    std::string c18_target_path;
    if (c18_write) {
        const bool legacy = target_c18_layout == save_backup::C18NativeBagLayout::kLegacy192;
        c18_target_path = save_backup::state::snapshot().data_dir + "/C18L" +
                          (legacy ? "bag" : "wh") + std::to_string(slot + 1) + ".dat";
    }

    // 2a) 仓库写回预校验/预加密（必须在写任何文件之前，保证 .rollback/ 与现存文件不被触碰）：
    //     fail-closed —— 任一项失败整次导入失败。
    //       flags=0（密文 wh4）：必须能用本机密钥解密（v2 旧备份仅支持同设备导入；
    //                            导入后再导出仍由 warehouse flags 路径决定可跨设备）。
    //       flags=1（明文 wh4）：补 slot + 重算 +0x38 完整性校验 + 重加密 + 往返自检。
    //     变体能力门禁 unsupported → 整步跳过（保持既有行为，不校验也不写）。
    std::vector<std::vector<uint8_t>> wh_payloads;
    qol::contracts::QolTargetFeatureState target_state =
        qol::contracts::QolTargetFeatureState::kUnknown;
    bool wh_write = false;
    if (parsed.has_warehouse) {
        if (save_backup::state::snapshot().game != nullptr && save_backup::state::snapshot().game->target.warehouse_companion_file_state != nullptr) {
            (void)save_backup::state::snapshot().game->target.warehouse_companion_file_state(&target_state);
        }
        if (target_state == qol::contracts::QolTargetFeatureState::kUnsupported) {
            SB_LOG("save backup import: slot%d warehouse companion unsupported (%s), skip %zu file(s)",
                   slot, save_backup::port::warehouse_state_name(target_state), parsed.warehouse.size());
        } else {
            wh_write = true;
            const char* wh_key = save_backup::port::hub_key();
            wh_payloads.resize(parsed.warehouse.size());
            for (size_t i = 0; i < parsed.warehouse.size(); ++i) {
                const save_backup::WarehouseFile& wf = parsed.warehouse[i];
                if ((wf.flags & save_backup::kWarehouseFlagPlain) != 0) {
                    const save_backup::WarehouseReencryptStatus status =
                        save_backup::port::warehouse_reencrypt_on_device(wf, slot, wh_key, wh_payloads[i]);
                    if (status != save_backup::WarehouseReencryptStatus::kOk) {
                        return result_error(("warehouse companion reencrypt failed (" +
                                       std::string(save_backup::warehouse_reencrypt_status_name(status)) +
                                       "): " + wf.suffix).c_str());
                    }
                } else {
                    if (!save_backup::port::warehouse_blob_usable_on_this_device(wf.data.data(), wf.data.size(),
                                                              wh_key)) {
                        return result_error(("warehouse companion not decryptable on this device: " +
                                       wf.suffix).c_str());
                    }
                    wh_payloads[i] = wf.data;
                }
            }
        }
    }

    // 3) 安全备份现存文件到 bundle 同目录的 .rollback/（手动 = backups/，auto = autobackup/）。
    const size_t path_slash = path.find_last_of('/');
    const std::string bundle_dir =
        path_slash == std::string::npos ? std::string() : path.substr(0, path_slash);
    const std::string rollback_dir = bundle_dir + "/" + kRollbackDirName;
    if (!ensure_dir(rollback_dir)) {
        slot_operation_release(&guard);
        return result_error("rollback dir mkdir failed");
    }
    const std::string dat_path = save_dir + "/save" + std::to_string(slot) + ".dat";
    const bool dat_existed = file_exists(dat_path);
    // 回滚副本按事务 id 命名：并发/重试不共用同一路径。
    const std::string tx_id = next_transaction_id();
    const std::string rb_dat =
        rollback_dir + "/save" + std::to_string(slot) + ".dat." + tx_id;
    if (dat_existed && !copy_file(dat_path, rb_dat)) {
        slot_operation_release(&guard);
        return result_error("rollback backup failed");
    }
    bool c18_target_existed = false;
    std::vector<uint8_t> previous_c18_target;
    const std::string rb_c18 = c18_write
                                   ? rollback_dir + "/" + file_name_of(c18_target_path) + "." + tx_id
                                   : std::string();
    if (c18_write &&
        (!snapshot_c18_target(c18_target_path, c18_target_existed, previous_c18_target) ||
         (c18_target_existed &&
          !write_file_bytes_atomic(rb_c18, previous_c18_target.data(),
                                   previous_c18_target.size())))) {
        remove_file(rb_dat);
        slot_operation_release(&guard);
        return result_error("rollback backup failed");
    }
    std::vector<uint8_t> prev_container;
    bool prev_container_exists = false;
    {
        std::vector<uint8_t> probe;
        if (store_read_container(slot, probe) == ContainerStatus::kOk) {
            prev_container.swap(probe);
            prev_container_exists = true;
        }
    }
    bool sidecar_touched = false;
    // 完整回滚结束才 release；回滚失败保留 referenced lease 与证据，不伪装收尾成功。
    auto fail_with_rollback = [&](const save_backup::RollbackResult& rollback,
                                  const char* ok_message, const char* degraded_message) {
        if (rollback.complete()) {
            remove_file(rb_c18);
            slot_operation_release(&guard);
            return result_error(ok_message);
        }
        slot_operation_hold_referenced(&guard);
        return result_error(degraded_message);
    };

    // 6) 个人仓库伴生文件写回（类别顺序由注册表驱动；预校验/预加密已完成，见 2a）。
    //    **语义为 additive**：只新增/覆盖 bundle 内后缀对应的目标文件，不删除目标槽其它
    //    后缀的 wh4 文件——模块不掌握游戏仓库文件的完整集合语义，批量删除可能误删其它数据。
    struct WhRollback {
        std::string target;
        std::string rb;
        bool existed = false;
    };
    std::vector<WhRollback> wh_rollback;
    auto restore_warehouse = [&wh_rollback]() {
        save_backup::RollbackResult result;
        for (auto it = wh_rollback.rbegin(); it != wh_rollback.rend(); ++it) {
            bool restored = true;
            if (it->existed) {
                restored = file_exists(it->rb) && copy_file_replacing(it->rb, it->target);
            } else {
                restored = remove_file(it->target);
            }
            if (restored) {
                remove_file(it->rb);
            } else {
                result.warehouse_restored = false;
                if (it->existed) {
                    result.evidence_preserved = file_exists(it->rb) && result.evidence_preserved;
                } else {
                    static constexpr char kMissingWarehouse[] = "warehouse target was absent\n";
                    result.evidence_preserved = preserve_rollback_record(
                                                    it->target + ".rollback-failed",
                                                    reinterpret_cast<const uint8_t*>(kMissingWarehouse),
                                                    sizeof(kMissingWarehouse) - 1) &&
                                                result.evidence_preserved;
                }
            }
        }
        return result;
    };

    auto restore_c18_native_bag = [&]() {
        save_backup::RollbackResult result;
        if (!c18_write) return result;
        bool restored = false;
        if (c18_target_existed) {
            restored = file_exists(rb_c18) && copy_file_replacing(rb_c18, c18_target_path);
        } else {
            restored = remove_file(c18_target_path);
        }
        if (restored) {
            remove_file(rb_c18);
        } else {
            result.c18_native_bag_restored = false;
            if (c18_target_existed) {
                result.evidence_preserved = file_exists(rb_c18);
            } else {
                static constexpr char kMissingC18Bag[] = "C18 target was absent\n";
                result.evidence_preserved = preserve_rollback_record(
                    rb_c18 + ".dat-missing",
                    reinterpret_cast<const uint8_t*>(kMissingC18Bag),
                    sizeof(kMissingC18Bag) - 1);
            }
        }
        return result;
    };

    // 4) original_dat：原子写原版 .dat（tmp+rename）。
    auto import_original_dat = [&]() -> std::string {
        if (!write_file_bytes_atomic(dat_path, cipher.data(), cipher.size())) {
            const save_backup::RollbackResult rollback = restore_from_rollback(
                slot, dat_path, rb_dat, dat_existed, prev_container, prev_container_exists,
                sidecar_touched, &guard);
            return fail_with_rollback(rollback, "save file write failed",
                                      "save file write failed; rollback failed");
        }
        return {};
    };

    // 5) module_sidecar：present 重定槽双写；absent 是完整替换——删除目标槽现存 sidecar。
    auto import_module_sidecar = [&]() -> std::string {
        if (module_present) {
            bool store_touched = false;
            const bool imported = store_import_container_locked_gate(
                guard, slot, parsed.module.data(), parsed.module.size(), &store_touched);
            sidecar_touched = store_touched;
            if (!imported) {
                const save_backup::RollbackResult rollback = restore_from_rollback(
                    slot, dat_path, rb_dat, dat_existed, prev_container, prev_container_exists,
                    sidecar_touched, &guard);
                return fail_with_rollback(rollback, "module container import failed",
                                          "module container import failed; rollback failed");
            }
        } else {
            // 删除入口可能已删除部分文件，回滚统一按“已触碰”处理以恢复快照。
            sidecar_touched = true;
            if (!store_delete_slot_locked_gate(guard, slot)) {
                const save_backup::RollbackResult rollback = restore_from_rollback(
                    slot, dat_path, rb_dat, dat_existed, prev_container, prev_container_exists,
                    sidecar_touched, &guard);
                return fail_with_rollback(rollback, "module container import failed",
                                          "module container import failed; rollback failed");
            }
        }
        return {};
    };

    // 6) warehouse_companion：仅 v2/v3/v4（has_warehouse=true）写回；v1 完全跳过。
    auto import_warehouse = [&]() -> std::string {
        if (!wh_write) return {};
        size_t wh_written = 0;
        size_t wh_reencrypted = 0;
        for (size_t i = 0; i < parsed.warehouse.size(); ++i) {
            const save_backup::WarehouseFile& wf = parsed.warehouse[i];
            const std::string target =
                save_dir + "/save" + std::to_string(slot) + ".dat" + wf.suffix;
            const std::vector<uint8_t>& payload = wh_payloads[i];
            const bool existed = file_exists(target);
            std::string rb;
            if (existed) {
                rb = rollback_dir + "/" + file_name_of(target) + "." + tx_id;
                if (!copy_file(target, rb)) {
                    remove_file(rb);  // 清理可能写了一半的副本
                    const save_backup::RollbackResult warehouse_rollback = restore_warehouse();
                    const save_backup::RollbackResult core_rollback = restore_from_rollback(
                        slot, dat_path, rb_dat, dat_existed, prev_container, prev_container_exists,
                        sidecar_touched, &guard);
                    if (warehouse_rollback.complete() && core_rollback.complete()) {
                        remove_file(rb_c18);
                        slot_operation_release(&guard);
                        return result_error("rollback backup failed");
                    }
                    slot_operation_hold_referenced(&guard);
                    return result_error("rollback backup failed; rollback failed");
                }
            }
            wh_rollback.push_back(WhRollback{target, rb, existed});
            if (!write_file_bytes_atomic(target, payload.data(), payload.size())) {
                const save_backup::RollbackResult warehouse_rollback = restore_warehouse();
                const save_backup::RollbackResult core_rollback = restore_from_rollback(
                    slot, dat_path, rb_dat, dat_existed, prev_container, prev_container_exists,
                    sidecar_touched, &guard);
                if (warehouse_rollback.complete() && core_rollback.complete()) {
                    remove_file(rb_c18);
                    slot_operation_release(&guard);
                    return result_error("warehouse file write failed");
                }
                slot_operation_hold_referenced(&guard);
                return result_error("warehouse file write failed; rollback failed");
            }
            ++wh_written;
            if ((wf.flags & save_backup::kWarehouseFlagPlain) != 0) ++wh_reencrypted;
        }
        if (wh_written > 0) {
            SB_LOG("save backup import: slot%d warehouse companion files wrote=%zu skipped=0 "
                   "reencrypted=%zu (state=%s)",
                   slot, wh_written, wh_reencrypted, save_backup::port::warehouse_state_name(target_state));
        }
        return {};
    };

    auto import_c18_native_bag = [&]() -> std::string {
        if (!c18_write) return {};
        if (!write_file_bytes_atomic(c18_target_path, c18_target_file.data(),
                                     c18_target_file.size())) {
            const save_backup::RollbackResult c18_rollback = restore_c18_native_bag();
            const save_backup::RollbackResult warehouse_rollback = restore_warehouse();
            const save_backup::RollbackResult core_rollback = restore_from_rollback(
                slot, dat_path, rb_dat, dat_existed, prev_container, prev_container_exists,
                sidecar_touched, &guard);
            save_backup::RollbackResult rollback;
            rollback.dat_restored = core_rollback.dat_restored;
            rollback.sidecar_restored = core_rollback.sidecar_restored;
            rollback.warehouse_restored = warehouse_rollback.warehouse_restored;
            rollback.c18_native_bag_restored = c18_rollback.c18_native_bag_restored;
            rollback.evidence_preserved = core_rollback.evidence_preserved &&
                                          warehouse_rollback.evidence_preserved &&
                                          c18_rollback.evidence_preserved;
            if (rollback.complete()) {
                slot_operation_release(&guard);
                return result_error("C18 bag file write failed");
            }
            slot_operation_hold_referenced(&guard);
            return result_error("C18 bag file write failed; rollback failed");
        }
        return {};
    };

    // 类别顺序由注册表唯一决定；任一类失败立即返回其既有回滚结果。
    size_t class_count = 0;
    const qol::features::save_backup::BackupFileClass* classes =
        qol::features::save_backup::backup_file_classes(&class_count);
    for (size_t i = 0; i < class_count; ++i) {
        std::string response;
        switch (classes[i].kind) {
            case qol::features::save_backup::BackupFileKind::kOriginalDat:
                response = import_original_dat();
                break;
            case qol::features::save_backup::BackupFileKind::kModuleSidecar:
                response = import_module_sidecar();
                break;
            case qol::features::save_backup::BackupFileKind::kWarehouseCompanion:
                response = import_warehouse();
                break;
        }
        if (!response.empty()) return response;
    }
    const std::string c18_response = import_c18_native_bag();
    if (!c18_response.empty()) return c18_response;

    // 7) 成功：清理该槽回滚副本（dat、wh 与 C18 bag；sidecar 无需副本）。
    for (const WhRollback& item : wh_rollback) remove_file(item.rb);
    remove_file(rb_dat);
    remove_file(rb_c18);
    slot_operation_release(&guard);
    save_backup::Entry entry;
    save_backup::entry_from_bundle(name, parsed, entry);
    entry.map_name = save_backup::state::mapName(entry.map_id);
    entry.class_name = save_backup_class_name(entry.class_idx);
    SB_LOG("save backup imported: %s -> slot%d", name.c_str(), slot);
    // 目标槽文件已被覆盖：槽区结构不再代表磁盘状态，下一次读取必须重载。
    save_backup::port::mark_stale();
    return save_backup::api::entry_response(entry);
}


std::string delete_json(const char* checksum) {
    std::lock_guard<std::mutex> lk(save_backup::persistence::file_mutex());
    const std::string ck = checksum != nullptr ? checksum : "";
    if (!save_backup::valid_checksum(ck)) return result_error("bad checksum");
    std::string dir;
    if (!backup_dir(dir)) return result_error("backup not found");
    const std::string path = find_by_checksum(dir, ck);
    if (path.empty()) return result_error("backup not found");
    // 删除前校验内容 checksum：防止篡改 payload 但仍以原 checksum 标识的 bundle 被清掉后
    // 无法追溯；解析失败沿用 unreadable 文案。
    std::vector<uint8_t> bytes;
    save_backup::ParsedBundle parsed;
    if (!read_file_bytes(path, bytes) ||
        !save_backup::parse_bundle(bytes.data(), bytes.size(), parsed)) {
        return result_error("backup unreadable or corrupt");
    }
    std::string meta_checksum;
    if (!save_backup::json_find_string(parsed.meta_json, "checksum", meta_checksum) ||
         !save_backup::valid_checksum(meta_checksum) ||
         meta_checksum != save_backup::compute_checksum(parsed.bundle_version, parsed.orig_plain,
                                                        parsed.module, parsed.warehouse_blob,
                                                        parsed.c18_native_bag_blob)) {
        return result_error("backup checksum mismatch");
    }
    if (::unlink(path.c_str()) != 0) {
        SB_LOG("save backup delete failed: %s", path.c_str());
        return result_error("delete failed");
    }
    SB_LOG("save backup deleted: %s", path.c_str());
    return save_backup::api::delete_response();
}


// 删除某个槽的游戏存档：原版 save{slot}.dat（游戏目录）+ 模块 sidecar
// （经槽操作 lease 的 store 事务入口）。两阶段：先把所有目标 rename 到各自父目录内的
// `.sb-trash-<id>`（dat 与 sidecar 不同文件系统，不能共用一次 rename），全部成功后再 unlink；
// 任一步失败把已移动文件移回。index 记录 trash 路径与原路径，进程中途死亡时由启动恢复处理。
// 安全边界：路径由整型 slot 精确拼接（save0/1/2.dat），不含通配/遍历；缺失的目标跳过。
// 全部失败路径返回结构化 op_err；无任何目标文件时返回 not found。
std::string delete_slot_json(int slot) {
    // 存在性探测持锁；gate/删除在锁外，同槽串行由槽操作门保证。
    std::unique_lock<std::mutex> lk(save_backup::persistence::file_mutex());
    if (slot < 0 || slot > 2) return result_error("bad slot");
    std::string save_dir;
    switch (locate_save_directory(save_dir)) {
        case 0:
            break;
        case 2:
            return result_error("save directory mkdir failed");
        default:
            return result_error("save directory not found");
    }
    const std::string dat_path = save_dir + "/save" + std::to_string(slot) + ".dat";
    const bool dat_existed = file_exists(dat_path);
    // store 无独立「是否存在」查询：read_container 对可解码 primary 返回 kOk；损坏 primary
    // 返回 kCorrupt（文件存在但不可用），两者都视为该槽存在 sidecar；kNotFound 视为不存在。
    std::vector<uint8_t> probe;
    const ContainerStatus probe_status = store_read_container(slot, probe);
    const bool sidecar_existed = probe_status == ContainerStatus::kOk ||
                                 probe_status == ContainerStatus::kCorrupt;
    const std::vector<std::string> wh_names =
        qol::features::save_backup::list_warehouse_companions(save_dir, slot);
    if (!dat_existed && !sidecar_existed && wh_names.empty()) {
        return result_error("save not found");
    }
    lk.unlock();

    SlotOperationGuard guard;
    if (!slot_operation_acquire(slot, &guard)) return result_error("slot operation unavailable");

    const std::string id = next_transaction_id();
    const std::string dat_trash = save_dir + "/.sb-trash-" + id;
    const bool dat_trash_needed = dat_existed || !wh_names.empty();
    std::string saves_dir;
    std::string sidecar_trash;
    if (sidecar_existed) {
        if (save_backup::state::snapshot().external_dir.empty()) {
            slot_operation_release(&guard);
            return result_error("delete sidecar failed");
        }
        saves_dir = save_backup::state::snapshot().external_dir + "/saves";
        if (!ensure_dir(saves_dir)) {
            slot_operation_release(&guard);
            return result_error("delete sidecar failed");
        }
        sidecar_trash = saves_dir + "/.sb-trash-" + id;
    }
    std::string backups_dir;
    if (!backup_dir(backups_dir)) {
        slot_operation_release(&guard);
        return result_error("delete save failed");
    }
    const std::string index_path = backups_dir + "/.trash-" + id + ".index";

    std::vector<TrashEntry> entries;
    // 顺序与注册表一致：dat → sidecar → wh4；失败回滚逆序。
    if (dat_existed) {
        entries.push_back({dat_trash + "/" + file_name_of(dat_path), dat_path});
    }
    if (sidecar_existed) {
        const std::string sidecar_base = saves_dir + "/slot-" + std::to_string(slot) + ".qol-save";
        entries.push_back({sidecar_trash + "/" + file_name_of(sidecar_base), sidecar_base});
        const std::string last_good = sidecar_base + ".last-good";
        entries.push_back({sidecar_trash + "/" + file_name_of(last_good), last_good});
        const std::string corrupt_marker = sidecar_base + ".unresolved-corrupt";
        entries.push_back({sidecar_trash + "/" + file_name_of(corrupt_marker), corrupt_marker});
    }
    for (const std::string& wh_name : wh_names) {
        entries.push_back({dat_trash + "/" + wh_name, save_dir + "/" + wh_name});
    }

    auto cleanup_trash_dirs = [&]() {
        if (dat_trash_needed) remove_dir_if_empty(dat_trash);
        if (sidecar_existed) remove_dir_if_empty(sidecar_trash);
    };
    // 已发生 rename 后的失败：先把仍留在 trash 的文件移回，全部移回才清 index/目录；
    // 移回不完整时保留 trash+index 供下次启动恢复。
    auto fail_after_moves = [&](const char* message) {
        if (restore_trash_entries(entries)) {
            remove_file(index_path);
            cleanup_trash_dirs();
        } else {
            SB_LOG_WARN("slot delete rollback incomplete, keep %s", index_path.c_str());
        }
        slot_operation_release(&guard);
        return result_error(message);
    };

    if (dat_trash_needed && ::mkdir(dat_trash.c_str(), 0755) != 0) {
        slot_operation_release(&guard);
        return result_error("delete save failed");
    }
    if (sidecar_existed && ::mkdir(sidecar_trash.c_str(), 0755) != 0) {
        if (dat_trash_needed) (void)::rmdir(dat_trash.c_str());
        slot_operation_release(&guard);
        return result_error("delete sidecar failed");
    }
    if (!write_trash_index(index_path, entries)) {
        if (dat_trash_needed) (void)::rmdir(dat_trash.c_str());
        if (sidecar_existed) (void)::rmdir(sidecar_trash.c_str());
        slot_operation_release(&guard);
        return result_error("delete save failed");
    }

    // 移动阶段按注册表顺序执行；任一失败移回全部已移动文件。
    size_t class_count = 0;
    const qol::features::save_backup::BackupFileClass* classes =
        qol::features::save_backup::backup_file_classes(&class_count);
    for (size_t i = 0; i < class_count; ++i) {
        switch (classes[i].kind) {
            case qol::features::save_backup::BackupFileKind::kOriginalDat:
                if (dat_existed &&
                    ::rename(dat_path.c_str(),
                             (dat_trash + "/" + file_name_of(dat_path)).c_str()) != 0) {
                    return fail_after_moves("delete save failed");
                }
                break;
            case qol::features::save_backup::BackupFileKind::kModuleSidecar:
                if (sidecar_existed) {
                    if (guard.api->abi.minor < 1 ||
                        guard.api->delete_container_to_trash == nullptr) {
                        SB_LOG_WARN("save slot delete: trash api unavailable slot=%d", slot);
                        return fail_after_moves("delete sidecar failed");
                    }
                    const QolResult status = guard.api->delete_container_to_trash(
                        guard.api->host_ctx, save_backup::state::snapshot().owner, &guard.lease, slot,
                        sidecar_trash.c_str());
                    if (!qol::contracts::succeeded(status)) {
                        SB_LOG("save slot delete sidecar move failed: slot=%d", slot);
                        return fail_after_moves("delete sidecar failed");
                    }
                }
                break;
            case qol::features::save_backup::BackupFileKind::kWarehouseCompanion:
                for (const std::string& wh_name : wh_names) {
                    const std::string source = save_dir + "/" + wh_name;
                    const std::string target = dat_trash + "/" + wh_name;
                    if (::rename(source.c_str(), target.c_str()) != 0) {
                        SB_LOG("save slot delete wh4 move failed: %s", source.c_str());
                        return fail_after_moves("delete save failed");
                    }
                }
                break;
        }
    }

    // 第二阶段：逐文件 unlink；失败时把尚未删除的文件移回。
    for (const TrashEntry& entry : entries) {
        if (!file_exists(entry.trash_path)) continue;
        if (unlink_file(entry.trash_path) != 0 && errno != ENOENT) {
            SB_LOG("save slot delete unlink failed: %s", entry.trash_path.c_str());
            if (restore_trash_entries(entries)) {
                remove_file(index_path);
                cleanup_trash_dirs();
            } else {
                SB_LOG_WARN("slot delete rollback incomplete, keep %s", index_path.c_str());
            }
            slot_operation_release(&guard);
            return result_error("delete incomplete");
        }
    }
    remove_file(index_path);
    cleanup_trash_dirs();
    slot_operation_release(&guard);
    SB_LOG("save slot deleted: slot=%d (dat=%d sidecar=%d wh4=%zu)", slot, dat_existed,
           sidecar_existed, wh_names.size());
    // 槽文件已删除：槽区结构不再代表磁盘状态，下一次读取必须重载。
    save_backup::port::mark_stale();
    return save_backup::api::deleted_slot_response(slot);
}


}  // namespace transaction
}  // namespace usecases
}  // namespace save_backup

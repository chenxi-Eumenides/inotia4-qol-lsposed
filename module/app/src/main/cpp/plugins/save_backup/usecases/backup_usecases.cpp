#include "backup_usecases.h"


#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../feature.h"
#include "../backup_files.h"
#include "../bundle.h"
#include "contracts/slot_operation.h"
#include "../log.h"
#include "../persistence/persistence.h"
#include "../port/save_port.h"
#include "../rollback.h"
#include "../state/state.h"
#include "../transaction/transactions.h"
#include "../api/json_codec.h"
#include "meta_backfill.h"

namespace save_backup::usecases {

namespace {

#define SB_LOG(...) SAVE_BACKUP_LOG_INFO(__VA_ARGS__)
#define SB_LOG_WARN(...) SAVE_BACKUP_LOG_WARN(__VA_ARGS__)

void bind_backup_files_source();

constexpr const char* kBackupDirName = "backups";
constexpr const char* kBundleSuffix = ".qol.bak";
// 旧版导出的后缀：继续列出/解析/导入，避免升级后旧备份消失。
constexpr const char* kLegacyBundleSuffix = ".qol_save";

// 列表扫描的本地 DoS 防护上限（不是备份格式或 HTTP 契约）：
// 单个 .qol.bak/.qol_save 超过 64 MiB 视为损坏/异常文件跳过；单次扫描累计读取超过 512 MiB 即停止。
constexpr long long kListMaxBundleBytes = 64LL * 1024 * 1024;
constexpr long long kListMaxTotalBytes = 512LL * 1024 * 1024;


// L2/L1 依赖：文件原语、目录、bundle 枚举、运行期快照。
using save_backup::persistence::copy_file;
using save_backup::persistence::copy_file_replacing;
using save_backup::persistence::dir_contains_save_dat;
using save_backup::persistence::ends_with;
using save_backup::persistence::ensure_dir;
using save_backup::persistence::file_exists;
using save_backup::persistence::file_name_of;
using save_backup::persistence::find_by_checksum;
using save_backup::persistence::is_bundle_file_name;
using save_backup::persistence::is_save_dat_name;
using save_backup::persistence::list_bundle_files;
using save_backup::persistence::local_stamp;
using save_backup::persistence::next_transaction_id;
using save_backup::persistence::now_ms;
using save_backup::persistence::read_file_bytes;
using save_backup::persistence::remove_file;
using save_backup::persistence::slot_file_exists;
using save_backup::persistence::write_file_bytes_atomic;

bool backup_dir_at(const std::string& external_dir, std::string& out) {
    return save_backup::persistence::backup_dir(external_dir, out);
}

bool backup_dir(std::string& out) {
    return backup_dir_at(save_backup::state::snapshot().external_dir, out);
}

int locate_save_directory_at(const std::string& data_dir, std::string& out) {
    if (data_dir.empty()) return 1;
    const char* key = save_backup::port::hub_key();
    return save_backup::persistence::locate_save_directory(
               data_dir, key != nullptr ? key : "", out)
               ? 0
               : 1;
}

int locate_save_directory(std::string& out) {
    return locate_save_directory_at(save_backup::state::snapshot().data_dir, out);
}

void read_hero_meta(int32_t slot, int& hero_level, int& hero_index, int& class_idx) {
    const save_backup::state::SlotMeta meta = save_backup::state::slotMeta(slot);
    hero_level = meta.valid ? meta.hero_level : -1;
    hero_index = meta.valid ? meta.hero_index : -1;
    class_idx = meta.valid ? meta.class_idx : -1;
}

void set_map_names_locked(const std::string& json) {
    save_backup::state::setMapNames(json);
}

std::unordered_map<int, std::string> snapshot_map_names() {
    return save_backup::state::mapNamesSnapshot();
}

bool save_api_ready() { return save_backup::port::save_api_ready(); }
bool save_encrypt_available() { return save_backup::port::encrypt_available(); }
const char* save_hub_key() { return save_backup::port::hub_key(); }
int save_encrypt_process(void* data, int len, int mode, const char* key) {
    return save_backup::port::encrypt_process(data, len, mode, key);
}
void save_mark_stale() { save_backup::port::mark_stale(); }
bool encrypt_plain_to_slot(int slot, const std::vector<uint8_t>& plain,
                           std::vector<uint8_t>& cipher) {
    return save_backup::port::encrypt_plain_to_slot(slot, plain, cipher);
}


std::string result_ok() {
    return save_backup::api::result_ok(save_backup::state::snapshot().sdk);
}

std::string result_error(const char* message) {
    return save_backup::api::result_error(save_backup::state::snapshot().sdk, message);
}

// 导入门禁：仅在主菜单（未加载任何槽，current_slot == -1）允许覆盖存档；游戏运行中
// 导入会与已加载槽的结构/文件冲突。current_slot 读取失败按不可用处理（fail-closed）。
bool import_allowed_now(std::string& err) {
    // world.facts state=4 为游戏主菜单；主菜单下 current_slot 可能仍保留上次槽号，不能单独依赖。
    if (save_backup::state::snapshot().world_facts != nullptr && save_backup::state::snapshot().world_facts->state != nullptr) {
        int32_t state = -1;
        const QolResult world = save_backup::state::snapshot().world_facts->state(&state);
        if (qol::contracts::succeeded(world)) {
            if (state == 4) return true;
            SB_LOG("save backup import gate: world state=%d (not main menu)", state);
            err = "import requires main menu";
            return false;
        }
        SB_LOG_WARN("save backup import gate: world state unavailable (code=%d), fallback current_slot",
                    world.code);
    } else {
        SB_LOG_WARN("save backup import gate: world facts unavailable, fallback current_slot");
    }

    if (save_backup::state::snapshot().save_facts == nullptr || save_backup::state::snapshot().save_facts->current_slot == nullptr) {
        err = "save state unavailable";
        return false;
    }
    int32_t current_slot = -1;
    const QolResult result = save_backup::state::snapshot().save_facts->current_slot(&current_slot);
    // 主菜单（未加载）时 Adapter 返回 NotReady 且 out=-1（game/adapter_ports/save_port.cpp:104-108），
    // 因此 NotReady 不能单独判为不可用；其余错误码 fail-closed。
    if (!qol::contracts::succeeded(result) && result.code != kQolErrNotReady) {
        SB_LOG_WARN("save backup import gate: current_slot unavailable (code=%d)", result.code);
        err = "save state unavailable";
        return false;
    }
    if (current_slot >= 0) {
        SB_LOG("save backup import gate: current_slot=%d (not main menu)", current_slot);
        err = "import requires main menu";
        return false;
    }
    SB_LOG("save backup import gate: fallback current_slot=%d (main menu)", current_slot);
    return true;
}

// ---- 地图名表（Kotlin 启动期从 MAPINFOBASE 下发：map_id → 中文名） ----
// 解析失败/空 JSON 视为空表（entry_json 输出空 map_name，UI 显示「未知地图」）。
// 全局单例：state 快照发布，启动期单线程写入、之后只读。

// ---- 基础文件原语 ----





// 目标必须不存在（O_EXCL）；失败时清理目标，供调用方重试。




// ---- 路径与目录 ----

// 备份目录 <external>/backups/（不存在则建）；未初始化/建目录失败返回 false。

// sidecar 容器操作（读 primary/校验/回退、重定槽双写、删除）统一经 infra/store；
// 本文件不再自行拼接 saves/ 路径或直接读写 sidecar 文件。

// 文件名全匹配 `save\d*\.dat`（与迁移前 Kotlin Regex 一致）。

// 新导出用 .qol.bak；旧版 .qol_save 继续识别（列表/导入/删除/去重）。

// 按 bundle 内部 checksum 定位备份：目录下全部 `*.qol.bak` / `*.qol_save` 均为候选，
// 逐个读入并完整解析（parse_bundle 含逐段边界与 CRC32 校验）后比对内部值；解析失败或
// 非 bundle 的候选跳过。文件名不参与匹配（`bundle_matches_checksum` 仅在旧备份 metaJson
// 缺失 checksum 字段时回退文件名后缀）。同 checksum 的多个候选返回目录序中的第一个；
// 未命中返回空串。

// ---- 时间 ----


// ---- 小端字段读取（游戏存档明文结构为 LE）----

uint16_t rd_u16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1]) << 8);
}

int16_t rd_i16(const uint8_t* p) {
    return static_cast<int16_t>(rd_u16(p));
}

uint32_t rd_u32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

uint64_t rd_u64(const uint8_t* p) {
    return uint64_t(rd_u32(p)) | uint64_t(rd_u32(p + 4)) << 32;
}

// 槽结构元数据：只读主线程刷新好的缓存；缓存未建立时保持 -1，绝不跨线程读游戏结构。
// 失败时 err 填错误串（与迁移前 data_op_save_read_original 一致）。
bool read_original_plain(int32_t slot, std::vector<uint8_t>& plain, int& map_id,
                         long long& save_time, int& version, int& hero_level, int& hero_index,
                         int& class_idx, std::string& err) {
    if (!save_api_ready()) {
        err = "symbol not resolved";
        return false;
    }
    void* buf = nullptr;
    int len = 0;
    const QolResult load_result = save_backup::state::snapshot().save_facts->load_data(slot, &buf, &len);
    // 明文头 8 字节 + 块表 offset(+8/+12)：不足 14 字节无法定位块 0/块 1。
    if (!qol::contracts::succeeded(load_result) || buf == nullptr || len < 14) {
        if (buf != nullptr) (void)save_backup::state::snapshot().save_call->write.free_data(buf);
        if (load_result.code == kQolErrNotReady) err = "symbol not resolved";
        else err = "load failed";
        return false;
    }
    // 明文拷出后立即归还游戏堆，后续处理与游戏内存解耦。
    plain.assign(static_cast<const uint8_t*>(buf), static_cast<const uint8_t*>(buf) + len);
    (void)save_backup::state::snapshot().save_call->write.free_data(buf);

    const size_t block0 = 8 + rd_u16(plain.data() + 8);
    const size_t block1 = 8 + rd_u16(plain.data() + 12);
    if (block0 + 33 > plain.size()) {  // 块0 +29 u32 version 需 33 字节
        err = "block0 out of range";
        return false;
    }
    if (block1 + 2 > plain.size()) {  // 块1 +0 i16 map_id
        err = "block1 out of range";
        return false;
    }
    // 归一化：块0 slot 字节写 0xFF，导入任意槽前可安全改写为目标槽。
    plain[block0] = 0xFF;

    version = static_cast<int>(rd_u32(plain.data() + block0 + 29));
    save_time = static_cast<long long>(rd_u64(plain.data() + block0 + 13));
    map_id = static_cast<int>(rd_i16(plain.data() + block1));
    read_hero_meta(slot, hero_level, hero_index, class_idx);
    // 等级/职业以存档明文为准：运行期槽结构在世界内可能指向当前受控佣兵或已释放对象，
    // 与存档主角不一致（真机 slot0 实测槽结构给祭司、存档主角是黑暗骑士）。
    int plain_level = -1;
    int plain_class = -1;
    if (save_backup::parse_plain_main_character(plain, plain_level, plain_class)) {
        hero_level = plain_level;
        class_idx = plain_class;
    }
    return true;
}


}  // namespace

void save_backup_bind_context(const qol::contracts::QolHostSdk* sdk,
                              const qol::contracts::SaveFactsApi* save_facts,
                              const qol::contracts::QolSaveCallPort* save_call,
                              const qol::contracts::QolGameHost* game,
                              qol::contracts::QolOwnerToken owner_token) {
    save_backup::state::Context ctx = save_backup::state::snapshot();
    ctx.sdk = sdk;
    ctx.game = game;
    ctx.save_facts = save_facts;
    ctx.save_call = save_call;
    ctx.owner = owner_token;
    save_backup::state::bind(std::move(ctx));
}

void save_backup_bind_world_facts(const qol::contracts::WorldFactsApi* world_facts) {
    save_backup::state::Context ctx = save_backup::state::snapshot();
    ctx.world_facts = world_facts;
    save_backup::state::bind(std::move(ctx));
}

const qol::contracts::QolHostSdk* save_backup_host_sdk() {
    return save_backup::state::snapshot().sdk;
}

const qol::contracts::QolGameHost* save_backup_game_host() {
    return save_backup::state::snapshot().game;
}

const qol::contracts::SaveFactsApi* save_backup_save_facts() {
    return save_backup::state::snapshot().save_facts;
}

const qol::contracts::QolSaveCallPort* save_backup_save_call() {
    return save_backup::state::snapshot().save_call;
}

std::string save_backup_result_ok() { return result_ok(); }

std::string save_backup_result_error(const char* message) { return result_error(message); }

bool save_backup_slot_file_exists(int32_t slot) {
    return slot_file_exists(save_backup::state::snapshot().data_dir, slot);
}


void save_backup_init(const char* data_dir, const char* external_files_dir) {
    save_backup::state::setDirs(data_dir != nullptr ? data_dir : "",
                                external_files_dir != nullptr ? external_files_dir : "");
    bind_backup_files_source();
    // 启动回滚优先：把上次删槽/删 sidecar 中途死亡留下的 trash 文件移回原位，不再继续删除。
    transaction::recover_deletes();
}

// 游戏主线程刷新槽元数据快照；HTTP/绘制线程只读快照，不直接调用 save.facts 游戏读取函数。
void save_backup_refresh_slot_meta_cache() {
    save_backup::state::refreshSlotMetaOnGameThread();
}

void save_backup_set_map_names(const char* json) {
    set_map_names_locked(json != nullptr ? std::string(json) : std::string());
}

std::string save_backup_map_name(int map_id) { return save_backup::state::mapName(map_id); }

const char* save_backup_class_name(int class_idx) {
    return save_backup::state::className(class_idx);
}

struct FileStamp {
    bool exists = false;
    long long size = 0;
    long long mtime_sec = 0;
    long long mtime_nsec = 0;
};

static FileStamp file_stamp(const std::string& path) {
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return {};
    return FileStamp{true, static_cast<long long>(info.st_size),
                     static_cast<long long>(info.st_mtim.tv_sec),
                     static_cast<long long>(info.st_mtim.tv_nsec)};
}

// 自动备份指纹：只记录影响导出内容/来源的文件状态，不读文件内容。
static std::string autobackup_fingerprint(int slot, const std::string& data_dir,
                                          const std::string& external_dir) {
    std::string save_dir;
    const bool have_save_dir = locate_save_directory_at(data_dir, save_dir) == 0;
    const FileStamp dat =
        have_save_dir ? file_stamp(save_dir + "/save" + std::to_string(slot) + ".dat") : FileStamp{};
    const FileStamp sidecar =
        file_stamp(external_dir + "/saves/slot-" + std::to_string(slot) + ".qol-save");
    const std::vector<std::string> wh_names =
        have_save_dir ? qol::features::save_backup::list_warehouse_companions(save_dir, slot)
                      : std::vector<std::string>{};

    std::string text = "v1\n";
    text += "dat " + std::to_string(dat.exists ? 1 : 0) + " " + std::to_string(dat.size) + " " +
            std::to_string(dat.mtime_sec) + " " + std::to_string(dat.mtime_nsec) + "\n";
    text += "sidecar " + std::to_string(sidecar.exists ? 1 : 0) + " " +
            std::to_string(sidecar.size) + " " + std::to_string(sidecar.mtime_sec) + " " +
            std::to_string(sidecar.mtime_nsec) + "\n";
    text += "wh4 " + std::to_string(wh_names.size()) + "\n";
    for (const std::string& name : wh_names) {
        const FileStamp stamp = file_stamp(save_dir + "/" + name);
        text += name + " " + std::to_string(stamp.size) + " " +
                std::to_string(stamp.mtime_sec) + " " + std::to_string(stamp.mtime_nsec) + "\n";
    }
    // C18 指纹与导出同口径：只看文件状态（per-slot 存在即参与；都不存在时只看 shared fallback），
    // 不读 game_identity/能力位。文件头版本字符串 "v1" 保持不变。
    text += "c18\n";
    if (data_dir.empty()) {
        text += "directory-unavailable\n";
    } else {
        const std::vector<std::string> c18_paths =
            qol::features::save_backup::list_c18_native_bag_files(data_dir, slot);
        for (const std::string& path : c18_paths) {
            const FileStamp stamp = file_stamp(path);
            text += path.substr(data_dir.size()) + " " + std::to_string(stamp.exists ? 1 : 0) +
                    " " + std::to_string(stamp.size) + " " + std::to_string(stamp.mtime_sec) +
                    " " + std::to_string(stamp.mtime_nsec) + "\n";
        }
        if (c18_paths.empty()) {
            const FileStamp shared = file_stamp(data_dir + "/C18Lbag.dat");
            if (shared.exists) {
                text += "shared " + std::to_string(shared.size) + " " +
                        std::to_string(shared.mtime_sec) + " " +
                        std::to_string(shared.mtime_nsec) + "\n";
            }
        }
    }
    return text;
}

void save_backup_autobackup_pre_save(int32_t slot) {
    if (slot < 0 || slot > 2) return;
    static std::atomic<bool> running{false};
    bool expected = false;
    if (!running.compare_exchange_strong(expected, true)) return;
    struct RunningReset {
        std::atomic<bool>& flag;
        ~RunningReset() { flag.store(false); }
    } reset{running};

    save_backup_refresh_slot_meta_cache();
    const auto ctx = save_backup::state::snapshot();
    const std::string& data_dir = ctx.data_dir;
    const std::string& external_dir = ctx.external_dir;
    if (external_dir.empty()) {
        SB_LOG_WARN("autobackup slot=%d skipped: external dir unavailable", slot);
        return;
    }
    const std::string dir = external_dir + "/backups/autobackup";
    if (!ensure_dir(dir)) {
        SB_LOG_WARN("autobackup slot=%d skipped: backup dir unavailable", slot);
        return;
    }
    const std::string stem = "autosave_s" + std::to_string(slot);
    const std::string bundle_path = dir + "/" + stem + kBundleSuffix;
    const std::string legacy_bundle_path = dir + "/" + stem + kLegacyBundleSuffix;
    const std::string fingerprint_path = dir + "/" + stem + ".fingerprint";
    const std::string current = autobackup_fingerprint(slot, data_dir, external_dir);
    std::vector<uint8_t> previous;
    const bool has_existing = file_exists(bundle_path) || file_exists(legacy_bundle_path);
    if (has_existing && read_file_bytes(fingerprint_path, previous) &&
        std::string(previous.begin(), previous.end()) == current) {
        return;
    }
    std::string error;
    if (!save_backup_export_slot_to_dir(slot, dir.c_str(), (stem + kBundleSuffix).c_str(), &error)) {
        SB_LOG_WARN("autobackup slot=%d export failed: %s", slot, error.c_str());
        return;
    }
    if (!write_file_bytes_atomic(fingerprint_path,
                                 reinterpret_cast<const uint8_t*>(current.data()),
                                 current.size())) {
        SB_LOG_WARN("autobackup slot=%d fingerprint write failed", slot);
    }
}


struct SbDirs {
    std::string data;
    std::string external;
};

namespace {

bool bridge_collect_dat(int32_t slot, qol::features::save_backup::CollectedDat& out,
                        std::string& err) {
    return read_original_plain(slot, out.plain, out.map_id, out.save_time, out.save_version,
                               out.hero_level, out.hero_index, out.class_idx, err);
}

bool bridge_collect_sidecar(int32_t slot, qol::features::save_backup::CollectedSidecar& out,
                            std::string& err) {
    out.bytes.clear();
    switch (transaction::store_read_container(slot, out.bytes)) {
        case transaction::ContainerStatus::kOk:
            out.state = qol::features::save_backup::CollectedSidecar::State::kPresent;
            return true;
        case transaction::ContainerStatus::kNotFound:
            out.state = qol::features::save_backup::CollectedSidecar::State::kAbsent;
            out.bytes.clear();
            return true;
        default:
            err = "module container unreadable";
            return false;
    }
}

bool bridge_locate_save_directory(std::string& out) {
    return locate_save_directory_at(save_backup::state::snapshot().data_dir, out) == 0;
}

bool bridge_get_data_directory(std::string& out) {
    out = save_backup::state::snapshot().data_dir;
    return !out.empty();
}

bool bridge_read_file(const std::string& path, std::vector<uint8_t>& out) {
    return read_file_bytes(path, out);
}

bool bridge_encrypt_available() { return save_encrypt_available(); }

const char* bridge_hub_key() { return save_hub_key(); }

int bridge_encrypt_process(void* data, int32_t len, int32_t mode, const char* key) {
    return save_encrypt_process(data, len, mode, key);
}

const qol::features::save_backup::BackupFilesSource kBackupFilesSource{
    &bridge_collect_dat,        &bridge_collect_sidecar,   &bridge_locate_save_directory,
    &bridge_read_file,          &bridge_encrypt_available, &bridge_hub_key,
    &bridge_encrypt_process,    &bridge_get_data_directory,
};

void bind_backup_files_source() { qol::features::save_backup::backup_files_bind_source(kBackupFilesSource); }

}  // namespace

static SbDirs snapshot_dirs() {
    const auto ctx = save_backup::state::snapshot();
    return SbDirs{ctx.data_dir, ctx.external_dir};
}


std::string save_backup_list_json() {
    const SbDirs dirs = snapshot_dirs();
    const std::unordered_map<int, std::string> map_names = snapshot_map_names();
    std::string dir;
    if (!backup_dir_at(dirs.external, dir)) return save_backup::api::list_response({});
    std::vector<std::string> names;
    if (!list_bundle_files(dir, names)) return save_backup::api::list_response({});
    // 先解析全部 bundle，再按 export_time 倒序（新→旧，UI 第 1 页顶部 = 最新备份）。
    // 权威字段 = bundle 头/metaJson 的 export_time（entry_from_bundle 填充）；
    // 同毫秒并列时按文件名倒序保持稳定口径（文件名前缀即本地时间戳）。
    std::vector<save_backup::Entry> entries;
    entries.reserve(names.size());
    long long scanned_bytes = 0;
    for (const std::string& name : names) {
        const std::string path = dir + "/" + name;
        struct stat st{};
        // 先 stat 再读：超限文件不必先分配内存。
        if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            static_cast<long long>(st.st_size) > kListMaxBundleBytes) {
            SB_LOG("save backup list skip: %s", name.c_str());
            continue;
        }
        const long long file_size = static_cast<long long>(st.st_size);
        if (scanned_bytes + file_size > kListMaxTotalBytes) {
            SB_LOG("save backup list scan stopped: total %lld bytes exceeds %lld", scanned_bytes,
                   kListMaxTotalBytes);
            break;
        }
        scanned_bytes += file_size;
        std::vector<uint8_t> bytes;
        save_backup::ParsedBundle parsed;
        if (!read_file_bytes(path, bytes) ||
            !save_backup::parse_bundle(bytes.data(), bytes.size(), parsed)) {
            SB_LOG("save backup list skip: %s", name.c_str());
            continue;
        }
        // 旧备份缺职业/等级：列表解析到即从明文补全并写回（只改 meta 段）。
        (void)meta_backfill::backfill_entry_meta_from_plain(path, bytes, parsed);
        save_backup::Entry entry;
        save_backup::entry_from_bundle(name, parsed, entry);
        // 从内存 map_id→name 表查表（启动期 Kotlin 下发）；未命中保持空串。
        auto it = map_names.find(entry.map_id);
        if (it != map_names.end()) entry.map_name = it->second;
        // 职业名由 native 常量表补齐；旧备份 class_idx=-1 → 空串（UI 退化显示）。
        entry.class_name = save_backup_class_name(entry.class_idx);
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(),
              [](const save_backup::Entry& a, const save_backup::Entry& b) {
                  if (a.export_time_ms != b.export_time_ms) {
                      return a.export_time_ms > b.export_time_ms;
                  }
                  return a.file_name > b.file_name;
              });
    return save_backup::api::list_response(entries);
}

Outcome<BackupFilesResult> list_backup_files() {
    const SbDirs dirs = snapshot_dirs();
    std::string dir;
    if (!backup_dir_at(dirs.external, dir)) return Outcome<BackupFilesResult>::success({});
    std::vector<std::string> names;
    if (!list_bundle_files(dir, names)) return Outcome<BackupFilesResult>::success({});

    // 只 stat 不读取内容；累计上限按 stat 尺寸计（不是实际读取量）。
    struct ListedFile {
        std::string name;
        long long size = 0;
        long long mtime_ms = 0;
    };
    std::vector<ListedFile> files;
    long long scanned_bytes = 0;
    for (const std::string& name : names) {
        const std::string path = dir + "/" + name;
        struct stat st{};
        if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            static_cast<long long>(st.st_size) > kListMaxBundleBytes) {
            SB_LOG("save backup list files skip: %s", name.c_str());
            continue;
        }
        const long long size = static_cast<long long>(st.st_size);
        if (scanned_bytes + size > kListMaxTotalBytes) {
            SB_LOG("save backup list files stopped: stat total %lld exceeds %lld", scanned_bytes,
                   kListMaxTotalBytes);
            break;
        }
        scanned_bytes += size;
        files.push_back(ListedFile{name, size,
                                   static_cast<long long>(st.st_mtim.tv_sec) * 1000 +
                                       static_cast<long long>(st.st_mtim.tv_nsec) / 1000000});
    }
    std::sort(files.begin(), files.end(), [](const ListedFile& a, const ListedFile& b) {
        if (a.name != b.name) return a.name > b.name;
        return a.mtime_ms > b.mtime_ms;
    });
    std::vector<save_backup::BackupFile> result;
    result.reserve(files.size());
    for (const ListedFile& file : files) {
        result.push_back(save_backup::BackupFile{
            file.name, static_cast<uint64_t>(file.size), file.mtime_ms});
    }
    return Outcome<BackupFilesResult>::success(BackupFilesResult{std::move(result)});
}

std::string save_backup_list_files_json() {
    const auto result = list_backup_files();
    return save_backup::api::files_response(result.ok ? result.value.files
                                                      : std::vector<save_backup::BackupFile>{});
}

// 仓库密文项能否用本机密钥解开：v1 无仓库段、v3+ flags=1（明文）项都返回 false。
// key/解密函数不可用时返回 false（无法判定时不误报）；任一项解不开即视为跨设备密文。
static bool warehouse_has_foreign_ciphertext(const save_backup::ParsedBundle& parsed) {
    if (!parsed.has_warehouse || parsed.warehouse.empty()) return false;
    if (!save_backup::port::encrypt_available()) return false;
    const char* key = save_backup::port::hub_key();
    if (key == nullptr) return false;
    for (const save_backup::WarehouseFile& wf : parsed.warehouse) {
        if ((wf.flags & save_backup::kWarehouseFlagPlain) != 0) continue;
        if (!save_backup::port::warehouse_blob_usable_on_this_device(wf.data.data(), wf.data.size(),
                                                                     key)) {
            return true;
        }
    }
    return false;
}

static Outcome<EntryResult> describe_backup_impl(const std::string& name, bool probe_warehouse) {
    const std::string file_name = name;
    if (file_name.empty() || file_name.find('/') != std::string::npos ||
        file_name.find("..") != std::string::npos || !is_bundle_file_name(file_name)) {
        return Outcome<EntryResult>::failure(BackupError::kInvalidArgument, "bad request");
    }
    const std::string external_dir = save_backup::state::snapshot().external_dir;
    if (external_dir.empty()) return Outcome<EntryResult>::failure(BackupError::kNotFound, "backup not found");
    const std::string path = external_dir + "/" + kBackupDirName + "/" + file_name;
    if (!file_exists(path)) return Outcome<EntryResult>::failure(BackupError::kNotFound, "backup not found");

    std::vector<uint8_t> bytes;
    save_backup::ParsedBundle parsed;
    if (!read_file_bytes(path, bytes) ||
        !save_backup::parse_bundle(bytes.data(), bytes.size(), parsed)) {
        return Outcome<EntryResult>::failure(BackupError::kUnreadable, "backup unreadable or corrupt");
    }
    std::string meta_checksum;
    if (!save_backup::json_find_string(parsed.meta_json, "checksum", meta_checksum) ||
         !save_backup::valid_checksum(meta_checksum) ||
         meta_checksum != save_backup::compute_checksum(parsed.bundle_version, parsed.orig_plain,
                                                        parsed.module, parsed.warehouse_blob,
                                                        parsed.c18_native_bag_blob)) {
        return Outcome<EntryResult>::failure(BackupError::kChecksumMismatch, "backup checksum mismatch");
    }
    // 旧备份缺职业/等级：从已解析的明文补全并写回（只改 meta 段，checksum/版本不变）。
    (void)meta_backfill::backfill_entry_meta_from_plain(path, bytes, parsed);
    save_backup::Entry entry;
    save_backup::entry_from_bundle(file_name, parsed, entry);
    // 仅面板路径探测（会调用游戏密钥/解密函数，必须在游戏线程；HTTP 线程不探测）。
    if (probe_warehouse) entry.warehouse_undecryptable = warehouse_has_foreign_ciphertext(parsed);
    const std::unordered_map<int, std::string> map_names = snapshot_map_names();
    auto it = map_names.find(entry.map_id);
    if (it != map_names.end()) entry.map_name = it->second;
    entry.class_name = save_backup_class_name(entry.class_idx);
    return Outcome<EntryResult>::success(EntryResult{std::move(entry)});
}

Outcome<EntryResult> describe_backup(const std::string& name) {
    return describe_backup_impl(name, false);
}

Outcome<EntryResult> describe_backup_for_panel(const std::string& name) {
    return describe_backup_impl(name, true);
}

std::string save_backup_entry_json(const char* name) {
    const auto result = describe_backup(name != nullptr ? name : "");
    if (!result.ok) return result_error(result.error.message.c_str());
    return save_backup::api::entry_response(result.value.entry);
}


struct BuiltExport {
    std::vector<uint8_t> bundle;
    std::vector<uint8_t> c18_native_bag_blob;
    std::string checksum;
    std::string original_sha256;
    std::string module_sha256;
    std::string class_name;
    long long export_time_ms = 0;
    long long save_time = 0;
    int map_id = 0;
    int hero_level = -1;
    int hero_index = -1;
    int class_idx = -1;
    int save_version = 0;
    int warehouse_count = 0;
};

// 构包共享实现：snapshot → meta/checksum → bundle；不落盘、不去重。
static bool build_export_bundle(int slot, BuiltExport& out, std::string& err) {
    bind_backup_files_source();
    qol::features::save_backup::SlotSnapshot snapshot;
    if (!qol::features::save_backup::collect_slot_snapshot(slot, snapshot, err)) return false;

    const int module_state =
        snapshot.module.state == qol::features::save_backup::CollectedSidecar::State::kPresent ? 1
                                                                                              : 0;
    const std::vector<uint8_t> warehouse_blob =
        save_backup::encode_warehouse(snapshot.warehouse.files);
    if (snapshot.c18_native_bag.present) {
        if (!save_backup::validate_c18_native_bag_image(snapshot.c18_native_bag.image)) {
            err = "C18 bag file unreadable or corrupt";
            return false;
        }
        out.c18_native_bag_blob =
            save_backup::encode_c18_native_bag_image(snapshot.c18_native_bag.image);
        if (out.c18_native_bag_blob.empty()) {
            err = "C18 bag file unreadable or corrupt";
            return false;
        }
    }
    out.export_time_ms = now_ms();
    out.original_sha256 = save_backup::sha256_hex(snapshot.dat.plain, {});
    out.module_sha256 = snapshot.module.bytes.empty()
                            ? std::string()
                            : save_backup::sha256_hex(snapshot.module.bytes, {});
    out.checksum = save_backup::compute_checksum(save_backup::kBundleVersion, snapshot.dat.plain,
                                                 snapshot.module.bytes, warehouse_blob,
                                                 out.c18_native_bag_blob);
    out.class_name = save_backup_class_name(snapshot.dat.class_idx);
    out.map_id = snapshot.dat.map_id;
    out.hero_level = snapshot.dat.hero_level;
    out.hero_index = snapshot.dat.hero_index;
    out.class_idx = snapshot.dat.class_idx;
    out.save_version = snapshot.dat.save_version;
    out.save_time = snapshot.dat.save_time;
    out.warehouse_count = static_cast<int>(snapshot.warehouse.files.size());
    const std::string warehouse_sha =
        snapshot.warehouse.files.empty() ? std::string()
                                         : save_backup::sha256_hex(warehouse_blob, {});
    const std::string meta_json = save_backup::build_meta_json(
        slot, out.export_time_ms, out.map_id, out.hero_level, out.hero_index, out.save_version,
        out.save_time, out.original_sha256, out.module_sha256, out.checksum, out.class_idx,
        out.class_name, out.warehouse_count, warehouse_sha, module_state);
    if (!save_backup::build_bundle(slot, out.export_time_ms, snapshot.dat.plain,
                                   snapshot.module.bytes, warehouse_blob, meta_json, out.bundle,
                                   snapshot.c18_native_bag.present
                                       ? &snapshot.c18_native_bag.image
                                       : nullptr)) {
        err = "C18 bag image invalid";
        return false;
    }
    return true;
}

static std::string export_json_with_lease(int slot) {
    const SbDirs dirs = snapshot_dirs();
    const std::unordered_map<int, std::string> map_names = snapshot_map_names();
    // 先收集完整快照并构包（共享核心）；去重/命名/落盘仍由本函数负责。
    BuiltExport built;
    std::string err;
    if (!build_export_bundle(slot, built, err)) return result_error(err.c_str());
    const std::string& checksum = built.checksum;

    std::string dir;
    if (!backup_dir_at(dirs.external, dir)) return result_error("backup dir unavailable");

    // 内容去重：同 checksum 备份已存在则不写新文件，直接返回既有备份
    //（metaJson 的 checksum 为权威，文件名后缀兜底）。
    const std::string existing = find_by_checksum(dir, checksum);
    if (!existing.empty()) {
        std::vector<uint8_t> bytes;
        save_backup::ParsedBundle parsed;
        if (read_file_bytes(existing, bytes) &&
            save_backup::parse_bundle(bytes.data(), bytes.size(), parsed)) {
            std::string inner_checksum;
            const bool checksum_ok =
                save_backup::json_find_string(parsed.meta_json, "checksum", inner_checksum) &&
                inner_checksum == checksum &&
                save_backup::compute_checksum(parsed.bundle_version, parsed.orig_plain,
                                              parsed.module, parsed.warehouse_blob,
                                              parsed.c18_native_bag_blob) == checksum;
            if (checksum_ok) {
                save_backup::Entry entry;
                save_backup::entry_from_bundle(file_name_of(existing), parsed, entry);
                auto dit = map_names.find(entry.map_id);
                if (dit != map_names.end()) entry.map_name = dit->second;
                entry.class_name = save_backup_class_name(entry.class_idx);
                SB_LOG("save backup deduplicated: %s (checksum=%s)", entry.file_name.c_str(),
                       checksum.c_str());
                return save_backup::api::export_response(entry, true);
            }
        }
        SB_LOG("save backup dedup hit unreadable or mismatched, writing new bundle: %s",
               existing.c_str());
    }

    char stamp[16] = {0};
    if (!local_stamp(built.export_time_ms, stamp)) return result_error("bundle write failed");
    const std::string base = std::string(stamp) + "_s" + std::to_string(slot) + "_" + checksum;
    std::string target = dir + "/" + base + kBundleSuffix;
    int collision = 0;
    while (file_exists(target)) {
        collision += 1;
        if (collision > 99) return result_error("backup name collision");
        target = dir + "/" + base + "_" + std::to_string(collision) + kBundleSuffix;
    }
    if (!write_file_bytes_atomic(target, built.bundle.data(), built.bundle.size())) {
        return result_error("bundle write failed");
    }

    save_backup::Entry entry;
    entry.file_name = file_name_of(target);
    entry.size_bytes = static_cast<long long>(built.bundle.size());
    entry.source_slot = slot;
    entry.export_time_ms = built.export_time_ms;
    entry.map_id = built.map_id;
    auto mit = map_names.find(built.map_id);
    if (mit != map_names.end()) entry.map_name = mit->second;
    entry.hero_level = built.hero_level;
    entry.hero_index = built.hero_index;
    entry.class_idx = built.class_idx;
    entry.class_name = built.class_name;
    entry.save_version = built.save_version;
    entry.save_time = built.save_time;
    entry.original_sha256 = built.original_sha256;
    entry.module_sha256 = built.module_sha256;
    entry.checksum = built.checksum;
    SB_LOG("save backup exported: %s (%zu bytes)", entry.file_name.c_str(), built.bundle.size());
    return save_backup::api::export_response(entry, false);
}

std::string save_backup_export_json(int slot) {
    if (slot < 0 || slot > 2) return result_error("bad slot");
    // 导出同样覆盖槽文件读取/备份写入：持 lease 与导入/删槽串行；文件锁只保护 bundle 读改写。
    transaction::SlotOperationGuard guard;
    if (!transaction::slot_operation_acquire(slot, &guard)) return result_error("slot operation unavailable");
    const std::string response = export_json_with_lease(slot);
    transaction::slot_operation_release(&guard);
    return response;
}

bool save_backup_export_slot_to_dir(int32_t slot, const char* dir, const char* file_name,
                                    std::string* out_error) {
    if (out_error != nullptr) out_error->clear();
    const auto reject = [out_error](const char* message) {
        if (out_error != nullptr) *out_error = message;
        return false;
    };
    if (slot < 0 || slot > 2) return reject("bad slot");
    if (dir == nullptr || dir[0] == '\0' || file_name == nullptr || file_name[0] == '\0' ||
        std::strchr(file_name, '/') != nullptr) {
        return reject("bad request");
    }
    if (!ensure_dir(dir)) return reject("backup dir unavailable");
    BuiltExport built;
    std::string err;
    if (!build_export_bundle(slot, built, err)) return reject(err.c_str());
    const std::string path = std::string(dir) + "/" + file_name;
    if (!write_file_bytes_atomic(path, built.bundle.data(), built.bundle.size())) {
        return reject("bundle write failed");
    }
    return true;
}

std::string save_backup_import_json(const char* checksum, int slot) {
    // 参数校验、门禁在锁外；bundle 定位只取文件锁，写盘/回滚由槽操作门保证串行。
    const std::string ck = checksum != nullptr ? checksum : "";
    if (!save_backup::valid_checksum(ck)) return result_error("bad checksum");
    if (slot < 0 || slot > 2) return result_error("bad slot");
    // 导入会覆盖槽文件，必须先确认当前未加载任何槽；导出不设此限制。
    std::string gate_error;
    if (!import_allowed_now(gate_error)) return result_error(gate_error.c_str());
    std::string dir;
    if (!backup_dir(dir)) return result_error("backup dir unavailable");
    std::string path;
    {
        std::lock_guard<std::mutex> lk(save_backup::persistence::file_mutex());
        path = find_by_checksum(dir, ck);
    }
    if (path.empty()) return result_error("backup not found");
    return transaction::import_bundle_file(path, slot);
}

static std::string auto_backup_path(const std::string& external_dir, int slot) {
    const std::string base =
        external_dir + "/backups/autobackup/autosave_s" + std::to_string(slot);
    const std::string path = base + kBundleSuffix;
    if (file_exists(path)) return path;
    const std::string legacy = base + kLegacyBundleSuffix;
    return file_exists(legacy) ? legacy : std::string();
}

Outcome<EntryResult> auto_entry(int slot) {
    if (slot < 0 || slot > 2) return Outcome<EntryResult>::failure(BackupError::kInvalidArgument, "bad slot");
    const std::string external_dir = save_backup::state::snapshot().external_dir;
    const std::string path = auto_backup_path(external_dir, slot);
    if (path.empty()) return Outcome<EntryResult>::failure(BackupError::kNotFound, "backup not found");
    std::vector<uint8_t> bytes;
    save_backup::ParsedBundle parsed;
    if (!read_file_bytes(path, bytes) ||
        !save_backup::parse_bundle(bytes.data(), bytes.size(), parsed)) {
        return Outcome<EntryResult>::failure(BackupError::kUnreadable, "backup unreadable or corrupt");
    }
    // 自动备份同样是右侧条目：缺职业/等级时从明文补全并写回。
    (void)meta_backfill::backfill_entry_meta_from_plain(path, bytes, parsed);
    save_backup::Entry entry;
    save_backup::entry_from_bundle(file_name_of(path), parsed, entry);
    const std::unordered_map<int, std::string> map_names = snapshot_map_names();
    auto it = map_names.find(entry.map_id);
    if (it != map_names.end()) entry.map_name = it->second;
    entry.class_name = save_backup_class_name(entry.class_idx);
    return Outcome<EntryResult>::success(EntryResult{std::move(entry)});
}

std::string save_backup_auto_entry_json(int slot) {
    const auto result = auto_entry(slot);
    if (!result.ok) {
        if (result.error.code == BackupError::kNotFound) {
            return save_backup::api::null_backup_response();
        }
        return result_error(result.error.message.c_str());
    }
    return save_backup::api::entry_response(result.value.entry);
}


std::string save_backup_import_auto_json(int source_slot, int target_slot) {
    if (source_slot < 0 || source_slot > 2 || target_slot < 0 || target_slot > 2) {
        return result_error("bad slot");
    }
    std::string gate_error;
    if (!import_allowed_now(gate_error)) return result_error(gate_error.c_str());
    const std::string external_dir = save_backup::state::snapshot().external_dir;
    if (external_dir.empty()) return result_error("backup dir unavailable");
    const std::string path = auto_backup_path(external_dir, source_slot);
    if (path.empty()) return result_error("backup not found");
    return transaction::import_bundle_file(path, target_slot);
}


}  // namespace save_backup::usecases

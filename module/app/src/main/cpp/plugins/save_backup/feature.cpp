// save_backup feature.cpp —— 存档管理器备份业务层（save-export 底层迁移自 Kotlin SaveBackupStore）。
//
// 职责：.qol_save bundle（原版存档明文 + 模块 sidecar + 元信息）的导出/导入/列表/删除；
// 导出按 checksum 去重；导入为「原版 .dat 重加密 + sidecar 重定槽」两段事务，
// 任一步失败从 backups/.rollback/ 还原。错误信封沿用 op_err（格式 A），
// 错误串与迁移前 HTTP 契约逐字一致。
//
// 明文容器结构（overhaul v1.3.2 逆向，SAVE_LoadData 解密产物）：
//   头 8 字节；块目录自 +8 起，每项 4 字节（u16 offset_rel + u16 len）；
//   块 i 数据 = plain + 8 + u16(plain + 8 + i*4)。
//   块0：+0 u8 slot（SAVE_IsValidInformation 要求 == 请求槽）、+13 u64 save_time、+29 u32 version（≤5）。
//   块1：+0 i16 map_id。
// 全部存档域函数指针经 Game Host 语义接口收口；本层不出现裸地址。

#include "feature.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "backup_files.h"
#include "bundle.h"
#include "contracts/slot_operation.h"
#include "log.h"
#include "rollback.h"

#if defined(QOL_STORE_TEST_SEAMS)
// 测试构建专用：与 store 共用同一个 unlink 注入点；生产分支直接调用 ::unlink。
namespace qol::infra::store {
int unlink_with_test_seam(const char* path);
}  // namespace qol::infra::store
#endif

namespace {

#define SB_LOG(...) SAVE_BACKUP_LOG_INFO(__VA_ARGS__)
#define SB_LOG_WARN(...) SAVE_BACKUP_LOG_WARN(__VA_ARGS__)

void bind_backup_files_source();

constexpr const char* kBackupDirName = "backups";
constexpr const char* kRollbackDirName = ".rollback";
constexpr const char* kBundleSuffix = ".qol_save";

// 列表扫描的本地 DoS 防护上限（不是备份格式或 HTTP 契约）：
// 单个 .qol_save 超过 64 MiB 视为损坏/异常文件跳过；单次扫描累计读取超过 512 MiB 即停止。
constexpr long long kListMaxBundleBytes = 64LL * 1024 * 1024;
constexpr long long kListMaxTotalBytes = 512LL * 1024 * 1024;

std::mutex g_sb_mtx;
std::string g_sb_data_dir;
std::string g_sb_external_dir;
const qol::contracts::QolHostSdk* g_sdk = nullptr;
const qol::contracts::QolGameHost* g_game = nullptr;
const qol::contracts::SaveFactsApi* g_save_facts = nullptr;
const qol::contracts::QolSaveCallPort* g_save_call = nullptr;
qol::contracts::QolOwnerToken g_owner_token = 0;

// 进程内文件级事务序号：临时文件名/trash id 只要求单进程序唯一，不参与持久化语义。
std::atomic<uint64_t> g_file_seq{0};

std::string next_file_suffix() {
    return std::to_string(g_file_seq.fetch_add(1, std::memory_order_relaxed) + 1);
}

// 删除原语：测试构建下与 store 共用同一 unlink 注入点，便于验证删除事务的移回路径。
int unlink_file(const std::string& path) {
#if defined(QOL_STORE_TEST_SEAMS)
    return qol::infra::store::unlink_with_test_seam(path.c_str());
#else
    return ::unlink(path.c_str());
#endif
}

// ---- 槽操作前置门（P3b-2）----
// import/删槽在首次 dat/warehouse 写删之前 acquire 覆盖受影响槽的 operation lease，
// 跨整个操作与回滚持有；完整回滚结束才 release，回滚失败置 referenced 保留证据。
// 生产无并发 gate 持有者时此门惰性（acquire 即成功即释放，行为不变）。
struct SlotOperationGuard {
    qol::contracts::QolSlotOperationLease lease{};
    const qol::contracts::QolSlotOperationApi* api = nullptr;
    bool held = false;
};

bool slot_operation_acquire(int slot, SlotOperationGuard* guard) {
    guard->api = nullptr;
    guard->held = false;
    guard->lease = qol::contracts::QolSlotOperationLease{};
    guard->lease.size = sizeof(guard->lease);
    if (g_sdk == nullptr || g_sdk->slot_operation == nullptr || g_owner_token == 0) return false;
    const auto* api = g_sdk->slot_operation;
    const QolResult result = api->acquire(api->host_ctx, g_owner_token, 1u << slot, &guard->lease);
    if (!qol::contracts::succeeded(result)) return false;
    guard->api = api;
    guard->held = true;
    return true;
}

void slot_operation_release(SlotOperationGuard* guard) {
    if (guard == nullptr || !guard->held) return;
    (void)guard->api->release(guard->api->host_ctx, g_owner_token, &guard->lease);
    guard->held = false;
}

void slot_operation_hold_referenced(SlotOperationGuard* guard) {
    // 回滚失败保留证据：置 referenced 后 owner 停用也不释放 lease。
    if (guard == nullptr || !guard->held) return;
    (void)guard->api->set_referenced(guard->api->host_ctx, g_owner_token, &guard->lease, 1);
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
    const QolResult result = guard.api->import_container(guard.api->host_ctx, g_owner_token,
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
        guard.api->delete_container(guard.api->host_ctx, g_owner_token, &guard.lease, slot));
}

std::string result_ok() {
    char buffer[64] = {};
    uint32_t size = 0;
    if (g_sdk != nullptr && g_sdk->result.write_ok != nullptr &&
        qol::contracts::succeeded(g_sdk->result.write_ok(buffer, sizeof(buffer), &size))) {
        return std::string(buffer, size);
    }
    return "{\"ok\":false,\"error\":\"result unavailable\"}";
}

std::string result_error(const char* message) {
    char buffer[256] = {};
    uint32_t size = 0;
    if (g_sdk != nullptr && g_sdk->result.write_error != nullptr && message != nullptr &&
        qol::contracts::succeeded(g_sdk->result.write_error(message, buffer, sizeof(buffer), &size))) {
        return std::string(buffer, size);
    }
    return "{\"ok\":false,\"error\":\"result unavailable\"}";
}

// 导入门禁：仅在主菜单（未加载任何槽，current_slot == -1）允许覆盖存档；游戏运行中
// 导入会与已加载槽的结构/文件冲突。current_slot 读取失败按不可用处理（fail-closed）。
bool import_allowed_now(std::string& err) {
    if (g_save_facts == nullptr || g_save_facts->current_slot == nullptr) {
        err = "save state unavailable";
        return false;
    }
    int32_t current_slot = -1;
    const QolResult result = g_save_facts->current_slot(&current_slot);
    // 主菜单（未加载）时 Adapter 返回 NotReady 且 out=-1（game/adapter_ports/save_port.cpp:104-108），
    // 因此 NotReady 不能单独判为不可用；其余错误码 fail-closed。
    if (!qol::contracts::succeeded(result) && result.code != kQolErrNotReady) {
        err = "save state unavailable";
        return false;
    }
    if (current_slot >= 0) {
        err = "import requires main menu";
        return false;
    }
    return true;
}

const char* warehouse_state_name(qol::contracts::QolTargetFeatureState state) {
    switch (state) {
        case qol::contracts::QolTargetFeatureState::kUnknown: return "unknown";
        case qol::contracts::QolTargetFeatureState::kUnsupported: return "unsupported";
        case qol::contracts::QolTargetFeatureState::kAvailable: return "available";
        case qol::contracts::QolTargetFeatureState::kUnavailable: return "unavailable";
    }
    return "unknown";
}

enum class ContainerStatus {
    kOk,
    kNotFound,
    kCorrupt,
    kOther,
};

ContainerStatus classify_container_result(const QolResult& result) {
    if (qol::contracts::succeeded(result)) return ContainerStatus::kOk;
    if (result.code == kQolErrNotFound) return ContainerStatus::kNotFound;
    if (result.code == kQolErrInternal) return ContainerStatus::kCorrupt;
    return ContainerStatus::kOther;
}

ContainerStatus store_read_container(int slot, std::vector<uint8_t>& out) {
    out.clear();
    if (g_sdk == nullptr || g_sdk->store.read_container == nullptr) {
        return ContainerStatus::kOther;
    }
    uint32_t size = 0;
    QolResult result = g_sdk->store.read_container(slot, nullptr, 0, &size);
    if (qol::contracts::succeeded(result)) return ContainerStatus::kOk;
    if (result.code != kQolErrInvalidArgument || size == 0) {
        return classify_container_result(result);
    }
    out.resize(size);
    result = g_sdk->store.read_container(slot, out.data(), size, &size);
    if (!qol::contracts::succeeded(result)) {
        out.clear();
        return classify_container_result(result);
    }
    out.resize(size);
    return ContainerStatus::kOk;
}

bool store_import_container(int slot, const uint8_t* data, size_t size) {
    return g_sdk != nullptr && g_sdk->store.import_container != nullptr &&
           size <= UINT32_MAX &&
           qol::contracts::succeeded(g_sdk->store.import_container(
               slot, data, static_cast<uint32_t>(size)));
}

ContainerStatus store_delete_slot(int slot) {
    if (g_sdk == nullptr || g_sdk->store.delete_slot == nullptr) return ContainerStatus::kOther;
    return classify_container_result(g_sdk->store.delete_slot(slot));
}

bool save_api_ready() {
    return g_save_facts != nullptr && g_save_facts->load_data != nullptr &&
           g_save_call != nullptr && g_save_call->write.free_data != nullptr;
}

bool save_encrypt_available() {
    return g_save_call != nullptr && g_save_call->write.encrypt != nullptr;
}

const char* save_hub_key() {
    if (g_save_facts == nullptr || g_save_facts->hub_key == nullptr) return nullptr;
    const char* key = nullptr;
    if (!qol::contracts::succeeded(g_save_facts->hub_key(&key))) return nullptr;
    return key;
}

int save_encrypt_process(void* data, int len, int mode, const char* key) {
    if (!save_encrypt_available() || data == nullptr || len < 0 || key == nullptr) return 0;
    int result = 0;
    if (!qol::contracts::succeeded(
            g_save_call->write.encrypt(data, len, mode, key, &result))) {
        return 0;
    }
    return result;
}

void save_mark_stale() {
    if (g_save_call != nullptr && g_save_call->write.mark_stale != nullptr) {
        (void)g_save_call->write.mark_stale();
    }
}

// ---- 地图名表（Kotlin 启动期从 MAPINFOBASE 下发：map_id → 中文名） ----
// 解析失败/空 JSON 视为空表（entry_json 输出空 map_name，UI 显示「未知地图」）。
// 全局单例：g_sb_mtx 保护，启动期单线程写入、之后只读。
std::unordered_map<int, std::string> g_map_names;

// 游戏主线程槽元数据快照：HTTP/绘制线程只读，避免跨线程调用 save.facts 游戏读取函数。
struct SlotMetaCache {
    bool valid = false;
    int hero_level = -1;
    int hero_index = -1;
    int class_idx = -1;
};
SlotMetaCache g_slot_meta[3];

void set_map_names_locked(const std::string& json) {
    g_map_names.clear();
    if (json.empty()) return;
    // 极简扁平解析：仅识别 "数字": "字符串" 对（Kotlin JSONObject.toString 输出）。
    // 字符级扫描足够（n ≤ 416 条记录，体积 < 32KB），不引入第三方依赖。
    size_t i = 0;
    const size_t n = json.size();
    int parsed = 0;
    while (i < n) {
        // 跳过空白/冒号/逗号/{/}，定位下一个 key。
        while (i < n && (json[i] == '{' || json[i] == '}' || json[i] == ',' ||
                         json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) {
            ++i;
        }
        if (i >= n) break;
        // 期望 key 为裸数字（Kotlin Map.toString 输出形如 {0="name", 30="..."}，需兼容）。
        // 但 API 注入统一通过 JSONObject → toString 产出 {"0":"...", ...}，对引号形式解析：
        size_t key_start = i;
        if (json[i] == '"') ++key_start;  // 跳过 key 起始引号（JSONObject.toString 输出带引号 key）
        i = key_start;
        while (i < n && json[i] != ':' && json[i] != ',') ++i;
        if (i >= n || json[i] != ':') break;
        std::string key = json.substr(key_start, i - key_start);
        // 去尾部引号/空白。
        while (!key.empty() && (key.back() == '"' || key.back() == ' ' || key.back() == '\t')) key.pop_back();
        ++i;
        // 跳过空白。
        while (i < n && (json[i] == ' ' || json[i] == '\t')) ++i;
        // value：双引号字符串或单引号/裸串；取终止前的全部内容作为 map_name。
        std::string value;
        if (i < n && json[i] == '"') {
            ++i;
            size_t v0 = i;
            while (i < n && json[i] != '"') {
                // 不展开转义（map_name 为纯中文/ASCII，不出现 \"）。
                ++i;
            }
            value = json.substr(v0, i - v0);
            if (i < n) ++i;
        } else {
            size_t v0 = i;
            while (i < n && json[i] != ',' && json[i] != '}') ++i;
            value = json.substr(v0, i - v0);
            // 去尾部空白。
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
        }
        int map_id = -1;
        try {
            map_id = std::stoi(key);
        } catch (...) {
            map_id = -1;
        }
        if (map_id >= 0 && !value.empty()) {
            g_map_names[map_id] = value;
            ++parsed;
        }
    }
    SB_LOG("save backup map_names loaded: %d entries (json bytes=%zu)", parsed, json.size());
}

// ---- 基础文件原语 ----

bool ensure_dir(const std::string& path) {
    struct stat st{};
    // lstat：已存在的 symlink 不是目录，不能当作可写目录使用。
    if (::lstat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    if (errno != ENOENT) return false;
    return ::mkdir(path.c_str(), 0755) == 0;
}

bool file_exists(const std::string& path) {
    struct stat st{};
    // symlink 视为不存在，避免通过链接读到/删到目标目录之外。
    return ::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool read_file_bytes(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        ::close(fd);
        return false;
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(static_cast<size_t>(st.st_size));
    uint8_t buf[8192];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        bytes.insert(bytes.end(), buf, buf + n);
    }
    if (::close(fd) != 0) return false;
    out.swap(bytes);
    return true;
}

// 原子写：写 <path>.tmp-<id> 再 rename；临时文件用 O_EXCL 创建，避免复用残留路径。
bool write_file_bytes_atomic(const std::string& path, const uint8_t* data, size_t size) {
    const std::string tmp = path + ".tmp-" + next_file_suffix();
    const int fd = ::open(tmp.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = true;
    size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::write(fd, data + offset, size - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (written == 0) {
            ok = false;
            break;
        }
        offset += static_cast<size_t>(written);
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
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

// 目标必须不存在（O_EXCL）；失败时清理目标，供调用方重试。
bool copy_file(const std::string& from, const std::string& to) {
    const int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (in < 0) return false;
    struct stat st{};
    if (::fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(in);
        return false;
    }
    const int out = ::open(to.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (out < 0) {
        ::close(in);
        return false;
    }
    uint8_t buf[8192];
    bool ok = true;
    for (;;) {
        const ssize_t n = ::read(in, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (n == 0) break;
        size_t offset = 0;
        while (offset < static_cast<size_t>(n)) {
            const ssize_t written = ::write(out, buf + offset, static_cast<size_t>(n) - offset);
            if (written < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            if (written == 0) {
                ok = false;
                break;
            }
            offset += static_cast<size_t>(written);
        }
        if (!ok) break;
    }
    if (::close(in) != 0) ok = false;
    if (::close(out) != 0) ok = false;
    if (!ok) {
        ::unlink(to.c_str());
        return false;
    }
    return true;
}

// 回滚还原：先复制到同目录临时文件，再 rename 覆盖目标（目标已存在时 copy_file 不能直写）。
bool copy_file_replacing(const std::string& from, const std::string& to) {
    const std::string tmp = to + ".restore-" + next_file_suffix();
    if (!copy_file(from, tmp)) return false;
    if (::rename(tmp.c_str(), to.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool remove_file(const std::string& path) {
    if (path.empty()) return true;
    return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

std::string file_name_of(const std::string& path) {
    const size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool ends_with(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// ---- 路径与目录 ----

// 备份目录 <external>/backups/（不存在则建）；未初始化/建目录失败返回 false。
bool backup_dir_at(const std::string& external_dir, std::string& out) {
    if (external_dir.empty()) {
        SB_LOG("save backup unavailable: store not initialized");
        return false;
    }
    out = external_dir + "/" + kBackupDirName;
    if (!ensure_dir(out)) {
        SB_LOG("save backup unavailable: mkdir failed: %s", out.c_str());
        return false;
    }
    return true;
}

bool backup_dir(std::string& out) { return backup_dir_at(g_sb_external_dir, out); }

// sidecar 容器操作（读 primary/校验/回退、重定槽双写、删除）统一经 infra/store；
// 本文件不再自行拼接 saves/ 路径或直接读写 sidecar 文件。

// 文件名全匹配 `save\d*\.dat`（与迁移前 Kotlin Regex 一致）。
bool is_save_dat_name(const std::string& n) {
    if (n.size() < 8) return false;
    if (n.compare(0, 4, "save") != 0) return false;
    if (n.compare(n.size() - 4, 4, ".dat") != 0) return false;
    for (size_t i = 4; i + 4 < n.size(); ++i) {
        if (n[i] < '0' || n[i] > '9') return false;
    }
    return true;
}

bool dir_contains_save_dat(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return false;
    bool found = false;
    while (dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        struct stat st{};
        const std::string full = dir + "/" + name;
        if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (is_save_dat_name(name)) {
            found = true;
            break;
        }
    }
    ::closedir(d);
    return found;
}

// 列出目录下全部 .qol_save 文件名；目录不可读返回 false。
bool list_bundle_files(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return false;
    while (dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        const std::string full = dir + "/" + name;
        struct stat st{};
        if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (ends_with(name, kBundleSuffix)) out.push_back(name);
    }
    ::closedir(d);
    return true;
}

// 按 bundle 内部 checksum 定位备份：目录下全部 `*.qol_save` 均为候选，逐个读入并完整解析
//（parse_bundle 含逐段边界与 CRC32 校验）后比对内部值；解析失败或非 bundle 的候选跳过。
// 文件名不参与匹配（`bundle_matches_checksum` 仅在旧备份 metaJson 缺失 checksum 字段时
// 回退文件名后缀）。同 checksum 的多个候选返回目录序中的第一个；未命中返回空串。
std::string find_by_checksum(const std::string& dir, const std::string& checksum) {
    std::vector<std::string> names;
    if (!list_bundle_files(dir, names)) return std::string();
    for (const std::string& name : names) {
        const std::string full = dir + "/" + name;
        std::vector<uint8_t> bytes;
        save_backup::ParsedBundle parsed;
        if (!read_file_bytes(full, bytes) ||
            !save_backup::parse_bundle(bytes.data(), bytes.size(), parsed)) {
            continue;
        }
        if (save_backup::bundle_matches_checksum(name, parsed, checksum)) return full;
    }
    return std::string();
}

// ---- 时间 ----

long long now_ms() {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// 事务 id：毫秒时间戳 + 进程内序号（十六进制），用于 trash 目录与导入回滚副本命名。
std::string next_transaction_id() {
    const uint64_t seq = g_file_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    char buffer[48] = {};
    std::snprintf(buffer, sizeof(buffer), "%llx%llx",
                  static_cast<unsigned long long>(now_ms()),
                  static_cast<unsigned long long>(seq));
    return buffer;
}

// 本地时间 yyyyMMdd-HHmmss（+NUL 共 16 字节）。
bool local_stamp(long long ms, char out[16]) {
    const time_t sec = static_cast<time_t>(ms / 1000);
    tm tmv{};
    if (::localtime_r(&sec, &tmv) == nullptr) return false;
    return ::strftime(out, 16, "%Y%m%d-%H%M%S", &tmv) == 15;
}

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
void read_hero_meta(int32_t slot, int& hero_level, int& hero_index, int& class_idx) {
    hero_level = -1;
    hero_index = -1;
    class_idx = -1;
    if (slot < 0 || slot > 2) return;
    std::lock_guard<std::mutex> lk(g_sb_mtx);
    if (!g_slot_meta[slot].valid) return;
    hero_level = g_slot_meta[slot].hero_level;
    hero_index = g_slot_meta[slot].hero_index;
    class_idx = g_slot_meta[slot].class_idx;
}

// 读槽明文并归一化：SAVE_LoadData 解密 → 拷出并 MEM_Free → 块0 slot 写 0xFF。
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
    const QolResult load_result = g_save_facts->load_data(slot, &buf, &len);
    // 明文头 8 字节 + 块表 offset(+8/+12)：不足 14 字节无法定位块 0/块 1。
    if (!qol::contracts::succeeded(load_result) || buf == nullptr || len < 14) {
        if (buf != nullptr) (void)g_save_call->write.free_data(buf);
        if (load_result.code == kQolErrNotReady) err = "symbol not resolved";
        else err = "load failed";
        return false;
    }
    // 明文拷出后立即归还游戏堆，后续处理与游戏内存解耦。
    plain.assign(static_cast<const uint8_t*>(buf), static_cast<const uint8_t*>(buf) + len);
    (void)g_save_call->write.free_data(buf);

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
    return true;
}

// 原版明文加密到目标槽：块0 slot 字节写目标槽 → ENCRYPT_Process2(mode=0)，密文 len+3 字节。
bool encrypt_plain_to_slot(int32_t slot, const std::vector<uint8_t>& plain,
                           std::vector<uint8_t>& cipher) {
    if (!save_encrypt_available()) return false;
    if (plain.size() < 14) return false;
    const size_t block0 = 8 + rd_u16(plain.data() + 8);
    if (block0 + 1 > plain.size()) return false;

    std::vector<uint8_t> buf = plain;
    buf[block0] = static_cast<uint8_t>(slot);
    // ENCRYPT_Process2 加密就地追加 3 字节，需 len+3 容量。
    buf.resize(plain.size() + 3, 0);
    const char* key = save_hub_key();
    if (key == nullptr) return false;
    if (save_encrypt_process(buf.data(), static_cast<int>(plain.size()), 0, key) != 1) {
        return false;
    }
    cipher.swap(buf);
    return true;
}

// wh4 容器与 saveN.dat 同方案；mode=1 为解密，返回 1 表示双校验和通过（本机密钥可解）。
int warehouse_decrypt_mode1(void* buf, int len, const char* key) {
    return save_encrypt_process(buf, len, 1, key);
}

// mode=0 为加密，就地写入 len+3 字节（需 len+3 容量），返回 1 成功。
int warehouse_encrypt_mode0(void* buf, int len, const char* key) {
    return save_encrypt_process(buf, len, 0, key);
}

// 判断 bundle 内的 wh4 容器在本机是否可用（可解密）。用于导入时拦截「换设备后解不开」的 wh4：
// 这类文件落盘会导致进档时 block3 校验失败 → SAVE_Load 提前返回 → SIGSEGV。
// 加解密函数缺失时传 nullptr 回调解密，纯逻辑层放行（无法判定时不拦）。
bool warehouse_blob_usable_on_this_device(const uint8_t* data, size_t len, const char* key) {
    const save_backup::WarehouseDecryptFn decrypt =
        save_encrypt_available() ? &warehouse_decrypt_mode1 : nullptr;
    return save_backup::warehouse_blob_decryptable(data, len, key, decrypt);
}

// v3 flags=1 明文项：补目标 slot + 用本机密钥重加密 + 往返自检。
// 加解密函数缺失时传 nullptr，纯逻辑层返回 kNoKey（宁缺勿错，不写盘）。
save_backup::WarehouseReencryptStatus warehouse_reencrypt_on_device(
    const save_backup::WarehouseFile& wf, int slot, const char* key,
    std::vector<uint8_t>& out_cipher) {
    const bool available = save_encrypt_available();
    const save_backup::WarehouseEncryptFn encrypt =
        available ? &warehouse_encrypt_mode0 : nullptr;
    const save_backup::WarehouseDecryptFn decrypt =
        available ? &warehouse_decrypt_mode1 : nullptr;
    return save_backup::warehouse_reencrypt_for_slot(wf.data, slot, key, encrypt, decrypt,
                                                     out_cipher);
}

// 定位原版存档目录：优先 dataDir 下含 save*.dat 的子目录；否则 dataDir/hex(MD5(key)) 并建目录。
// 返回 0=成功、1=未找到（含 key 不可得）、2=建目录失败。
int locate_save_directory_at(const std::string& data_dir, std::string& out) {
    if (!data_dir.empty()) {
        DIR* d = ::opendir(data_dir.c_str());
        if (d != nullptr) {
            while (dirent* ent = ::readdir(d)) {
                const std::string name = ent->d_name;
                if (name == "." || name == "..") continue;
                const std::string sub = data_dir + "/" + name;
                struct stat st{};
                if (::stat(sub.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                if (dir_contains_save_dat(sub)) {
                    ::closedir(d);
                    out = sub;
                    return 0;
                }
            }
            ::closedir(d);
        }
    }
    const char* key = save_hub_key();
    if (key == nullptr) return 1;
    const std::string dir_name = save_backup::md5_hex(reinterpret_cast<const uint8_t*>(key),
                                                      std::strlen(key));
    const std::string dir = data_dir + "/" + dir_name;
    if (!ensure_dir(dir)) return 2;
    SB_LOG("save backup import: save directory fallback %s", dir.c_str());
    out = dir;
    return 0;
}

int locate_save_directory(std::string& out) {
    return locate_save_directory_at(g_sb_data_dir, out);
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

}  // namespace

void save_backup_bind_context(const qol::contracts::QolHostSdk* sdk,
                              const qol::contracts::SaveFactsApi* save_facts,
                              const qol::contracts::QolSaveCallPort* save_call,
                              const qol::contracts::QolGameHost* game,
                              qol::contracts::QolOwnerToken owner_token) {
    g_sdk = sdk;
    g_game = game;
    g_save_facts = save_facts;
    g_save_call = save_call;
    g_owner_token = owner_token;
}

const qol::contracts::QolHostSdk* save_backup_host_sdk() { return g_sdk; }

const qol::contracts::QolGameHost* save_backup_game_host() { return g_game; }

const qol::contracts::SaveFactsApi* save_backup_save_facts() { return g_save_facts; }

const qol::contracts::QolSaveCallPort* save_backup_save_call() { return g_save_call; }

std::string save_backup_result_ok() { return result_ok(); }

std::string save_backup_result_error(const char* message) { return result_error(message); }

void save_backup_init(const char* data_dir, const char* external_files_dir) {
    {
        std::lock_guard<std::mutex> lk(g_sb_mtx);
        g_sb_data_dir = data_dir != nullptr ? data_dir : "";
        g_sb_external_dir = external_files_dir != nullptr ? external_files_dir : "";
    }
    bind_backup_files_source();
    // 启动回滚优先：把上次删槽/删 sidecar 中途死亡留下的 trash 文件移回原位，不再继续删除。
    save_backup_recover_deletes();
}

void save_backup_recover_deletes() {
    std::lock_guard<std::mutex> lk(g_sb_mtx);
    if (g_sb_external_dir.empty()) return;
    const std::string backups = g_sb_external_dir + "/" + kBackupDirName;
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
    recover_orphan_sidecar_trash(g_sb_external_dir + "/saves");
}

// 游戏主线程刷新槽元数据快照；HTTP/绘制线程只读缓存，不直接调用 save.facts 游戏读取函数。
void save_backup_refresh_slot_meta_cache() {
    if (g_save_facts == nullptr || g_save_facts->slot_exists == nullptr) return;
    SlotMetaCache fresh[3];
    for (int slot = 0; slot < 3; ++slot) {
        fresh[slot].valid = true;
        int32_t exists = 0;
        if (!qol::contracts::succeeded(g_save_facts->slot_exists(slot, &exists)) || exists == 0) {
            continue;
        }
        int32_t value = 0;
        if (g_save_facts->slot_hero_index != nullptr &&
            qol::contracts::succeeded(g_save_facts->slot_hero_index(slot, &value))) {
            fresh[slot].hero_index = value;
        }
        if (g_save_facts->hero_level != nullptr &&
            qol::contracts::succeeded(g_save_facts->hero_level(slot, &value))) {
            fresh[slot].hero_level = value;
        }
        // 职业索引（0-5）；type==2 装饰物该字段非职业，但本路径取的是主角 hero 结构。
        if (g_save_facts->hero_class != nullptr &&
            qol::contracts::succeeded(g_save_facts->hero_class(slot, &value))) {
            fresh[slot].class_idx = value;
        }
    }
    std::lock_guard<std::mutex> lk(g_sb_mtx);
    for (int slot = 0; slot < 3; ++slot) g_slot_meta[slot] = fresh[slot];
}

// 游戏删档回调（SaveSlot_Delete）PtrHook wrapper 的同步体：游戏内存档面板删档确认后，
// 同步删除该槽模块 sidecar（slot-{n}.qol-save + .last-good）。
// 执行上下文：游戏主线程（UIPopupMsg_ButtonOKExe → blr），slot 由 wrapper 传入。约束：
//  - 不取 g_virtual_bag_mtx、不走 JNI/Kotlin（避免跨层锁序与主线程阻塞）；
//  - 不在本函数内再调 SAVE_CreateSaveSlot（原函数已调）；
//  - 短取 g_sb_mtx，与 save_backup 导入/导出串行（锁序 g_sb_mtx → store）。
void save_backup_slot_deleted_sync(int32_t slot) {
    if (slot < 0 || slot > 2) return;
    if (g_sb_external_dir.empty()) {
        SB_LOG_WARN("slot delete hook: external dir unavailable, sidecar untouched slot=%d", slot);
        save_mark_stale();
        return;
    }
    std::lock_guard<std::mutex> lk(g_sb_mtx);
    // dat 已被游戏原函数删除，这里只处理 sidecar；两阶段（trash → unlink）保证失败可退回。
    SlotOperationGuard guard;
    if (!slot_operation_acquire(slot, &guard)) {
        SB_LOG_WARN("slot delete hook: slot operation unavailable slot=%d", slot);
        save_mark_stale();
        return;
    }
    const std::string saves_dir = g_sb_external_dir + "/saves";
    if (!ensure_dir(saves_dir)) {
        SB_LOG_WARN("slot delete hook: saves dir unavailable slot=%d", slot);
        slot_operation_release(&guard);
        save_mark_stale();
        return;
    }
    std::string backups_dir;
    if (!backup_dir(backups_dir)) {
        SB_LOG_WARN("slot delete hook: backup dir unavailable slot=%d", slot);
        slot_operation_release(&guard);
        save_mark_stale();
        return;
    }
    const std::string id = next_transaction_id();
    const std::string trash_dir = saves_dir + "/.sb-trash-" + id;
    const std::string index_path = backups_dir + "/.trash-" + id + ".index";
    std::string save_dir;
    std::vector<std::string> wh_names;
    if (locate_save_directory_at(g_sb_data_dir, save_dir) == 0) {
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
        save_mark_stale();
        return;
    }
    if (data_trash_needed && ::mkdir(data_trash.c_str(), 0755) != 0) {
        (void)::rmdir(trash_dir.c_str());
        SB_LOG_WARN("slot delete hook: data trash mkdir failed slot=%d", slot);
        slot_operation_release(&guard);
        save_mark_stale();
        return;
    }
    if (!write_trash_index(index_path, entries)) {
        (void)::rmdir(trash_dir.c_str());
        if (data_trash_needed) (void)::rmdir(data_trash.c_str());
        SB_LOG_WARN("slot delete hook: index write failed slot=%d", slot);
        slot_operation_release(&guard);
        save_mark_stale();
        return;
    }
    if (guard.api->abi.minor < 1 || guard.api->delete_container_to_trash == nullptr) {
        SB_LOG_WARN("slot delete hook: trash api unavailable slot=%d", slot);
        (void)restore_trash_entries(entries);
        remove_file(index_path);
        cleanup_trash_dirs();
        slot_operation_release(&guard);
        save_mark_stale();
        return;
    }
    const QolResult status = guard.api->delete_container_to_trash(
        guard.api->host_ctx, g_owner_token, &guard.lease, slot, trash_dir.c_str());
    if (!qol::contracts::succeeded(status)) {
        (void)restore_trash_entries(entries);
        remove_file(index_path);
        cleanup_trash_dirs();
        SB_LOG_WARN("slot delete hook: sidecar move failed slot=%d", slot);
        slot_operation_release(&guard);
        save_mark_stale();
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
            save_mark_stale();
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
        save_mark_stale();
        return;
    }
    remove_file(index_path);
    cleanup_trash_dirs();
    slot_operation_release(&guard);
    SB_LOG("slot delete hook: removed sidecar/wh4 slot=%d wh4=%zu", slot, wh_names.size());
    // 游戏删档后槽文件已变：槽区结构不再代表磁盘状态，下一次读取必须重载。
    save_mark_stale();
}

void save_backup_set_map_names(const char* json) {
    std::lock_guard<std::mutex> lk(g_sb_mtx);
    set_map_names_locked(json != nullptr ? std::string(json) : std::string());
}

std::string save_backup_map_name(int map_id) {
    // g_map_names 由 g_sb_mtx 保护：启动期单次写入，此后只读；按值返回避免调用方
    // 持有跨锁引用。未命中（表未下发/map_id 未知）返回空串，由调用方走「地图N」兜底。
    std::lock_guard<std::mutex> lk(g_sb_mtx);
    auto it = g_map_names.find(map_id);
    return it != g_map_names.end() ? it->second : std::string();
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
    std::string data_dir;
    std::string external_dir;
    {
        std::lock_guard<std::mutex> lk(g_sb_mtx);
        data_dir = g_sb_data_dir;
        external_dir = g_sb_external_dir;
    }
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
    const std::string bundle_path = dir + "/" + stem + ".qol_save";
    const std::string fingerprint_path = dir + "/" + stem + ".fingerprint";
    const std::string current = autobackup_fingerprint(slot, data_dir, external_dir);
    std::vector<uint8_t> previous;
    if (file_exists(bundle_path) && read_file_bytes(fingerprint_path, previous) &&
        std::string(previous.begin(), previous.end()) == current) {
        return;
    }
    std::string error;
    if (!save_backup_export_slot_to_dir(slot, dir.c_str(), (stem + ".qol_save").c_str(), &error)) {
        SB_LOG_WARN("autobackup slot=%d export failed: %s", slot, error.c_str());
        return;
    }
    if (!write_file_bytes_atomic(fingerprint_path,
                                 reinterpret_cast<const uint8_t*>(current.data()),
                                 current.size())) {
        SB_LOG_WARN("autobackup slot=%d fingerprint write failed", slot);
    }
}

const char* save_backup_class_name(int class_idx) {
    // 与游戏 CHARCLASSBASE 文本逐条核对（apk/static-data/tables/CHARCLASSBASE.json
    // + text/zh-Hans.json，0..5）：不使用 Kotlin 下发通道，避免新增 JNI/HTTP 依赖；
    // 6 个职业为游戏固定枚举，硬编码稳定。表为常量、无锁。
    static const char* const kNames[6] = {"黑暗骑士", "忍者", "黑魔导", "祭司",
                                          "暗影猎手", "狂战士"};
    if (class_idx < 0 || class_idx > 5) return "";
    return kNames[class_idx];
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
    switch (store_read_container(slot, out.bytes)) {
        case ContainerStatus::kOk:
            out.state = qol::features::save_backup::CollectedSidecar::State::kPresent;
            return true;
        case ContainerStatus::kNotFound:
            out.state = qol::features::save_backup::CollectedSidecar::State::kAbsent;
            out.bytes.clear();
            return true;
        default:
            err = "module container unreadable";
            return false;
    }
}

bool bridge_locate_save_directory(std::string& out) {
    std::string data_dir;
    {
        std::lock_guard<std::mutex> lk(g_sb_mtx);
        data_dir = g_sb_data_dir;
    }
    return locate_save_directory_at(data_dir, out) == 0;
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
    &bridge_collect_dat,       &bridge_collect_sidecar, &bridge_locate_save_directory,
    &bridge_read_file,         &bridge_encrypt_available, &bridge_hub_key,
    &bridge_encrypt_process,
};

void bind_backup_files_source() { qol::features::save_backup::backup_files_bind_source(kBackupFilesSource); }

}  // namespace

static SbDirs snapshot_dirs() {
    std::lock_guard<std::mutex> lk(g_sb_mtx);
    return SbDirs{g_sb_data_dir, g_sb_external_dir};
}

static std::unordered_map<int, std::string> snapshot_map_names() {
    std::lock_guard<std::mutex> lk(g_sb_mtx);
    return g_map_names;
}

std::string save_backup_list_json() {
    std::string s = "{\"ok\":true,\"backups\":[";
    const SbDirs dirs = snapshot_dirs();
    const std::unordered_map<int, std::string> map_names = snapshot_map_names();
    std::string dir;
    if (!backup_dir_at(dirs.external, dir)) return s + "]}";
    std::vector<std::string> names;
    if (!list_bundle_files(dir, names)) return s + "]}";
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
    bool first = true;
    for (const save_backup::Entry& entry : entries) {
        if (!first) s += ",";
        s += save_backup::entry_json(entry);
        first = false;
    }
    return s + "]}";
}

struct BuiltExport {
    std::vector<uint8_t> bundle;
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
    out.export_time_ms = now_ms();
    out.original_sha256 = save_backup::sha256_hex(snapshot.dat.plain, {});
    out.module_sha256 = snapshot.module.bytes.empty()
                            ? std::string()
                            : save_backup::sha256_hex(snapshot.module.bytes, {});
    out.checksum = save_backup::compute_checksum(save_backup::kBundleVersion, snapshot.dat.plain,
                                                 snapshot.module.bytes, warehouse_blob);
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
    save_backup::build_bundle(slot, out.export_time_ms, snapshot.dat.plain, snapshot.module.bytes,
                              warehouse_blob, meta_json, out.bundle);
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
                                              parsed.module, parsed.warehouse_blob) == checksum;
            if (checksum_ok) {
                save_backup::Entry entry;
                save_backup::entry_from_bundle(file_name_of(existing), parsed, entry);
                auto dit = map_names.find(entry.map_id);
                if (dit != map_names.end()) entry.map_name = dit->second;
                entry.class_name = save_backup_class_name(entry.class_idx);
                SB_LOG("save backup deduplicated: %s (checksum=%s)", entry.file_name.c_str(),
                       checksum.c_str());
                return "{\"ok\":true,\"backup\":" + save_backup::entry_json(entry) +
                       ",\"deduplicated\":true}";
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
    return "{\"ok\":true,\"backup\":" + save_backup::entry_json(entry) + "}";
}

std::string save_backup_export_json(int slot) {
    if (slot < 0 || slot > 2) return result_error("bad slot");
    // 导出同样覆盖槽文件读取/备份写入：持 lease 与导入/删槽串行；不持 g_sb_mtx 做 IO。
    SlotOperationGuard guard;
    if (!slot_operation_acquire(slot, &guard)) return result_error("slot operation unavailable");
    const std::string response = export_json_with_lease(slot);
    slot_operation_release(&guard);
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
    // 参数校验、bundle 定位与解析持锁；gate/容器/文件 IO 段在锁外执行——同槽串行由
    // 槽操作门保证，不以跨插件持锁保证串行。
    std::unique_lock<std::mutex> lk(g_sb_mtx);
    const std::string ck = checksum != nullptr ? checksum : "";
    if (!save_backup::valid_checksum(ck)) return result_error("bad checksum");
    if (slot < 0 || slot > 2) return result_error("bad slot");
    // 导入会覆盖槽文件，必须先确认当前未加载任何槽；导出不设此限制。
    std::string gate_error;
    if (!import_allowed_now(gate_error)) return result_error(gate_error.c_str());
    std::string dir;
    if (!backup_dir(dir)) return result_error("backup dir unavailable");
    const std::string path = find_by_checksum(dir, ck);
    if (path.empty()) return result_error("backup not found");
    const std::string name = file_name_of(path);

    std::vector<uint8_t> bytes;
    save_backup::ParsedBundle parsed;
    if (!read_file_bytes(path, bytes) ||
        !save_backup::parse_bundle(bytes.data(), bytes.size(), parsed)) {
        return result_error("backup unreadable or corrupt");
    }
    // checksum 必须覆盖全部可还原内容（v3 含 warehouse_blob），不能只信 meta 声明。
    std::string meta_checksum;
    if (!save_backup::json_find_string(parsed.meta_json, "checksum", meta_checksum) ||
        !save_backup::valid_checksum(meta_checksum) ||
        meta_checksum != save_backup::compute_checksum(parsed.bundle_version, parsed.orig_plain,
                                                       parsed.module, parsed.warehouse_blob)) {
        return result_error("backup checksum mismatch");
    }
    // module_state：新备份显式 0/1；旧备份无字段时按 module 是否为空推断。
    long long module_state_raw = -1;
    const bool has_module_state =
        save_backup::json_find_int(parsed.meta_json, "module_state", module_state_raw);
    if (has_module_state && module_state_raw != 0 && module_state_raw != 1) {
        return result_error("backup unreadable or corrupt");
    }
    const bool module_present =
        has_module_state ? module_state_raw == 1 : !parsed.module.empty();
    if (module_present != !parsed.module.empty()) {
        return result_error("backup unreadable or corrupt");
    }
    // shared 状态（map 名表/目录根）在锁外只读；此处起进入锁外段。
    lk.unlock();

    // 前置门：首次写删之前 acquire；parsed.module 为空也必须 acquire（dat 写入本身
    // 就是不可逆变更）。
    SlotOperationGuard guard;
    if (!slot_operation_acquire(slot, &guard)) return result_error("slot operation unavailable");

    // 1) 原版明文加密到目标槽（块0 slot 字节改写目标槽并追加校验尾）。
    std::vector<uint8_t> cipher;
    if (!encrypt_plain_to_slot(slot, parsed.orig_plain, cipher) || cipher.empty()) {
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

    // 2a) 仓库写回预校验/预加密（必须在写任何文件之前，保证 .rollback/ 与现存文件不被触碰）：
    //     fail-closed —— 任一项失败整次导入失败。
    //       flags=0（密文 wh4）：必须能用本机密钥解密（v2 旧备份仅支持同设备导入；
    //                            导入后再导出 v3 才变为可跨设备）。
    //       flags=1（明文 wh4）：补 slot + 重算 +0x38 完整性校验 + 重加密 + 往返自检。
    //     变体能力门禁 unsupported → 整步跳过（保持既有行为，不校验也不写）。
    std::vector<std::vector<uint8_t>> wh_payloads;
    qol::contracts::QolTargetFeatureState target_state =
        qol::contracts::QolTargetFeatureState::kUnknown;
    bool wh_write = false;
    if (parsed.has_warehouse) {
        if (g_game != nullptr && g_game->target.warehouse_companion_file_state != nullptr) {
            (void)g_game->target.warehouse_companion_file_state(&target_state);
        }
        if (target_state == qol::contracts::QolTargetFeatureState::kUnsupported) {
            SB_LOG("save backup import: slot%d warehouse companion unsupported (%s), skip %zu file(s)",
                   slot, warehouse_state_name(target_state), parsed.warehouse.size());
        } else {
            wh_write = true;
            const char* wh_key = save_hub_key();
            wh_payloads.resize(parsed.warehouse.size());
            for (size_t i = 0; i < parsed.warehouse.size(); ++i) {
                const save_backup::WarehouseFile& wf = parsed.warehouse[i];
                if ((wf.flags & save_backup::kWarehouseFlagPlain) != 0) {
                    const save_backup::WarehouseReencryptStatus status =
                        warehouse_reencrypt_on_device(wf, slot, wh_key, wh_payloads[i]);
                    if (status != save_backup::WarehouseReencryptStatus::kOk) {
                        return result_error(("warehouse companion reencrypt failed (" +
                                       std::string(save_backup::warehouse_reencrypt_status_name(status)) +
                                       "): " + wf.suffix).c_str());
                    }
                } else {
                    if (!warehouse_blob_usable_on_this_device(wf.data.data(), wf.data.size(),
                                                              wh_key)) {
                        return result_error(("warehouse companion not decryptable on this device: " +
                                       wf.suffix).c_str());
                    }
                    wh_payloads[i] = wf.data;
                }
            }
        }
    }

    // 3) 安全备份现存文件到 .rollback/（dat 落副本；sidecar 现状快照留在内存，失败时经 store 还原）。
    const std::string rollback_dir = dir + "/" + kRollbackDirName;
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

    // 6) warehouse_companion：仅 v2/v3（has_warehouse=true）写回；v1 完全跳过。
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
                   slot, wh_written, wh_reencrypted, warehouse_state_name(target_state));
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

    // 7) 成功：清理该槽回滚副本（dat 与 wh 副本；sidecar 无需副本），并释放前置门 lease。
    for (const WhRollback& item : wh_rollback) remove_file(item.rb);
    remove_file(rb_dat);
    slot_operation_release(&guard);
    save_backup::Entry entry;
    save_backup::entry_from_bundle(name, parsed, entry);
    auto iit = g_map_names.find(entry.map_id);
    if (iit != g_map_names.end()) entry.map_name = iit->second;
    entry.class_name = save_backup_class_name(entry.class_idx);
    SB_LOG("save backup imported: %s -> slot%d", name.c_str(), slot);
    // 目标槽文件已被覆盖：槽区结构不再代表磁盘状态，下一次读取必须重载。
    save_mark_stale();
    return "{\"ok\":true,\"backup\":" + save_backup::entry_json(entry) + "}";
}

std::string save_backup_delete_json(const char* checksum) {
    std::lock_guard<std::mutex> lk(g_sb_mtx);
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
                                                       parsed.module, parsed.warehouse_blob)) {
        return result_error("backup checksum mismatch");
    }
    if (::unlink(path.c_str()) != 0) {
        SB_LOG("save backup delete failed: %s", path.c_str());
        return result_error("delete failed");
    }
    SB_LOG("save backup deleted: %s", path.c_str());
    return "{\"ok\":true}";
}

// 删除某个槽的游戏存档：原版 save{slot}.dat（游戏目录）+ 模块 sidecar
// （经槽操作 lease 的 store 事务入口）。两阶段：先把所有目标 rename 到各自父目录内的
// `.sb-trash-<id>`（dat 与 sidecar 不同文件系统，不能共用一次 rename），全部成功后再 unlink；
// 任一步失败把已移动文件移回。index 记录 trash 路径与原路径，进程中途死亡时由启动恢复处理。
// 安全边界：路径由整型 slot 精确拼接（save0/1/2.dat），不含通配/遍历；缺失的目标跳过。
// 全部失败路径返回结构化 op_err；无任何目标文件时返回 not found。
std::string save_backup_delete_slot_json(int slot) {
    // 存在性探测持锁；gate/删除在锁外，同槽串行由槽操作门保证。
    std::unique_lock<std::mutex> lk(g_sb_mtx);
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
        if (g_sb_external_dir.empty()) {
            slot_operation_release(&guard);
            return result_error("delete sidecar failed");
        }
        saves_dir = g_sb_external_dir + "/saves";
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
                        guard.api->host_ctx, g_owner_token, &guard.lease, slot,
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
    save_mark_stale();
    return "{\"ok\":true,\"deleted_slot\":" + std::to_string(slot) + "}";
}

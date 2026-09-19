// save_backup feature.cpp —— 存档管理器备份业务层（save-export 底层迁移自 Kotlin SaveBackupStore）。
//
// 职责：.qol_save bundle（原版存档明文 + 模块 sidecar + 元信息）的导出/导入/列表/删除；
// 导出按 checksum 去重；导入为「原版 .dat 重加密 + sidecar 重定槽」两段事务，
// 任一步失败从 save_backup/.rollback/ 还原。错误信封沿用 op_err（格式 A），
// 错误串与迁移前 HTTP 契约逐字一致。
//
// 明文容器结构（overhaul v1.3.2 逆向，SAVE_LoadData 解密产物）：
//   头 8 字节；块目录自 +8 起，每项 4 字节（u16 offset_rel + u16 len）；
//   块 i 数据 = plain + 8 + u16(plain + 8 + i*4)。
//   块0：+0 u8 slot（SAVE_IsValidInformation 要求 == 请求槽）、+13 u64 save_time、+29 u32 version（≤5）。
//   块1：+0 i16 map_id。
// 全部存档域函数指针经 Game Host 语义接口收口；本层不出现裸地址。

#include "plugins/save_backup/feature.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/native/game_feature.h"
#include "core/native/game_ops_common.h"
#include "core/native/qol_log.h"
#include "plugins/save_backup/bundle.h"
#include "plugins/save_backup/rollback.h"

namespace {

#define SB_LOG(...) QOL_LOG_INFO(QolDomain::kSaveBackup, __VA_ARGS__)
#define SB_LOG_WARN(...) QOL_LOG_WARN(QolDomain::kSaveBackup, __VA_ARGS__)

constexpr const char* kBackupDirName = "save_backup";
constexpr const char* kRollbackDirName = ".rollback";
constexpr const char* kBundleSuffix = ".qol_save";

std::mutex g_sb_mtx;
std::string g_sb_data_dir;
std::string g_sb_external_dir;
const qol::contracts::QolHostSdk* g_sdk = nullptr;
const qol::contracts::QolGameHost* g_game = nullptr;

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
    return g_game != nullptr && g_game->extra.save.load_data != nullptr &&
           g_game->extra.save_call.free_data != nullptr;
}

bool save_encrypt_available() {
    return g_game != nullptr && g_game->extra.save_call.encrypt != nullptr;
}

const char* save_hub_key() {
    if (g_game == nullptr || g_game->extra.save.hub_key == nullptr) return nullptr;
    const char* key = nullptr;
    if (!qol::contracts::succeeded(g_game->extra.save.hub_key(&key))) return nullptr;
    return key;
}

int save_encrypt_process(void* data, int len, int mode, const char* key) {
    if (!save_encrypt_available() || data == nullptr || len < 0 || key == nullptr) return 0;
    int result = 0;
    if (!qol::contracts::succeeded(
            g_game->extra.save_call.encrypt(data, len, mode, key, &result))) {
        return 0;
    }
    return result;
}

void save_mark_stale() {
    if (g_game != nullptr && g_game->extra.save_call.mark_stale != nullptr) {
        (void)g_game->extra.save_call.mark_stale();
    }
}

// ---- 地图名表（Kotlin 启动期从 MAPINFOBASE 下发：map_id → 中文名） ----
// 解析失败/空 JSON 视为空表（entry_json 输出空 map_name，UI 显示「未知地图」）。
// 全局单例：g_sb_mtx 保护，启动期单线程写入、之后只读。
std::unordered_map<int, std::string> g_map_names;

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
    if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return ::mkdir(path.c_str(), 0755) == 0;
}

bool file_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool read_file_bytes(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    out.clear();
    uint8_t buf[8192];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok) out.clear();
    return ok;
}

// 原子写：写 <path>.tmp 再 rename。
bool write_file_bytes_atomic(const std::string& path, const uint8_t* data, size_t size) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) return false;
    const bool written = size == 0 || std::fwrite(data, 1, size, f) == size;
    if (std::fclose(f) != 0 || !written) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool copy_file(const std::string& from, const std::string& to) {
    FILE* in = std::fopen(from.c_str(), "rb");
    if (in == nullptr) return false;
    FILE* out = std::fopen(to.c_str(), "wb");
    if (out == nullptr) {
        std::fclose(in);
        return false;
    }
    uint8_t buf[8192];
    size_t n = 0;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
        if (std::fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    ok = ok && std::ferror(in) == 0;
    std::fclose(in);
    if (std::fclose(out) != 0) ok = false;
    return ok;
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

// 备份目录 <external>/save_backup/（不存在则建）；未初始化/建目录失败返回 false。
bool backup_dir(std::string& out) {
    if (g_sb_external_dir.empty()) {
        SB_LOG("save backup unavailable: store not initialized");
        return false;
    }
    out = g_sb_external_dir + "/" + kBackupDirName;
    if (!ensure_dir(out)) {
        SB_LOG("save backup unavailable: mkdir failed: %s", out.c_str());
        return false;
    }
    return true;
}

// sidecar 容器操作（读 primary/校验/回退、重定槽双写、删除）统一经 infra/store；
// 本文件不再自行拼接 module-saves/ 路径或直接读写 sidecar 文件。

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

// 槽结构元数据（尽力而为：槽结构未加载时返回 -1/-1/-1，不阻塞明文导出）。
void read_hero_meta(int32_t slot, int& hero_level, int& hero_index, int& class_idx) {
    hero_level = -1;
    hero_index = -1;
    class_idx = -1;
    if (g_game == nullptr || g_game->extra.save.slot_exists == nullptr ||
        g_game->extra.save.slot_hero_index == nullptr ||
        g_game->extra.save.hero_level == nullptr || g_game->extra.save.hero_class == nullptr) {
        return;
    }
    int32_t exists = 0;
    if (!qol::contracts::succeeded(g_game->extra.save.slot_exists(slot, &exists)) || exists == 0) {
        return;
    }
    int32_t value = 0;
    if (qol::contracts::succeeded(g_game->extra.save.slot_hero_index(slot, &value))) {
        hero_index = value;
    }
    if (qol::contracts::succeeded(g_game->extra.save.hero_level(slot, &value))) {
        hero_level = value;
    }
    // 职业索引（0-5）；type==2 装饰物该字段非职业，但本路径取的是主角 hero 结构。
    if (qol::contracts::succeeded(g_game->extra.save.hero_class(slot, &value))) {
        class_idx = value;
    }
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
    const QolResult load_result = g_game->extra.save.load_data(slot, &buf, &len);
    if (!qol::contracts::succeeded(load_result) || buf == nullptr || len < 8) {
        if (buf != nullptr) (void)g_game->extra.save_call.free_data(buf);
        if (load_result.code == kQolErrNotReady) err = "symbol not resolved";
        else err = "load failed";
        return false;
    }
    // 明文拷出后立即归还游戏堆，后续处理与游戏内存解耦。
    plain.assign(static_cast<const uint8_t*>(buf), static_cast<const uint8_t*>(buf) + len);
    (void)g_game->extra.save_call.free_data(buf);

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
    if (plain.size() < 8) return false;
    const size_t block0 = 8 + rd_u16(plain.data() + 8);
    if (block0 >= plain.size()) return false;

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
int locate_save_directory(std::string& out) {
    if (!g_sb_data_dir.empty()) {
        DIR* d = ::opendir(g_sb_data_dir.c_str());
        if (d != nullptr) {
            while (dirent* ent = ::readdir(d)) {
                const std::string name = ent->d_name;
                if (name == "." || name == "..") continue;
                const std::string sub = g_sb_data_dir + "/" + name;
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
    const std::string dir = g_sb_data_dir + "/" + dir_name;
    if (!ensure_dir(dir)) return 2;
    SB_LOG("save backup import: save directory fallback %s", dir.c_str());
    out = dir;
    return 0;
}

// 导入失败还原：dat 按 .rollback 副本还原；sidecar 仅在本次已改动时经 store 还原为
// 导入前快照（prev_container）。导入前无可用 primary（prev_container_exists=false）时删除该槽 sidecar。
bool preserve_rollback_record(const std::string& path, const uint8_t* data, size_t size) {
    return write_file_bytes_atomic(path, data, size);
}

save_backup::RollbackResult restore_from_rollback(
    int slot, const std::string& dat_path, const std::string& rb_dat, bool dat_existed,
    const std::vector<uint8_t>& prev_container, bool prev_container_exists, bool sidecar_touched) {
    save_backup::RollbackResult result;
    if (dat_existed) {
        result.dat_restored = file_exists(rb_dat) && copy_file(rb_dat, dat_path);
    } else {
        result.dat_restored = remove_file(dat_path);
    }
    if (sidecar_touched) {
        if (prev_container_exists) {
            result.sidecar_restored = store_import_container(
                slot, prev_container.data(), prev_container.size());
        } else {
            result.sidecar_restored = store_delete_slot(slot) == ContainerStatus::kOk;
        }
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
        const std::string sidecar_record = rb_dat + ".module-save";
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

}  // namespace

void save_backup_bind_context(const qol::contracts::QolHostSdk* sdk,
                              const qol::contracts::QolGameHost* game) {
    g_sdk = sdk;
    g_game = game;
}

const qol::contracts::QolHostSdk* save_backup_host_sdk() { return g_sdk; }

const qol::contracts::QolGameHost* save_backup_game_host() { return g_game; }

void save_backup_init(const char* data_dir, const char* external_files_dir) {
    std::lock_guard<std::mutex> lk(g_sb_mtx);
    g_sb_data_dir = data_dir != nullptr ? data_dir : "";
    g_sb_external_dir = external_files_dir != nullptr ? external_files_dir : "";
}

// 游戏删档回调（SaveSlot_Delete）PtrHook wrapper 的同步体：游戏内存档面板删档确认后，
// 同步删除该槽模块 sidecar（slot-{n}.module-save + .last-good）。
// 执行上下文：游戏主线程（UIPopupMsg_ButtonOKExe → blr），slot 由 wrapper 传入。约束：
//  - 不取 g_virtual_bag_mtx、不走 JNI/Kotlin（避免跨层锁序与主线程阻塞）；
//  - 不在本函数内再调 SAVE_CreateSaveSlot（原函数已调）；
//  - 短取 g_sb_mtx，与 save_backup 导入/导出串行（锁序 g_sb_mtx → store）。
void save_backup_slot_deleted_sync(int32_t slot) {
    if (slot < 0 || slot > 2) return;
    std::lock_guard<std::mutex> lk(g_sb_mtx);
    const ContainerStatus status = store_delete_slot(slot);
    if (status == ContainerStatus::kOk) {
        SB_LOG("slot delete hook: removed sidecar slot=%d", slot);
    } else {
        SB_LOG("slot delete hook: sidecar delete skipped/failed slot=%d status=%s", slot,
               status == ContainerStatus::kNotFound ? "not_found"
               : status == ContainerStatus::kCorrupt ? "corrupt" : "unavailable");
    }
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

const char* save_backup_class_name(int class_idx) {
    // 与游戏 CHARCLASSBASE 文本逐条核对（apk/static-data/tables/CHARCLASSBASE.json
    // + text/zh-Hans.json，0..5）：不使用 Kotlin 下发通道，避免新增 JNI/HTTP 依赖；
    // 6 个职业为游戏固定枚举，硬编码稳定。表为常量、无锁。
    static const char* const kNames[6] = {"黑暗骑士", "忍者", "黑魔导", "祭司",
                                          "暗影猎手", "狂战士"};
    if (class_idx < 0 || class_idx > 5) return "";
    return kNames[class_idx];
}

std::string save_backup_list_json() {
    std::lock_guard<std::mutex> lk(g_sb_mtx);
    std::string s = "{\"ok\":true,\"backups\":[";
    std::string dir;
    if (!backup_dir(dir)) return s + "]}";
    std::vector<std::string> names;
    if (!list_bundle_files(dir, names)) return s + "]}";
    // 先解析全部 bundle，再按 export_time 倒序（新→旧，UI 第 1 页顶部 = 最新备份）。
    // 权威字段 = bundle 头/metaJson 的 export_time（entry_from_bundle 填充）；
    // 同毫秒并列时按文件名倒序保持稳定口径（文件名前缀即本地时间戳）。
    std::vector<save_backup::Entry> entries;
    entries.reserve(names.size());
    for (const std::string& name : names) {
        std::vector<uint8_t> bytes;
        save_backup::ParsedBundle parsed;
        if (!read_file_bytes(dir + "/" + name, bytes) ||
            !save_backup::parse_bundle(bytes.data(), bytes.size(), parsed)) {
            SB_LOG("save backup list skip: %s", name.c_str());
            continue;
        }
        save_backup::Entry entry;
        save_backup::entry_from_bundle(name, parsed, entry);
        // 从内存 map_id→name 表查表（启动期 Kotlin 下发）；未命中保持空串。
        auto it = g_map_names.find(entry.map_id);
        if (it != g_map_names.end()) entry.map_name = it->second;
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

std::string save_backup_export_json(int slot) {
    std::lock_guard<std::mutex> lk(g_sb_mtx);
    if (slot < 0 || slot > 2) return op_err("bad slot");

    std::vector<uint8_t> plain;
    int map_id = 0;
    long long save_time = 0;
    int version = 0;
    int hero_level = -1;
    int hero_index = -1;
    int class_idx = -1;
    std::string err;
    if (!read_original_plain(slot, plain, map_id, save_time, version, hero_level, hero_index,
                             class_idx, err)) {
        return op_err(err.c_str());
    }

    // sidecar 缺失（如从未经 API 进档的槽）或损坏时仅导出原版明文；导入侧跳过 sidecar 步骤。
    // read_container 只读 primary 且完整解码，损坏/缺失均返回非 ok（损坏 primary 不 quarantine）。
    std::vector<uint8_t> module;
    {
        std::vector<uint8_t> container;
        if (store_read_container(slot, container) == ContainerStatus::kOk) {
            module.swap(container);
        } else {
            SB_LOG("save backup export slot=%d: module sidecar unavailable, exporting original only",
                   slot);
        }
    }

    // 个人仓库伴生文件（monster 改版）：收集同存档目录下 `save{slot}.dat.wh4-*` 普通文件
    //（含游戏自建 `.bak`），排除目录与写入中间态 `.tmp`/`.wh-tmp`；按文件名升序保证确定性。
    // 单文件 >1MiB 视为异常跳过并记日志；目录不可得/无命中则为空列表（导出仅原版 + sidecar）。
    std::vector<save_backup::WarehouseFile> warehouse;
    {
        constexpr size_t kMaxWarehouseExportBytes = 1 * 1024 * 1024;
        std::string save_dir;
        if (locate_save_directory(save_dir) == 0) {
            const std::string dat_name = "save" + std::to_string(slot) + ".dat";
            const std::string prefix = dat_name + ".wh4-";
            DIR* d = ::opendir(save_dir.c_str());
            if (d != nullptr) {
                std::vector<std::string> names;
                while (dirent* ent = ::readdir(d)) {
                    const std::string name = ent->d_name;
                    if (name.size() <= prefix.size()) continue;
                    if (name.compare(0, prefix.size(), prefix) != 0) continue;
                    if (ends_with(name, ".tmp") || ends_with(name, ".wh-tmp")) continue;
                    const std::string full = save_dir + "/" + name;
                    struct stat st{};
                    if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
                    names.push_back(name);
                }
                ::closedir(d);
                std::sort(names.begin(), names.end());
                for (const std::string& name : names) {
                    const std::string suffix = name.substr(dat_name.size());
                    if (!save_backup::valid_warehouse_suffix(suffix)) {
                        SB_LOG("save backup export slot=%d: skip invalid warehouse name: %s", slot,
                               name.c_str());
                        continue;
                    }
                    std::vector<uint8_t> bytes;
                    if (!read_file_bytes(save_dir + "/" + name, bytes)) {
                        SB_LOG("save backup export slot=%d: warehouse read failed: %s", slot,
                               name.c_str());
                        continue;
                    }
                    if (bytes.size() > kMaxWarehouseExportBytes) {
                        SB_LOG("save backup export slot=%d: warehouse too large (%zu), skip: %s",
                               slot, bytes.size(), name.c_str());
                        continue;
                    }
                    save_backup::WarehouseFile wf;
                    wf.suffix = suffix;
                    // v3：优先用本机密钥解密为明文存储（导入端可用目标设备密钥重加密，跨设备可移植）；
                    // 解密失败（长度不足/无 key/双校验和不通过）则原样存密文，flags=0 走旧校验路径。
                    // 注意：mode=1 会读 buf[len-3 .. len) 的尾部 3 字节做校验，必须传**完整容器**，
                    // 只传 payload 会越界读到垃圾导致必然返回 0（真机实测）。
                    bool decrypted = false;
                    if (bytes.size() >= 4 && save_encrypt_available()) {
                        const char* wh_key = save_hub_key();
                        if (wh_key != nullptr) {
                            const size_t payload_len = bytes.size() - 3;
                            std::vector<uint8_t> buf = bytes;
                            if (save_encrypt_process(buf.data(), static_cast<int>(payload_len), 1,
                                                     wh_key) == 1) {
                                wf.data.assign(buf.begin(), buf.begin() + payload_len);
                                wf.flags = save_backup::kWarehouseFlagPlain;
                                decrypted = true;
                                SB_LOG("save backup export slot=%d: warehouse decrypted for "
                                       "cross-device: %s (%zu bytes)",
                                       slot, name.c_str(), wf.data.size());
                            }
                        }
                    }
                    if (!decrypted) {
                        wf.data.swap(bytes);
                        wf.flags = 0;
                        SB_LOG("save backup export slot=%d: warehouse kept as ciphertext "
                               "(decrypt unavailable): %s (%zu bytes)",
                               slot, name.c_str(), wf.data.size());
                    }
                    warehouse.push_back(std::move(wf));
                }
            }
        }
    }
    // 导出统一分离：若本次确实收齐了外部 wh4，则把存档 block3 内嵌的 WH96v002 仓库段剥离，
    // 使存档退回基础形态、仓库一律走外部 wh4（跨设备可重加密）。保守规则：段不在 block3 末尾
    // 或块表异常时维持原状。无外部 wh4 时不得剥离（否则丢仓库）。
    if (!warehouse.empty()) {
        const save_backup::InlineWarehouseStrip strip =
            save_backup::warehouse_strip_inline_segment(plain);
        if (strip == save_backup::InlineWarehouseStrip::kStripped) {
            SB_LOG("save backup export slot=%d: inline WH96v002 segment stripped (origLen=%zu)", slot,
                   plain.size());
        } else {
            SB_LOG("save backup export slot=%d: inline warehouse kept (%s)", slot,
                   save_backup::inline_warehouse_strip_name(strip));
        }
    } else {
        SB_LOG("save backup export slot=%d: no external warehouse collected, keep inline warehouse",
               slot);
    }

    const std::vector<uint8_t> warehouse_blob = save_backup::encode_warehouse(warehouse);

    const long long export_time_ms = now_ms();
    const std::string original_sha = save_backup::sha256_hex(plain, {});
    const std::string module_sha = module.empty() ? std::string() : save_backup::sha256_hex(module, {});
    const std::string warehouse_sha =
        warehouse.empty() ? std::string() : save_backup::sha256_hex(warehouse_blob, {});
    // checksum = sha256(origPlain ‖ module ‖ warehouseBlob) 前 12 位小写 hex。
    // 无仓库时 warehouseBlob 为空 → 与旧版逐字节一致（硬约束）。
    std::vector<uint8_t> module_and_wh = module;
    module_and_wh.insert(module_and_wh.end(), warehouse_blob.begin(), warehouse_blob.end());
    const std::string checksum = save_backup::sha256_hex(plain, module_and_wh).substr(0, 12);

    std::string dir;
    if (!backup_dir(dir)) return op_err("backup dir unavailable");

    // 内容去重：同 checksum 备份已存在则不写新文件，直接返回既有备份
    //（metaJson 的 checksum 为权威，文件名后缀兜底）。
    const std::string existing = find_by_checksum(dir, checksum);
    if (!existing.empty()) {
        std::vector<uint8_t> bytes;
        save_backup::ParsedBundle parsed;
        if (read_file_bytes(existing, bytes) &&
            save_backup::parse_bundle(bytes.data(), bytes.size(), parsed)) {
            save_backup::Entry entry;
            save_backup::entry_from_bundle(file_name_of(existing), parsed, entry);
            auto dit = g_map_names.find(entry.map_id);
            if (dit != g_map_names.end()) entry.map_name = dit->second;
            entry.class_name = save_backup_class_name(entry.class_idx);
            SB_LOG("save backup deduplicated: %s (checksum=%s)", entry.file_name.c_str(),
                   checksum.c_str());
            return "{\"ok\":true,\"backup\":" + save_backup::entry_json(entry) +
                   ",\"deduplicated\":true}";
        }
        SB_LOG("save backup dedup hit unreadable, writing new bundle: %s", existing.c_str());
    }

    const std::string class_name = save_backup_class_name(class_idx);
    const std::string meta_json = save_backup::build_meta_json(
        slot, export_time_ms, map_id, hero_level, hero_index, version, save_time, original_sha,
        module_sha, checksum, class_idx, class_name, static_cast<int>(warehouse.size()),
        warehouse_sha);
    std::vector<uint8_t> bundle;
    save_backup::build_bundle(slot, export_time_ms, plain, module, warehouse_blob, meta_json, bundle);

    char stamp[16] = {0};
    if (!local_stamp(export_time_ms, stamp)) return op_err("bundle write failed");
    const std::string base = std::string(stamp) + "_s" + std::to_string(slot) + "_" + checksum;
    std::string target = dir + "/" + base + kBundleSuffix;
    int collision = 0;
    while (file_exists(target)) {
        collision += 1;
        if (collision > 99) return op_err("backup name collision");
        target = dir + "/" + base + "_" + std::to_string(collision) + kBundleSuffix;
    }
    if (!write_file_bytes_atomic(target, bundle.data(), bundle.size())) {
        return op_err("bundle write failed");
    }

    save_backup::Entry entry;
    entry.file_name = file_name_of(target);
    entry.size_bytes = static_cast<long long>(bundle.size());
    entry.source_slot = slot;
    entry.export_time_ms = export_time_ms;
    entry.map_id = map_id;
    auto mit = g_map_names.find(map_id);
    if (mit != g_map_names.end()) entry.map_name = mit->second;
    entry.hero_level = hero_level;
    entry.hero_index = hero_index;
    entry.class_idx = class_idx;
    entry.class_name = class_name;
    entry.save_version = version;
    entry.save_time = save_time;
    entry.original_sha256 = original_sha;
    entry.module_sha256 = module_sha;
    entry.checksum = checksum;
    SB_LOG("save backup exported: %s (%zu bytes)", entry.file_name.c_str(), bundle.size());
    return "{\"ok\":true,\"backup\":" + save_backup::entry_json(entry) + "}";
}

std::string save_backup_import_json(const char* checksum, int slot) {
    std::lock_guard<std::mutex> lk(g_sb_mtx);
    const std::string ck = checksum != nullptr ? checksum : "";
    if (!save_backup::valid_checksum(ck)) return op_err("bad checksum");
    if (slot < 0 || slot > 2) return op_err("bad slot");
    std::string dir;
    if (!backup_dir(dir)) return op_err("backup dir unavailable");
    const std::string path = find_by_checksum(dir, ck);
    if (path.empty()) return op_err("backup not found");
    const std::string name = file_name_of(path);

    std::vector<uint8_t> bytes;
    save_backup::ParsedBundle parsed;
    if (!read_file_bytes(path, bytes) ||
        !save_backup::parse_bundle(bytes.data(), bytes.size(), parsed)) {
        return op_err("backup unreadable or corrupt");
    }

    // 1) 原版明文加密到目标槽（块0 slot 字节改写目标槽并追加校验尾）。
    std::vector<uint8_t> cipher;
    if (!encrypt_plain_to_slot(slot, parsed.orig_plain, cipher) || cipher.empty()) {
        return op_err("encrypt failed");
    }

    // 2) 定位存档目录：优先 dataDir 下含 save*.dat 的子目录，退回 md5(密钥) 目录。
    std::string save_dir;
    switch (locate_save_directory(save_dir)) {
        case 0:
            break;
        case 2:
            return op_err("save directory mkdir failed");
        default:
            return op_err("save directory not found");
    }

    // 2a) 仓库写回预校验/预加密（必须在写任何文件之前，保证 .rollback/ 与现存文件不被触碰）：
    //     fail-closed —— 任一项失败整次导入失败。
    //       flags=0（密文 wh4）：必须能用本机密钥解密（v2 旧备份仅支持同设备导入；
    //                            导入后再导出 v3 才变为可跨设备）。
    //       flags=1（明文 wh4）：补 slot + 重算 +0x38 完整性校验 + 重加密 + 往返自检。
    //     变体能力门禁 unsupported → 整步跳过（保持既有行为，不校验也不写）。
    std::vector<std::vector<uint8_t>> wh_payloads;
    qol::FeatureState wh_state = qol::FeatureState::kUnsupported;
    bool wh_write = false;
    if (parsed.has_warehouse) {
        wh_state = qol::game_feature_state(qol::GameFeature::kWarehouseCompanionFile);
        if (wh_state == qol::FeatureState::kUnsupported) {
            SB_LOG("save backup import: slot%d warehouse companion unsupported (%s), skip %zu file(s)",
                   slot, qol::game_feature_state_name(wh_state), parsed.warehouse.size());
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
                        return op_err(("warehouse companion reencrypt failed (" +
                                       std::string(save_backup::warehouse_reencrypt_status_name(status)) +
                                       "): " + wf.suffix).c_str());
                    }
                } else {
                    if (!warehouse_blob_usable_on_this_device(wf.data.data(), wf.data.size(),
                                                              wh_key)) {
                        return op_err(("warehouse companion not decryptable on this device: " +
                                       wf.suffix).c_str());
                    }
                    wh_payloads[i] = wf.data;
                }
            }
        }
    }

    // 3) 安全备份现存文件到 .rollback/（dat 落副本；sidecar 现状快照留在内存，失败时经 store 还原）。
    const std::string rollback_dir = dir + "/" + kRollbackDirName;
    if (!ensure_dir(rollback_dir)) return op_err("rollback dir mkdir failed");
    const std::string dat_path = save_dir + "/save" + std::to_string(slot) + ".dat";
    const bool dat_existed = file_exists(dat_path);
    const std::string rb_dat = rollback_dir + "/save" + std::to_string(slot) + ".dat";
    if (dat_existed && !copy_file(dat_path, rb_dat)) return op_err("rollback backup failed");
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

    // 4) 原子写原版 .dat（tmp+rename）。
    if (!write_file_bytes_atomic(dat_path, cipher.data(), cipher.size())) {
        const save_backup::RollbackResult rollback = restore_from_rollback(
            slot, dat_path, rb_dat, dat_existed, prev_container, prev_container_exists,
            sidecar_touched);
        return op_err(rollback.complete() ? "save file write failed"
                                           : "save file write failed; rollback failed");
    }

    // 5) sidecar：经 infra/store 重定槽（按源容器头槽号校验）并双写 primary + last-good；
    //    generation 由 store 按容器语义递增（空 module = 仅原版备份，跳过）。
    if (!parsed.module.empty()) {
        sidecar_touched = true;
        if (!store_import_container(slot, parsed.module.data(), parsed.module.size())) {
            const save_backup::RollbackResult rollback = restore_from_rollback(
                slot, dat_path, rb_dat, dat_existed, prev_container, prev_container_exists,
                sidecar_touched);
            return op_err(rollback.complete() ? "module container import failed"
                                               : "module container import failed; rollback failed");
        }
    }

    // 6) 个人仓库伴生文件写回：仅当 v2/v3 备份 parsed.has_warehouse 为真时执行；
    //    v1 旧备份（has_warehouse=false）完全跳过，绝不触碰现存 wh4 文件（保护旧备份导入用户）。
    //    预校验/预加密已完成（见 2a），此处只做落盘。
    //    **语义为 additive**：只新增/覆盖 bundle 内后缀对应的目标文件，不删除目标槽其它
    //    后缀的 wh4 文件——理由：模块不掌握游戏仓库文件的完整集合语义，批量删除可能误删
    //    其它存档数据。suffix 已由 decode_warehouse 校验（`.wh4-` 开头、无路径分隔/`..`），
    //    因此 target 由 save_dir + 槽号 + suffix 直接拼接是安全的，不再引入额外路径处理。
    //    任一步失败：先还原本函数已处理的全部 wh 文件，再回滚 dat/sidecar。
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
                restored = file_exists(it->rb) && copy_file(it->rb, it->target);
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
    if (wh_write) {
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
                rb = rollback_dir + "/" + file_name_of(target);
                if (!copy_file(target, rb)) {
                    remove_file(rb);  // 清理可能写了一半的副本
                    const save_backup::RollbackResult warehouse_rollback = restore_warehouse();
                    const save_backup::RollbackResult core_rollback = restore_from_rollback(
                        slot, dat_path, rb_dat, dat_existed, prev_container, prev_container_exists,
                        sidecar_touched);
                    return op_err(warehouse_rollback.complete() && core_rollback.complete()
                                      ? "rollback backup failed"
                                      : "rollback backup failed; rollback failed");
                }
            }
            wh_rollback.push_back(WhRollback{target, rb, existed});
            if (!write_file_bytes_atomic(target, payload.data(), payload.size())) {
                const save_backup::RollbackResult warehouse_rollback = restore_warehouse();
                const save_backup::RollbackResult core_rollback = restore_from_rollback(
                    slot, dat_path, rb_dat, dat_existed, prev_container, prev_container_exists,
                    sidecar_touched);
                return op_err(warehouse_rollback.complete() && core_rollback.complete()
                                  ? "warehouse file write failed"
                                  : "warehouse file write failed; rollback failed");
            }
            ++wh_written;
            if ((wf.flags & save_backup::kWarehouseFlagPlain) != 0) ++wh_reencrypted;
        }
        if (wh_written > 0) {
            SB_LOG("save backup import: slot%d warehouse companion files wrote=%zu skipped=0 "
                   "reencrypted=%zu (state=%s)",
                   slot, wh_written, wh_reencrypted, qol::game_feature_state_name(wh_state));
        }
    }

    // 7) 成功：清理该槽回滚副本（dat 与 wh 副本；sidecar 无需副本）。
    for (const WhRollback& item : wh_rollback) remove_file(item.rb);
    remove_file(rb_dat);
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
    if (!save_backup::valid_checksum(ck)) return op_err("bad checksum");
    std::string dir;
    if (!backup_dir(dir)) return op_err("backup not found");
    const std::string path = find_by_checksum(dir, ck);
    if (path.empty()) return op_err("backup not found");
    if (::unlink(path.c_str()) != 0) {
        SB_LOG("save backup delete failed: %s", path.c_str());
        return op_err("delete failed");
    }
    SB_LOG("save backup deleted: %s", path.c_str());
    return "{\"ok\":true}";
}

// 删除某个槽的游戏存档：原版 save{slot}.dat（游戏目录，本文件直接删除）+ 模块 sidecar
// （经 infra/store::delete_slot 删除 primary/.last-good）。安全边界：路径由整型 slot 精确
// 拼接（save0/1/2.dat），不含通配/遍历，绝不删除其它槽；缺失的目标跳过。
// 全部失败路径返回结构化 op_err，不吞异常；无任何目标文件时返回 not found。
std::string save_backup_delete_slot_json(int slot) {
    std::lock_guard<std::mutex> lk(g_sb_mtx);
    if (slot < 0 || slot > 2) return op_err("bad slot");
    std::string save_dir;
    switch (locate_save_directory(save_dir)) {
        case 0:
            break;
        case 2:
            return op_err("save directory mkdir failed");
        default:
            return op_err("save directory not found");
    }
    const std::string dat_path = save_dir + "/save" + std::to_string(slot) + ".dat";
    const bool dat_existed = file_exists(dat_path);
    // store 无独立「是否存在」查询：read_container 对可解码 primary 返回 kOk；损坏 primary
    // 返回 kCorrupt（文件存在但不可用），两者都视为该槽存在 sidecar；kNotFound 视为不存在。
    std::vector<uint8_t> probe;
    const ContainerStatus probe_status = store_read_container(slot, probe);
    const bool sidecar_existed = probe_status == ContainerStatus::kOk ||
                                 probe_status == ContainerStatus::kCorrupt;
    if (!dat_existed && !sidecar_existed) return op_err("save not found");
    // 逐个删除并检查返回值：任一失败立即返回结构化错误（已删除的部分不回滚，与单文件删除语义一致）。
    if (dat_existed && ::unlink(dat_path.c_str()) != 0) {
        SB_LOG("save slot delete failed: %s", dat_path.c_str());
        return op_err("delete save failed");
    }
    if (sidecar_existed) {
        const ContainerStatus del = store_delete_slot(slot);
        if (del != ContainerStatus::kOk) {
            SB_LOG("save slot delete failed: slot=%d sidecar status=%s", slot,
                   del == ContainerStatus::kNotFound ? "not_found"
                   : del == ContainerStatus::kCorrupt ? "corrupt" : "unavailable");
            return op_err("delete sidecar failed");
        }
    }
    SB_LOG("save slot deleted: slot=%d (dat=%d sidecar=%d)", slot, dat_existed, sidecar_existed);
    // 槽文件已删除：槽区结构不再代表磁盘状态，下一次读取必须重载。
    save_mark_stale();
    return "{\"ok\":true,\"deleted_slot\":" + std::to_string(slot) + "}";
}

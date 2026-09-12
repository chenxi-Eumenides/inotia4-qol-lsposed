// save_backup.cpp —— 存档管理器备份 feature（save-export 底层迁移自 Kotlin SaveBackupStore）。
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
// 全部 VMA/偏移经 game_symbols.h 常量；游戏函数经 game_access fn 指针。

#include "feature/save_backup/save_backup.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <android/log.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/native/game_ops_common.h"
#include "feature/save_backup/save_backup_bundle.h"
#include "game_access.h"

namespace {

constexpr const char* kLogTag = "Inotia4SaveBackup";

#define SB_LOG(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)

constexpr const char* kBackupDirName = "save_backup";
constexpr const char* kRollbackDirName = ".rollback";
constexpr const char* kBundleSuffix = ".qol_save";
constexpr const char* kModuleSaveDirName = "module-saves";
constexpr const char* kModuleSaveSuffix = ".module-save";
constexpr const char* kLastGoodSuffix = ".last-good";

std::mutex g_sb_mtx;
std::string g_sb_data_dir;
std::string g_sb_external_dir;

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

void remove_file(const std::string& path) {
    if (path.empty()) return;
    ::unlink(path.c_str());
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

// sidecar 路径：<external>/module-saves/slot-{n}.module-save（primary）+ .last-good。
// external 未初始化或 module-saves 目录不可得时 sidecar 主路径为空。
void module_sidecar_paths(int slot, std::string& primary, std::string& last_good) {
    primary.clear();
    last_good.clear();
    if (g_sb_external_dir.empty()) return;
    const std::string dir = g_sb_external_dir + "/" + kModuleSaveDirName;
    if (!ensure_dir(dir)) {
        SB_LOG("module save unavailable: mkdir failed: %s", dir.c_str());
        return;
    }
    primary = dir + "/slot-" + std::to_string(slot) + kModuleSaveSuffix;
    last_good = primary + kLastGoodSuffix;
}

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

// 按文件名后缀 `_<checksum>.qol_save` 定位备份；未命中返回空串。
std::string find_by_checksum(const std::string& dir, const std::string& checksum) {
    const std::string suffix = std::string("_") + checksum + kBundleSuffix;
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return std::string();
    std::string hit;
    while (dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (name.size() <= suffix.size()) continue;
        const std::string full = dir + "/" + name;
        struct stat st{};
        if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            hit = full;
            break;
        }
    }
    ::closedir(d);
    return hit;
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
    if (fn_save_get_save_slot == nullptr || fn_saveslot_get_hero == nullptr) return;
    // 主菜单（STATE==4）下槽结构未随启动刷新；与 data_save_slots_json 一致，仅此状态重载三槽。
    if (g_state != nullptr && *reinterpret_cast<uint16_t*>(g_state) == 4 &&
        fn_save_create_save_slot != nullptr) {
        fn_save_create_save_slot();
    }
    void* slot_struct = fn_save_get_save_slot(slot);
    if (slot_struct == nullptr) return;
    uint8_t* p = reinterpret_cast<uint8_t*>(slot_struct);
    if (p[SAVESLOT_EXISTS] == 0) return;
    hero_index = static_cast<int8_t>(*reinterpret_cast<const int8_t*>(p + SAVESLOT_HERO_INDEX));
    void* hero = fn_saveslot_get_hero(slot_struct);
    if (hero == nullptr) return;
    uint8_t* hp = reinterpret_cast<uint8_t*>(hero);
    hero_level = static_cast<int8_t>(*reinterpret_cast<const int8_t*>(hp + C_LEVEL));
    // 职业索引（0-5）；type==2 装饰物该字段非职业，但本路径取的是主角 hero 结构。
    class_idx = static_cast<int8_t>(*reinterpret_cast<const int8_t*>(hp + C_CLASS));
}

// 读槽明文并归一化：SAVE_LoadData 解密 → 拷出并 MEM_Free → 块0 slot 写 0xFF。
// 失败时 err 填错误串（与迁移前 data_op_save_read_original 一致）。
bool read_original_plain(int32_t slot, std::vector<uint8_t>& plain, int& map_id,
                         long long& save_time, int& version, int& hero_level, int& hero_index,
                         int& class_idx, std::string& err) {
    if (fn_save_load_data == nullptr || fn_mem_free == nullptr) {
        err = "symbol not resolved";
        return false;
    }
    void* buf = nullptr;
    int len = 0;
    if (fn_save_load_data(slot, &buf, &len) != 1 || buf == nullptr || len < 8) {
        if (buf != nullptr) fn_mem_free(buf);
        err = "load failed";
        return false;
    }
    // 明文拷出后立即归还游戏堆，后续处理与游戏内存解耦。
    plain.assign(static_cast<const uint8_t*>(buf), static_cast<const uint8_t*>(buf) + len);
    fn_mem_free(buf);

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
    if (fn_encrypt_process2 == nullptr || fn_hub_save_get_key == nullptr) return false;
    if (plain.size() < 8) return false;
    const size_t block0 = 8 + rd_u16(plain.data() + 8);
    if (block0 >= plain.size()) return false;

    std::vector<uint8_t> buf = plain;
    buf[block0] = static_cast<uint8_t>(slot);
    // ENCRYPT_Process2 加密就地追加 3 字节，需 len+3 容量。
    buf.resize(plain.size() + 3, 0);
    const char* key = fn_hub_save_get_key();
    if (key == nullptr) return false;
    if (fn_encrypt_process2(buf.data(), static_cast<int>(plain.size()), 0, key) != 1) return false;
    cipher.swap(buf);
    return true;
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
    if (fn_hub_save_get_key == nullptr) return 1;
    const char* key = fn_hub_save_get_key();
    if (key == nullptr) return 1;
    const std::string dir_name = save_backup::md5_hex(reinterpret_cast<const uint8_t*>(key),
                                                      std::strlen(key));
    const std::string dir = g_sb_data_dir + "/" + dir_name;
    if (!ensure_dir(dir)) return 2;
    SB_LOG("save backup import: save directory fallback %s", dir.c_str());
    out = dir;
    return 0;
}

// 导入失败还原：dat 与 sidecar 的 existed/not-existed 两种情形都覆盖。
void restore_from_rollback(const std::string& dat_path, const std::string& rb_dat,
                           bool dat_existed, const std::string& sidecar_path,
                           const std::string& rb_sidecar, bool sidecar_existed) {
    if (dat_existed) {
        if (file_exists(rb_dat)) copy_file(rb_dat, dat_path);
    } else {
        remove_file(dat_path);
    }
    if (!sidecar_path.empty()) {
        if (sidecar_existed) {
            if (file_exists(rb_sidecar)) copy_file(rb_sidecar, sidecar_path);
        } else {
            remove_file(sidecar_path);
            remove_file(sidecar_path + kLastGoodSuffix);
        }
    }
    remove_file(rb_dat);
    remove_file(rb_sidecar);
}

}  // namespace

void save_backup_init(const char* data_dir, const char* external_files_dir) {
    std::lock_guard<std::mutex> lk(g_sb_mtx);
    g_sb_data_dir = data_dir != nullptr ? data_dir : "";
    g_sb_external_dir = external_files_dir != nullptr ? external_files_dir : "";
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
    std::vector<uint8_t> module;
    {
        std::string sidecar_path;
        std::string last_good;
        module_sidecar_paths(slot, sidecar_path, last_good);
        std::vector<uint8_t> bytes;
        if (!sidecar_path.empty() && read_file_bytes(sidecar_path, bytes) &&
            save_backup::module_container_valid(bytes)) {
            module.swap(bytes);
        } else {
            SB_LOG("save backup export slot=%d: module sidecar unavailable, exporting original only",
                   slot);
        }
    }

    const long long export_time_ms = now_ms();
    const std::string original_sha = save_backup::sha256_hex(plain, {});
    const std::string module_sha = module.empty() ? std::string() : save_backup::sha256_hex(module, {});
    // checksum = sha256(origPlain ‖ module) 前 12 位小写 hex。
    const std::string checksum = save_backup::sha256_hex(plain, module).substr(0, 12);

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
        module_sha, checksum, class_idx, class_name);
    std::vector<uint8_t> bundle;
    save_backup::build_bundle(slot, export_time_ms, plain, module, meta_json, bundle);

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

    // 3) 安全备份现存文件到 .rollback/。
    const std::string rollback_dir = dir + "/" + kRollbackDirName;
    if (!ensure_dir(rollback_dir)) return op_err("rollback dir mkdir failed");
    const std::string dat_path = save_dir + "/save" + std::to_string(slot) + ".dat";
    std::string sidecar_path;
    std::string sidecar_last_good;
    module_sidecar_paths(slot, sidecar_path, sidecar_last_good);
    const bool dat_existed = file_exists(dat_path);
    const bool sidecar_existed = !sidecar_path.empty() && file_exists(sidecar_path);
    const std::string rb_dat = rollback_dir + "/save" + std::to_string(slot) + ".dat";
    std::string rb_sidecar;
    if (!sidecar_path.empty()) rb_sidecar = rollback_dir + "/" + file_name_of(sidecar_path);
    if (dat_existed && !copy_file(dat_path, rb_dat)) return op_err("rollback backup failed");
    if (sidecar_existed && !copy_file(sidecar_path, rb_sidecar)) {
        return op_err("rollback backup failed");
    }

    // 4) 原子写原版 .dat（tmp+rename）。
    if (!write_file_bytes_atomic(dat_path, cipher.data(), cipher.size())) {
        restore_from_rollback(dat_path, rb_dat, dat_existed, sidecar_path, rb_sidecar,
                              sidecar_existed);
        return op_err("save file write failed");
    }

    // 5) sidecar：只改容器第 6 字节 slot + 重算尾部 u32 CRC32（不解析 section），
    //    primary 与 last-good 都写（空 module = 仅原版备份，跳过）。
    if (!parsed.module.empty()) {
        std::vector<uint8_t> container = parsed.module;
        bool sidecar_ok = save_backup::module_container_reslot(container, slot);
        if (sidecar_ok && !sidecar_path.empty()) {
            sidecar_ok = write_file_bytes_atomic(sidecar_path, container.data(), container.size()) &&
                         write_file_bytes_atomic(sidecar_last_good, container.data(),
                                                 container.size());
        } else {
            sidecar_ok = false;
        }
        if (!sidecar_ok) {
            restore_from_rollback(dat_path, rb_dat, dat_existed, sidecar_path, rb_sidecar,
                                  sidecar_existed);
            return op_err("module container import failed");
        }
    }

    // 6) 成功：清理该槽回滚副本。
    remove_file(rb_dat);
    remove_file(rb_sidecar);
    save_backup::Entry entry;
    save_backup::entry_from_bundle(name, parsed, entry);
    auto iit = g_map_names.find(entry.map_id);
    if (iit != g_map_names.end()) entry.map_name = iit->second;
    entry.class_name = save_backup_class_name(entry.class_idx);
    SB_LOG("save backup imported: %s -> slot%d", name.c_str(), slot);
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

// 删除某个槽的游戏存档：原版 save{slot}.dat + 模块 sidecar（module-saves/slot-{slot}.module-save
// 与 .last-good）。安全边界：路径由整型 slot 精确拼接（save0/1/2.dat、slot-0/1/2.*），
// 不含通配/遍历，绝不删除其它槽；只删存在的目标，缺失的目标跳过。
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
    std::string sidecar_path;
    std::string sidecar_last_good;
    module_sidecar_paths(slot, sidecar_path, sidecar_last_good);
    const bool dat_existed = file_exists(dat_path);
    const bool sidecar_existed = !sidecar_path.empty() && file_exists(sidecar_path);
    const bool last_good_existed = !sidecar_last_good.empty() && file_exists(sidecar_last_good);
    if (!dat_existed && !sidecar_existed && !last_good_existed) return op_err("save not found");
    // 逐个删除并检查返回值：任一失败立即返回结构化错误（已删除的部分不回滚，与单文件删除语义一致）。
    if (dat_existed && ::unlink(dat_path.c_str()) != 0) {
        SB_LOG("save slot delete failed: %s", dat_path.c_str());
        return op_err("delete save failed");
    }
    if (sidecar_existed && ::unlink(sidecar_path.c_str()) != 0) {
        SB_LOG("save slot delete failed: %s", sidecar_path.c_str());
        return op_err("delete sidecar failed");
    }
    if (last_good_existed && ::unlink(sidecar_last_good.c_str()) != 0) {
        SB_LOG("save slot delete failed: %s", sidecar_last_good.c_str());
        return op_err("delete sidecar failed");
    }
    SB_LOG("save slot deleted: slot=%d (dat=%d sidecar=%d last_good=%d)", slot, dat_existed,
           sidecar_existed, last_good_existed);
    return "{\"ok\":true,\"deleted_slot\":" + std::to_string(slot) + "}";
}

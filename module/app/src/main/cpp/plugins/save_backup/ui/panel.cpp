#include "panel.h"

#include "../../../contracts/panels.h"
#include "../../../contracts/ui.h"
#include "plugins/save_backup/bundle.h"
#include "plugins/save_backup/feature.h"
#include "plugins/save_backup/log.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#define SB_UI_LOG(...) SAVE_BACKUP_LOG_INFO(__VA_ARGS__)

#define CB_TEXT_SIZE 0x40

// 面板布局（与 core.ui 设置页同构：逻辑坐标 0-960×0-640 空间，root 相对全屏居中）
// 视觉与设置页同一套：顶部居中消息/标题区（同设置页 addr 标签位）+ 左上角「返回」
// 原版贴图按钮（屏幕 (8,8)，同设置页位置/尺寸）；主体左（存档框）/中（操作按钮，无组框，
// 居中于左右栏之间）/右（备份列表框）三栏，左右两栏同宽、右缘=内容区右缘；
// 三栏沿同一水平中线垂直居中（左框/中按钮组按各自高度上下居中，见 CONTENT_MID_Y）。
#define ROOT_W 0x3c0
#define ROOT_H 0x280
#define FRAME_BORDER 0x03
#define CONTENT_X 0x13
#define CONTENT_W 0x39A
#define CONTENT_Y 0x13
#define CONTENT_H (ROOT_H - 0x20 - 2 * FRAME_BORDER)
#define GAP 0x10
#define SLOT_COL_W 0x120
#define OPS_COL_W 0x80
#define LIST_COL_W (CONTENT_W - SLOT_COL_W - OPS_COL_W - 2 * GAP)
#define SLOT_COL_X CONTENT_X
#define OPS_COL_X (SLOT_COL_X + SLOT_COL_W + GAP)
#define LIST_COL_X (OPS_COL_X + OPS_COL_W + GAP)
#define LIST_ROW_H 0x48
#define LIST_ROW_GAP 0x04
#define LIST_COLS_PER_PAGE 5
#define LIST_COLS_MAX 64
#define AUTO_Y (CONTENT_Y + LIST_ROW_H / 2)
#define AUTO_H 0x58
#define AUTO_X LIST_COL_X
#define SLOT_ROW_H 0x68
#define SLOT_ROW_GAP 0x04
#define SLOT_FRAME_H (3 * SLOT_ROW_H + 2 * FRAME_BORDER - SLOT_ROW_GAP)
#define CONTENT_BOTTOM (CONTENT_Y + CONTENT_H)
#define CONTENT_MID_Y (CONTENT_Y + CONTENT_H / 2)
#define SLOT_FRAME_Y (CONTENT_Y + (CONTENT_H - SLOT_FRAME_H) / 2)
#define OPS_BTN_H 0x60
#define OPS_BTN_W 0x70
#define OPS_BTN_GAP 0x10
#define OPS_Y0 (CONTENT_MID_Y - (3 * OPS_BTN_H + 2 * OPS_BTN_GAP) / 2)
#define LIST_PAGE_BTN_H 0x30
#define LIST_PAGE_BTN_W 0x38
#define PAGE_ROW_Y (CONTENT_BOTTOM - LIST_ROW_H / 2 - LIST_PAGE_BTN_H)
#define FRAME_Y (AUTO_Y + AUTO_H + GAP)
#define FRAME_H (PAGE_ROW_Y - GAP - FRAME_Y)
// ABGR colors are only passed to the component text primitive; controls use component theme states.
#define UI_GOLD 0xFF00B4D7
#define COLOR_ARROW_GOLD 0xFFA7CCDB
#define UI_OPTION_TEXT 0xFFC8C7C1
#define COLOR_TEXT_WHITE 0xFFFFFFFF
#define COLOR_TEXT_GRAY 0xFF808080
#define COLOR_SEPARATOR 0xFF6F8EA2
#define COLOR_SELECTED_FILL_RGB565 0x3984
#define COLOR_SELECTED_ALPHA 90
#define COLOR_SELECTED_RADIUS 8
#define COLOR_OK 0xFFB0E89C
#define COLOR_ERR 0xFFE89C9C

namespace qol::features::save_backup {
namespace {

using UiRect = contracts::UiRect;

const contracts::QolUiAdapter* ui_adapter() {
    const contracts::QolHostSdk* sdk = save_backup_host_sdk();
    return sdk == nullptr ? nullptr : sdk->ui;
}

bool ui_ok(const QolResult& result) { return contracts::succeeded(result); }

const char* ui_screen_name() {
    const contracts::QolUiAdapter* ui = ui_adapter();
    const char* name = nullptr;
    if (ui != nullptr && ui->screen_name != nullptr && ui_ok(ui->screen_name(&name)) &&
        name != nullptr) {
        return name;
    }
    return "";
}

bool ui_get_rect(void* control, UiRect* rect) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    return ui != nullptr && ui->get_rect != nullptr && rect != nullptr &&
           ui_ok(ui->get_rect(control, rect));
}

void ui_begin_frame() {
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui != nullptr && ui->begin_frame != nullptr) (void)ui->begin_frame();
}

void ui_end_frame() {
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui != nullptr && ui->end_frame != nullptr) (void)ui->end_frame();
}

bool ui_resolution_width(int32_t* out) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    return ui != nullptr && ui->screen_inset_width != nullptr && out != nullptr &&
           ui_ok(ui->screen_inset_width(out));
}

bool ui_resolution_height(int32_t* out) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    return ui != nullptr && ui->screen_inset_height != nullptr && out != nullptr &&
           ui_ok(ui->screen_inset_height(out));
}

bool ui_resolution_ready() {
    int32_t width = 0;
    int32_t height = 0;
    return ui_resolution_width(&width) && ui_resolution_height(&height);
}

// ============== 全局状态 ==============
std::mutex g_sbui_mtx;

bool g_panel_active = false;
void* g_root = nullptr;
const contracts::QolUiComponents* g_ui_components = nullptr;
const contracts::UiConfirmationApi* g_ui_confirmation_api = nullptr;
// 主线程 API 的确认框 token；0 表示没有由本面板打开的确认框。
uint64_t g_confirmation_token = 0;
int g_back_pressed = 0;

// 三栏控件容器（label 类型，参与命中但 DrawProc 自绘；DrawProc 走 fn_ctrl_btn_draw → 自定义 cb）。
void* g_slot_btns[3] = {nullptr, nullptr, nullptr};
void* g_ops_btns[3] = {nullptr, nullptr, nullptr};   // 0=导出 1=导入 2=删除
void* g_list_btns[LIST_COLS_MAX] = {nullptr};       // 当前页 5 个
void* g_auto_btn = nullptr;
void* g_prev_btn = nullptr;
void* g_next_btn = nullptr;
void* g_back_btn = nullptr;                          // 底部「← 返回」：关闭面板
int g_selected_slot = -1;                            // 左栏选中槽 0/1/2；-1=未选（默认两侧都不选）
int g_selected_backup = -1;                          // 选中手动备份在列表中的绝对索引；-1=未选
bool g_selected_auto = false;
int g_page_index = 0;                                // 翻页：0..N
// 列表记录先由 files JSON 建占位；只有当前页解析完成后才填充 Entry。
// map_name 由 entry_json 输出携带（Kotlin 启动期下发的 map_id→中文名表）；未命中为空串。
// class_idx/class_name 由导出时写入 metaJson；旧备份缺失时 -1/空串（UI 退化显示）。
struct BackupEntry {
    std::string name;
    uint64_t size = 0;
    long long mtime_ms = 0;
    bool parsed = false;
    std::string checksum;
    std::string map_name;
    int hero_level = -1;
    int class_idx = -1;
    std::string class_name;
    int map_id = 0;
    long long export_time_ms = 0;
    long long save_time = 0;
};
BackupEntry g_auto_backup;
bool g_auto_backup_valid = false;
std::vector<BackupEntry> g_backups;
std::map<std::string, BackupEntry> g_entry_cache;
struct ParseTask {
    bool is_auto = false;
    std::string name;
    uint64_t size = 0;
    long long mtime_ms = 0;
    int slot = -1;
};
std::deque<ParseTask> g_parse_queue;
std::string g_selected_backup_name;
int g_total_pages = 0;
// 保留状态字段与消息控件；操作反馈统一写日志，标题区不显示结果横幅。
std::string g_msg_text;
bool g_msg_is_error = false;
enum class DialogKind { kNone, kExport, kImport, kDeleteSlot, kDeleteBackup };
DialogKind g_dialog = DialogKind::kNone;
// 确认框正文。
std::string g_confirmation_prompt;
std::string g_dialog_checksum;
int g_dialog_slot = -1;
bool g_dialog_auto = false;
// 左栏三槽缓存：enter 后由游戏线程读取磁盘存档（load_data + 明文摘要），DrawProc 只读缓存。
struct SlotInfo {
    bool exists = false;
    int state = 0;        // 原版槽状态：0=缺失/空、1=加载失败（损坏）、2=加载成功
    int hero_level = -1;
    int hero_index = -1;
    int class_idx = -1;   // 主角职业索引 0-5（hero+C_CLASS）；-1=未知，UI 退化显示
    int map_id = -1;
};
SlotInfo g_slots[3];
bool g_backups_loading = false;
bool g_auto_loading = false;
bool g_skip_parse_frame = false;

// Component ExecuteProc callback, dispatched by core.custom_ui.
void component_action_clicked(void* user);
void close_active_confirmation();

// 列表按钮 rect 依赖当前页文件记录数，定义在 panel.inc。
void sync_list_btn_rects_locked();

// 解析单条 Entry JSON（纯 CPU；调用方按当前页队列逐条调用）。
void parse_backups_json(const std::string& json, std::vector<BackupEntry>& entries) {
    entries.clear();
    // 极简解析：从 `{"ok":true,"backups":[...]}` 提取每个备份对象的 key。
    // 注意：entry_json 内含双引号字符串 / 数字字段；按外层 {} 配对扫描。
    size_t i = json.find("\"backups\":[");
    if (i == std::string::npos) return;
    i += std::string("\"backups\":[").size();
    const size_t end = json.rfind(']');
    if (end == std::string::npos || end <= i) return;
    while (i < end) {
        size_t open = json.find('{', i);
        if (open == std::string::npos || open >= end) break;
        size_t close = json.find('}', open);
        if (close == std::string::npos || close >= end) break;
        const std::string obj = json.substr(open, close - open + 1);
        BackupEntry e;
        auto find_str = [&](const char* key, std::string& out) {
            const std::string pat = std::string("\"") + key + "\":\"";
            size_t p = obj.find(pat);
            if (p == std::string::npos) return false;
            p += pat.size();
            size_t q = obj.find('"', p);
            if (q == std::string::npos) return false;
            out = obj.substr(p, q - p);
            return true;
        };
        auto find_int = [&](const char* key, long long& out) {
            const std::string pat = std::string("\"") + key + "\":";
            size_t p = obj.find(pat);
            if (p == std::string::npos) return false;
            p += pat.size();
            char* endp = nullptr;
            out = std::strtoll(obj.c_str() + p, &endp, 10);
            return endp != obj.c_str() + p;
        };
        find_str("checksum", e.checksum);
        find_str("map_name", e.map_name);
        find_str("class_name", e.class_name);
        long long v = 0;
        if (find_int("hero_level", v)) e.hero_level = static_cast<int>(v);
        if (find_int("class_idx", v)) e.class_idx = static_cast<int>(v);
        if (find_int("map_id", v)) e.map_id = static_cast<int>(v);
        if (find_int("export_time", v)) e.export_time_ms = v;
        if (find_int("save_time", v)) e.save_time = v;
        if (!e.checksum.empty()) entries.push_back(e);
        i = close + 1;
    }
}

bool parse_auto_json(const std::string& json, BackupEntry& entry) {
    const size_t key = json.find("\"backup\":");
    if (key == std::string::npos) return false;
    const size_t open = json.find('{', key);
    if (open == std::string::npos) return false;
    const size_t close = json.find('}', open);
    if (close == std::string::npos) return false;
    std::vector<BackupEntry> parsed;
    parse_backups_json(std::string("{\"backups\":[") + json.substr(open, close - open + 1) + "]}", parsed);
    if (parsed.empty()) return false;
    entry = parsed.front();
    entry.parsed = true;
    return true;
}

struct BackupFile {
    std::string name;
    uint64_t size = 0;
    long long mtime_ms = 0;
};

void parse_backup_files_json(const std::string& json, std::vector<BackupFile>& files) {
    files.clear();
    const size_t key = json.find("\"files\":[");
    if (key == std::string::npos) return;
    size_t pos = key + std::strlen("\"files\":[");
    const size_t end = json.rfind(']');
    if (end == std::string::npos || end <= pos) return;
    while (pos < end) {
        const size_t open = json.find('{', pos);
        if (open == std::string::npos || open >= end) break;
        const size_t close = json.find('}', open);
        if (close == std::string::npos || close >= end) break;
        const std::string obj = json.substr(open, close - open + 1);
        BackupFile file;
        const size_t name_key = obj.find("\"name\":\"");
        if (name_key != std::string::npos) {
            const size_t value = name_key + std::strlen("\"name\":\"");
            const size_t quote = obj.find('"', value);
            if (quote != std::string::npos) file.name = obj.substr(value, quote - value);
        }
        auto read_number = [&](const char* field, long long& out) {
            const std::string pattern = std::string("\"") + field + "\":";
            size_t p = obj.find(pattern);
            if (p == std::string::npos) return;
            p += pattern.size();
            char* endp = nullptr;
            const long long value = std::strtoll(obj.c_str() + p, &endp, 10);
            if (endp != obj.c_str() + p) out = value;
        };
        long long size = 0;
        read_number("size", size);
        read_number("mtime_ms", file.mtime_ms);
        if (!file.name.empty() && size >= 0) {
            file.size = static_cast<uint64_t>(size);
            files.push_back(std::move(file));
        }
        pos = close + 1;
    }
}

std::string backup_cache_key(const std::string& name, uint64_t size, long long mtime_ms) {
    return name + '\n' + std::to_string(size) + '\n' + std::to_string(mtime_ms);
}

// 清除待确认状态（调用方须持 g_sbui_mtx）。
void clear_pending_locked() {
    g_dialog = DialogKind::kNone;
    g_dialog_checksum.clear();
    g_dialog_slot = -1;
    g_dialog_auto = false;
}

// Queue only visible rows and the selected slot's auto entry. Caller holds g_sbui_mtx.
void rebuild_parse_queue_locked() {
    g_parse_queue.clear();
    g_auto_loading = false;
    const int begin = g_page_index * LIST_COLS_PER_PAGE;
    const int end = std::min(begin + LIST_COLS_PER_PAGE, static_cast<int>(g_backups.size()));
    for (int i = begin; i < end; ++i) {
        const BackupEntry& item = g_backups[static_cast<size_t>(i)];
        if (!item.parsed) g_parse_queue.push_back({false, item.name, item.size, item.mtime_ms, -1});
    }
    if (g_selected_slot >= 0 && !g_auto_backup_valid && !g_auto_loading) {
        g_auto_loading = true;
        g_parse_queue.push_back({true, "", 0, 0, g_selected_slot});
    }
}

void refresh_panel_data() {
    const std::string json = save_backup_list_files_json();
    std::vector<BackupFile> files;
    parse_backup_files_json(json, files);
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    const std::string selected_name = g_selected_backup_name;
    std::map<std::string, BackupEntry> current_cache;
    std::vector<BackupEntry> items;
    items.reserve(files.size());
    for (const BackupFile& file : files) {
        const std::string key = backup_cache_key(file.name, file.size, file.mtime_ms);
        BackupEntry item;
        item.name = file.name;
        item.size = file.size;
        item.mtime_ms = file.mtime_ms;
        const auto cached = g_entry_cache.find(key);
        if (cached != g_entry_cache.end()) {
            item = cached->second;
            current_cache.emplace(key, item);
        }
        items.push_back(std::move(item));
    }
    g_entry_cache.swap(current_cache);  // Entries missing or changed on disk are evicted.
    g_backups.swap(items);
    g_selected_backup = -1;
    if (!selected_name.empty()) {
        for (size_t i = 0; i < g_backups.size(); ++i) {
            if (g_backups[i].name == selected_name) {
                g_selected_backup = static_cast<int>(i);
                break;
            }
        }
        if (g_selected_backup < 0) g_selected_backup_name.clear();
    }
    g_total_pages = static_cast<int>((g_backups.size() + LIST_COLS_PER_PAGE - 1) / LIST_COLS_PER_PAGE);
    if (g_total_pages == 0) g_page_index = 0;
    else if (g_page_index >= g_total_pages) g_page_index = g_total_pages - 1;
    g_auto_backup_valid = false;
    g_auto_loading = false;
    g_backups_loading = true;
    rebuild_parse_queue_locked();
    SB_UI_LOG("backup files enumerated: %zu files, %d pages, parse queue=%zu",
              g_backups.size(), g_total_pages, g_parse_queue.size());
}

void process_one_parse_task() {
    ParseTask task;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        if (g_parse_queue.empty()) {
            g_backups_loading = false;
            return;
        }
        task = std::move(g_parse_queue.front());
        g_parse_queue.pop_front();
    }

    BackupEntry parsed;
    bool ok = false;
    std::string response;
    if (task.is_auto) {
        response = save_backup_auto_entry_json(task.slot);
        ok = parse_auto_json(response, parsed);
    } else {
        response = save_backup_entry_json(task.name.c_str());
        ok = parse_auto_json(response, parsed);
        parsed.name = task.name;
        parsed.size = task.size;
        parsed.mtime_ms = task.mtime_ms;
    }

    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    if (task.is_auto) {
        g_auto_loading = false;
        if (task.slot == g_selected_slot) {
            g_auto_backup_valid = ok;
            if (ok) g_auto_backup = std::move(parsed);
            else SB_UI_LOG("auto backup parse failed: slot=%d", task.slot);
        }
    } else {
        auto it = std::find_if(g_backups.begin(), g_backups.end(), [&](const BackupEntry& item) {
            return item.name == task.name && item.size == task.size && item.mtime_ms == task.mtime_ms;
        });
        if (it != g_backups.end()) {
            if (ok) {
                *it = std::move(parsed);
                g_entry_cache[backup_cache_key(task.name, task.size, task.mtime_ms)] = *it;
            } else {
                SB_UI_LOG("backup entry parse failed: %s", task.name.c_str());
                if (g_selected_backup_name == task.name) {
                    g_selected_backup_name.clear();
                    g_selected_backup = -1;
                }
                g_backups.erase(it);
                g_total_pages = static_cast<int>((g_backups.size() + LIST_COLS_PER_PAGE - 1) / LIST_COLS_PER_PAGE);
                if (g_total_pages == 0) g_page_index = 0;
                else if (g_page_index >= g_total_pages) g_page_index = g_total_pages - 1;
                g_selected_backup = -1;
                if (!g_selected_backup_name.empty()) {
                    for (size_t i = 0; i < g_backups.size(); ++i) {
                        if (g_backups[i].name == g_selected_backup_name) {
                            g_selected_backup = static_cast<int>(i);
                            break;
                        }
                    }
                    if (g_selected_backup < 0) g_selected_backup_name.clear();
                }
                rebuild_parse_queue_locked();
            }
        }
    }
    g_backups_loading = !g_parse_queue.empty();
}

// 读三槽元数据到 out（无共享状态，锁外调用；必须在游戏线程执行，见 refresh_slots_on_game_thread）。
// 以磁盘文件为准，不读运行期槽状态：备份面板打开期间游戏槽结构可能尚未重载（导入后尤其明显），
// 用槽状态会把可读存档误报成「损坏」。判定：save{slot}.dat 可读 → 有效并解析明文；
// 文件存在但不可读 → 损坏；文件不存在 → 空。等级/职业/地图从存档明文解析（与导出同源）。
void read_slots_into(SlotInfo out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = SlotInfo{};
    const auto* facts = save_backup_save_facts();
    const auto* save_call = save_backup_save_call();
    const bool can_load = facts != nullptr && facts->load_data != nullptr &&
                          save_call != nullptr && save_call->write.free_data != nullptr;
    for (int i = 0; i < 3; ++i) {
        void* buf = nullptr;
        int len = 0;
        const bool load_ok =
            can_load && qol::contracts::succeeded(facts->load_data(i, &buf, &len)) &&
            buf != nullptr && len > 0;
        if (!load_ok) {
            if (buf != nullptr) (void)save_call->write.free_data(buf);
            // 读不到时用文件存在性区分空槽与损坏文件（只读目录扫描，不触发游戏槽重载）。
            out[i].state = save_backup_slot_file_exists(i) ? 1 : 0;
            continue;
        }
        out[i].state = 2;
        out[i].exists = true;
        int32_t value = 0;
        if (facts->slot_hero_index != nullptr &&
            qol::contracts::succeeded(facts->slot_hero_index(i, &value))) {
            out[i].hero_index = value;
        }
        const std::vector<uint8_t> plain(static_cast<const uint8_t*>(buf),
                                         static_cast<const uint8_t*>(buf) + len);
        int map_id = -1;
        int level = -1;
        int class_idx = -1;
        const bool map_ok = ::save_backup::parse_plain_map_id(plain, map_id);
        const bool char_ok = ::save_backup::parse_plain_main_character(plain, level, class_idx);
        if (map_ok) out[i].map_id = map_id;
        if (char_ok) {
            out[i].hero_level = level;
            out[i].class_idx = class_idx;
        }
        (void)save_call->write.free_data(buf);
        // 明文摘要解析失败时回退运行期槽结构（只用于展示；导出仍以明文为准）。
        if (!map_ok && facts->map_id != nullptr &&
            qol::contracts::succeeded(facts->map_id(i, &value))) {
            out[i].map_id = value;
        }
        if (!char_ok) {
            if (facts->hero_level != nullptr &&
                qol::contracts::succeeded(facts->hero_level(i, &value))) {
                out[i].hero_level = value;
            }
            if (facts->hero_class != nullptr &&
                qol::contracts::succeeded(facts->hero_class(i, &value))) {
                out[i].class_idx = value;
            }
        }
    }
}

// 槽位刷新：必须在游戏线程调用。SAVE_CreateSaveSlot 会重载三槽并走原版保存链，
// 从后台线程调用会因游戏线程上下文/全局竞态崩溃；结果存入 g_slots 缓存供 DrawProc 读。
void refresh_slots_on_game_thread() {
    SlotInfo slots[3];
    read_slots_into(slots);
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    for (int i = 0; i < 3; ++i) g_slots[i] = slots[i];
}

const BackupEntry* backup_at_page(int local_index) {
    const int abs = g_page_index * LIST_COLS_PER_PAGE + local_index;
    if (abs < 0 || abs >= static_cast<int>(g_backups.size())) return nullptr;
    const BackupEntry* item = &g_backups[static_cast<size_t>(abs)];
    return item->parsed ? item : nullptr;
}

const BackupEntry* backup_item_at_page(int local_index) {
    const int abs = g_page_index * LIST_COLS_PER_PAGE + local_index;
    if (abs < 0 || abs >= static_cast<int>(g_backups.size())) return nullptr;
    return &g_backups[static_cast<size_t>(abs)];
}

const BackupEntry* selected_manual_backup() {
    return g_selected_backup >= 0 && g_selected_backup < static_cast<int>(g_backups.size())
               && g_backups[static_cast<size_t>(g_selected_backup)].parsed
               ? &g_backups[static_cast<size_t>(g_selected_backup)] : nullptr;
}

// ============== Component 绘制 ==============
void slot_btn_draw(void* ctrl);
void ops_btn_draw(void* ctrl);
void list_btn_draw(void* ctrl);
void auto_btn_draw(void* ctrl);
void sync_component_enabled_locked();
void page_btn_draw(void* ctrl);
void root_decor_draw();

// ============== 几何（控件 rect）==============
UiRect panel_root_rect();
UiRect slot_btn_rect(int i);
UiRect ops_btn_rect(int i);
UiRect list_btn_rect(int i);
UiRect auto_btn_rect();
int page_entry_count();

// ============== 面板回调 ==============
void panel_enter_impl();
void panel_process_impl();
void panel_f3_impl();
void panel_f4_impl();
uint32_t panel_event_impl(uint64_t event, uint64_t param, uint64_t param2);

#include "ui/panel.inc"
#include "ui/render.inc"
#include "ui/injection.inc"

}  // namespace

// PanelCallbacks 需要外部链接：匿名 namespace 内的实现经这组薄包装交给 PopupHost。
void savebackup_panel_enter() { panel_enter_impl(); }
void savebackup_panel_process() { panel_process_impl(); }
void savebackup_panel_f3() { panel_f3_impl(); }
void savebackup_panel_f4() { panel_f4_impl(); }
uint32_t savebackup_panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    return panel_event_impl(event, param, param2);
}

void savebackup_set_ui_components(const contracts::QolUiComponents* components) {
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    g_ui_components = components;
}

void savebackup_set_ui_confirmation_api(const contracts::UiConfirmationApi* api) {
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    g_ui_confirmation_api = api;
}

// ============== PanelProviderSpec ==============

// PopupState 占用、push 与 set_process_info 由 core.custom_ui 的 PopupHost 负责
// （provider 携带 callbacks 时 host 在 open 中完成），provider 只判断本面板能否打开。
bool savebackup_provider_open() {
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    return g_ui_components != nullptr && g_ui_confirmation_api != nullptr;
}

void savebackup_provider_close() {
    close_active_confirmation();
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    clear_pending_locked();
    g_msg_text.clear();
    g_panel_active = false;
    // 控件树由随后游戏弹出的 f3 销毁（与旧流程一致）；close 不直接 pop，pop 由 host 执行。
    SB_UI_LOG("provider close: panel inactive");
}

bool savebackup_provider_active() {
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    return g_panel_active;
}

const char* savebackup_provider_block_reason() {
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    return g_panel_active ? "ui occupied: backup panel" : nullptr;
}

// ============== 对外 API（原 data_savebackup_ui_*）==============

std::string data_savebackup_ui_inject() {
    // PopupState 注入已移交 core.custom_ui 的 PopupHost：打开面板时才 acquire。
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    if (g_ui_components == nullptr) return save_backup_result_error("ui components unavailable");
    return save_backup_result_ok();
}

std::string data_savebackup_ui_status_json() {
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    char buf[640];
    snprintf(buf, sizeof(buf),
        "{"
        "\"panel_active\":%s,\"root\":\"%p\","
        "\"selected_slot\":%d,\"selected_backup\":%d,"
        "\"page_index\":%d,\"total_pages\":%d,\"backup_count\":%zu,"
        "\"msg\":\"%s\",\"msg_is_error\":%s,"
        "\"screen\":\"%s\"}",
        g_panel_active ? "true" : "false", g_root,
        g_selected_slot, g_selected_backup,
        g_page_index, g_total_pages, g_backups.size(),
        g_msg_text.c_str(), g_msg_is_error ? "true" : "false",
        ui_screen_name());
    return std::string(buf);
}

std::string data_savebackup_ui_restore() {
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    g_panel_active = false;
    return save_backup_result_ok();
}

std::string data_savebackup_ui_open_panel() {
    const contracts::QolHostSdk* sdk = save_backup_host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) {
        return save_backup_result_error("ui_panels unavailable");
    }
    const void* iface = nullptr;
    if (!ui_ok(sdk->capabilities.require(
            contracts::kUiPanelsCapabilityId,
            {static_cast<uint16_t>(contracts::kUiPanelsVersionMajor),
             static_cast<uint16_t>(contracts::kUiPanelsVersionMinor)},
            &iface))) {
        return save_backup_result_error("ui_panels unavailable");
    }
    const auto* api = static_cast<const contracts::UiPanelsApi*>(iface);
    if (api->open == nullptr) return save_backup_result_error("ui_panels unavailable");
    return api->open("save_backup") ? save_backup_result_ok()
                                     : save_backup_result_error("panel open failed");
}

std::string data_savebackup_ui_self_check() {
    // Diagnostics report the current cache without triggering filesystem work or bundle parsing.
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"backup_count\":%zu,\"total_pages\":%d,\"page_index\":%d}",
        g_backups.size(), g_total_pages, g_page_index);
    return std::string(buf);
}

void save_backup_inject_map_names_for_test(const char* json) {
    save_backup_set_map_names(json);
}

}  // namespace qol::features::save_backup

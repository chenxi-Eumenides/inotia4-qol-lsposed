#include "game_ui_savebackup.h"

#include "feature/save_backup/save_backup.h"
#include "game_access.h"
#include "game_ops_common.h"
#include "game_ptr_hook.h"
#include "game_state.h"
#include "game_symbols.h"
#include "game_ui.h"
#include "game_ui_kit.h"
#include "game_ui_settings.h"

#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define SB_UI_TAG "Inotia4UISaveBackup"
#define SB_UI_LOG(...) __android_log_print(ANDROID_LOG_INFO, SB_UI_TAG, __VA_ARGS__)

#define POPUP_STATE_SIZE 0x40
#define CB_TEXT_SIZE 0x40

// 面板布局（与 game_ui_settings 同构：逻辑坐标 0-960×0-640 空间，root 相对全屏居中）
// 留出顶部 title 区域 + 底部 hint/状态栏 + 三栏主体。
#define ROOT_W 0x3c0
#define ROOT_H 0x280
#define TITLE_H 0x38
#define HINT_H 0x38
#define CONTENT_X 0x18
#define CONTENT_W (ROOT_W - 0x30)
#define CONTENT_Y TITLE_H
#define CONTENT_H (ROOT_H - TITLE_H - HINT_H)
// 三栏：左（槽位）、中（操作）、右（备份列表）；栏间分隔线 0x06。
// 基址链：左栏固定 CONTENT_X 起 → 中栏 OPS_COL_X 在左栏后 → 右栏 LIST_COL_X 在中栏后；
// LIST_COL_W = 右栏起点到内容区右缘（CONTENT_X + CONTENT_W），保证不溢出。
#define SLOT_COL_W 0x90
#define OPS_COL_W 0x58
#define GAP 0x06
#define OPS_COL_X (CONTENT_X + SLOT_COL_W + GAP)
#define LIST_COL_X (OPS_COL_X + OPS_COL_W + GAP)
#define LIST_COL_W (CONTENT_X + CONTENT_W - LIST_COL_X)
// 左栏 3 槽纵列；中栏 3 操作按钮纵列；右栏每列 4 项 + 翻页头尾。
#define SLOT_ROW_H 0x52
#define SLOT_Y (CONTENT_Y + 0x06)
#define OPS_BTN_H 0x40
#define OPS_BTN_W 0x50   // 与 OPS_COL_W(0x58) 留 4px 边距；保证两行 hint 文字不溢出中栏
#define OPS_BTN_GAP 0x0a
#define OPS_Y0 (CONTENT_Y + 0x10)
#define LIST_ROW_H 0x3c
#define LIST_COLS_PER_PAGE 4
#define LIST_COLS_MAX 64
// 翻页按钮高度 = 列高；按钮宽 0x60
#define LIST_PAGE_BTN_H LIST_ROW_H
#define LIST_PAGE_BTN_W 0x60

// 颜色（ABGR：与扩展背包一致，0xFF00B4D7=暗金描边）
#define COLOR_BG 0xFF181012
#define COLOR_PANEL 0xFF241C18
#define COLOR_BORDER 0xFFB49067
#define COLOR_TEXT 0xFFE8D6B8
#define COLOR_TEXT_DIM 0xFF98816A
#define COLOR_HIGHLIGHT 0xFF655444
#define COLOR_HIGHLIGHT_SEL 0xFFB49067
#define COLOR_OK 0xFFB0E89C
#define COLOR_ERR 0xFFE89C9C
#define COLOR_INFO 0xFF9CC2E8

namespace {

// ============== 全局状态 ==============
std::mutex g_sbui_mtx;
std::atomic<bool> g_thread_started{false};

bool g_panel_active = false;
uint8_t* g_state_entry = nullptr;
uint8_t g_state_backup[POPUP_STATE_SIZE] = {0};
int g_state_id = -1;
void* g_root = nullptr;
int g_back_pressed = 0;
int g_close_delay_frames = 0;
const char* kTitle = "存档备份";

// 三栏控件容器（label 类型，参与命中但 DrawProc 自绘；DrawProc 走 fn_ctrl_btn_draw → 自定义 cb）。
void* g_slot_btns[3] = {nullptr, nullptr, nullptr};
void* g_ops_btns[3] = {nullptr, nullptr, nullptr};   // 0=导出 1=导入 2=删除
void* g_list_btns[LIST_COLS_MAX] = {nullptr};       // 当前页 4 个
void* g_prev_btn = nullptr;
void* g_next_btn = nullptr;
void* g_back_btn = nullptr;                          // 底部「← 返回」：关闭面板
void* g_msg_label = nullptr;                         // 操作结果提示区
int g_selected_slot = 0;                             // 左栏选中槽 0/1/2
int g_selected_backup = -1;                          // 右栏当前页选中项下标（0..page_size-1）；-1=未选
int g_page_index = 0;                                // 翻页：0..N
// 备份列表缓存（从 save_backup_list_json 解析）；每项 checksum + hero_level + map_name + export_time。
// map_name 由 entry_json 输出携带（Kotlin 启动期下发的 map_id→中文名表）；未命中为空串。
struct BackupEntry {
    std::string checksum;
    std::string map_name;
    int hero_level = -1;
    int map_id = 0;
    long long export_time_ms = 0;
    long long save_time = 0;
};
std::vector<BackupEntry> g_backups;
int g_total_pages = 0;
// 结果提示文本（不弹窗，在面板内独立显示区；新消息覆盖旧消息，不自动消失）。
std::string g_msg_text;
uint32_t g_msg_color = COLOR_TEXT;
bool g_msg_is_error = false;
// 面板内两步确认状态（导入/删除不依赖原版 YesNo 弹窗——自定义面板下该弹窗不渲染）。
// 0=无待确认 1=待确认导入 2=待确认删除；与目标备份 checksum + 目标槽绑定：
// 第二次点击同操作按钮且 checksum/槽均一致才执行，否则按新操作重新武装。
// 任何其它交互（切换槽位/选中、翻页、其它操作按钮）及列表刷新（apply_backups_locked）
// 都会清除待确认（列表刷新后 checksum 可能失效）。
int g_pending_op = 0;
std::string g_pending_checksum;
int g_pending_slot = -1;
// 左栏三槽缓存：enter 后由后台线程读 fn_save_get_save_slot / fn_saveslot_get_hero /
// C_LEVEL 填充（见 read_slots_into），DrawProc 只读缓存，不再直接读游戏内存。
struct SlotInfo {
    bool exists = false;
    int hero_level = -1;
    int hero_index = -1;
};
SlotInfo g_slots[3];
// 备份列表/槽位后台刷新状态：IO（save_backup_list_json + 主菜单重载三槽）全部在
// 后台线程锁外执行，完成后一次性拿 g_sbui_mtx 应用，游戏线程 enter/process 永不等 IO。
std::atomic<bool> g_refresh_running{false};
std::atomic<bool> g_refresh_queued{false};
std::atomic<bool> g_backups_loading{false};  // 右栏空列表时显示「加载中…」

// ============== 控件 ID（ExecuteProc 复用）==============
struct ClickId {
    int kind;       // 0=slot, 1=ops, 2=list, 3=prev, 4=next
    int index;
};
// 三栏控件共用同一 UiClickProc（点击后用 ctrl 指针反查 g_slot_btns/g_ops_btns/g_list_btns 识别）。
void on_btn_clicked(void* ctrl);
void on_back_clicked(void* ctrl);

// 设置页互斥引用：通过 settings 模块函数询问其面板是否激活。
// settings_panel_active_for_savebackup 已在 game_ui_settings.h（全局命名空间）声明，
// 此处位于匿名命名空间内，unqualified lookup 仍可解析到全局版本。

// 解析 save_backup_list_json() 输出到 entries（纯 CPU，无共享状态，锁外调用）。
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
        long long v = 0;
        if (find_int("hero_level", v)) e.hero_level = static_cast<int>(v);
        if (find_int("map_id", v)) e.map_id = static_cast<int>(v);
        if (find_int("export_time", v)) e.export_time_ms = v;
        if (find_int("save_time", v)) e.save_time = v;
        if (!e.checksum.empty()) entries.push_back(e);
        i = close + 1;
    }
}

// 清除待确认状态（调用方须持 g_sbui_mtx）。
void clear_pending_locked() {
    g_pending_op = 0;
    g_pending_checksum.clear();
    g_pending_slot = -1;
}

// 应用刷新结果（调用方须持 g_sbui_mtx）。
void apply_backups_locked(std::vector<BackupEntry>& entries) {
    g_backups.swap(entries);
    g_selected_backup = -1;
    // 列表已重建：旧 pending 绑定的 checksum 可能已失效（备份被删/改），一并清除。
    clear_pending_locked();
    int total = static_cast<int>(g_backups.size());
    g_total_pages = (total + LIST_COLS_PER_PAGE - 1) / LIST_COLS_PER_PAGE;
    if (g_page_index >= g_total_pages && g_total_pages > 0) g_page_index = g_total_pages - 1;
    if (g_page_index < 0) g_page_index = 0;
}

// 读三槽元数据到 out（无共享状态，锁外调用；通常在后台线程执行）。
// 口径与 save_backup.cpp:read_hero_meta / api/native/game_save.cpp:data_save_slots_json 一致：
// 主菜单（STATE==4）下 SAVE_CreateSaveSlot 会重载三槽并覆盖角色相关全局，仅此状态调用；
// 其余状态只读现有槽结构。feature 侧只经 game_access.h 的 fn_* 与 game_symbols.h 常量访问游戏。
void read_slots_into(SlotInfo out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = SlotInfo{};
    if (fn_save_get_save_slot == nullptr || fn_saveslot_get_hero == nullptr) return;
    if (g_state != nullptr && *reinterpret_cast<uint16_t*>(g_state) == 4 &&
        fn_save_create_save_slot != nullptr) {
        fn_save_create_save_slot();
    }
    for (int i = 0; i < 3; ++i) {
        void* slot = fn_save_get_save_slot(i);
        if (slot == nullptr) continue;
        uint8_t* p = reinterpret_cast<uint8_t*>(slot);
        if (p[SAVESLOT_EXISTS] == 0) continue;
        out[i].exists = true;
        out[i].hero_index = static_cast<int8_t>(
            *reinterpret_cast<const int8_t*>(p + SAVESLOT_HERO_INDEX));
        void* hero = fn_saveslot_get_hero(slot);
        if (hero == nullptr) continue;
        out[i].hero_level = static_cast<int8_t>(*reinterpret_cast<const int8_t*>(
            reinterpret_cast<uint8_t*>(hero) + C_LEVEL));
    }
}

// 后台刷新备份列表（仅目录 IO，锁外；应用阶段才拿 g_sbui_mtx）。
// 注意：槽位刷新不在此线程执行——SAVE_CreateSaveSlot 走原版保存链（SAVE_LoadData→HubSave_*），
// 必须在游戏线程调用（见 refresh_slots_on_game_thread）。从后台线程调用曾真机 SIGSEGV
// （tombstone: HubSave_GetFolderName 空指针）。已在刷新时置 queued，本轮结束后补一轮。
void refresh_panel_data_async() {
    bool expected = false;
    if (!g_refresh_running.compare_exchange_strong(expected, true)) {
        g_refresh_queued.store(true);
        return;
    }
    std::thread([]() {
        for (;;) {
            g_backups_loading.store(true);
            const auto t0 = std::chrono::steady_clock::now();
            const std::string json = save_backup_list_json();
            std::vector<BackupEntry> entries;
            parse_backups_json(json, entries);
            const auto t1 = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(g_sbui_mtx);
                apply_backups_locked(entries);
            }
            g_backups_loading.store(false);
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            SB_UI_LOG("panel data refresh: %zu backups, io %.1f ms",
                      entries.size(), ms);
            g_refresh_running.store(false);
            if (!g_refresh_queued.exchange(false)) break;
            bool again = false;
            if (g_refresh_running.compare_exchange_strong(again, true)) continue;
            break;
        }
    }).detach();
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
    return &g_backups[abs];
}

void set_msg_locked(const char* text, uint32_t color, bool is_error) {
    g_msg_text = text != nullptr ? text : "";
    g_msg_color = color;
    g_msg_is_error = is_error;
}

bool settings_panel_active() { return settings_panel_active_for_savebackup(); }

// ============== DrawProc（控件内容绘制）==============
void slot_btn_draw(void* ctrl);
void ops_btn_draw(void* ctrl);
void list_btn_draw(void* ctrl);
void page_btn_draw(void* ctrl);
void msg_label_draw(void* ctrl);
void back_btn_draw(void* ctrl);
void root_decor_draw();

// ============== 几何（控件 rect）==============
UiRect panel_root_rect();
UiRect slot_btn_rect(int i);
UiRect ops_btn_rect(int i);
UiRect list_btn_rect(int i);
UiRect page_btn_rect(bool next);
UiRect msg_label_rect();
UiRect back_btn_rect();

// ============== PopupState 注入 ==============
uint8_t* find_state_entry(uintptr_t enter_vma);
bool inject_state_entry_locked();
void restore_state_entry_locked();
void ensure_inject_thread();

void savebackup_panel_enter();
void savebackup_panel_process();
void savebackup_panel_f3();
void savebackup_panel_f4();
uint32_t savebackup_panel_event(uint64_t event, uint64_t param, uint64_t param2);

#include "game_ui_savebackup_panel.inc"
#include "game_ui_savebackup_render.inc"
#include "game_ui_savebackup_injection.inc"

}  // namespace

// ============== 对外 API ==============
std::string data_savebackup_ui_inject() {
    if (!inject_state_entry_locked()) return op_err("state entry inject failed");
    ensure_inject_thread();
    return op_ok();
}

std::string data_savebackup_ui_status_json() {
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    char buf[768];
    snprintf(buf, sizeof(buf),
        "{"
        "\"state_id\":%d,\"state_injected\":%s,"
        "\"panel_active\":%s,\"root\":\"%p\","
        "\"selected_slot\":%d,\"selected_backup\":%d,"
        "\"page_index\":%d,\"total_pages\":%d,\"backup_count\":%zu,"
        "\"msg\":\"%s\",\"msg_is_error\":%s,"
        "\"screen\":\"%s\"}",
        g_state_id, g_state_entry != nullptr ? "true" : "false",
        g_panel_active ? "true" : "false", g_root,
        g_selected_slot, g_selected_backup,
        g_page_index, g_total_pages, g_backups.size(),
        g_msg_text.c_str(), g_msg_is_error ? "true" : "false",
        data_ui_screen());
    return std::string(buf);
}

std::string data_savebackup_ui_restore() {
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    restore_state_entry_locked();
    g_panel_active = false;
    g_close_delay_frames = 0;
    g_back_pressed = 0;
    return op_ok();
}

std::string data_savebackup_ui_open_panel() {
    if (!inject_state_entry_locked()) return op_err("state entry inject failed");
    if (fn_ui_set_popup_process_info == nullptr) return op_err("symbol not resolved");

    // 优先尝试嵌套 push（保持设置页 keep open 在栈下方）。
    // 真机实测：fn_popupstate_push(0x122424) 内部 Array_Add 失败但 GetTop 返回当前栈顶
    // 地址（设置页 entry），导致原代码 rc==0 当 success 的判断逻辑误报失败，
    // 而实际栈顶未切换为 savebackup entry。修复：
    //   1) rc != 0 才视为 push 成功（按反汇编：rc=0 = enter NULL / push 内部失败）
    //   2) 校验栈顶 enter_func 必须是 &savebackup_panel_enter（防止 GetTop 返回旧栈顶）
    if (fn_popupstate_push != nullptr) {
        const int rc = fn_popupstate_push(static_cast<uint32_t>(g_state_id));
        SB_UI_LOG("open save backup panel via push: state=%d rc=0x%x", g_state_id, rc);
        if (rc != 0 && is_savebackup_at_stack_top()) {
            return op_ok();
        }
        SB_UI_LOG("push didn't switch stack top (rc=0x%x); fallback to switch mode", rc);
    }

    // 切换式兜底（game_ui_savebackup.h 注释约定的退化路径）：
    //   1) settings_ui_close_panel 清理设置页 panel 状态 + 还原注入的 state entry
    //   2) fn_ui_set_popup_process_info(3, 0) 关闭 settings UI（push state 0 + clear draw flag，
    //      即游戏内部官方 close panel 路径，data_op_panel_close 已验证）
    //   3) fn_ui_set_popup_process_info(1, g_state_id) 异步 push savebackup 到 popup_array
    // 主循环 UI_PopupProcess 按 FIFO 消费 popup_array：先关再开。
    // 副作用：settings_panel_f3 被调用清状态，settings 不在栈中保留；
    //        关闭 backup panel 后回到主菜单（不再回设置页），符合"嵌套 push 失败时退化"约定。
    settings_ui_close_panel();
    fn_ui_set_popup_process_info(3, 0);
    fn_ui_set_popup_process_info(1, g_state_id);
    SB_UI_LOG("open save backup panel via switch: state=%d", g_state_id);
    return op_ok();
}

std::string data_savebackup_ui_self_check() {
    // 进入面板前由 native 一次性读目录，验证 list 链路。
    // IO 在 g_sbui_mtx 外执行：目录遍历 + 逐 bundle 读文件/校验可能秒级（冷 IO 实测），
    // 持锁做 IO 会阻塞游戏线程 enter（v0.7.x 面板打开卡 13s 的直接原因之一）。
    const auto t0 = std::chrono::steady_clock::now();
    const std::string json = save_backup_list_json();
    std::vector<BackupEntry> entries;
    parse_backups_json(json, entries);
    const auto t1 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    apply_backups_locked(entries);
    const auto t2 = std::chrono::steady_clock::now();
    SB_UI_LOG("self_check: list+parse %.1f ms, apply %.3f ms, %zu backups",
              std::chrono::duration<double, std::milli>(t1 - t0).count(),
              std::chrono::duration<double, std::milli>(t2 - t1).count(),
              g_backups.size());
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"backup_count\":%zu,\"total_pages\":%d,\"page_index\":%d}",
        g_backups.size(), g_total_pages, g_page_index);
    return std::string(buf);
}

void save_backup_inject_map_names_for_test(const char* json) {
    save_backup_set_map_names(json);
}

const char* savebackup_ui_block_reason() {
    if (g_state_entry == nullptr) return nullptr;  // 未注入 → 不阻塞
    if (g_panel_active) return "ui occupied: backup panel";
    if (settings_panel_active()) return "ui occupied: settings panel";
    return nullptr;
}

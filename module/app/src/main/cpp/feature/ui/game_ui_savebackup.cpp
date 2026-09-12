#include "game_ui_savebackup.h"

#include "feature/save_backup/save_backup.h"
#include "game_access.h"
#include "game_ops_common.h"
#include "game_ptr_hook.h"
#include "game_state.h"
#include "game_symbols.h"
#include "game_ui.h"
#include "game_ui_components.h"
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
// 视觉与设置页同一套：顶部居中消息/标题区（同设置页 addr 标签位）+ 左上角「返回」
// 原版贴图按钮（屏幕 (8,8)，同设置页位置/尺寸）；主体左（存档框）/中（操作按钮，无组框，
// 居中于左右栏之间）/右（备份列表框）三栏，左右两栏同宽、右缘=内容区右缘；
// 三栏沿同一水平中线垂直居中（左框/中按钮组按各自高度上下居中，见 CONTENT_MID_Y）。
#define ROOT_W 0x3c0
#define ROOT_H 0x280
#define CONTENT_X 0x18
#define CONTENT_W (ROOT_W - 0x30)
#define CONTENT_Y 0x60
// 三栏：左、右两栏同宽 W，中栏居中于两栏之间（两侧等 GAP=0x0A，间隔加宽）。
//   W 由内容区总宽分配：2W + OPS_COL_W + 2*GAP = CONTENT_W
//     → W = (912 - 88 - 20) / 2 = 402 = 0x192（整除，无需再取整）。
//   字宽（libgame 0xa72ec 字形循环 / 0xa48d4 测宽反汇编：font=1 ASCII=6、空格=4、CJK=9
//   原生单位；原生 480 宽 UI 对应本面板 960 逻辑宽，故 ×2）。
//   现正文改 font=3（见下），宽度按字号比 28/24 放大；最长「职业名 Lv<L> <地图名>」
//   （4 CJK 职业 + 16 字节地图名）在本模型下 ≈ 128 原生 ×2 ×(28/24) ≈ 299 逻辑 px，
//   按钮可用宽 = (W-0x08) - 0x08 左内距 - 0x04 右边距 = 382 px，仍富余；
//   渲染前仍按估算宽度截断地图名（render.inc est/truncate 助手）以防极端名；来源先截 16 字节。
// 链式验算（单位 px）：
//   SLOT 右缘  = 0x18 + 0x192 = 0x1AA
//   OPS_COL_X  = 0x1AA + 0x0A = 0x1B4；中栏 [0x1B4, 0x20C]，中心 0x1E0 = 内容区中心
//              （CONTENT_X + CONTENT_W/2 = 0x18 + 0x1C8 = 0x1E0）✓
//   LIST_COL_X = 0x1B4 + 0x58 + 0x0A = 0x216
//   LIST_COL_X + W = 0x216 + 0x192 = 0x3A8 = CONTENT_X + CONTENT_W（右框右缘=内容区右缘）✓
//   三栏间隙均为 GAP=0x0A，互不重叠、不越界 ✓
#define GAP 0x0A
#define OPS_COL_W 0x58
#define COL_W 0x192
#define SLOT_COL_W COL_W
#define LIST_COL_W COL_W
#define SLOT_COL_X CONTENT_X
#define OPS_COL_X (SLOT_COL_X + SLOT_COL_W + GAP)
#define LIST_COL_X (OPS_COL_X + OPS_COL_W + GAP)
// 正文字号：fn_grpx_draw_string_with_font 第 5 参 font = 游戏字体表下标。GRPX_CreateFont
// (0x8f5f4) 用同一字体族创建 5 个字号：size={20,24,38,28,14}（vaddr 0x243c00 的
// GRPX_GetFontSize 表：0..3），行高表 [0x2f5968]={22,26,40,30,16}。左栏与右栏正文统一
// font=3（28/行高30）：比 font=1(24) 大，且比上一版 font=2(38) 收敛以适配更矮的行；
// 同一字体族 → CJK 字形覆盖相同。中栏/翻页/标题/返回保持 font=1。
#define SB_TEXT_FONT 3
#define SB_FONT_H 0x1E      // font=3 行高 30（GRPX_GetFontHeight 表），用于垂直居中预算（最坏）
// 右栏两行文本的 y 间隔：两行块高 = 间隔 + 行高。
#define LIST_LINE_GAP 0x1C  // 0x1C(28)+0x1E(30)=0x3A(58) ≤ 按钮高 0x42(66)，居中 offset=4
// 三栏垂直：右框最高（FRAME_H），以其中心为公共水平中线；左框/中栏按各自高度居中。
//   框线内边界（draw_frame_box）：ui_draw_panel_decor 上边 [frame.y, frame.y+3)、
//   下边 [frame.y+h-3, frame.y+h)；ui_draw_vertical_line 左边 [x, x+3)、右边 [x+w-3, x+w)
//   （3px，左闭右开）。故框内可用区 y ∈ [frame.y+3, frame.y+h-3)，高 h-6；左右各内缩 3px。
//   首/末行紧贴框内边缘：行按钮高 = ROW_H - ROW_GAP（行间仅留 ROW_GAP=4 缝隙），
//     首行顶 = frame.y + FRAME_BORDER；末行底 = frame.y+3 + n*ROW_H - ROW_GAP；
//     令其 = frame.y + h - 3 → h = n*ROW_H + 2*FRAME_BORDER - ROW_GAP。
//   故 FRAME_H = 5*0x46 + 6 - 4 = 0x160(352)；SLOT_FRAME_H = 3*0x48 + 6 - 4 = 0xDA(218)。
//   右框 5 行：pitch 0x46、按钮高 0x42(66)；左框 3 槽：pitch 0x48、按钮高 0x44(68)。
//   FRAME_Y 下移：0x60 → 0x68（CONTENT_Y+8）；标题/消息保持绝对 y=0x18 不动。
#define FRAME_Y (CONTENT_Y + 0x08)
#define FRAME_BORDER 0x03
#define LIST_ROW_H 0x46               // 70
#define LIST_ROW_GAP 0x04             // 行间缝隙；按钮高 = 0x46-4 = 0x42(66)
#define FRAME_H (5 * LIST_ROW_H + 2 * FRAME_BORDER - LIST_ROW_GAP)  // = 0x160(352)
#define SLOT_ROW_H 0x48               // 72
#define SLOT_ROW_GAP 0x04             // 按钮高 = 0x48-4 = 0x44(68)
#define SLOT_FRAME_H (3 * SLOT_ROW_H + 2 * FRAME_BORDER - SLOT_ROW_GAP)  // = 0xDA(218)
#define LIST_COLS_PER_PAGE 5
#define LIST_COLS_MAX 64
// 公共水平中线 = 右框中心；左框顶 SLOT_FRAME_Y / 中栏顶 OPS_Y0 据此推出。
//   CONTENT_MID_Y = FRAME_Y + FRAME_H/2 = 0x68 + 0xB0 = 0x118(280)
//   SLOT_FRAME_Y  = CONTENT_MID_Y - SLOT_FRAME_H/2 = 0x118 - 0x6D = 0xAB(171)
//   OPS_Y0        = CONTENT_MID_Y - (3*0x40 + 2*0x0A)/2 = 0x118 - 0x6A = 0xAE(174)
//   三者中心：0xAB+0x6D = 0xAE+0x6A = 0x68+0xB0 = 0x118 ✓
//   左框底 = 0xAB+0xDA = 0x185 < 右框底 0x68+0x160 = 0x1C8；左框与右框互不重叠（异 x）。
#define CONTENT_MID_Y (FRAME_Y + FRAME_H / 2)
#define SLOT_FRAME_Y (CONTENT_MID_Y - SLOT_FRAME_H / 2)
// 中栏 3 操作按钮：无组框（每个按钮 ui_custom::draw_button 自带 2px 边框），
// 按钮组按公共中线垂直居中。
#define OPS_BTN_H 0x40
#define OPS_BTN_W 0x50   // 与 OPS_COL_W(0x58) 留 4px 边距；单行标签不溢出中栏
#define OPS_BTN_GAP 0x0a
#define OPS_Y0 (CONTENT_MID_Y - (3 * OPS_BTN_H + 2 * OPS_BTN_GAP) / 2)
// 翻页按钮：右栏框下方一行，prev 左对齐 / next 右对齐（互不重叠）；
// 页码指示（如「1/2」）画在两按钮正中间（LIST_COL_X + LIST_COL_W/2），只画文字无框。
//   翻页行底 = PAGE_ROW_Y+0x30 = (0x68+0x160+0x08)+0x30 = 0x200 = 512 (root.y=0) < ROOT_H 0x280 ✓
//   prev x ∈ [LIST_COL_X+0x04, +0x7C]，next x ∈ [LIST_COL_X+0x116, +0x18E]（W=0x192）；
//   两按钮内缘间隙 [X+0x7C, X+0x116]，正中 = X+0xC9 = LIST_COL_X+LIST_COL_W/2。
#define LIST_PAGE_BTN_H 0x30
#define LIST_PAGE_BTN_W 0x78
#define PAGE_ROW_Y (FRAME_Y + FRAME_H + 0x08)
// 顶部消息/标题区：与设置页 addr 标签同高（settings.cpp ADDR_Y=0x18，label rect 高 ADDR_H=0x28）。
//   MSG_Y + root.y == 0x18（root.y=0 → MSG_Y=0x18）；文本绘制偏移同 settings address_draw 的 0x18。
#define MSG_X (ROOT_W / 2 - MSG_W / 2)
#define MSG_Y 0x18
#define MSG_W 0x120
#define MSG_H 0x28
// 返回按钮：与 game_ui_settings 完全同尺寸（原版 option 贴图量级），屏幕左上 (8,8)。
#define BACK_BTN_W 0x4a
#define BACK_BTN_H 0x51
// 背景贴图 unit/loc：与 game_ui_settings 同值（title background 左右两半）。
#define TITLE_BACKGROUND_LEFT_UNIT 0x4f
#define TITLE_BACKGROUND_RIGHT_UNIT 0x50
#define TITLE_BACKGROUND_LOC 0x0

// 颜色（ABGR 0xAABBGGRR，文字/框线/按钮用）：与 game_ui_settings 同一套视觉语言——
// UI_GOLD 金色 = 分割线/框线/次要文本/左栏有档槽文本/右栏首行文本/中栏可用按钮文字；
// UI_OPTION_TEXT = 正文文字；COLOR_TEXT_WHITE = 右栏次行（时间+校验）；
// COLOR_TEXT_GRAY = 左栏空槽文本 / 中栏不可用按钮文字。
//
// 注意：选中底色走 ui_fill_rect_alpha → GRPX_FillRectAlpha，颜色格式与上面不同！
// 反汇编 libgame.so 0x8fccc：color(w4)/alpha(w5) 经 GRPX_GetColorFromGRPWithAlpha(0x8fc88)
// 转换后再 SGL_grpFillRect。该转换把 color 低 16 位当作 **RGB565**（R=bit15..11、
// G=bit10..5、B=bit4..0），展开为 ABGR8888；alpha 是 **0..100 百分比**（w5>0x64 直接 return，
// 内部按 alpha*255/100 写入高字节）。故此处颜色必须写 RGB565，不能写 ABGR8888——
// 之前写 0xFF102E4A 被当作 RGB565(0x2E4A) 解码成亮绿（实测），即此故。
// COLOR_SEL_BG 深琥珀 0x5182 = RGB565(R5=10,G6=12,B5=2) → 渲染 RGB(0x50,0x30,0x10)
// 暖暗棕（明度低），反衬 UI_GOLD 金色文字（深底亮字高对比），仍可与冷色近黑的
// COLOR_BTN_BG/遮罩区分。COLOR_SEL_BG_ALPHA=0x50 即 80% 不透明度（百分比口径）。
#define UI_GOLD 0xFF00B4D7
#define UI_OPTION_TEXT 0xFFCB9EE2
#define COLOR_TEXT_WHITE 0xFFFFFFFF
#define COLOR_TEXT_GRAY 0xFF808080
// RGB565（GRPX_FillRectAlpha 口径）：0x5182 → RGB(0x50,0x30,0x10) 深琥珀/深棕暖暗色。
#define COLOR_SEL_BG 0x5182
#define COLOR_SEL_BG_ALPHA 0x50
#define COLOR_BTN_BG 0xFF241C18
#define COLOR_BTN_BG_DIM 0xFF181012
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
// 视觉资源（与 game_ui_settings 同款）：面板背景贴图两半 + 原版返回按钮贴图（unit 0x59）。
// 注意：option 贴图单元与设置页共用，本面板不做「已加载」标志——enter 无条件确保加载
// （幂等）、f3 不卸载（所有者是设置页）；返回按钮绘制直接尝试贴图、失败退化自绘，
// 避免标志与实际单元状态不一致（设置页 f3 卸载后本面板 resumed 时标志仍为 true 的窗口）。
bool g_title_background_images_loaded = false;
bool g_background_unavailable_logged = false;
// 返回按钮几何：创建控件时按 root 修正为屏幕绝对 (8,8)（同设置页 ui_create_button
// {8-root.x, 8-root.y} 口径）；命中测试与绘制只读 w/h，x/y 仅创建时使用。
UiRect g_back_btn_rect = {8, 8, BACK_BTN_W, BACK_BTN_H};

// 三栏控件容器（label 类型，参与命中但 DrawProc 自绘；DrawProc 走 fn_ctrl_btn_draw → 自定义 cb）。
void* g_slot_btns[3] = {nullptr, nullptr, nullptr};
void* g_ops_btns[3] = {nullptr, nullptr, nullptr};   // 0=导出 1=导入 2=删除
void* g_list_btns[LIST_COLS_MAX] = {nullptr};       // 当前页 5 个
void* g_prev_btn = nullptr;
void* g_next_btn = nullptr;
void* g_back_btn = nullptr;                          // 底部「← 返回」：关闭面板
void* g_msg_label = nullptr;                         // 操作结果提示区
int g_selected_slot = -1;                            // 左栏选中槽 0/1/2；-1=未选（默认两侧都不选）
int g_selected_backup = -1;                          // 右栏当前页选中项下标（0..page_size-1）；-1=未选
int g_page_index = 0;                                // 翻页：0..N
// 备份列表缓存（从 save_backup_list_json 解析）；每项 checksum + hero_level + class + map_name + export_time。
// map_name 由 entry_json 输出携带（Kotlin 启动期下发的 map_id→中文名表）；未命中为空串。
// class_idx/class_name 由导出时写入 metaJson；旧备份缺失时 -1/空串（UI 退化显示）。
struct BackupEntry {
    std::string checksum;
    std::string map_name;
    int hero_level = -1;
    int class_idx = -1;
    std::string class_name;
    int map_id = 0;
    long long export_time_ms = 0;
    long long save_time = 0;
};
std::vector<BackupEntry> g_backups;
int g_total_pages = 0;
// 结果提示文本（不弹窗，在面板内独立显示区；新消息覆盖旧消息，不自动消失）。
std::string g_msg_text;
uint32_t g_msg_color = UI_OPTION_TEXT;
bool g_msg_is_error = false;
// 面板内两步确认状态（导入/删除不依赖原版 YesNo 弹窗——自定义面板下该弹窗不渲染）。
// 0=无待确认 1=待确认导入 2=待确认删除；与目标备份 checksum + 目标槽绑定：
// 第二次点击同操作按钮且 checksum/槽均一致才执行，否则按新操作重新武装。
// 任何其它交互（切换槽位/选中、翻页、其它操作按钮）及列表刷新（apply_backups_locked）
// 都会清除待确认（列表刷新后 checksum 可能失效）。
int g_pending_op = 0;
std::string g_pending_checksum;
int g_pending_slot = -1;
// 左栏三槽缓存：enter 后由游戏线程读 fn_save_get_save_slot / fn_saveslot_get_hero /
// C_LEVEL / C_CLASS / SAVESLOT_MAP_ID 填充（见 read_slots_into），DrawProc 只读缓存，不读游戏内存。
struct SlotInfo {
    bool exists = false;
    int hero_level = -1;
    int hero_index = -1;
    int class_idx = -1;   // 主角职业索引 0-5（hero+C_CLASS）；-1=未知，UI 退化显示
    int map_id = -1;
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

// 列表按钮 rect 依赖当前页条目数（垂直居中分布），定义在 panel.inc；
// apply_backups_locked 在 inc 之前先用到，此处前置声明。
void sync_list_btn_rects_locked();

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

// 清除待确认状态（调用方须持 g_sbui_mtx）。
void clear_pending_locked() {
    g_pending_op = 0;
    g_pending_checksum.clear();
    g_pending_slot = -1;
}

// 应用刷新结果（调用方须持 g_sbui_mtx）。
void apply_backups_locked(std::vector<BackupEntry>& entries) {
    const size_t old_count = g_backups.size();
    g_backups.swap(entries);
    g_selected_backup = -1;
    // 列表已重建：旧 pending 绑定的 checksum 可能已失效（备份被删/改），一并清除。
    clear_pending_locked();
    int total = static_cast<int>(g_backups.size());
    g_total_pages = (total + LIST_COLS_PER_PAGE - 1) / LIST_COLS_PER_PAGE;
    if (g_page_index >= g_total_pages && g_total_pages > 0) g_page_index = g_total_pages - 1;
    if (g_page_index < 0) g_page_index = 0;
    // 条目数/页码变化 → 本页条目垂直居中分布。控件 rect 的写回必须在游戏线程：
    // 本函数可能在后台刷新线程执行（apply 阶段），故这里不直接 ui_set_rect；
    // 由 savebackup_panel_process（游戏线程、持锁）每帧 sync_list_btn_rects_locked()。
    // 命中/绘制本身已不依赖控件 rect（panel_abs_origin + 实时计算 rect），见 panel.inc/render.inc。
    // 条数变化日志：真机验收据此确认「导出 → 后台读目录 → apply」链路把新条目带到列表
    // （导出成功路径额外把 page_index 归零，见 export_selected）。
    if (old_count != g_backups.size()) {
        SB_UI_LOG("apply_backups: %zu -> %zu backups, total_pages=%d, page_index=%d",
                  old_count, g_backups.size(), g_total_pages, g_page_index);
    }
}

// 读三槽元数据到 out（无共享状态，锁外调用；必须在游戏线程执行，见 refresh_slots_on_game_thread）。
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
        out[i].map_id = static_cast<int>(
            *reinterpret_cast<const uint16_t*>(p + SAVESLOT_MAP_ID));
        void* hero = fn_saveslot_get_hero(slot);
        if (hero == nullptr) continue;
        uint8_t* hp = reinterpret_cast<uint8_t*>(hero);
        out[i].hero_level = static_cast<int8_t>(*reinterpret_cast<const int8_t*>(hp + C_LEVEL));
        // 职业索引 0-5（hero 结构 C_CLASS）；越界/异常值由 UI 端 save_backup_class_name 兜底。
        out[i].class_idx = static_cast<int8_t>(*reinterpret_cast<const int8_t*>(hp + C_CLASS));
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
    // 提示文本写入控件自身（msg_label_draw 用 ui_draw_text_centered 绘制控件文本）；
    // 无消息时控件文本回退为面板标题。
    if (g_msg_label != nullptr && fn_ctrl_btn_set_text != nullptr) {
        fn_ctrl_btn_set_text(g_msg_label,
                             g_msg_text.empty() ? const_cast<char*>(kTitle)
                                                : const_cast<char*>(g_msg_text.c_str()));
    }
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
int page_entry_count();

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

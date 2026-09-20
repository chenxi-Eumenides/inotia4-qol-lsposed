#include "panel.h"

#include "../../../contracts/panels.h"
#include "../../../contracts/ui.h"
#include "plugins/save_backup/feature.h"
#include "plugins/save_backup/log.h"

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

#define SB_UI_LOG(...) SAVE_BACKUP_LOG_INFO(__VA_ARGS__)

#define CB_TEXT_SIZE 0x40

// 面板布局（与 core.ui 设置页同构：逻辑坐标 0-960×0-640 空间，root 相对全屏居中）
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
// 顶部消息/标题区：与设置页 header 标签同高（settings.cpp HEADER_Y=0x18，label rect 高 HEADER_H=0x28）。
//   MSG_Y + root.y == 0x18（root.y=0 → MSG_Y=0x18）；文本绘制偏移同 settings header_draw 的 0x18。
#define MSG_X (ROOT_W / 2 - MSG_W / 2)
#define MSG_Y 0x18
#define MSG_W 0x120
#define MSG_H 0x28
// 返回按钮：与 core.ui 设置页完全同尺寸（原版 option 贴图量级），屏幕左上 (8,8)。
#define BACK_BTN_W 0x4a
#define BACK_BTN_H 0x51
// 背景贴图 unit/loc：与 core.ui 设置页同值（title background 左右两半）。
#define TITLE_BACKGROUND_LEFT_UNIT 0x4f
#define TITLE_BACKGROUND_RIGHT_UNIT 0x50
#define TITLE_BACKGROUND_LOC 0x0

// 颜色（ABGR 0xAABBGGRR，文字/框线/按钮用）：与 core.ui 设置页同一套视觉语言——
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

namespace qol::features::save_backup {
namespace {

using UiRect = contracts::UiRect;
using UiClickProc = contracts::QolUiClickFn;
using UiDrawProc = contracts::QolUiDrawFn;

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

void* ui_create_control(uint32_t kind, void* parent, UiRect rect, const char* text,
                        UiClickProc click_proc, UiDrawProc draw_proc) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui == nullptr || ui->create_control == nullptr || ui->set_rect == nullptr) return nullptr;
    void* control = nullptr;
    if (!ui_ok(ui->create_control(kind, parent, &control)) || control == nullptr) return nullptr;
    if (!ui_ok(ui->set_rect(control, rect))) return nullptr;
    if (text != nullptr && (ui->set_text == nullptr || !ui_ok(ui->set_text(control, text)))) {
        return nullptr;
    }
    if (click_proc != nullptr &&
        (ui->set_click_proc == nullptr || !ui_ok(ui->set_click_proc(control, click_proc, control)))) {
        return nullptr;
    }
    if (draw_proc != nullptr &&
        (ui->set_draw_proc == nullptr || !ui_ok(ui->set_draw_proc(control, draw_proc)))) {
        return nullptr;
    }
    return control;
}

void* ui_create_root(UiRect rect) {
    return ui_create_control(0, nullptr, rect, nullptr, nullptr, nullptr);
}

void* ui_create_button(void* parent, UiRect rect, const char* text, UiClickProc click_proc,
                       UiDrawProc draw_proc) {
    return ui_create_control(1, parent, rect, text, click_proc, draw_proc);
}

void* ui_create_label(void* parent, UiRect rect, const char* text, UiDrawProc draw_proc) {
    return ui_create_control(2, parent, rect, text, nullptr, draw_proc);
}

void ui_set_rect(void* control, UiRect rect) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui != nullptr && ui->set_rect != nullptr) (void)ui->set_rect(control, rect);
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

void ui_fill_rect_alpha(UiRect rect, uint32_t color, uint32_t alpha) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui != nullptr && ui->fill_rect_alpha != nullptr) {
        (void)ui->fill_rect_alpha(rect, color, alpha);
    }
}

void ui_draw_panel_decor(UiRect panel, const int64_t* separators, size_t count, uint32_t color) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui != nullptr && ui->draw_panel_decor != nullptr) {
        (void)ui->draw_panel_decor(panel, separators, static_cast<uint32_t>(count), color);
    }
}

void ui_draw_vertical_line(int64_t x, int64_t y, int64_t height, uint32_t color, int thickness) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui != nullptr && ui->draw_vertical_line != nullptr) {
        (void)ui->draw_vertical_line(x, y, height, color, thickness);
    }
}

bool ui_load_image_unit(int32_t unit) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    return ui != nullptr && ui->load_image_unit != nullptr && ui_ok(ui->load_image_unit(unit));
}

bool ui_draw_image_part(int32_t unit, int32_t loc, int32_t x, int32_t y, int32_t type,
                        int32_t flip) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    return ui != nullptr && ui->draw_image_part != nullptr &&
           ui_ok(ui->draw_image_part(unit, loc, x, y, type, flip));
}

void ui_set_text(void* control, const char* text) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui != nullptr && ui->set_text != nullptr) (void)ui->set_text(control, text);
}

void ui_draw_control(void* control) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui != nullptr && ui->draw_control != nullptr) (void)ui->draw_control(control);
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

bool ui_set_popup_process_info(int32_t process, int32_t state_id) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    return ui != nullptr && ui->set_popup_process_info != nullptr &&
           ui_ok(ui->set_popup_process_info(process, state_id));
}

bool ui_push_popup_state(uint32_t state_id) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    return ui != nullptr && ui->push_popup_state != nullptr &&
           ui_ok(ui->push_popup_state(state_id));
}

bool ui_popup_top_matches(contracts::QolUiPopupEnterFn enter) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    int32_t matches = 0;
    return ui != nullptr && ui->popup_stack_top_matches != nullptr && enter != nullptr &&
           ui_ok(ui->popup_stack_top_matches(enter, &matches)) && matches != 0;
}

void ui_clear_control_proc(void* control) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui != nullptr && ui->clear_control_proc != nullptr && control != nullptr) {
        (void)ui->clear_control_proc(control);
    }
}

void ui_control_absolute_origin(void* control, int64_t out[2]) {
    out[0] = 0;
    out[1] = 0;
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui != nullptr && ui->control_absolute_origin != nullptr && control != nullptr) {
        (void)ui->control_absolute_origin(control, &out[0], &out[1]);
    }
}

bool ui_set_font_color_rgb(int32_t red, int32_t green, int32_t blue) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    return ui != nullptr && ui->set_font_color_rgb != nullptr &&
           ui_ok(ui->set_font_color_rgb(red, green, blue));
}

bool ui_draw_string_with_font(const char* text, int32_t x, int32_t y, int32_t align, int32_t font) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    return ui != nullptr && ui->draw_string_with_font != nullptr &&
           ui_ok(ui->draw_string_with_font(text, x, y, align, font));
}

namespace ui_original {

bool load_option_images() {
    const contracts::QolUiAdapter* ui = ui_adapter();
    return ui != nullptr && ui->load_option_images != nullptr && ui_ok(ui->load_option_images());
}

bool draw_back_button(void* control, bool pressed) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    return ui != nullptr && ui->draw_back_button != nullptr &&
           ui_ok(ui->draw_back_button(control, pressed ? 1 : 0));
}

}  // namespace ui_original

namespace ui_custom {

void draw_button(void* control, UiRect size, uint32_t background, uint32_t border,
                 int border_thickness, const char* text, uint32_t text_color, int text_x,
                 int text_y) {
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui != nullptr && ui->draw_custom_button != nullptr) {
        (void)ui->draw_custom_button(control, size, background, border, border_thickness, text,
                                      text_color, text_x, text_y);
    }
}

}  // namespace ui_custom

// ============== 全局状态 ==============
std::mutex g_sbui_mtx;

bool g_panel_active = false;
int g_state_id = -1;
void* g_root = nullptr;
int g_back_pressed = 0;
int g_close_delay_frames = 0;
const char* kTitle = "存档备份";
// own:popup.savebackup 的 PopupState 占用条目（资源事务 acquire 写入、release 写回）。
contracts::QolPopupStateEntry g_popup_entry{};
bool g_popup_acquired = false;
// 视觉资源（与 core.ui 设置页同款）：面板背景贴图两半 + 原版返回按钮贴图（unit 0x59）。
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
// 左栏三槽缓存：enter 后由游戏线程经 Game Host 读取槽元数据，DrawProc 只读缓存。
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
// 三栏控件共用同一 UiClickProc（点击后用 ctrl 指针反查 g_slot_btns/g_ops_btns/g_list_btns 识别）。
void on_btn_clicked(void* ctrl);
void on_back_clicked(void* ctrl);

// 列表按钮 rect 依赖当前页条目数（垂直居中分布），定义在 panel.inc；
// apply_backups_locked 在 inc 之前先用到，此处前置声明。
void sync_list_btn_rects_locked();

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
// 其余状态只读现有槽结构。槽元数据经 Game Host 读取。
void read_slots_into(SlotInfo out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = SlotInfo{};
    const auto* game = save_backup_game_host();
    if (game == nullptr || game->read_only.save.slot_exists == nullptr ||
        game->read_only.save.slot_hero_index == nullptr ||
        game->read_only.save.hero_level == nullptr ||
        game->read_only.save.hero_class == nullptr || game->read_only.save.map_id == nullptr) return;
    for (int i = 0; i < 3; ++i) {
        int32_t exists = 0;
        if (!qol::contracts::succeeded(game->read_only.save.slot_exists(i, &exists)) ||
            exists == 0) continue;
        out[i].exists = true;
        int32_t value = 0;
        if (qol::contracts::succeeded(game->read_only.save.slot_hero_index(i, &value))) {
            out[i].hero_index = value;
        }
        if (qol::contracts::succeeded(game->read_only.save.hero_level(i, &value))) {
            out[i].hero_level = value;
        }
        if (qol::contracts::succeeded(game->read_only.save.hero_class(i, &value))) {
            out[i].class_idx = value;
        }
        if (qol::contracts::succeeded(game->read_only.save.map_id(i, &value))) {
            out[i].map_id = value;
        }
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
    if (g_msg_label != nullptr && ui_adapter() != nullptr && ui_adapter()->set_text != nullptr) {
        ui_set_text(g_msg_label,
                    g_msg_text.empty() ? kTitle : g_msg_text.c_str());
    }
}

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

// ============== 面板回调 ==============
void savebackup_panel_enter();
void savebackup_panel_process();
void savebackup_panel_f3();
void savebackup_panel_f4();
uint32_t savebackup_panel_event(uint64_t event, uint64_t param, uint64_t param2);

#include "ui/panel.inc"
#include "ui/render.inc"
#include "ui/injection.inc"

}  // namespace

// ============== own:popup.savebackup（kOwnership）==============

int32_t savebackup_popup_acquire(const char* id) {
    (void)id;
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    if (g_popup_entry.slot != nullptr) return 0;
    contracts::QolPopupStateEntry entry{};
    const contracts::QolHostSdk* sdk = save_backup_host_sdk();
    if (sdk == nullptr || sdk->hooks.popup_resolve == nullptr ||
        !ui_ok(sdk->hooks.popup_resolve("F_PANEL_UNK2_ENTER", &entry))) {
        SB_UI_LOG("popup.savebackup: state entry (INAP_GOODS) not found");
        return 1;
    }
    const contracts::QolPopupStateHooks popup_hooks{
        &savebackup_panel_enter,
        &savebackup_panel_process,
        &savebackup_panel_f3,
        &savebackup_panel_f4,
        &savebackup_panel_event,
    };
    if (sdk->hooks.popup_acquire == nullptr ||
        !ui_ok(sdk->hooks.popup_acquire(&entry, &popup_hooks))) {
        SB_UI_LOG("popup.savebackup: acquire rejected");
        return 1;
    }
    g_popup_entry = entry;
    g_state_id = entry.state_id;
    g_popup_acquired = true;
    SB_UI_LOG("popup.savebackup acquired: state id=%d", g_state_id);
    return 0;
}

void savebackup_popup_release(const char* id) {
    (void)id;
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    const contracts::QolHostSdk* sdk = save_backup_host_sdk();
    if (g_popup_entry.slot != nullptr && sdk != nullptr && sdk->hooks.popup_restore != nullptr) {
        (void)sdk->hooks.popup_restore(&g_popup_entry);
    }
    g_popup_acquired = false;
    g_state_id = -1;
    SB_UI_LOG("popup.savebackup released");
}

// ============== PanelProviderSpec ==============

bool savebackup_provider_open() {
    if (g_popup_entry.slot == nullptr) return false;
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui == nullptr || ui->set_popup_process_info == nullptr) return false;

    // 优先尝试嵌套 push（保持设置页 keep open 在栈下方）。
    // 真机实测：fn_popupstate_push(0x122424) 内部 Array_Add 失败但 GetTop 返回当前栈顶
    // 地址（设置页 entry），导致原代码 rc==0 当 success 的判断逻辑误报失败，
    // 而实际栈顶未切换为 savebackup entry。修复：
    //   1) rc != 0 才视为 push 成功（按反汇编：rc=0 = enter NULL / push 内部失败）
    //   2) 校验栈顶 enter_func 必须是 &savebackup_panel_enter（防止 GetTop 返回旧栈顶）
    // 未切换栈顶时返回 false，由 core.ui 面板栈关栈顶后重试（原切换式兜底已上移）。
    if (ui != nullptr && ui->push_popup_state != nullptr) {
        const bool pushed = ui_push_popup_state(static_cast<uint32_t>(g_state_id));
        SB_UI_LOG("open save backup panel via push: state=%d rc=%d", g_state_id, pushed ? 1 : 0);
        if (pushed && is_savebackup_at_stack_top()) {
            return true;
        }
        // 未切换栈顶：返回 false，由 core.ui 面板栈关栈顶后重试（原切换式兜底已上移）。
        SB_UI_LOG("push didn't switch stack top (rc=%d); stack will close top and retry",
                  pushed ? 1 : 0);
        return false;
    }
    // 无 push 符号：切换式兜底（关当前 UI → 异步打开本面板）。栈记录由 active() 在下次 open 剔除。
    if (!ui_set_popup_process_info(3, 0) || !ui_set_popup_process_info(1, g_state_id)) {
        return false;
    }
    SB_UI_LOG("open save backup panel via switch: state=%d", g_state_id);
    return true;
}

void savebackup_provider_close() {
    bool was_active = false;
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        was_active = g_panel_active;
        clear_pending_locked();
        set_msg_locked("", UI_OPTION_TEXT, false);
        g_panel_active = false;
        g_close_delay_frames = 0;
        g_back_pressed = 0;
        // 控件树保留复用（同 f3：递归删除 SIGBUS）；再次进入走 enter 的 resumed 分支。
    }
    // 幂等：未激活时不触碰游戏 UI（避免栈对陈旧栈顶调用 close 时误关当前顶层面板）。
    if (!was_active) return;
    // fn_ui_set_popup_process_info 必须锁外调用：pop 链会触发 savebackup_panel_f3 拿
    // g_sbui_mtx（非递归），持锁调用会死锁。
    (void)ui_set_popup_process_info(3, 0);
    SB_UI_LOG("back clicked: panel closed");
}

bool savebackup_provider_active() {
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    return g_panel_active;
}

const char* savebackup_provider_block_reason() {
    std::lock_guard<std::mutex> lock(g_sbui_mtx);
    if (g_popup_entry.slot == nullptr) return nullptr;  // 未注入 → 不阻塞
    return g_panel_active ? "ui occupied: backup panel" : nullptr;
}

// ============== 对外 API（原 data_savebackup_ui_*）==============

std::string data_savebackup_ui_inject() {
    if (savebackup_popup_acquire(nullptr) != 0) return save_backup_result_error("state entry inject failed");
    return save_backup_result_ok();
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
        g_state_id, g_popup_acquired ? "true" : "false",
        g_panel_active ? "true" : "false", g_root,
        g_selected_slot, g_selected_backup,
        g_page_index, g_total_pages, g_backups.size(),
        g_msg_text.c_str(), g_msg_is_error ? "true" : "false",
        ui_screen_name());
    return std::string(buf);
}

std::string data_savebackup_ui_restore() {
    {
        std::lock_guard<std::mutex> lock(g_sbui_mtx);
        g_panel_active = false;
        g_close_delay_frames = 0;
        g_back_pressed = 0;
    }
    savebackup_popup_release(nullptr);
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

}  // namespace qol::features::save_backup

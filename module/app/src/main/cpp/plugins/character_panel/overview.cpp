#include "overview.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../../contracts/game_host.h"
#include "../../contracts/game_ports/character_extras_port.h"
#include "../../contracts/ui.h"
#include "../../contracts/ui_theme.h"
#include "feature.h"
#include "overview_model.h"

// 属性总览页面：4 页双列（主属性与面板属性 / 隐藏属性 / 元素与抗性 / 全局加成），
// 每列最多 11 行；分页表见 overview_model.h。P4 只有左列。
//
// 绘制只用 ui_components：面板根、标准导航盒、左右箭头按钮、draw_control_text。
// 适配层只提供帧封装（begin_frame/end_frame）与屏幕 inset；不做 fill_rect /
// draw_string_with_font 等低层手绘。数据读取只在 enter 内执行一次。

namespace qol::features::character_panel {
namespace {

constexpr int64_t kBasePanelW = 960;
constexpr int64_t kBasePanelH = 640;
constexpr int64_t kHeaderY = 100;
constexpr int64_t kSectionY = 148;
constexpr int64_t kRowsTopY = 190;
constexpr int64_t kRowPitch = 32;
// 双列：左列标签 90 / 数值右缘 430；右列标签 500 / 数值右缘 840（均设计区局部坐标）。
constexpr int64_t kLeftLabelX = 90;
constexpr int64_t kLeftValueRightX = 430;
constexpr int64_t kRightLabelX = 500;
constexpr int64_t kRightValueRightX = 840;
// 分页行：左箭头 / 页码 / 右箭头同一行，不额外占一行。
constexpr int64_t kPageRowY = 556;
constexpr int64_t kPageTextY = kPageRowY + 16;
constexpr int64_t kPageArrowSize = 56;
constexpr int64_t kPageArrowCenterOffset = 120;
constexpr int32_t kTitleFont = 2;
constexpr int32_t kBodyFont = 1;
constexpr size_t kValueBufferSize = 40;

constexpr const char* kTitleText = "人物属性";
constexpr const char* kBlockedComponents = "ui components unavailable";
constexpr const char* kBlockedDisabled = "character overview disabled";

const contracts::QolUiComponents* g_components = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::QolCharacterExtrasPort* g_character_extras = nullptr;
contracts::QolUiHandle g_root = nullptr;
contracts::QolUiHandle g_nav = nullptr;
contracts::QolUiHandle g_prev = nullptr;
contracts::QolUiHandle g_next = nullptr;
contracts::QolUiHandle g_touch_root = nullptr;
std::atomic<bool> g_active{false};
std::atomic<bool> g_overview_enabled{true};
uint32_t g_page = 0;
uint32_t g_page_count = 1;  // enter 前也安全（绘制不会发生）

// 每行一次的读取快照：ok=false 时该行显示 `--`，不影响其它行。
struct Snapshot {
    int32_t value[kSectionCount][kMaxSectionRows];
    uint8_t ok[kSectionCount][kMaxSectionRows];
    // kGroupSkill 行的已学标志（1=已学）；其它行恒 0。
    uint8_t learned[kSectionCount][kMaxSectionRows];
};
Snapshot g_snapshot{};

const contracts::QolUiAdapter* ui_adapter() {
    const contracts::QolHostSdk* sdk = host_sdk();
    return sdk == nullptr ? nullptr : sdk->ui;
}

bool components_ready(const contracts::QolUiComponents* components) {
    if (components == nullptr) return false;
    constexpr size_t kRequiredSize =
        offsetof(contracts::QolUiComponents, arrow_button_draw) +
        sizeof(((contracts::QolUiComponents*)nullptr)->arrow_button_draw);
    return components->major == contracts::kUiContractMajor &&
           components->minor >= contracts::kUiContractMinor &&
           components->size >= kRequiredSize &&
           components->panel_create != nullptr && components->nav_button_create != nullptr &&
           components->nav_button_draw != nullptr &&
           components->arrow_button_create != nullptr &&
           components->arrow_button_set_enabled != nullptr &&
           components->arrow_button_draw != nullptr &&
           components->draw_control_text != nullptr &&
           components->dispatch_native_event != nullptr && components->destroy != nullptr;
}

// ---- 属性读取（仅 enter 调用；全部在游戏主线程）----

bool read_attr(void* character, int32_t attr_id, int32_t* out) {
    if (character == nullptr || out == nullptr || g_game == nullptr ||
        g_game->character_read.attr == nullptr) {
        return false;
    }
    return contracts::succeeded(g_game->character_read.attr(character, attr_id, out));
}

bool read_main_stat(void* character, int32_t index, int32_t* out) {
    if (character == nullptr || out == nullptr || g_game == nullptr ||
        g_game->character_read.main_stat == nullptr) {
        return false;
    }
    return contracts::succeeded(g_game->character_read.main_stat(character, index, out));
}

bool read_max_hp(void* character, int32_t* out) {
    if (character == nullptr || out == nullptr || g_character_extras == nullptr ||
        g_character_extras->read.max_hp == nullptr) {
        return false;
    }
    return contracts::succeeded(g_character_extras->read.max_hp(character, out));
}

bool read_row_value(void* character, const RowSpec& row, int32_t* out) {
    switch (row.kind) {
        case RowKind::kText:
            return true;  // 说明行不读游戏数据
        case RowKind::kGroupSkill:
            // 佣兵团技能（全队生效）：act 119..142，返回 1=已学/0=未学。
            if (g_game == nullptr || g_game->character_read.group_skill_learned == nullptr) {
                return false;
            }
            return contracts::succeeded(
                g_game->character_read.group_skill_learned(row.arg, out));
        case RowKind::kMainStat:
            return read_main_stat(character, row.arg, out);
        case RowKind::kAttrInt:
            // attr30（HP 上限）必须走 CHAR_GetMaxHP：CHAR_GetAttr 在 HP > maxHP 时会把 HP
            // 夹回上限并写回角色，读属性不应产生副作用。取不到时该行显示 `--`，不回退 attr。
            if (row.arg == 30) return read_max_hp(character, out);
            // attr31（MP 上限）暂无对应宿主 getter，仍直接读 attr。
            return read_attr(character, row.arg, out);
        case RowKind::kAttrMilli:
            return read_attr(character, row.arg, out);
        case RowKind::kDefense: {
            int32_t base = 0;
            int32_t bonus = 0;
            if (!read_attr(character, 17, &base) || !read_attr(character, 18, &bonus)) return false;
            // 面板防御公式：attr17 * (attr18 + 1000) / 1000；int64 防止乘法溢出。
            const int64_t scaled = static_cast<int64_t>(base) *
                                   (static_cast<int64_t>(bonus) + 1000);
            *out = static_cast<int32_t>(scaled / 1000);
            return true;
        }
        case RowKind::kNormalDamage:
            if (g_game == nullptr || g_game->character_read.normal_damage == nullptr) return false;
            return contracts::succeeded(
                g_game->character_read.normal_damage(character, out));
        case RowKind::kNormalMagicDamage:
            if (g_game == nullptr || g_game->character_read.normal_magic_damage == nullptr) {
                return false;
            }
            return contracts::succeeded(
                g_game->character_read.normal_magic_damage(character, out));
        case RowKind::kHitRate:
            if (g_game == nullptr || g_game->character_read.hit_rate_1000 == nullptr) return false;
            return contracts::succeeded(
                g_game->character_read.hit_rate_1000(character, out));
        case RowKind::kEvasion:
            if (g_game == nullptr || g_game->character_read.evasion_rate_1000 == nullptr) {
                return false;
            }
            return contracts::succeeded(
                g_game->character_read.evasion_rate_1000(character, out));
        case RowKind::kDamageReduce:
            if (g_game == nullptr ||
                g_game->character_read.display_damage_reduce_rate == nullptr) {
                return false;
            }
            return contracts::succeeded(
                g_game->character_read.display_damage_reduce_rate(character, out));
    }
    return false;
}

// 角色指针：优先队伍 0 号位，失败回退菜单角色；两者都失败时全部行显示 `--`。
void* resolve_character() {
    void* character = nullptr;
    if (g_character_extras != nullptr) {
        const contracts::QolCharacterExtrasReadApi& read = g_character_extras->read;
        if (read.party_member != nullptr &&
            contracts::succeeded(read.party_member(0, &character)) && character != nullptr) {
            return character;
        }
        character = nullptr;
        if (read.menu_character != nullptr &&
            contracts::succeeded(read.menu_character(&character)) && character != nullptr) {
            return character;
        }
    }
    return nullptr;
}

void read_snapshot() {
    g_snapshot = Snapshot{};
    void* character = resolve_character();
    for (uint32_t section = 0; section < kSectionCount; ++section) {
        const SectionSpec& spec = kSections[section];
        for (uint32_t row = 0; row < spec.count && row < kMaxSectionRows; ++row) {
            int32_t value = 0;
            const bool ok = spec.rows[row].kind == RowKind::kText ||
                            read_row_value(character, spec.rows[row], &value);
            g_snapshot.value[section][row] = value;
            g_snapshot.ok[section][row] = spec.rows[row].kind == RowKind::kText
                                              ? 1
                                              : (ok ? 1 : 0);
            g_snapshot.learned[section][row] =
                spec.rows[row].kind == RowKind::kGroupSkill && ok && value != 0 ? 1 : 0;
            if (spec.rows[row].kind == RowKind::kText) continue;
            if (!ok) g_snapshot.value[section][row] = 0;
        }
    }
}

// ---- 绘制 ----

void destroy_controls() {
    contracts::QolUiHandle root = g_root;
    g_root = nullptr;
    g_nav = nullptr;
    g_prev = nullptr;
    g_next = nullptr;
    g_touch_root = nullptr;
    if (root == nullptr) return;
    if (g_components != nullptr && g_components->destroy != nullptr) {
        (void)g_components->destroy(root);
    }
    // panel/label 的 native 控件不由组件表删除：与 save_backup 同口径补一次
    // adapter delete_control（root 递归释放子控件）。
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui != nullptr && ui->delete_control != nullptr) (void)ui->delete_control(root);
}

void draw_text(const char* text, int64_t x, int32_t y, uint32_t color, int32_t align,
               int32_t font) {
    if (g_root == nullptr || g_components == nullptr || text == nullptr) return;
    (void)g_components->draw_control_text(g_root, text, static_cast<int32_t>(x), y, color, align,
                                          font);
}

// 根面板放在屏幕 inset 原点、尺寸固定 960×640（设计区）；所有子控件与文本都用设计区
// 局部坐标（0..960 / 0..640），绝对位置 = 根原点 + 局部坐标，因此横屏/平板黑边不会
// 把导航按钮或文本挤出可视区。
constexpr int64_t kDesignCenterX = kBasePanelW / 2;

void on_previous_page(void*) {
    if (g_page == 0) return;
    --g_page;
}

void on_next_page(void*) {
    if (g_page + 1 >= g_page_count) return;
    ++g_page;
}

bool create_controls() {
    const contracts::QolUiComponents* components = g_components;
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (!components_ready(components) || ui == nullptr) return false;
    if (ui->screen_inset_width == nullptr || ui->screen_inset_height == nullptr) return false;
    int32_t inset_width = 0;
    int32_t inset_height = 0;
    if (!contracts::succeeded(ui->screen_inset_width(&inset_width)) ||
        !contracts::succeeded(ui->screen_inset_height(&inset_height)) ||
        inset_width < 0 || inset_height < 0) {
        return false;
    }
    const int64_t root_x = inset_width;
    const int64_t root_y = inset_height;
    const contracts::UiPanelDesc panel_desc{
        {root_x, root_y, kBasePanelW, kBasePanelH},
        {root_x, root_y, kBasePanelW, kBasePanelH}, 0, 0};
    contracts::QolUiHandle root = nullptr;
    if (!contracts::succeeded(components->panel_create(&panel_desc, nullptr, &root)) ||
        root == nullptr) {
        return false;
    }
    g_root = root;
    // 导航盒固定在整屏左上角 (8,8)；root 原点在屏幕 inset，局部坐标需减去 root 原点。
    const contracts::UiNavButtonDesc nav_desc{
        {contracts::ui_theme::kNavBoxAbsX - root_x,
         contracts::ui_theme::kNavBoxAbsY - root_y,
         contracts::ui_theme::kNavBoxW, contracts::ui_theme::kNavBoxH},
        contracts::kUiNavButtonAuto, nullptr, nullptr};
    if (!contracts::succeeded(components->nav_button_create(&nav_desc, root, &g_nav)) ||
        g_nav == nullptr) {
        destroy_controls();
        return false;
    }
    const contracts::UiArrowButtonDesc prev_desc{
        {kDesignCenterX - kPageArrowCenterOffset - kPageArrowSize / 2, kPageRowY,
         kPageArrowSize, kPageArrowSize},
        contracts::kUiArrowLeft, 0, &on_previous_page, nullptr};
    const contracts::UiArrowButtonDesc next_desc{
        {kDesignCenterX + kPageArrowCenterOffset - kPageArrowSize / 2, kPageRowY,
         kPageArrowSize, kPageArrowSize},
        contracts::kUiArrowRight, 1, &on_next_page, nullptr};
    if (!contracts::succeeded(components->arrow_button_create(&prev_desc, root, &g_prev)) ||
        g_prev == nullptr ||
        !contracts::succeeded(components->arrow_button_create(&next_desc, root, &g_next)) ||
        g_next == nullptr) {
        destroy_controls();
        return false;
    }
    return true;
}

void draw_column(const ColumnSpec& column, int64_t label_x, int64_t value_right_x) {
    if (column.row_count == 0 || column.section >= kSectionCount) return;
    const SectionSpec& section = kSections[column.section];
    for (uint32_t i = 0; i < column.row_count; ++i) {
        const uint32_t row_index = column.first_row + i;
        if (row_index >= section.count) break;
        const RowSpec& row = section.rows[row_index];
        const int32_t y = static_cast<int32_t>(kRowsTopY + static_cast<int64_t>(i) * kRowPitch);
        if (row.kind == RowKind::kText) {
            // 说明行没有数值列：整行左对齐，逐行带注记（行可自定义 note）。
            char line[128];
            (void)std::snprintf(line, sizeof(line), "%s：%s", row.label,
                                row.note != nullptr ? row.note : kGlobalNote);
            draw_text(line, label_x, y, contracts::ui_theme::kTextNormal, 0, kBodyFont);
            continue;
        }
        char value[kValueBufferSize];
        format_value(row, g_snapshot.ok[column.section][row_index] != 0,
                     g_snapshot.learned[column.section][row_index] != 0,
                     g_snapshot.value[column.section][row_index], value, sizeof(value));
        draw_text(row.label, label_x, y, contracts::ui_theme::kTextNormal, 0, kBodyFont);
        draw_text(value, value_right_x, y, contracts::ui_theme::kTextBody, 1, kBodyFont);
    }
}

void draw_page() {
    // draw_control_text 的偏移相对根控件原点（= 屏幕 inset 原点），传入设计区局部坐标。
    draw_text(kTitleText, kDesignCenterX, static_cast<int32_t>(kHeaderY),
              contracts::ui_theme::kTextGold, 2, kTitleFont);

    const PageSpec* page = nullptr;
    if (!page_at(g_page, &page) || page == nullptr) return;
    draw_text(page->title, kDesignCenterX, static_cast<int32_t>(kSectionY),
              contracts::ui_theme::kTextNormal, 2, kBodyFont);

    draw_column(page->left, kLeftLabelX, kLeftValueRightX);
    draw_column(page->right, kRightLabelX, kRightValueRightX);

    char page_text[48];
    (void)std::snprintf(page_text, sizeof(page_text), "第 %u / %u 页",
                        static_cast<unsigned>(g_page + 1),
                        static_cast<unsigned>(g_page_count));
    draw_text(page_text, kDesignCenterX, static_cast<int32_t>(kPageTextY),
              contracts::ui_theme::kTextDisabled, 2, kBodyFont);
}

}  // namespace

void overview_bind_components(const void* ui_components) {
    g_components = static_cast<const contracts::QolUiComponents*>(ui_components);
}

void overview_bind_context(const qol::contracts::QolGameHost* game,
                           const void* character_extras_port) {
    g_game = game;
    g_character_extras =
        static_cast<const contracts::QolCharacterExtrasPort*>(character_extras_port);
}

void set_overview_enabled(bool enabled) {
    g_overview_enabled.store(enabled, std::memory_order_release);
}

bool overview_enabled() { return g_overview_enabled.load(std::memory_order_acquire); }

bool overview_provider_open() {
    return components_ready(g_components) && overview_enabled();
}

void overview_provider_close() {
    destroy_controls();
    g_active.store(false, std::memory_order_release);
}

bool overview_provider_active() { return g_active.load(std::memory_order_acquire); }

const char* overview_provider_block_reason() {
    if (!overview_enabled()) return kBlockedDisabled;
    if (!components_ready(g_components)) return kBlockedComponents;
    return nullptr;
}

void overview_panel_enter() {
    if (g_root == nullptr && !create_controls()) return;
    // 每次打开（含嵌套返回后的重新进入）只读取一次数据；翻页不再读取。
    g_page_count = kPageCount;
    g_page = 0;
    read_snapshot();
    g_active.store(true, std::memory_order_release);
}

void overview_panel_process() {
    if (!g_active.load(std::memory_order_acquire) || g_root == nullptr) return;
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui == nullptr || ui->begin_frame == nullptr || ui->end_frame == nullptr) return;
    if (!contracts::succeeded(ui->begin_frame())) return;
    draw_page();
    if (g_prev != nullptr) {
        (void)g_components->arrow_button_set_enabled(g_prev, g_page > 0 ? 1 : 0);
        (void)g_components->arrow_button_draw(g_prev);
    }
    if (g_next != nullptr) {
        (void)g_components->arrow_button_set_enabled(g_next, g_page + 1 < g_page_count ? 1 : 0);
        (void)g_components->arrow_button_draw(g_next);
    }
    if (g_nav != nullptr) (void)g_components->nav_button_draw(g_nav);
    (void)ui->end_frame();
}

void overview_panel_f3() {
    // 游戏侧主动终止：只销毁本页控件，pop/restore 仍由 PopupHost 处理。
    destroy_controls();
    g_active.store(false, std::memory_order_release);
}

void overview_panel_f4() {}

uint32_t overview_panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    if (!g_active.load(std::memory_order_acquire) || g_root == nullptr ||
        g_components == nullptr || g_components->dispatch_native_event == nullptr) {
        return 0;
    }
    if (event != 0x17 && event != 0x18 && event != 0x19) return 0;
    if (event == 0x17) g_touch_root = g_root;
    const contracts::QolUiHandle dispatch_root = g_touch_root != nullptr ? g_touch_root : g_root;
    uint32_t consumed = 0;
    (void)g_components->dispatch_native_event(dispatch_root, event, param, param2, &consumed);
    if (event == 0x18) g_touch_root = nullptr;
    return 1;
}

}  // namespace qol::features::character_panel

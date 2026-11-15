#include "../plugin.h"

#include "../../../contracts/panels.h"
#include "../../../contracts/ui.h"
#include "../../../contracts/ui_theme.h"
#include "../../../contracts/item_equipment_limits.h"
#include "../../../contracts/attribute_quality.h"
#include "../../../contracts/game_ports/item_port.h"
#include "../../../contracts/log_domain.h"
#include "../../../contracts/item_display_name.h"
#include "../model/draft.h"
#include "panel_internal.h"
#include "actions.h"
#include "cards/cards.h"
#include "cards/numeric_card.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace qol::features::item_editor {

// panel_internal.h 声明的共享定义集中在本文件；ui/cards/ 经该头文件使用。
namespace panel_internal {
using namespace contracts;

// 主面板几何与共享绘制/读取 helper 的私有常量；不进入头文件。
constexpr int32_t kPanelW = 900;
constexpr int32_t kPanelH = 560;
constexpr int32_t kRightRowH = 75;

ItemEditorDraft g_draft{};

const QolUiComponents* components() {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* value = nullptr;
    if (!succeeded(sdk->capabilities.require(kUiComponentsCapabilityId,
            {kUiContractMajor, 27}, &value))) return nullptr;
    const auto* api = static_cast<const QolUiComponents*>(value);
    constexpr size_t required_size = offsetof(QolUiComponents,item_slot_set_quality) +
        sizeof(decltype(QolUiComponents::item_slot_set_quality));
    return api != nullptr && api->minor >= 27 &&
        api->size >= required_size ? api : nullptr;
}
const QolUiAdapter* ui() { return host_sdk() == nullptr ? nullptr : host_sdk()->ui; }
UiRect panel_rect() {
    int32_t inset_w = 0, inset_h = 0;
    const auto* adapter = ui();
    if (adapter != nullptr) {
        if (adapter->screen_inset_width != nullptr) (void)adapter->screen_inset_width(&inset_w);
        if (adapter->screen_inset_height != nullptr) (void)adapter->screen_inset_height(&inset_h);
    }
    const int64_t sw = 960 + static_cast<int64_t>(inset_w) * 2;
    const int64_t sh = 640 + static_cast<int64_t>(inset_h) * 2;
    const int64_t logical_w = sh > 0 ? sw * 640 / sh : 960;
    return {(logical_w-kPanelW)/2, (640-kPanelH)/2, kPanelW, kPanelH};
}

void log_panel(const char* message) {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    (void)sdk->log.write(1, static_cast<int32_t>(QolDomain::kUi), "item_editor/panel",
                         message, static_cast<uint32_t>(std::strlen(message)));
}

void draw_text(QolUiHandle control, const char* text, int32_t x, int32_t y,
               uint32_t color, int32_t align, int32_t font) {
    const auto* c = components();
    if (c != nullptr && c->draw_control_text != nullptr)
        (void)c->draw_control_text(control, text, x, y, color, align, font);
}

int32_t text_pixel_width(const char* text, int32_t font) {
    if (text == nullptr || text[0] == '\0') return 0;
    const auto* adapter = ui();
    int32_t width = 0;
    if (adapter != nullptr && adapter->minor >= 40 && adapter->text_width != nullptr &&
        succeeded(adapter->text_width(text, font, &width)) && width > 0) {
        return width;
    }
    // 测量接口不可用时保守留出 80px，避免文本从右边界继续向右溢出。
    return 80;
}

void draw_center_text(QolUiHandle control, const char* text, int32_t center,
                      int32_t y, uint32_t color, int32_t font) {
    const int32_t width = text_pixel_width(text, font);
    draw_text(control, text, center - width / 2, y, color, 0, font);
}

void read_item() {
    draft_clear(&g_draft);
    g_draft.item_id = -1;
    const auto* adapter = ui();
    int32_t detail_open = 0;
    if (adapter != nullptr && adapter->item_detail_open != nullptr) {
        if (!succeeded(adapter->item_detail_open(&detail_open)) || detail_open == 0) return;
    }
    const auto* port = item_port();
    if (port == nullptr || port->read.detail_item_context == nullptr ||
        port->read.inventory_item_view == nullptr || port->read.item_class_index == nullptr) return;
    QolItemDetailContext detail{};
    if (!succeeded(port->read.detail_item_context(&detail)) || detail.item.slot == 0) return;
    // 1 是袋信息面板，不是物品详情；不能把 cursor 默认 0 当成首格物品。
    if (detail.desc_type != 0 && detail.desc_type != 2) return;
    QolItemInventoryView view{};
    if (!succeeded(port->read.inventory_item_view(detail.item, &view))) return;
    int32_t class_index = -1;
    if (!succeeded(port->read.item_class_index(detail.item, &class_index))) class_index = -1;
    QolItemType item_type = QolItemType::kUnknown;
    if (port->read.item_type != nullptr)
        (void)port->read.item_type(detail.item, &item_type);
    draft_load(&g_draft, detail.item, view, class_index, item_type);
    const char* prefix_class = nullptr;
    if (port->read.static_text != nullptr &&
        succeeded(port->read.static_text(static_cast<int32_t>(
            QolItemStaticTextKind::kItemPrefixClass), view.category, &prefix_class)) &&
        prefix_class != nullptr) {
        g_draft.prefix_class_bits = std::atoi(prefix_class);
    }
}

const char* static_text(QolItemStaticTextKind kind, int32_t id) {
    const auto* port = item_port();
    const char* text = nullptr;
    if (port != nullptr && port->read.static_text != nullptr &&
        succeeded(port->read.static_text(static_cast<int32_t>(kind), id, &text))) return text;
    return nullptr;
}

void resolve_variant_series() {
    if (g_draft.source_valid && g_draft.variant_series != 0) return;
    const auto* game = game_host();
    if (game == nullptr || game->target.query_variant == nullptr) return;
    QolTargetVariant variant{};
    if (succeeded(game->target.query_variant(&variant)))
        g_draft.variant_series = static_cast<int32_t>(variant.id);
}

// 左栏行集合：卡片确认只经重建函数刷新，不改行集合本身。
enum class LeftRowKind : uint8_t {
    kQuantity,
    kPrefix,
    kEnchantDurability,
    kSocketTotal,
    kDamage,
    kDefense,
    kMagicDamage,
    kRequiredLevel,
    kChaosRate,
};
LeftRowKind g_left_rows[8]{};
uint32_t g_left_row_count = 0;

void build_left_rows() {
    g_left_row_count = 0;
    if (item_class_is_equipment(g_draft.class_index)) {
        g_left_rows[g_left_row_count++] = LeftRowKind::kPrefix;
        g_left_rows[g_left_row_count++] = LeftRowKind::kEnchantDurability;
        g_left_rows[g_left_row_count++] = LeftRowKind::kSocketTotal;
        if (g_draft.damage != 0)
            g_left_rows[g_left_row_count++] = LeftRowKind::kDamage;
        if (g_draft.defense != 0)
            g_left_rows[g_left_row_count++] = LeftRowKind::kDefense;
        const int64_t magic_scaled =
            static_cast<int64_t>(g_draft.damage) * g_draft.magic_rate;
        if (magic_scaled != 0)
            g_left_rows[g_left_row_count++] = LeftRowKind::kMagicDamage;
        g_left_rows[g_left_row_count++] = LeftRowKind::kRequiredLevel;
        g_left_rows[g_left_row_count++] = LeftRowKind::kChaosRate;
    } else if (item_class_valid(g_draft.class_index)) {
        g_left_rows[g_left_row_count++] = LeftRowKind::kQuantity;
    }
}

}  // namespace panel_internal

using namespace panel_internal;

namespace {
using namespace contracts;
// 行背景右边缘与右对齐文本之间保留的安全间距。
constexpr int32_t kRowTextRightMargin = 8;
QolUiHandle g_root = nullptr;
QolUiHandle g_list = nullptr;
QolUiHandle g_left_list = nullptr;
QolUiHandle g_close = nullptr;
QolUiHandle g_icon_slot = nullptr;
QolUiHandle g_icon_hit_button = nullptr;
QolUiHandle g_name_row = nullptr;
QolUiHandle g_category_row = nullptr;
std::atomic<bool> g_active{false};

void noop_click(void*) {}
void noop_draw(void*) {}
void item_slot_clicked(void*) { item_card_open(); }
void category_row_clicked(void*) { category_card_open(); }

const char* item_type_name(QolItemType type) {
    switch (type) {
        case QolItemType::kConsumable: return "消耗品";
        case QolItemType::kEquipment: return "装备";
        case QolItemType::kBackpack: return "背包";
        case QolItemType::kJewel: return "宝石";
        case QolItemType::kEnchantScroll: return "附魔卷轴";
        case QolItemType::kMercenarySeal: return "佣兵封印";
        case QolItemType::kSkillBook: return "技能书";
        case QolItemType::kItemBox: return "道具箱";
        case QolItemType::kSynthSet: return "合成套";
        case QolItemType::kPotion: return "药水";
        case QolItemType::kDice: return "骰子";
        case QolItemType::kReviveScroll: return "复活卷轴";
        case QolItemType::kLamp: return "灯";
        case QolItemType::kUseAfterConfirm: return "确认后使用";
        case QolItemType::kOther: return "其他";
        case QolItemType::kGemRemover: return "宝石拆卸器";
        case QolItemType::kUnknown:
        default: return "未知";
    }
}

void format_category_row(char* text, size_t capacity) {
    if (text == nullptr || capacity == 0) return;
    if (!draft_has_item(g_draft)) {
        if (item_class_valid(g_draft.class_index)) {
            const char* class_name = static_text(
                QolItemStaticTextKind::kItemClassList, g_draft.class_index);
            std::snprintf(text,capacity,"%s",
                class_name == nullptr ? "未知类别" : class_name);
        } else {
            text[0] = '\0';
        }
        return;
    }
    const char* type_name = item_type_name(g_draft.item_type);
    const int32_t type_id = static_cast<int32_t>(g_draft.item_type);
    if (item_class_is_equipment(g_draft.class_index)) {
        const char* class_name = static_text(
            QolItemStaticTextKind::kItemClassName, g_draft.item_id);
        if (class_name != nullptr && class_name[0] != '\0')
            std::snprintf(text,capacity,"%s",class_name);
        else
            std::snprintf(text,capacity,"%s",type_name);
    } else {
        std::snprintf(text,capacity,"%s (id:%d)",type_name,type_id);
    }
}
void draw_right_text(QolUiHandle control, const char* text, int32_t right, int32_t y,
                     uint32_t color, int32_t font = 0) {
    const int32_t width = text_pixel_width(text, font);
    draw_text(control, text, right - width, y, color, 0, font);
}

void draw_right_text_abs(int32_t right, int32_t y, const char* text, uint32_t color,
                         int32_t font = 0) {
    const auto* adapter = ui();
    if (adapter == nullptr || adapter->set_font_color_rgb == nullptr ||
        adapter->draw_string_with_font == nullptr || text == nullptr) return;
    const int32_t width = text_pixel_width(text, font);
    (void)adapter->set_font_color_rgb(static_cast<int32_t>(color & 0xff),
        static_cast<int32_t>((color >> 8) & 0xff),
        static_cast<int32_t>((color >> 16) & 0xff));
    (void)adapter->draw_string_with_font(text, right - width, y, 0, font);
}
const AttributeQualityApi* attribute_quality_api() {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const QolResult result = sdk->capabilities.require(kAttributeQualityCapabilityId,
        {kAttributeQualityVersionMajor, kAttributeQualityVersionMinor}, &iface);
    if (!succeeded(result) || iface == nullptr) return nullptr;
    const auto* api = static_cast<const AttributeQualityApi*>(iface);
    return attribute_quality_api_size_valid(api) ? api : nullptr;
}
char quality_color_code(int32_t level) {
    switch (level) {
        case 0: return 'G';
        case 1: return 'W';
        case 2: return 'C';
        case 3: return 'L';
        case 4: return 'V';
        case 5: return 'Y';
        default: return '\0';
    }
}
bool section_enabled(uint32_t section) {
    const ItemClassGroup group = item_class_group(g_draft.class_index);
    if (group == ItemClassGroup::kWeapon || group == ItemClassGroup::kEquipment) return true;
    return g_draft.class_index == kItemClassGem && section == 1;
}
uint32_t option_count(uint32_t section) {
    if (section == 0)
        return std::min(g_draft.native_option_count, kQolItemInventoryNativeOptionCapacity);
    if (section == 1) {
        if (g_draft.class_index == kItemClassGem) {
            return 1u;
        }
        return std::min(g_draft.jewel_option_count, kQolItemInventoryJewelOptionCapacity);
    }
    return std::min(g_draft.enchant_option_count, kQolItemInventoryEnchantOptionCapacity);
}
bool row_info(uint32_t row, uint32_t* section, uint32_t* index, bool* is_header) {
    if (section == nullptr || index == nullptr || is_header == nullptr) return false;
    for (uint32_t current = 0; current < 3; ++current) {
        if (row == 0) {
            *section = current;
            *index = 0;
            *is_header = true;
            return true;
        }
        --row;
        const uint32_t count = option_count(current);
        if (row < count) {
            *section = current;
            *index = row;
            *is_header = false;
            return true;
        }
        row -= count;
    }
    return false;
}

// 右列表行内 +/× 命中动作。行按钮由列表拥有并随列表销毁；user 上下文
// 常驻 g_right_hits 静态池，回调只记录待处理动作，改动统一放在下一帧
// panel_process 安全点执行，不在回调内同步重建列表。
enum class RightRowAction : uint8_t { kAdd, kDelete };
struct RightRowHit {
    uint32_t section;
    uint32_t index;
    RightRowAction action;
};
// 上限 = 3 个标题 + 3×32 词条行（宝石段实际受 15 限制），取整留裕量。
constexpr uint32_t kMaxRightRowHits = 256;
RightRowHit g_right_hits[kMaxRightRowHits]{};
uint32_t g_right_hit_count = 0;
RightRowHit g_pending_right_hit{};
bool g_pending_right_valid = false;

void right_row_hit_clicked(void* user) {
    if (user == nullptr) return;
    g_pending_right_hit = *static_cast<const RightRowHit*>(user);
    g_pending_right_valid = true;
}

// 行本体点击打开词条编辑卡；行内 +/× 子按钮会先消费自己的区域。
void right_row_selected(void*, uint32_t row_index) {
    uint32_t section = 0, index = 0;
    bool is_header = false;
    if (!row_info(row_index, &section, &index, &is_header) || is_header) return;
    if (!section_enabled(section) || index >= option_count(section)) return;
    option_card_open(section, index);
}

// 标题行 + 是否可用：段禁用、宝石本体段或达到容量/孔数上限时不可添加。
bool section_add_available(uint32_t section) {
    if (!section_enabled(section)) return false;
    if (section == 0)
        return g_draft.native_option_count < kQolItemInventoryNativeOptionCapacity;
    if (section == 1) {
        // 宝石本体段没有“添加词条”语义；装备上的宝石词条受孔数上限 15
        // （socket_total/filled 是 4 位字段），数组容量 32 不会先达到。
        if (g_draft.class_index == kItemClassGem) return false;
        return g_draft.jewel_option_count < kItemSocketTotalMax;
    }
    return g_draft.enchant_option_count < kQolItemInventoryEnchantOptionCapacity;
}

// 词条行 × 是否可用：宝石本体行禁用。
bool row_delete_available(uint32_t section, uint32_t index) {
    if (!section_enabled(section)) return false;
    if (section == 1 && g_draft.class_index == kItemClassGem) return false;
    return index < option_count(section);
}

// 段末尾追加空词条 {id=0,value=0}；只改 draft。宝石词条段额外把
// socket_total/filled 抬到实际占用数，不缩小已有值。
void add_empty_option(uint32_t section) {
    const int32_t type = static_cast<int32_t>(
        section == 0 ? QolItemInventoryOptionType::kNative
                     : (section == 1 ? QolItemInventoryOptionType::kJewel
                                     : QolItemInventoryOptionType::kEnchant));
    // QolItemInventoryOptionView 字段顺序是 {value,id,type}；ID 0 是合法词条（力量），
    // 空词条必须把 id 设为 -1，不能把 -1 写进 value。
    const QolItemInventoryOptionView empty{0, -1, type};
    switch (section) {
        case 0:
            if (g_draft.native_option_count >= kQolItemInventoryNativeOptionCapacity)
                return;
            g_draft.native_options[g_draft.native_option_count++] = empty;
            break;
        case 1:
            if (g_draft.class_index == kItemClassGem) return;
            if (g_draft.jewel_option_count >= kQolItemInventoryJewelOptionCapacity)
                return;
            if (g_draft.jewel_option_count >= kItemSocketTotalMax) return;
            if (g_draft.socket_total > kItemSocketTotalMax ||
                g_draft.socket_filled > kItemSocketTotalMax)
                return;
            g_draft.jewel_options[g_draft.jewel_option_count++] = empty;
            g_draft.socket_total = std::max(g_draft.socket_total,
                static_cast<int32_t>(g_draft.jewel_option_count));
            g_draft.socket_filled = std::max(g_draft.socket_filled,
                static_cast<int32_t>(g_draft.jewel_option_count));
            break;
        case 2:
            if (g_draft.enchant_option_count >= kQolItemInventoryEnchantOptionCapacity)
                return;
            g_draft.enchant_options[g_draft.enchant_option_count++] = empty;
            break;
        default:
            return;
    }
    g_draft.dirty = true;
    rebuild_right_list();
}

// 命中回调的延迟执行点：添加立即生效；删除转成确认卡打开请求，
// 由本轮 panel_process 的 open 路由创建确认卡。
void process_pending_right_hit() {
    if (!g_pending_right_valid) return;
    const RightRowHit hit = g_pending_right_hit;
    g_pending_right_valid = false;
    g_pending_right_hit = {};
    if (hit.action == RightRowAction::kAdd) add_empty_option(hit.section);
    else delete_option_card_open(hit.section, hit.index);
}

void draw_right_row(void*, uint32_t row_index, QolUiHandle row, uint8_t) {
    const auto* adapter = ui();
    const auto* c = components();
    if (adapter == nullptr || c == nullptr) return;
    int64_t abs_x = 0, abs_y = 0;
    if (adapter->control_absolute_origin == nullptr ||
        !succeeded(adapter->control_absolute_origin(row, &abs_x, &abs_y))) return;
    uint32_t section = 0, index = 0;
    bool is_header = false;
    if (!row_info(row_index, &section, &index, &is_header)) return;
    if (is_header) {
        constexpr const char* titles[] = {"装备词条", "宝石词条", "附魔词条"};
        draw_text(row, titles[section], 14, 26, section_enabled(section) ? kGold : kMuted);
        draw_text(row, "+", 366, 18, section_add_available(section) ? kGold : kMuted, 1, 2);
        return;
    }
    const bool enabled = section_enabled(section);
    if (adapter->draw_help_option_row != nullptr) {
        int32_t pressed = 0;
        if (enabled && adapter->control_pressed != nullptr)
            (void)adapter->control_pressed(row, &pressed);
        (void)adapter->draw_help_option_row({abs_x + 4, abs_y + 4, 384, 67}, 0,
                                            static_cast<uint8_t>(pressed != 0));
        // 禁用段保留同一格框，再压暗；删除正文命中高亮但不要删掉框。
        if (!enabled && adapter->fill_rect_alpha != nullptr)
            (void)adapter->fill_rect_alpha({abs_x + 4, abs_y + 4, 384, 67},
                                           0xFF000000u, 40);
    }
    const uint32_t count = option_count(section);
    const QolItemInventoryOptionView* option = section == 0 ? g_draft.native_options + index :
        (section == 1 ? g_draft.jewel_options + index : g_draft.enchant_options + index);
    const char* name = nullptr;
    char value[64]{};
    uint32_t value_color = enabled ? kText : kMuted;
    if (index < count) {
        const bool jewel_self = section == 1 && g_draft.class_index == kItemClassGem;
        const int32_t option_id = jewel_self ? g_draft.jewel_attribute_type : option->id;
        const int32_t option_value = jewel_self ? g_draft.jewel_value : option->value;
        // 通过 + 添加的空词条 id=0：显示“空”且不显示值，不查静态表和品质，
        // 避免把 id=0 的真实词条名误显示在空词条上。
        const bool empty_option = !jewel_self && option_id < 0;
        name = empty_option ? "空"
                            : static_text(QolItemStaticTextKind::kOptionName, option_id);
        const bool unknown_name = !empty_option &&
            (name == nullptr || name[0] == '\0');
        if (unknown_name) name = "未知";
        const auto* port = item_port();
        const char* percent_text = nullptr;
        const bool is_percent = port != nullptr && port->read.static_text != nullptr &&
            succeeded(port->read.static_text(
                static_cast<int32_t>(QolItemStaticTextKind::kItemOptionPercent),
                option_id, &percent_text)) && percent_text != nullptr &&
            percent_text[0] == '1' && !unknown_name;
        const bool colorable = (section == 0 &&
            item_class_is_equipment(g_draft.class_index)) ||
            (section == 1 && g_draft.class_index == kItemClassGem);
        if (colorable && !unknown_name) {
            const auto* api = draft_has_item(g_draft) ? attribute_quality_api() : nullptr;
            if (api != nullptr) {
                AttributeQualityItem quality{};
                const QolResult result = api->query_option_context(
                    g_draft.source, option_id, option_value, nullptr, &quality);
                if (result.ok == kQolOk && quality.status == kQolOk) {
                    const char color_code = quality_color_code(quality.quality.level);
                    if (color_code != '\0' && adapter->inline_color_from_code != nullptr) {
                        uint32_t packed = value_color;
                        if (succeeded(adapter->inline_color_from_code(
                                static_cast<int32_t>(color_code), &packed)))
                            value_color = packed;
                    }
                }
            }
        }
        if (!empty_option) {
            if (is_percent) {
                std::snprintf(value, sizeof(value), "%+.1f%%",
                              static_cast<double>(option_value) / 10.0);
            } else {
                std::snprintf(value, sizeof(value), "%+d", option_value);
            }
        }
    } else {
        name = "空";
    }
    draw_text(row, name, 20, 26, enabled ? kText : kMuted);
    draw_right_text(row, value, 300, 26, value_color, 1);
    draw_text(row, "×", 366, 18, row_delete_available(section, index) ? kGold : kMuted, 1, 2);
}

void draw_name_row(const char* value) {
    const auto* adapter = ui();
    if (adapter == nullptr || g_name_row == nullptr) return;
    constexpr UiRect rect{122,152,316,48};
    const bool enabled = item_class_valid(g_draft.class_index);
    int32_t pressed = 0;
    if (enabled && adapter->control_pressed != nullptr)
        (void)adapter->control_pressed(g_name_row, &pressed);
    if (adapter->draw_help_option_row != nullptr) {
        const UiRect panel = panel_rect();
        (void)adapter->draw_help_option_row({panel.x + rect.x, panel.y + rect.y,
            rect.w, rect.h}, 0, static_cast<uint8_t>(pressed != 0));
    }
    draw_text(g_root, "名称", static_cast<int32_t>(rect.x + 16),
              static_cast<int32_t>(rect.y + 14), enabled ? kText : kMuted, 0, 0);
    // 名称行背景传入宽 316，draw_help_option_row 内缩后右边缘为 rect.x + rect.w - 4。
    draw_right_text(g_root, value == nullptr ? "" : value,
                    static_cast<int32_t>(rect.x + rect.w - 4 - kRowTextRightMargin),
                    static_cast<int32_t>(rect.y + 14), enabled ? kText : kMuted);
}
void draw_fixed_category_row() {
    const auto* adapter = ui();
    if (adapter == nullptr || g_category_row == nullptr) return;
    int64_t abs_x = 0,abs_y = 0;
    if (adapter->control_absolute_origin == nullptr ||
        !succeeded(adapter->control_absolute_origin(g_category_row,&abs_x,&abs_y))) return;
    int32_t pressed = 0;
    if (adapter->control_pressed != nullptr)
        (void)adapter->control_pressed(g_category_row,&pressed);
    if (adapter->draw_help_option_row != nullptr)
        (void)adapter->draw_help_option_row({abs_x,abs_y,316,48},0,
            static_cast<uint8_t>(pressed != 0));
    char value[120]{};
    format_category_row(value,sizeof(value));
    draw_text(g_category_row,"类别",16,14,kText,0,0);
    draw_right_text(g_category_row,value,312 - kRowTextRightMargin,14,kText);
}
void draw_left_info_row(void*, uint32_t index, QolUiHandle row, uint8_t) {
    const auto* adapter = ui();
    if (adapter == nullptr || index >= g_left_row_count) return;
    const bool enabled = g_left_rows[index] != LeftRowKind::kQuantity ||
                         g_draft.stackable;
    int64_t abs_x = 0, abs_y = 0;
    if (adapter->control_absolute_origin == nullptr ||
        !succeeded(adapter->control_absolute_origin(row, &abs_x, &abs_y))) return;
    if (adapter->draw_help_option_row != nullptr) {
        int32_t pressed = 0;
        if (enabled && adapter->control_pressed != nullptr)
            (void)adapter->control_pressed(row, &pressed);
        (void)adapter->draw_help_option_row({abs_x + 4, abs_y + 2, 308, 44}, 0,
                                            static_cast<uint8_t>(pressed != 0));
    }
    char key[40]{};
    char value[128]{};
    switch (g_left_rows[index]) {
        case LeftRowKind::kQuantity:
            std::snprintf(key, sizeof(key), "数量");
            std::snprintf(value, sizeof(value), "%d", g_draft.count);
            break;
        case LeftRowKind::kPrefix: {
            const char* prefix = contracts::item_rarity_prefix(g_draft.variant_series,
                g_draft.prefix_class_bits, g_draft.raw_rarity);
            const bool has_prefix = prefix != nullptr && prefix[0] != '\0';
            const char* label = has_prefix ? prefix : "无";
            size_t label_length = std::strlen(label);
            while (has_prefix && label_length > 0 && label[label_length - 1] == ' ')
                --label_length;
            std::snprintf(key, sizeof(key), "前缀");
            std::snprintf(value, sizeof(value), "%.*s (raw:%d)",
                static_cast<int>(label_length), label, g_draft.raw_rarity);
            break;
        }
        case LeftRowKind::kEnchantDurability:
            std::snprintf(key, sizeof(key), "强化耐久度");
            std::snprintf(value, sizeof(value), "%d", g_draft.enchant_remaining);
            break;
        case LeftRowKind::kSocketTotal:
            std::snprintf(key, sizeof(key), "宝石孔数");
            std::snprintf(value, sizeof(value), "%d", g_draft.socket_total);
            break;
        case LeftRowKind::kDamage:
            std::snprintf(key, sizeof(key), "伤害");
            std::snprintf(value, sizeof(value), "%d", g_draft.damage);
            break;
        case LeftRowKind::kDefense:
            std::snprintf(key, sizeof(key), "防御");
            std::snprintf(value, sizeof(value), "%d", g_draft.defense);
            break;
        case LeftRowKind::kMagicDamage: {
            std::snprintf(key, sizeof(key), "魔法攻击");
            if (g_draft.damage == 0 && g_draft.magic_rate == 0) {
                std::snprintf(value, sizeof(value), "0（0.00x）");
            } else {
                const double multiplier = static_cast<double>(g_draft.magic_rate) / 100.0;
                const double magic_damage = static_cast<double>(g_draft.damage) * multiplier;
                std::snprintf(value, sizeof(value), "%.1f（%.2fx）",
                    magic_damage, multiplier);
            }
            break;
        }
        case LeftRowKind::kRequiredLevel:
            std::snprintf(key, sizeof(key), "需要等级");
            std::snprintf(value, sizeof(value), "%d", g_draft.need_level);
            break;
        case LeftRowKind::kChaosRate:
            std::snprintf(key, sizeof(key), "混沌属性倍率");
            std::snprintf(value, sizeof(value), "%d%%", g_draft.chaos_rate);
            break;
        default:
            return;
    }
    draw_text(row, key, 16, 14, enabled ? kText : kMuted, 0, 0);
    // 背景由 draw_help_option_row 以绝对坐标绘制；value 必须使用同一坐标系，
    // 不能用 scroll row 的局部坐标，否则右侧会被列表裁剪。
    // draw_help_option_row({x+4, ..., 308}) 内部再内缩 4px，实际右边缘是 x+308；
    // 再留出安全间距，避免文字贴住背景边缘。
    draw_right_text_abs(static_cast<int32_t>(abs_x) + 308 - kRowTextRightMargin,
                        static_cast<int32_t>(abs_y) + 14, value,
                        enabled ? kText : kMuted);
}
void left_info_selected(void*, uint32_t index) {
    if (index >= g_left_row_count) return;
    switch (g_left_rows[index]) {
        case LeftRowKind::kQuantity:
            open_quantity_card();
            return;
        case LeftRowKind::kPrefix:
            prefix_card_open();
            return;
        case LeftRowKind::kEnchantDurability:
            open_enchant_card();
            return;
        case LeftRowKind::kSocketTotal:
            open_socket_card();
            return;
        case LeftRowKind::kChaosRate:
            open_chaos_rate_card();
            return;
        default:
            return;
    }
}

void destroy() {
    actions_destroy();
    destroy_item_card();
    destroy_category_card();
    destroy_prefix_card();
    destroy_numeric_card();
    destroy_delete_option_card();
    destroy_option_card();
    g_right_hit_count = 0;
    g_pending_right_valid = false;
    g_pending_right_hit = {};
    const auto* c = components();
    if (c != nullptr && c->destroy != nullptr) {
        if (g_list != nullptr) (void)c->destroy(g_list);
        if (g_left_list != nullptr) (void)c->destroy(g_left_list);
        if (g_icon_slot != nullptr) (void)c->destroy(g_icon_slot);
        if (g_icon_hit_button != nullptr) (void)c->destroy(g_icon_hit_button);
        if (g_category_row != nullptr) (void)c->destroy(g_category_row);
        if (g_name_row != nullptr) (void)c->destroy(g_name_row);
        if (g_root != nullptr) (void)c->destroy(g_root);
    }
    g_list = nullptr;
    g_left_list = nullptr;
    g_icon_slot = nullptr;
    g_icon_hit_button = nullptr;
    g_category_row = nullptr;
    g_name_row = nullptr;
    g_root = nullptr;
    g_close = nullptr;
    g_active.store(false, std::memory_order_release);
}
bool create() {
    const auto* c = components();
    if (c == nullptr || c->panel_create == nullptr || c->scroll_list_create == nullptr ||
        c->nav_button_create == nullptr || c->button_create == nullptr) return false;
    resolve_variant_series();
    const UiRect panel = panel_rect();
    const UiPanelDesc desc{{panel.x,panel.y,panel.w,panel.h},
        {0,0,panel.x*2+panel.w,640},0,0};
    if (!succeeded(c->panel_create(&desc, nullptr, &g_root))) return false;
    rebuild_item_slot();
    if (g_icon_slot == nullptr) return false;
    const UiButtonDesc name_button{{122,152,316,48},"",0,&item_slot_clicked,nullptr};
    if (!succeeded(c->button_create(&name_button, g_root, &g_name_row))) return false;
    if (ui() != nullptr && ui()->set_draw_proc != nullptr)
        (void)ui()->set_draw_proc(g_name_row, [](void*) {});
    const UiButtonDesc category_button{{122,210,316,48},"",0,
        &category_row_clicked,nullptr};
    if (!succeeded(c->button_create(&category_button,g_root,&g_category_row))) return false;
    const auto* adapter = ui();
    if (adapter == nullptr || adapter->set_draw_proc == nullptr ||
        !succeeded(adapter->set_draw_proc(g_category_row,&noop_draw))) return false;
    rebuild_left_list();
    rebuild_right_list();
    if (g_list == nullptr) return false;
    const UiNavButtonDesc close{{contracts::ui_theme::kNavBoxAbsX - panel.x,
        contracts::ui_theme::kNavBoxAbsY - panel.y,
        contracts::ui_theme::kNavBoxW, contracts::ui_theme::kNavBoxH},
        kUiNavButtonForceClose,nullptr,nullptr};
    if (!succeeded(c->nav_button_create(&close,g_root,&g_close))) return false;
    // 动作列 root 独立于主面板，避免被主面板 rect 裁剪；失败按 fail-closed 关闭面板。
    if (!actions_create()) return false;
    return true;
}
}

// 列表重建依赖匿名命名空间内的面板 handle 与绘制回调，
// 因此定义放在匿名命名空间之后；声明在 panel_internal.h。
namespace panel_internal {

void rebuild_left_list() {
    const auto* c = components();
    if (c == nullptr || c->scroll_list_create == nullptr) return;
    if (g_left_list != nullptr && c->destroy != nullptr) (void)c->destroy(g_left_list);
    g_left_list = nullptr;
    if (g_left_row_count == 0) return;
    const UiScrollListDesc list{{122,268,316,232},48,g_left_row_count,-1,
        &draw_left_info_row,&left_info_selected,nullptr};
    (void)c->scroll_list_create(&list,g_root,&g_left_list);
}
void rebuild_right_list() {
    const auto* c = components();
    if (c == nullptr || c->scroll_list_create == nullptr) return;
    if (g_list != nullptr && c->destroy != nullptr) (void)c->destroy(g_list);
    g_list = nullptr;
    g_right_hit_count = 0;
    const uint32_t rows = 3 + option_count(0) + option_count(1) + option_count(2);
    const UiScrollListDesc list{{470,44,400,472},kRightRowH,rows,-1,
        &draw_right_row,&right_row_selected,nullptr};
    (void)c->scroll_list_create(&list,g_root,&g_list);
    if (g_list == nullptr) return;
    (void)c->scroll_list_set_selection_enabled(g_list,1);
    // +/× 命中按钮：随列表销毁的行内子按钮，透明绘制，只覆盖图标区域；
    // user 指向 g_right_hits 静态池条目，回调后仍需保持有效。
    const auto* adapter = ui();
    for (uint32_t row = 0; row < rows && g_right_hit_count < kMaxRightRowHits; ++row) {
        uint32_t section = 0, index = 0;
        bool is_header = false;
        if (!row_info(row, &section, &index, &is_header)) continue;
        const RightRowAction action =
            is_header ? RightRowAction::kAdd : RightRowAction::kDelete;
        const UiRect hit_rect{340,0,60,75};
        const bool available = is_header ? section_add_available(section)
                                         : row_delete_available(section, index);
        if (!available) continue;
        RightRowHit& context = g_right_hits[g_right_hit_count];
        context = RightRowHit{section, index, action};
        QolUiHandle button = nullptr;
        if (!succeeded(c->scroll_row_button_create(g_list,row,
                hit_rect,&right_row_hit_clicked,&context,&button)))
            continue;
        if (adapter == nullptr || adapter->set_draw_proc == nullptr ||
            !succeeded(adapter->set_draw_proc(button,&noop_draw))) {
            if (c->destroy != nullptr) (void)c->destroy(button);
            continue;
        }
        ++g_right_hit_count;
    }
}
void rebuild_item_slot() {
    const auto* c = components();
    const auto* adapter = ui();
    if (c == nullptr || c->item_slot_create == nullptr || c->button_create == nullptr ||
        adapter == nullptr || adapter->set_draw_proc == nullptr) return;
    if (g_icon_slot != nullptr && c->destroy != nullptr) (void)c->destroy(g_icon_slot);
    if (g_icon_hit_button != nullptr && c->destroy != nullptr)
        (void)c->destroy(g_icon_hit_button);
    g_icon_slot = nullptr;
    g_icon_hit_button = nullptr;
    const uint32_t slot_flags = kUiItemSlotShowFrame |
        (item_class_valid(g_draft.class_index) ? 0u : kUiItemSlotDimmed);
    const int32_t quality = item_class_is_equipment(g_draft.class_index)
        ? contracts::item_rarity_from_raw_prefix(g_draft.raw_rarity) : -1;
    const int32_t displayed_quality = c->item_slot_set_quality == nullptr
        ? -1 : quality;
    const UiItemSlotDesc icon{{243,68,74,74},g_draft.item_id,0,slot_flags,
        displayed_quality};
    if (succeeded(c->item_slot_create(&icon,g_root,&g_icon_slot)) &&
        g_icon_slot != nullptr && c->item_slot_set_quality != nullptr)
        (void)c->item_slot_set_quality(g_icon_slot,quality);
    const UiButtonDesc hit{{243,68,74,74},"",0,&item_slot_clicked,nullptr};
    if (succeeded(c->button_create(&hit,g_root,&g_icon_hit_button))) {
        if (!succeeded(adapter->set_draw_proc(g_icon_hit_button,&noop_draw))) {
            if (c->destroy != nullptr) (void)c->destroy(g_icon_hit_button);
            g_icon_hit_button = nullptr;
        }
    } else {
        g_icon_hit_button = nullptr;
    }
}

}  // namespace panel_internal

void panel_enter() {
    read_item();
    resolve_variant_series();
    build_left_rows();
    if (!create()) { destroy(); panel_request_close(); return; }
    g_active.store(true, std::memory_order_release);
}
void panel_process() {
    if (!g_active.load(std::memory_order_acquire) || g_root == nullptr) return;
    const auto* sdk = host_sdk();
    uint8_t op = 0;
    if (sdk == nullptr || sdk->security == nullptr || sdk->security->op_enabled == nullptr ||
        !succeeded(sdk->security->op_enabled(sdk->security->context, &op)) || op == 0) {
        item_editor_force_disable();
        return;
    }
    category_card_apply();
    item_card_apply();
    prefix_card_apply();
    numeric_card_apply();
    delete_option_card_apply();
    option_card_apply();
    // 库存写动作在 begin_frame 之前执行，不能在绘制块内做。
    actions_process();
    process_pending_right_hit();
    if (prefix_card_open_requested()) {
        prefix_card_clear_open_request();
        category_card_clear_open_request();
        item_card_clear_open_request();
        numeric_card_clear_open_request();
        delete_option_card_clear_open_request();
        option_card_clear_open_request();
        destroy_category_card();
        destroy_item_card();
        destroy_numeric_card();
        destroy_delete_option_card();
        destroy_option_card();
        if (!create_prefix_card()) destroy_prefix_card();
    } else if (numeric_card_open_requested()) {
        numeric_card_clear_open_request();
        category_card_clear_open_request();
        item_card_clear_open_request();
        prefix_card_clear_open_request();
        delete_option_card_clear_open_request();
        option_card_clear_open_request();
        destroy_prefix_card();
        destroy_category_card();
        destroy_item_card();
        destroy_delete_option_card();
        destroy_option_card();
        if (!create_numeric_card()) destroy_numeric_card();
    } else if (item_card_open_requested()) {
        item_card_clear_open_request();
        category_card_clear_open_request();
        prefix_card_clear_open_request();
        numeric_card_clear_open_request();
        delete_option_card_clear_open_request();
        option_card_clear_open_request();
        destroy_prefix_card();
        destroy_category_card();
        destroy_numeric_card();
        destroy_delete_option_card();
        destroy_option_card();
        if (!create_item_card()) destroy_item_card();
    } else if (category_card_open_requested()) {
        category_card_clear_open_request();
        prefix_card_clear_open_request();
        item_card_clear_open_request();
        numeric_card_clear_open_request();
        delete_option_card_clear_open_request();
        option_card_clear_open_request();
        destroy_prefix_card();
        destroy_item_card();
        destroy_numeric_card();
        destroy_delete_option_card();
        destroy_option_card();
        if (!create_category_card()) destroy_category_card();
    } else if (option_card_open_requested()) {
        option_card_clear_open_request();
        category_card_clear_open_request();
        item_card_clear_open_request();
        prefix_card_clear_open_request();
        numeric_card_clear_open_request();
        delete_option_card_clear_open_request();
        destroy_prefix_card();
        destroy_category_card();
        destroy_item_card();
        destroy_numeric_card();
        destroy_delete_option_card();
        destroy_option_card();
        if (!create_option_card()) destroy_option_card();
    } else if (delete_option_card_open_requested()) {
        delete_option_card_clear_open_request();
        category_card_clear_open_request();
        item_card_clear_open_request();
        prefix_card_clear_open_request();
        numeric_card_clear_open_request();
        destroy_prefix_card();
        destroy_category_card();
        destroy_item_card();
        destroy_numeric_card();
        destroy_option_card();
        if (!create_delete_option_card()) destroy_delete_option_card();
    }
    const auto* adapter = ui(); const auto* c = components();
    if (adapter == nullptr || c == nullptr) return;
    if (adapter->begin_frame != nullptr) (void)adapter->begin_frame();
    const UiRect panel = panel_rect();
    if (c->draw_container_styled != nullptr) {
        const UiContainerStyleDesc total{{panel.x,panel.y,panel.w,panel.h},0x0000,60,1};
        const UiContainerStyleDesc left{{panel.x+93,panel.y+44,374,472},0x0000,0,0};
        (void)c->draw_container_styled(&total);
        (void)c->draw_container_styled(&left);
    }
    if (c->item_slot_draw != nullptr && g_icon_slot != nullptr) (void)c->item_slot_draw(g_icon_slot);
    if (!draft_has_item(g_draft)) {
        draw_text(g_root, "选择", 280, 85, kMuted, 2, 0);
        draw_text(g_root, "物品", 280, 105, kMuted, 2, 0);
    }
    char text[160]{};
    const char* name = draft_has_item(g_draft) ?
        static_text(QolItemStaticTextKind::kItemName,g_draft.item_id) : nullptr;
    if (draft_has_item(g_draft))
        std::snprintf(text,sizeof(text),"%s (id:%d)",
            name == nullptr ? "" : name,g_draft.item_id);
    else text[0] = '\0';
    draw_name_row(text);
    draw_fixed_category_row();
    if (!category_card_active() && !item_card_active() &&
        !prefix_card_active() && !numeric_card_active() &&
        !delete_option_card_active() && !option_card_active()) {
        if (g_left_list != nullptr) (void)c->scroll_list_process(g_left_list);
        (void)c->scroll_list_process(g_list);
    }
    if (g_left_list != nullptr) (void)c->scroll_list_draw(g_left_list);
    (void)c->scroll_list_draw(g_list);
    if (c->nav_button_draw != nullptr) (void)c->nav_button_draw(g_close);
    draw_category_card();
    draw_item_card();
    draw_prefix_card();
    if (numeric_card_active()) draw_numeric_card();
    draw_delete_option_card();
    draw_option_card();
    // 动作列只负责绘制；库存写已在 begin_frame 之前处理。
    actions_draw();
    if (adapter->end_frame != nullptr) (void)adapter->end_frame();
}
void panel_f3() { panel_destroy(); }
void panel_f4() {}
uint32_t panel_event(uint64_t event,uint64_t param,uint64_t param2) {
    const auto* c=components(); if(!g_active.load(std::memory_order_acquire)||g_root==nullptr||c==nullptr||c->dispatch_native_event==nullptr)return 1;
    uint32_t consumed=0;
    if (numeric_card_active()) {
        uint32_t numeric_consumed = 0;
        (void)numeric_card_handle_event(event,param,param2,&numeric_consumed);
        return numeric_consumed != 0 ? numeric_consumed : 1;
    }
    // 模态卡存活时吞掉全部事件；物品卡/类别卡未消费时继续派发给主面板。
    if (prefix_card_handle_event(event,param,param2,&consumed))
        return consumed != 0 ? consumed : 1;
    if (delete_option_card_handle_event(event,param,param2,&consumed))
        return consumed != 0 ? consumed : 1;
    if (option_card_handle_event(event,param,param2,&consumed))
        return consumed != 0 ? consumed : 1;
    if (item_card_handle_event(event,param,param2,&consumed) && consumed != 0)
        return consumed;
    if (category_card_handle_event(event,param,param2,&consumed) && consumed != 0)
        return consumed;
    (void)c->dispatch_native_event(g_root,event,param,param2,&consumed);
    // 主面板未消费时派发给独立动作列 root。
    if (consumed == 0) consumed = actions_dispatch_event(event,param,param2);
    return consumed;
}
void panel_destroy() { destroy(); }
bool panel_is_active() { return g_active.load(std::memory_order_acquire); }
}  // namespace qol::features::item_editor

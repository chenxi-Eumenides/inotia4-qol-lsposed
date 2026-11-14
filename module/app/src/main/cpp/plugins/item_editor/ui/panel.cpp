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

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

namespace qol::features::item_editor {
namespace {
using namespace contracts;
constexpr int32_t kPanelW = 900;
constexpr int32_t kPanelH = 560;
constexpr int32_t kRightRowH = 75;
// draw_control_text / draw_string_with_font 的颜色是 AABBGGRR；金色取统一主题常量。
constexpr uint32_t kText = 0xFFD5E8F2u;
constexpr uint32_t kGold = contracts::ui_theme::kTextGold;
constexpr uint32_t kMuted = 0xFF727A81u;
enum class LeftRowKind : uint8_t {
    kQuantity,
    kPrefix,
    kEnchantDurability,
    kSocketTotal,
    kDamage,
    kDefense,
    kMagicDamage,
    kRequiredLevel,
    kChaosLevel,
    kChaosRate,
};
QolUiHandle g_root = nullptr;
QolUiHandle g_list = nullptr;
QolUiHandle g_left_list = nullptr;
QolUiHandle g_close = nullptr;
QolUiHandle g_icon_slot = nullptr;
QolUiHandle g_icon_hit_button = nullptr;
QolUiHandle g_name_row = nullptr;
QolUiHandle g_category_row = nullptr;
QolUiHandle g_card_root = nullptr;
QolUiHandle g_card_footer_root = nullptr;
QolUiHandle g_card_list = nullptr;
QolUiHandle g_card_confirm = nullptr;
QolUiHandle g_card_cancel = nullptr;
QolUiHandle g_card_buttons[kItemClassCount]{};
int32_t g_card_button_classes[kItemClassCount]{};
ItemEditorDraft g_draft{};
LeftRowKind g_left_rows[9]{};
uint32_t g_left_row_count = 0;
int32_t g_card_selected_class = -1;
bool g_card_open_requested = false;
enum class CategoryCardAction : uint8_t { kNone, kConfirm, kCancel };
CategoryCardAction g_card_pending_action = CategoryCardAction::kNone;
QolUiHandle g_item_card_root = nullptr;
QolUiHandle g_item_card_footer_root = nullptr;
QolUiHandle g_item_card_list = nullptr;
QolUiHandle g_item_card_confirm = nullptr;
QolUiHandle g_item_card_cancel = nullptr;
std::vector<int32_t> g_item_card_ids;
std::vector<QolUiHandle> g_item_card_buttons;
constexpr size_t kItemCardSlotPoolSize = 12;
std::array<QolUiHandle, kItemCardSlotPoolSize> g_item_card_slots{};
int32_t g_item_card_selected_id = -1;
bool g_item_card_open_requested = false;
enum class ItemCardAction : uint8_t { kNone, kConfirm, kCancel };
ItemCardAction g_item_card_pending_action = ItemCardAction::kNone;
QolUiHandle g_quantity_card_root = nullptr;
QolUiHandle g_quantity_adjust_buttons[8]{};
QolUiHandle g_quantity_confirm = nullptr;
QolUiHandle g_quantity_cancel = nullptr;
int32_t g_quantity_value = 1;
bool g_quantity_card_open_requested = false;
enum class QuantityCardAction : uint8_t { kNone, kConfirm, kCancel };
QuantityCardAction g_quantity_card_pending_action = QuantityCardAction::kNone;
int32_t g_quantity_deltas[] = {-1,-10,-100,-1000,1,10,100,1000};
constexpr const char* kQuantityLabels[] = {
    "-1","-10","-100","-1000","+1","+10","+100","+1000"};
QolUiHandle g_prefix_card_root = nullptr;
QolUiHandle g_prefix_buttons[15]{};
int32_t g_prefix_option_raw[15]{};
uint32_t g_prefix_option_count = 0;
QolUiHandle g_prefix_confirm = nullptr;
QolUiHandle g_prefix_cancel = nullptr;
bool g_prefix_card_open_requested = false;
int32_t g_prefix_selected_raw = -1;
enum class PrefixCardAction : uint8_t { kNone, kConfirm, kCancel };
PrefixCardAction g_prefix_card_pending_action = PrefixCardAction::kNone;
std::atomic<bool> g_active{false};

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
void noop_click(void*) {}
void item_slot_clicked(void*) {
    if (!item_class_valid(g_draft.class_index)) return;
    g_item_card_open_requested = true;
}
void category_row_clicked(void*) { g_card_open_requested = true; }

void log_panel(const char* message) {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    (void)sdk->log.write(1, static_cast<int32_t>(QolDomain::kUi), "item_editor/panel",
                         message, static_cast<uint32_t>(std::strlen(message)));
}

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
        g_left_rows[g_left_row_count++] = LeftRowKind::kChaosLevel;
        g_left_rows[g_left_row_count++] = LeftRowKind::kChaosRate;
    } else if (item_class_valid(g_draft.class_index)) {
        g_left_rows[g_left_row_count++] = LeftRowKind::kQuantity;
    }
}

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
void format_category_row(char* text, size_t capacity) {
    if (text == nullptr || capacity == 0) return;
    if (!draft_has_item(g_draft)) {
        if (item_class_valid(g_draft.class_index)) {
            const char* class_name = static_text(
                QolItemStaticTextKind::kItemClassList, g_draft.class_index);
            std::snprintf(text,capacity,"类别：%s",
                class_name == nullptr ? "未知类别" : class_name);
        } else {
            std::snprintf(text,capacity,"类别：");
        }
        return;
    }
    const char* type_name = item_type_name(g_draft.item_type);
    const int32_t type_id = static_cast<int32_t>(g_draft.item_type);
    if (item_class_is_equipment(g_draft.class_index)) {
        const char* class_name = static_text(
            QolItemStaticTextKind::kItemClassName, g_draft.item_id);
        if (class_name != nullptr && class_name[0] != '\0')
            std::snprintf(text,capacity,"类别：%s（%s） (id:%d)",
                type_name,class_name,type_id);
        else
            std::snprintf(text,capacity,"类别：%s (id:%d)",type_name,type_id);
    } else {
        std::snprintf(text,capacity,"类别：%s (id:%d)",type_name,type_id);
    }
}
void draw_text(QolUiHandle control, const char* text, int32_t x, int32_t y,
               uint32_t color, int32_t align = 0, int32_t font = 1) {
    const auto* c = components();
    if (c != nullptr && c->draw_control_text != nullptr)
        (void)c->draw_control_text(control, text, x, y, color, align, font);
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
            return g_draft.jewel_attribute_type_valid ? 1u : 0u;
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
        draw_text(row, "+", 366, 18, section_enabled(section) ? kGold : kMuted, 1, 2);
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
        name = static_text(QolItemStaticTextKind::kOptionName, option_id);
        const bool unknown_name = name == nullptr || name[0] == '\0';
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
        if (is_percent) {
            std::snprintf(value, sizeof(value), "%+.1f%%",
                          static_cast<double>(option_value) / 10.0);
        } else {
            std::snprintf(value, sizeof(value), "%+d", option_value);
        }
    } else {
        name = "空";
    }
    draw_text(row, name, 20, 26, enabled ? kText : kMuted);
    draw_text(row, value, 300, 26, value_color, 2, 1);
    draw_text(row, "×", 366, 18, enabled ? kGold : kMuted, 1, 2);
}

void draw_name_row(const char* text) {
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
    draw_text(g_root, text, static_cast<int32_t>(rect.x + 16),
              static_cast<int32_t>(rect.y + 14), enabled ? kText : kMuted, 0, 0);
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
    char text[160]{};
    format_category_row(text,sizeof(text));
    draw_text(g_category_row,text,16,14,kText,0,0);
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
    char text[160]{};
    switch (g_left_rows[index]) {
        case LeftRowKind::kQuantity:
            std::snprintf(text, sizeof(text), "数量：%d", g_draft.count);
            break;
        case LeftRowKind::kPrefix: {
            const char* prefix = contracts::item_rarity_prefix(g_draft.variant_series,
                g_draft.prefix_class_bits, g_draft.raw_rarity);
            const bool has_prefix = prefix != nullptr && prefix[0] != '\0';
            const char* label = has_prefix ? prefix : "无";
            size_t label_length = std::strlen(label);
            while (has_prefix && label_length > 0 && label[label_length - 1] == ' ')
                --label_length;
            std::snprintf(text, sizeof(text), "前缀：%.*s（raw:%d）",
                static_cast<int>(label_length), label, g_draft.raw_rarity);
            break;
        }
        case LeftRowKind::kEnchantDurability:
            std::snprintf(text, sizeof(text), "强化耐久度：%d", g_draft.enchant_remaining);
            break;
        case LeftRowKind::kSocketTotal:
            std::snprintf(text, sizeof(text), "宝石孔数：%d", g_draft.socket_total);
            break;
        case LeftRowKind::kDamage:
            std::snprintf(text, sizeof(text), "伤害：%d", g_draft.damage);
            break;
        case LeftRowKind::kDefense:
            std::snprintf(text, sizeof(text), "防御：%d", g_draft.defense);
            break;
        case LeftRowKind::kMagicDamage:
            if (g_draft.damage == 0 && g_draft.magic_rate == 0) {
                std::snprintf(text, sizeof(text), "法伤：0（0.00x）");
            } else {
                const double multiplier = static_cast<double>(g_draft.magic_rate) / 100.0;
                const double magic_damage = static_cast<double>(g_draft.damage) * multiplier;
                std::snprintf(text, sizeof(text), "法伤：%.1f（%.2fx）",
                    magic_damage, multiplier);
            }
            break;
        case LeftRowKind::kRequiredLevel:
            std::snprintf(text, sizeof(text), "需要等级：%d", g_draft.need_level);
            break;
        case LeftRowKind::kChaosLevel:
            std::snprintf(text, sizeof(text), "混沌等级：%d", g_draft.chaos_level);
            break;
        case LeftRowKind::kChaosRate:
            std::snprintf(text, sizeof(text), "混沌数值率：%d%%", g_draft.chaos_rate);
            break;
        default:
            return;
    }
    draw_text(row, text, 16, 14, enabled ? kText : kMuted, 0, 0);
}
void resolve_variant_series() {
    if (g_draft.source_valid && g_draft.variant_series != 0) return;
    const auto* game = game_host();
    if (game == nullptr || game->target.query_variant == nullptr) return;
    QolTargetVariant variant{};
    if (succeeded(game->target.query_variant(&variant)))
        g_draft.variant_series = static_cast<int32_t>(variant.id);
}
void left_info_selected(void*, uint32_t index) {
    if (index >= g_left_row_count) return;
    switch (g_left_rows[index]) {
        case LeftRowKind::kQuantity:
            if (!item_class_valid(g_draft.class_index) ||
                item_class_is_equipment(g_draft.class_index) ||
                !g_draft.stackable) return;
            g_quantity_card_open_requested = true;
            return;
        case LeftRowKind::kPrefix:
            if (g_draft.prefix_class_bits == 0) return;
            resolve_variant_series();
            g_prefix_card_open_requested = true;
            return;
        default:
            return;
    }
}

void category_card_noop_draw(void*) {}
void category_class_clicked(void* user) {
    const auto* class_index = static_cast<const int32_t*>(user);
    if (class_index == nullptr || !item_class_valid(*class_index)) return;
    g_card_selected_class = g_card_selected_class == *class_index ? -1 : *class_index;
}
void category_card_confirm_clicked(void*) {
    if (item_class_valid(g_card_selected_class))
        g_card_pending_action = CategoryCardAction::kConfirm;
}
void category_card_cancel_clicked(void*) {
    g_card_pending_action = CategoryCardAction::kCancel;
}
bool category_card_row_classes(uint32_t row, int32_t* first, int32_t* count) {
    if (first == nullptr || count == nullptr) return false;
    if (row >= 1 && row <= 3) {
        *first = static_cast<int32_t>((row - 1) * 3);
        *count = 3;
        return true;
    }
    if (row >= 5 && row <= 9) {
        *first = 9 + static_cast<int32_t>(row - 5) * 3;
        *count = std::min(3, 22 - *first);
        return true;
    }
    if (row >= 11 && row <= 15) {
        *first = 22 + static_cast<int32_t>(row - 11) * 3;
        *count = std::min(3, kItemClassCount - *first);
        return true;
    }
    return false;
}
bool category_class_location(int32_t class_index, uint32_t* row, int32_t* column) {
    if (!item_class_valid(class_index) || row == nullptr || column == nullptr) return false;
    if (class_index <= 8) {
        *row = 1u + static_cast<uint32_t>(class_index / 3);
        *column = class_index % 3;
    } else if (class_index <= 21) {
        *row = 5u + static_cast<uint32_t>((class_index - 9) / 3);
        *column = (class_index - 9) % 3;
    } else {
        *row = 11u + static_cast<uint32_t>((class_index - 22) / 3);
        *column = (class_index - 22) % 3;
    }
    return true;
}
void draw_category_card_row(void*, uint32_t row_index, QolUiHandle row, uint8_t) {
    if (row_index == 0 || row_index == 4 || row_index == 10) {
        const char* title = row_index == 0 ? "武器" : row_index == 4 ? "装备" : "物品";
        draw_text(row, title, 304, 16, kGold, 2, 1);
        return;
    }
    int32_t first = 0, count = 0;
    if (!category_card_row_classes(row_index, &first, &count)) return;
    const auto* adapter = ui();
    if (adapter == nullptr) return;
    for (int32_t column = 0; column < count; ++column) {
        const int32_t class_index = first + column;
        const QolUiHandle button = g_card_buttons[class_index];
        if (button == nullptr) continue;
        int64_t abs_x = 0, abs_y = 0;
        if (adapter->control_absolute_origin == nullptr ||
            !succeeded(adapter->control_absolute_origin(button, &abs_x, &abs_y))) continue;
        int32_t pressed = 0;
        if (adapter->control_pressed != nullptr)
            (void)adapter->control_pressed(button, &pressed);
        (void)adapter->draw_help_option_row({abs_x, abs_y, 186, 48},
            static_cast<uint8_t>(g_card_selected_class == class_index),
            static_cast<uint8_t>(pressed != 0));
        const char* name = static_text(QolItemStaticTextKind::kItemClassList, class_index);
        if (name == nullptr || name[0] == '\0') name = "未知类别";
        draw_text(row, name, 15 + column * 196 + 93, 16,
            g_card_selected_class == class_index ? kGold : kText, 2, 0);
    }
}
void draw_category_card_footer_button(QolUiHandle button, const char* label,
                                      bool enabled) {
    const auto* adapter = ui();
    if (adapter == nullptr || button == nullptr) return;
    int64_t abs_x = 0, abs_y = 0;
    if (adapter->control_absolute_origin == nullptr ||
        !succeeded(adapter->control_absolute_origin(button, &abs_x, &abs_y))) return;
    int32_t pressed = 0;
    if (enabled && adapter->control_pressed != nullptr)
        (void)adapter->control_pressed(button, &pressed);
    (void)adapter->draw_help_option_row({abs_x, abs_y, 140, 48}, 0,
        static_cast<uint8_t>(pressed != 0));
    draw_text(button, label, 70, 14, enabled ? kText : kMuted, 2, 0);
}

void destroy_category_card() {
    const auto* c = components();
    if (c != nullptr && c->destroy != nullptr) {
        if (g_card_list != nullptr) (void)c->destroy(g_card_list);
        if (g_card_confirm != nullptr) (void)c->destroy(g_card_confirm);
        if (g_card_cancel != nullptr) (void)c->destroy(g_card_cancel);
        if (g_card_footer_root != nullptr) (void)c->destroy(g_card_footer_root);
        if (g_card_root != nullptr) (void)c->destroy(g_card_root);
    }
    g_card_root = nullptr;
    g_card_footer_root = nullptr;
    g_card_list = nullptr;
    g_card_confirm = nullptr;
    g_card_cancel = nullptr;
    for (auto& button : g_card_buttons) button = nullptr;
}
bool create_category_card() {
    const auto* c = components();
    const auto* adapter = ui();
    if (c == nullptr || adapter == nullptr || c->panel_create == nullptr ||
        c->scroll_list_create == nullptr || c->scroll_row_button_create == nullptr ||
        c->button_create == nullptr || adapter->set_draw_proc == nullptr ||
        c->scroll_list_set_selection_enabled == nullptr) {
        log_panel("op=category_card_create result=failed stage=precondition code=-1");
        return false;
    }
    const auto fail = [](const char* stage, QolResult result) {
        char message[128]{};
        std::snprintf(message, sizeof(message),
            "op=category_card_create result=failed stage=%s code=%d", stage, result.code);
        log_panel(message);
        return false;
    };
    // panel_create 只接受空 parent；卡片作为独立 root 由 panel_event 单独派发。
    // 卡片根必须落在面板绝对原点，子控件与 draw_container_styled 才共用同一坐标系。
    const UiRect panel = panel_rect();
    const UiPanelDesc card_desc{{panel.x+130,panel.y+30,640,500},{0,0,640,500},0,0};
    QolResult result = c->panel_create(&card_desc, nullptr, &g_card_root);
    if (!succeeded(result)) return fail("card_panel", result);
    const UiScrollListDesc list{{16,16,608,400},52,16,-1,
        &draw_category_card_row,nullptr,nullptr};
    result = c->scroll_list_create(&list,g_card_root,&g_card_list);
    if (!succeeded(result)) return fail("card_list", result);
    result = c->scroll_list_set_selection_enabled(g_card_list,0);
    if (!succeeded(result)) return fail("card_selection", result);
    g_card_selected_class = item_class_valid(g_draft.class_index) ? g_draft.class_index : -1;
    for (int32_t class_index = 0; class_index < kItemClassCount; ++class_index) {
        uint32_t row = 0;
        int32_t column = 0;
        if (!category_class_location(class_index, &row, &column)) {
            log_panel("op=category_card_create result=failed stage=class_location code=-1");
            return false;
        }
        g_card_button_classes[class_index] = class_index;
        const UiRect rect{15 + column * 196,2,186,48};
        result = c->scroll_row_button_create(g_card_list,row,rect,
                &category_class_clicked,&g_card_button_classes[class_index],
                &g_card_buttons[class_index]);
        if (!succeeded(result)) {
            char message[96]{};
            std::snprintf(message, sizeof(message),
                "op=category_card_create result=failed stage=button code=%d class=%d",
                result.code, class_index);
            log_panel(message);
            return false;
        }
        result = adapter->set_draw_proc(g_card_buttons[class_index],
                &category_card_noop_draw);
        if (!succeeded(result)) {
            char message[96]{};
            std::snprintf(message, sizeof(message),
                "op=category_card_create result=failed stage=draw code=%d class=%d",
                result.code, class_index);
            log_panel(message);
            return false;
        }
    }
    // 列表的 scroll target group 会延伸到 viewport 外；底部按钮放到独立 root，
    // 事件先派发给它，避免被列表内容区的命中区吞掉。
    const UiPanelDesc footer_desc{{panel.x+130,panel.y+466,640,48},{0,0,640,48},0,0};
    result = c->panel_create(&footer_desc, nullptr, &g_card_footer_root);
    if (!succeeded(result)) return fail("card_footer", result);
    const UiButtonDesc confirm{{172,0,140,48},"",0,
        &category_card_confirm_clicked,nullptr};
    result = c->button_create(&confirm,g_card_footer_root,&g_card_confirm);
    if (!succeeded(result)) return fail("card_confirm", result);
    result = adapter->set_draw_proc(g_card_confirm,&category_card_noop_draw);
    if (!succeeded(result)) return fail("card_confirm_draw", result);
    const UiButtonDesc cancel{{328,0,140,48},"",0,
        &category_card_cancel_clicked,nullptr};
    result = c->button_create(&cancel,g_card_footer_root,&g_card_cancel);
    if (!succeeded(result)) return fail("card_cancel", result);
    result = adapter->set_draw_proc(g_card_cancel,&category_card_noop_draw);
    if (!succeeded(result)) return fail("card_cancel_draw", result);
    return true;
}
void draw_category_card() {
    if (g_card_root == nullptr) return;
    const auto* adapter = ui();
    const auto* c = components();
    if (adapter == nullptr || c == nullptr) return;
    const UiRect panel = panel_rect();
    if (adapter->fill_rect_alpha != nullptr)
        (void)adapter->fill_rect_alpha({panel.x,panel.y,panel.w,panel.h},0x0000,40);
    if (c->draw_container_styled != nullptr) {
        const UiContainerStyleDesc card{{panel.x+130,panel.y+30,640,500},0x0000,60,1};
        (void)c->draw_container_styled(&card);
    }
    (void)c->scroll_list_process(g_card_list);
    (void)c->scroll_list_draw(g_card_list);
    draw_category_card_footer_button(g_card_confirm,"确认",
        item_class_valid(g_card_selected_class));
    draw_category_card_footer_button(g_card_cancel,"取消",true);
}
bool parse_item_ids(const char* csv, std::vector<int32_t>* out_ids) {
    if (csv == nullptr || out_ids == nullptr) return false;
    out_ids->clear();
    const char* cursor = csv;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
            ++cursor;
        if (*cursor == '\0') break;
        if (*cursor == ',') return false;
        errno = 0;
        char* end = nullptr;
        const long parsed = std::strtol(cursor, &end, 10);
        if (end == cursor || errno == ERANGE || parsed < 0 || parsed > INT32_MAX)
            return false;
        out_ids->push_back(static_cast<int32_t>(parsed));
        cursor = end;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
            ++cursor;
        if (*cursor == ',') {
            ++cursor;
            const char* next = cursor;
            while (*next == ' ' || *next == '\t' || *next == '\r' || *next == '\n')
                ++next;
            if (*next == '\0') return false;
        } else if (*cursor != '\0') {
            return false;
        }
    }
    return true;
}
void item_card_noop_draw(void*) {}

void item_card_cell_clicked(void* user) {
    const intptr_t encoded = reinterpret_cast<intptr_t>(user);
    if (encoded <= 0 || encoded - 1 > INT32_MAX) return;
    const int32_t item_id = static_cast<int32_t>(encoded - 1);
    g_item_card_selected_id = g_item_card_selected_id == item_id ? -1 : item_id;
}
void item_card_confirm_clicked(void*) {
    if (g_item_card_selected_id >= 0)
        g_item_card_pending_action = ItemCardAction::kConfirm;
}
void item_card_cancel_clicked(void*) {
    g_item_card_pending_action = ItemCardAction::kCancel;
}
void draw_item_card_row(void*, uint32_t row_index, QolUiHandle row, uint8_t) {
    if (g_item_card_ids.empty()) {
        draw_text(row, "该类别暂无物品", 304, 46, kMuted, 2, 0);
        return;
    }
    const uint32_t first = row_index * 3;
    for (uint32_t column = 0; column < 3; ++column) {
        const uint32_t item_index = first + column;
        if (item_index >= g_item_card_ids.size() ||
            item_index >= g_item_card_buttons.size()) break;
        const int32_t item_id = g_item_card_ids[item_index];
        const bool selected = item_id == g_item_card_selected_id;
        const char* name = static_text(QolItemStaticTextKind::kItemName,item_id);
        if (name == nullptr || name[0] == '\0') name = "未知";
        char label[96]{};
        size_t input_bytes = std::strlen(name);
        size_t input_offset = 0;
        size_t output_bytes = 0;
        uint32_t codepoints = 0;
        bool truncated = false;
        while (input_offset < input_bytes && codepoints < 8) {
            const unsigned char lead = static_cast<unsigned char>(name[input_offset]);
            size_t sequence = lead < 0x80 ? 1u :
                ((lead & 0xE0) == 0xC0 ? 2u :
                 ((lead & 0xF0) == 0xE0 ? 3u : ((lead & 0xF8) == 0xF0 ? 4u : 1u)));
            if (sequence > input_bytes - input_offset) sequence = 1;
            if (output_bytes + sequence + 4 >= sizeof(label)) {
                truncated = true;
                break;
            }
            std::memcpy(label + output_bytes,name + input_offset,sequence);
            output_bytes += sequence;
            input_offset += sequence;
            ++codepoints;
        }
        if (input_offset < input_bytes) truncated = true;
        if (truncated) {
            label[output_bytes++] = static_cast<char>(0xE2);
            label[output_bytes++] = static_cast<char>(0x80);
            label[output_bytes++] = static_cast<char>(0xA6);
        }
        label[output_bytes] = '\0';
        // 名称在格底外，紧贴 74×74 格底下方 3px；cell 原点 x=15+col*196。
        draw_text(row,label,15 + static_cast<int32_t>(column) * 196 + 93, 81,
                  selected ? kGold : kText, 2, 0);
    }
}
void destroy_item_card() {
    const auto* c = components();
    if (c != nullptr && c->destroy != nullptr) {
        if (g_item_card_list != nullptr) (void)c->destroy(g_item_card_list);
        if (g_item_card_confirm != nullptr) (void)c->destroy(g_item_card_confirm);
        if (g_item_card_cancel != nullptr) (void)c->destroy(g_item_card_cancel);
        if (g_item_card_footer_root != nullptr) (void)c->destroy(g_item_card_footer_root);
        for (auto& slot : g_item_card_slots) {
            if (slot != nullptr) (void)c->destroy(slot);
            slot = nullptr;
        }
        if (g_item_card_root != nullptr) (void)c->destroy(g_item_card_root);
    }
    g_item_card_root = nullptr;
    g_item_card_footer_root = nullptr;
    g_item_card_list = nullptr;
    g_item_card_confirm = nullptr;
    g_item_card_cancel = nullptr;
    for (auto& slot : g_item_card_slots) slot = nullptr;
    g_item_card_ids.clear();
    g_item_card_buttons.clear();
    g_item_card_selected_id = -1;
}
bool create_item_card() {
    const auto* c = components();
    const auto* adapter = ui();
    const auto* port = item_port();
    if (c == nullptr || adapter == nullptr || port == nullptr ||
        c->panel_create == nullptr || c->scroll_list_create == nullptr ||
        c->scroll_row_button_create == nullptr || c->button_create == nullptr ||
        c->scroll_list_set_selection_enabled == nullptr || c->item_slot_create == nullptr ||
        c->item_slot_set == nullptr || c->item_slot_set_flags == nullptr ||
        adapter->set_draw_proc == nullptr || !item_class_valid(g_draft.class_index) ||
        port->read.draw_icon_id == nullptr) {
        log_panel("op=item_card_create result=failed stage=precondition code=-1");
        return false;
    }
    const char* item_list = static_text(QolItemStaticTextKind::kItemList,g_draft.class_index);
    if (!parse_item_ids(item_list,&g_item_card_ids)) {
        log_panel("op=item_card_create result=failed stage=item_list_parse code=-1");
        return false;
    }
    uint32_t rows = 1;
    if (!g_item_card_ids.empty())
        rows = static_cast<uint32_t>((g_item_card_ids.size() + 2u) / 3u);
    const UiRect panel = panel_rect();
    const UiPanelDesc card{{panel.x+130,panel.y+30,640,500},{0,0,640,500},0,0};
    QolResult result = c->panel_create(&card,nullptr,&g_item_card_root);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message,sizeof(message),
            "op=item_card_create result=failed stage=card_panel code=%d",result.code);
        log_panel(message);
        return false;
    }
    // 可见格池使用真正的 core.custom_ui item_slot 组件；池先建，列表行按钮后建，
    // 触摸仍优先命中行按钮。每帧按滚动位移重排并只绘制可见项。
    for (auto& slot : g_item_card_slots) {
        const UiItemSlotDesc desc{{0,-200,74,74},-1,-1,kUiItemSlotShowFrame,-1};
        result = c->item_slot_create(&desc,g_item_card_root,&slot);
        if (!succeeded(result)) {
            char message[128]{};
            std::snprintf(message,sizeof(message),
                "op=item_card_create result=failed stage=slot_pool code=%d",result.code);
            log_panel(message);
            return false;
        }
    }
    const UiScrollListDesc list{{16,16,608,400},122,rows,-1,
        &draw_item_card_row,nullptr,nullptr};
    result = c->scroll_list_create(&list,g_item_card_root,&g_item_card_list);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message,sizeof(message),
            "op=item_card_create result=failed stage=card_list code=%d",result.code);
        log_panel(message);
        return false;
    }
    result = c->scroll_list_set_selection_enabled(g_item_card_list,0);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message,sizeof(message),
            "op=item_card_create result=failed stage=list_selection code=%d",result.code);
        log_panel(message);
        return false;
    }
    g_item_card_buttons.assign(g_item_card_ids.size(),nullptr);
    for (uint32_t index = 0; index < g_item_card_ids.size(); ++index) {
        const uint32_t row = index / 3;
        const int32_t column = static_cast<int32_t>(index % 3);
        const UiRect cell{15 + column * 196,2,186,112};
        const intptr_t encoded = static_cast<intptr_t>(g_item_card_ids[index]) + 1;
        result = c->scroll_row_button_create(g_item_card_list,row,cell,
            &item_card_cell_clicked,reinterpret_cast<void*>(encoded),
            &g_item_card_buttons[index]);
        if (!succeeded(result)) {
            char message[128]{};
            std::snprintf(message,sizeof(message),
                "op=item_card_create result=failed stage=cell_button code=%d index=%u",
                result.code,index);
            log_panel(message);
            return false;
        }
        result = adapter->set_draw_proc(g_item_card_buttons[index],&item_card_noop_draw);
        if (!succeeded(result)) {
            char message[128]{};
            std::snprintf(message,sizeof(message),
                "op=item_card_create result=failed stage=cell_draw code=%d index=%u",
                result.code,index);
            log_panel(message);
            return false;
        }
    }
    g_item_card_selected_id = -1;
    const auto selected_item = std::find(
        g_item_card_ids.begin(),g_item_card_ids.end(),g_draft.item_id);
    if (draft_has_item(g_draft) && selected_item != g_item_card_ids.end()) {
        g_item_card_selected_id = g_draft.item_id;
        if (c->scroll_set_offset != nullptr) {
            const auto index = static_cast<uint32_t>(
                std::distance(g_item_card_ids.begin(),selected_item));
            const int32_t row_top = static_cast<int32_t>((index / 3) * 122);
            const int32_t offset = std::max(0,row_top - (400 - 122) / 2);
            (void)c->scroll_set_offset(g_item_card_list,offset);
        }
    }
    const UiPanelDesc footer{{panel.x+130,panel.y+466,640,48},{0,0,640,48},0,0};
    result = c->panel_create(&footer,nullptr,&g_item_card_footer_root);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message,sizeof(message),
            "op=item_card_create result=failed stage=footer_panel code=%d",result.code);
        log_panel(message);
        return false;
    }
    const UiButtonDesc confirm{{172,0,140,48},"",0,
        &item_card_confirm_clicked,nullptr};
    result = c->button_create(&confirm,g_item_card_footer_root,&g_item_card_confirm);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message,sizeof(message),
            "op=item_card_create result=failed stage=confirm code=%d",result.code);
        log_panel(message);
        return false;
    }
    result = adapter->set_draw_proc(g_item_card_confirm,&item_card_noop_draw);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message,sizeof(message),
            "op=item_card_create result=failed stage=confirm_draw code=%d",result.code);
        log_panel(message);
        return false;
    }
    const UiButtonDesc cancel{{328,0,140,48},"",0,
        &item_card_cancel_clicked,nullptr};
    result = c->button_create(&cancel,g_item_card_footer_root,&g_item_card_cancel);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message,sizeof(message),
            "op=item_card_create result=failed stage=cancel code=%d",result.code);
        log_panel(message);
        return false;
    }
    result = adapter->set_draw_proc(g_item_card_cancel,&item_card_noop_draw);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message,sizeof(message),
            "op=item_card_create result=failed stage=cancel_draw code=%d",result.code);
        log_panel(message);
        return false;
    }
    return true;
}
void draw_item_card_footer_button(QolUiHandle button,const char* label,bool enabled) {
    const auto* adapter = ui();
    if (adapter == nullptr || button == nullptr) return;
    int64_t abs_x = 0,abs_y = 0;
    if (adapter->control_absolute_origin == nullptr ||
        !succeeded(adapter->control_absolute_origin(button,&abs_x,&abs_y))) return;
    int32_t pressed = 0;
    if (enabled && adapter->control_pressed != nullptr)
        (void)adapter->control_pressed(button,&pressed);
    if (adapter->draw_help_option_row != nullptr)
        (void)adapter->draw_help_option_row({abs_x,abs_y,140,48},0,
            static_cast<uint8_t>(pressed != 0));
    draw_text(button,label,70,14,enabled ? kText : kMuted,2,0);
}
void draw_item_card_slots(const QolUiComponents* c, const QolUiAdapter* adapter,
                          const UiRect& panel, int64_t offset) {
    if (c == nullptr || adapter == nullptr || c->set_rect == nullptr ||
        c->item_slot_set == nullptr || c->item_slot_set_flags == nullptr ||
        c->item_slot_draw == nullptr) return;
    const int64_t view_x = panel.x + 130 + 16;
    const int64_t view_y = panel.y + 30 + 16;
    const int64_t view_w = 608;
    const int64_t view_h = 400;
    if (adapter->set_clip != nullptr)
        (void)adapter->set_clip(static_cast<int32_t>(view_x), static_cast<int32_t>(view_y),
                                static_cast<int32_t>(view_x + view_w),
                                static_cast<int32_t>(view_y + view_h));
    if (c->item_slot_set_quality != nullptr) {
        for (const QolUiHandle slot : g_item_card_slots) {
            if (slot != nullptr) (void)c->item_slot_set_quality(slot,-1);
        }
    }
    size_t pool_index = 0;
    for (size_t index = 0; index < g_item_card_ids.size() &&
                           pool_index < g_item_card_slots.size(); ++index) {
        const int32_t row = static_cast<int32_t>(index / 3);
        const int32_t column = static_cast<int32_t>(index % 3);
        const int64_t local_y = 16 + static_cast<int64_t>(row) * 122 - offset + 5;
        if (local_y + 74 <= 16 || local_y + 74 > 416) continue;
        const QolUiHandle slot = g_item_card_slots[pool_index++];
        if (slot == nullptr) continue;
        const int32_t item_id = g_item_card_ids[index];
        (void)c->set_rect(slot, {87 + column * 196, static_cast<int32_t>(local_y), 74, 74});
        (void)c->item_slot_set(slot, item_id, -1);
        const uint32_t flags = kUiItemSlotShowFrame |
            (item_id == g_item_card_selected_id ? kUiItemSlotSelected : 0u);
        (void)c->item_slot_set_flags(slot, flags);
        (void)c->item_slot_draw(slot);
    }
    for (; pool_index < g_item_card_slots.size(); ++pool_index) {
        if (g_item_card_slots[pool_index] != nullptr)
            (void)c->set_rect(g_item_card_slots[pool_index], {0, -200, 74, 74});
    }
    if (adapter->reset_clip != nullptr) (void)adapter->reset_clip();
}

void draw_item_card() {
    if (g_item_card_root == nullptr) return;
    const auto* adapter = ui();
    const auto* c = components();
    if (adapter == nullptr || c == nullptr) return;
    const UiRect panel = panel_rect();
    if (adapter->fill_rect_alpha != nullptr)
        (void)adapter->fill_rect_alpha({panel.x,panel.y,panel.w,panel.h},0x0000,40);
    if (c->draw_container_styled != nullptr) {
        const UiContainerStyleDesc card{{panel.x+130,panel.y+30,640,500},0x0000,60,1};
        (void)c->draw_container_styled(&card);
    }
    (void)c->scroll_list_process(g_item_card_list);
    (void)c->scroll_list_draw(g_item_card_list);
    int64_t offset = 0;
    if (c->scroll_list_get_offset != nullptr)
        (void)c->scroll_list_get_offset(g_item_card_list, &offset);
    draw_item_card_slots(c, adapter, panel, offset < 0 ? 0 : offset);
    draw_item_card_footer_button(g_item_card_confirm,"确认",
        g_item_card_selected_id >= 0);
    draw_item_card_footer_button(g_item_card_cancel,"取消",true);
}
void rebuild_item_slot();
void rebuild_left_list();
void rebuild_right_list();
void apply_item_card_action() {
    if (g_item_card_pending_action == ItemCardAction::kNone) return;
    const ItemCardAction action = g_item_card_pending_action;
    g_item_card_pending_action = ItemCardAction::kNone;
    bool changed = false;
    if (action == ItemCardAction::kConfirm && g_item_card_selected_id >= 0 &&
        g_item_card_selected_id != g_draft.item_id) {
        g_draft.item_id = g_item_card_selected_id;
        g_draft.item_type = item_type_for_class(g_draft.class_index);
        const auto* port = item_port();
        if (port != nullptr && port->read.read_item_base_stats != nullptr) {
            QolItemBaseStats stats{};
            const QolResult result =
                port->read.read_item_base_stats(g_draft.item_id,&stats);
            if (succeeded(result)) {
                g_draft.damage = stats.damage;
                g_draft.defense = stats.defense;
                g_draft.magic_rate = stats.magic_rate;
                g_draft.need_level = stats.need_level;
                g_draft.stackable = stats.stackable != 0;
                if (!g_draft.stackable) g_draft.count = 1;
            } else {
                char message[128]{};
                std::snprintf(message,sizeof(message),
                    "fail item=%d op=load_item_stats result=failed stage=read code=%d reason=base_stats_read_failed",
                    g_draft.item_id,result.code);
                log_panel(message);
            }
        } else {
            log_panel("fail op=load_item_stats result=failed stage=api code=-1 reason=api_unavailable");
        }
        g_draft.dirty = true;
        changed = true;
    }
    destroy_item_card();
    if (changed) {
        build_left_rows();
        rebuild_left_list();
        rebuild_right_list();
        rebuild_item_slot();
    }
}
void quantity_adjust_clicked(void* user) {
    const auto* delta = static_cast<const int32_t*>(user);
    if (delta == nullptr) return;
    const int64_t adjusted = static_cast<int64_t>(g_quantity_value) + *delta;
    g_quantity_value = static_cast<int32_t>(std::clamp<int64_t>(
        adjusted,1,stack_limit_max_count()));
}
void quantity_confirm_clicked(void*) {
    g_quantity_card_pending_action = QuantityCardAction::kConfirm;
}
void quantity_cancel_clicked(void*) {
    g_quantity_card_pending_action = QuantityCardAction::kCancel;
}
void destroy_quantity_card() {
    const auto* c = components();
    if (c != nullptr && c->destroy != nullptr) {
        for (auto& button : g_quantity_adjust_buttons) {
            if (button != nullptr) (void)c->destroy(button);
            button = nullptr;
        }
        if (g_quantity_confirm != nullptr) (void)c->destroy(g_quantity_confirm);
        if (g_quantity_cancel != nullptr) (void)c->destroy(g_quantity_cancel);
        if (g_quantity_card_root != nullptr) (void)c->destroy(g_quantity_card_root);
    }
    for (auto& button : g_quantity_adjust_buttons) button = nullptr;
    g_quantity_confirm = nullptr;
    g_quantity_cancel = nullptr;
    g_quantity_card_root = nullptr;
}
bool create_quantity_card() {
    const auto* c = components();
    const auto* adapter = ui();
    if (c == nullptr || adapter == nullptr || c->panel_create == nullptr ||
        c->button_create == nullptr || adapter->set_draw_proc == nullptr ||
        !item_class_valid(g_draft.class_index) ||
        item_class_is_equipment(g_draft.class_index) ||
        !g_draft.stackable) {
        log_panel("op=quantity_card_create result=failed stage=precondition code=-1");
        return false;
    }
    g_quantity_value = std::clamp(g_draft.count,1,stack_limit_max_count());
    const UiRect panel = panel_rect();
    const UiRect card_rect{panel.x+(panel.w-420)/2,panel.y+(panel.h-250)/2,420,250};
    const UiPanelDesc card{card_rect,{0,0,420,250},0,0};
    QolResult result = c->panel_create(&card,nullptr,&g_quantity_card_root);
    if (!succeeded(result)) {
        char message[112]{};
        std::snprintf(message,sizeof(message),
            "op=quantity_card_create result=failed stage=card_root code=%d",result.code);
        log_panel(message);
        return false;
    }
    for (uint32_t index = 0; index < 8; ++index) {
        const int32_t column = static_cast<int32_t>(index % 4);
        const int32_t row = static_cast<int32_t>(index / 4);
        const UiButtonDesc button{{22+column*96,96+row*50,88,42},"",0,
            &quantity_adjust_clicked,
            &g_quantity_deltas[index]};
        result = c->button_create(&button,g_quantity_card_root,
            &g_quantity_adjust_buttons[index]);
        if (!succeeded(result)) {
            char message[112]{};
            std::snprintf(message,sizeof(message),
                "op=quantity_card_create result=failed stage=adjust_button code=%d index=%u",
                result.code,index);
            log_panel(message);
            return false;
        }
        result = adapter->set_draw_proc(g_quantity_adjust_buttons[index],
            &category_card_noop_draw);
        if (!succeeded(result)) {
            char message[112]{};
            std::snprintf(message,sizeof(message),
                "op=quantity_card_create result=failed stage=adjust_draw code=%d index=%u",
                result.code,index);
            log_panel(message);
            return false;
        }
    }
    const UiButtonDesc confirm{{100,200,100,42},"",0,
        &quantity_confirm_clicked,nullptr};
    result = c->button_create(&confirm,g_quantity_card_root,&g_quantity_confirm);
    if (!succeeded(result)) {
        char message[112]{};
        std::snprintf(message,sizeof(message),
            "op=quantity_card_create result=failed stage=confirm_button code=%d",result.code);
        log_panel(message);
        return false;
    }
    result = adapter->set_draw_proc(g_quantity_confirm,&category_card_noop_draw);
    if (!succeeded(result)) {
        char message[112]{};
        std::snprintf(message,sizeof(message),
            "op=quantity_card_create result=failed stage=confirm_draw code=%d",result.code);
        log_panel(message);
        return false;
    }
    const UiButtonDesc cancel{{220,200,100,42},"",0,
        &quantity_cancel_clicked,nullptr};
    result = c->button_create(&cancel,g_quantity_card_root,&g_quantity_cancel);
    if (!succeeded(result)) {
        char message[112]{};
        std::snprintf(message,sizeof(message),
            "op=quantity_card_create result=failed stage=cancel_button code=%d",result.code);
        log_panel(message);
        return false;
    }
    result = adapter->set_draw_proc(g_quantity_cancel,&category_card_noop_draw);
    if (!succeeded(result)) {
        char message[112]{};
        std::snprintf(message,sizeof(message),
            "op=quantity_card_create result=failed stage=cancel_draw code=%d",result.code);
        log_panel(message);
        return false;
    }
    return true;
}
void draw_quantity_card_button(QolUiHandle button,const char* label,
                               int32_t width,int32_t height) {
    const auto* adapter = ui();
    if (adapter == nullptr || button == nullptr) return;
    int64_t abs_x = 0,abs_y = 0;
    if (adapter->control_absolute_origin == nullptr ||
        !succeeded(adapter->control_absolute_origin(button,&abs_x,&abs_y))) return;
    int32_t pressed = 0;
    if (adapter->control_pressed != nullptr)
        (void)adapter->control_pressed(button,&pressed);
    if (adapter->draw_help_option_row != nullptr)
        (void)adapter->draw_help_option_row({abs_x,abs_y,width,height},0,
            static_cast<uint8_t>(pressed != 0));
    draw_text(button,label,width/2,height/2-8,kText,2,0);
}
void draw_quantity_card() {
    if (g_quantity_card_root == nullptr) return;
    const auto* adapter = ui();
    const auto* c = components();
    if (adapter == nullptr || c == nullptr) return;
    const UiRect panel = panel_rect();
    const UiRect card{panel.x+(panel.w-420)/2,panel.y+(panel.h-250)/2,420,250};
    if (adapter->fill_rect_alpha != nullptr)
        (void)adapter->fill_rect_alpha({panel.x,panel.y,panel.w,panel.h},0x0000,40);
    if (c->draw_container_styled != nullptr) {
        const UiContainerStyleDesc style{card,0x0000,60,1};
        (void)c->draw_container_styled(&style);
    }
    char quantity[48]{};
    std::snprintf(quantity,sizeof(quantity),"数量：%d",g_quantity_value);
    draw_text(g_quantity_card_root,quantity,210,36,kText,2,2);
    for (uint32_t index = 0; index < 8; ++index)
        draw_quantity_card_button(g_quantity_adjust_buttons[index],
            kQuantityLabels[index],88,42);
    draw_quantity_card_button(g_quantity_confirm,"确定",100,42);
    draw_quantity_card_button(g_quantity_cancel,"取消",100,42);
}
void apply_quantity_action() {
    if (g_quantity_card_pending_action == QuantityCardAction::kNone) return;
    const QuantityCardAction action = g_quantity_card_pending_action;
    g_quantity_card_pending_action = QuantityCardAction::kNone;
    const bool changed = action == QuantityCardAction::kConfirm &&
        item_class_valid(g_draft.class_index) &&
        !item_class_is_equipment(g_draft.class_index) &&
        g_draft.stackable;
    if (changed) {
        g_draft.count = std::clamp(g_quantity_value,1,stack_limit_max_count());
        g_draft.dirty = true;
    }
    destroy_quantity_card();
    if (changed) {
        build_left_rows();
        rebuild_left_list();
        rebuild_item_slot();
    }
}
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
    const uint32_t rows = 3 + option_count(0) + option_count(1) + option_count(2);
    const UiScrollListDesc list{{470,44,400,472},kRightRowH,rows,-1,
        &draw_right_row,nullptr,nullptr};
    (void)c->scroll_list_create(&list,g_root,&g_list);
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
        if (!succeeded(adapter->set_draw_proc(g_icon_hit_button,&category_card_noop_draw))) {
            if (c->destroy != nullptr) (void)c->destroy(g_icon_hit_button);
            g_icon_hit_button = nullptr;
        }
    } else {
        g_icon_hit_button = nullptr;
    }
}
void apply_category_card_action() {
    if (g_card_pending_action == CategoryCardAction::kNone) return;
    const CategoryCardAction action = g_card_pending_action;
    g_card_pending_action = CategoryCardAction::kNone;
    bool changed = false;
    if (action == CategoryCardAction::kConfirm &&
        item_class_valid(g_card_selected_class)) {
        changed = draft_change_class(&g_draft,g_card_selected_class);
    }
    destroy_category_card();
    if (changed) {
        build_left_rows();
        rebuild_left_list();
        rebuild_right_list();
        rebuild_item_slot();
    }
}

void prefix_option_clicked(void* user) {
    const auto* raw = static_cast<const int32_t*>(user);
    if (raw == nullptr || *raw < 0 || *raw > 14) return;
    g_prefix_selected_raw = *raw;
}
void prefix_confirm_clicked(void*) {
    if (g_prefix_selected_raw >= 0)
        g_prefix_card_pending_action = PrefixCardAction::kConfirm;
}
void prefix_cancel_clicked(void*) {
    g_prefix_card_pending_action = PrefixCardAction::kCancel;
}
void destroy_prefix_card() {
    const auto* c = components();
    if (c != nullptr && c->destroy != nullptr) {
        for (auto& button : g_prefix_buttons) {
            if (button != nullptr) (void)c->destroy(button);
            button = nullptr;
        }
        if (g_prefix_confirm != nullptr) (void)c->destroy(g_prefix_confirm);
        if (g_prefix_cancel != nullptr) (void)c->destroy(g_prefix_cancel);
        if (g_prefix_card_root != nullptr) (void)c->destroy(g_prefix_card_root);
    }
    for (auto& button : g_prefix_buttons) button = nullptr;
    g_prefix_confirm = nullptr;
    g_prefix_cancel = nullptr;
    g_prefix_card_root = nullptr;
    g_prefix_option_count = 0;
    g_prefix_selected_raw = -1;
}
void build_prefix_options() {
    g_prefix_option_count = 0;
    for (int32_t raw = 0; raw <= 14; ++raw) {
        if (g_draft.variant_series == 4 &&
            g_draft.prefix_class_bits == 3 && raw < 10 && raw != 2) continue;
        const char* prefix = contracts::item_rarity_prefix(
            g_draft.variant_series,g_draft.prefix_class_bits,raw);
        if (raw != 2 && (prefix == nullptr || prefix[0] == '\0')) continue;
        g_prefix_option_raw[g_prefix_option_count++] = raw;
    }
}
bool prefix_raw_available(int32_t raw) {
    for (uint32_t index = 0; index < g_prefix_option_count; ++index) {
        if (g_prefix_option_raw[index] == raw) return true;
    }
    return false;
}
bool create_prefix_card() {
    const auto* c = components();
    const auto* adapter = ui();
    if (c == nullptr || adapter == nullptr || c->panel_create == nullptr ||
        c->button_create == nullptr || adapter->set_draw_proc == nullptr ||
        g_draft.prefix_class_bits == 0) {
        log_panel("op=prefix_card_create result=failed stage=precondition code=-1");
        return false;
    }
    resolve_variant_series();
    build_prefix_options();
    g_prefix_selected_raw = prefix_raw_available(g_draft.raw_rarity)
        ? g_draft.raw_rarity : -1;
    const UiRect panel = panel_rect();
    const UiRect card_rect{panel.x+(panel.w-420)/2,panel.y+(panel.h-360)/2,420,360};
    const UiPanelDesc card{card_rect,{0,0,420,360},0,0};
    QolResult result = c->panel_create(&card,nullptr,&g_prefix_card_root);
    if (!succeeded(result)) {
        char message[112]{};
        std::snprintf(message,sizeof(message),
            "op=prefix_card_create result=failed stage=card_root code=%d",result.code);
        log_panel(message);
        return false;
    }
    for (uint32_t index = 0; index < g_prefix_option_count; ++index) {
        const int32_t column = static_cast<int32_t>(index % 3);
        const int32_t row = static_cast<int32_t>(index / 3);
        const UiButtonDesc button{{12+column*132,54+row*50,126,44},"",0,
            &prefix_option_clicked,&g_prefix_option_raw[index]};
        result = c->button_create(&button,g_prefix_card_root,&g_prefix_buttons[index]);
        if (!succeeded(result)) {
            char message[112]{};
            std::snprintf(message,sizeof(message),
                "op=prefix_card_create result=failed stage=option_button code=%d index=%u",
                result.code,index);
            log_panel(message);
            return false;
        }
        result = adapter->set_draw_proc(g_prefix_buttons[index],&category_card_noop_draw);
        if (!succeeded(result)) {
            char message[112]{};
            std::snprintf(message,sizeof(message),
                "op=prefix_card_create result=failed stage=option_draw code=%d index=%u",
                result.code,index);
            log_panel(message);
            return false;
        }
    }
    const UiButtonDesc confirm{{86,310,110,42},"",0,
        &prefix_confirm_clicked,nullptr};
    result = c->button_create(&confirm,g_prefix_card_root,&g_prefix_confirm);
    if (!succeeded(result)) {
        char message[112]{};
        std::snprintf(message,sizeof(message),
            "op=prefix_card_create result=failed stage=confirm_button code=%d",result.code);
        log_panel(message);
        return false;
    }
    result = adapter->set_draw_proc(g_prefix_confirm,&category_card_noop_draw);
    if (!succeeded(result)) {
        log_panel("op=prefix_card_create result=failed stage=confirm_draw code=-1");
        return false;
    }
    const UiButtonDesc cancel{{224,310,110,42},"",0,
        &prefix_cancel_clicked,nullptr};
    result = c->button_create(&cancel,g_prefix_card_root,&g_prefix_cancel);
    if (!succeeded(result)) {
        char message[112]{};
        std::snprintf(message,sizeof(message),
            "op=prefix_card_create result=failed stage=cancel_button code=%d",result.code);
        log_panel(message);
        return false;
    }
    result = adapter->set_draw_proc(g_prefix_cancel,&category_card_noop_draw);
    if (!succeeded(result)) {
        log_panel("op=prefix_card_create result=failed stage=cancel_draw code=-1");
        return false;
    }
    return true;
}
void draw_prefix_card_button(QolUiHandle button,const char* label,
                             int32_t width,int32_t height,bool selected) {
    const auto* adapter = ui();
    if (adapter == nullptr || button == nullptr) return;
    int64_t abs_x = 0,abs_y = 0;
    if (adapter->control_absolute_origin == nullptr ||
        !succeeded(adapter->control_absolute_origin(button,&abs_x,&abs_y))) return;
    int32_t pressed = 0;
    if (adapter->control_pressed != nullptr)
        (void)adapter->control_pressed(button,&pressed);
    if (adapter->draw_help_option_row != nullptr)
        (void)adapter->draw_help_option_row({abs_x,abs_y,width,height},
            static_cast<uint8_t>(selected),static_cast<uint8_t>(pressed != 0));
    draw_text(button,label,width/2,height/2-8,selected ? kGold : kText,2,0);
}
void draw_prefix_card() {
    if (g_prefix_card_root == nullptr) return;
    const auto* adapter = ui();
    const auto* c = components();
    if (adapter == nullptr || c == nullptr) return;
    const UiRect panel = panel_rect();
    const UiRect card{panel.x+(panel.w-420)/2,panel.y+(panel.h-360)/2,420,360};
    if (adapter->fill_rect_alpha != nullptr)
        (void)adapter->fill_rect_alpha({panel.x,panel.y,panel.w,panel.h},0x0000,40);
    if (c->draw_container_styled != nullptr) {
        const UiContainerStyleDesc style{card,0x0000,60,1};
        (void)c->draw_container_styled(&style);
    }
    draw_text(g_prefix_card_root,"选择前缀",210,18,kGold,2,2);
    for (uint32_t index = 0; index < g_prefix_option_count; ++index) {
        const int32_t raw = g_prefix_option_raw[index];
        const char* prefix = raw == 2 ? "无" : contracts::item_rarity_prefix(
            g_draft.variant_series,g_draft.prefix_class_bits,raw);
        if (prefix == nullptr) prefix = "";
        char label[64]{};
        const size_t length = std::strlen(prefix);
        size_t trimmed = length;
        while (trimmed > 0 && prefix[trimmed - 1] == ' ') --trimmed;
        std::snprintf(label,sizeof(label),"%.*s（%d）",
            static_cast<int>(trimmed),prefix,raw);
        draw_prefix_card_button(g_prefix_buttons[index],label,126,44,
            raw == g_prefix_selected_raw);
    }
    draw_prefix_card_button(g_prefix_confirm,"确定",110,42,
        g_prefix_selected_raw >= 0);
    draw_prefix_card_button(g_prefix_cancel,"取消",110,42,true);
}
void apply_prefix_card_action() {
    if (g_prefix_card_pending_action == PrefixCardAction::kNone) return;
    const PrefixCardAction action = g_prefix_card_pending_action;
    g_prefix_card_pending_action = PrefixCardAction::kNone;
    const bool changed = action == PrefixCardAction::kConfirm &&
        prefix_raw_available(g_prefix_selected_raw);
    if (changed) {
        g_draft.raw_rarity = g_prefix_selected_raw;
        g_draft.dirty = true;
    }
    destroy_prefix_card();
    if (changed) {
        build_left_rows();
        rebuild_left_list();
        rebuild_item_slot();
    }
}

void destroy() {
    destroy_item_card();
    destroy_category_card();
    destroy_quantity_card();
    destroy_prefix_card();
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
    g_card_open_requested = false;
    g_card_pending_action = CategoryCardAction::kNone;
    g_item_card_open_requested = false;
    g_item_card_pending_action = ItemCardAction::kNone;
    g_quantity_card_open_requested = false;
    g_quantity_card_pending_action = QuantityCardAction::kNone;
    g_prefix_card_open_requested = false;
    g_prefix_card_pending_action = PrefixCardAction::kNone;
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
        !succeeded(adapter->set_draw_proc(g_category_row,&category_card_noop_draw))) return false;
    rebuild_left_list();
    rebuild_right_list();
    if (g_list == nullptr) return false;
    const UiNavButtonDesc close{{contracts::ui_theme::kNavBoxAbsX - panel.x,
        contracts::ui_theme::kNavBoxAbsY - panel.y,
        contracts::ui_theme::kNavBoxW, contracts::ui_theme::kNavBoxH},
        kUiNavButtonForceClose,nullptr,nullptr};
    if (!succeeded(c->nav_button_create(&close,g_root,&g_close))) return false;
    return true;
}
}

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
    apply_category_card_action();
    apply_item_card_action();
    apply_quantity_action();
    apply_prefix_card_action();
    if (g_prefix_card_open_requested) {
        g_prefix_card_open_requested = false;
        g_card_open_requested = false;
        g_item_card_open_requested = false;
        g_quantity_card_open_requested = false;
        destroy_quantity_card();
        destroy_category_card();
        destroy_item_card();
        if (!create_prefix_card()) destroy_prefix_card();
    } else if (g_quantity_card_open_requested) {
        g_quantity_card_open_requested = false;
        g_card_open_requested = false;
        g_item_card_open_requested = false;
        destroy_prefix_card();
        destroy_category_card();
        destroy_item_card();
        if (!create_quantity_card()) destroy_quantity_card();
    } else if (g_item_card_open_requested) {
        g_item_card_open_requested = false;
        g_card_open_requested = false;
        g_prefix_card_open_requested = false;
        destroy_quantity_card();
        destroy_prefix_card();
        destroy_category_card();
        if (!create_item_card()) destroy_item_card();
    } else if (g_card_open_requested) {
        g_card_open_requested = false;
        g_prefix_card_open_requested = false;
        destroy_quantity_card();
        destroy_prefix_card();
        destroy_item_card();
        if (!create_category_card()) {
            destroy_category_card();
        }
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
        std::snprintf(text,sizeof(text),"名称：%s (id:%d)",
            name == nullptr ? "" : name,g_draft.item_id);
    else std::snprintf(text,sizeof(text),"名称：");
    draw_name_row(text);
    draw_fixed_category_row();
    if (g_card_root == nullptr && g_item_card_root == nullptr &&
        g_quantity_card_root == nullptr &&
        g_prefix_card_root == nullptr) {
        if (g_left_list != nullptr) (void)c->scroll_list_process(g_left_list);
        (void)c->scroll_list_process(g_list);
    }
    if (g_left_list != nullptr) (void)c->scroll_list_draw(g_left_list);
    (void)c->scroll_list_draw(g_list);
    if (c->nav_button_draw != nullptr) (void)c->nav_button_draw(g_close);
    if (g_card_root != nullptr) draw_category_card();
    if (g_item_card_root != nullptr) draw_item_card();
    if (g_quantity_card_root != nullptr) draw_quantity_card();
    if (g_prefix_card_root != nullptr) draw_prefix_card();
    if (adapter->end_frame != nullptr) (void)adapter->end_frame();
}
void panel_f3() { panel_destroy(); }
void panel_f4() {}
uint32_t panel_event(uint64_t event,uint64_t param,uint64_t param2) {
    const auto* c=components(); if(!g_active.load(std::memory_order_acquire)||g_root==nullptr||c==nullptr||c->dispatch_native_event==nullptr)return 1;
    uint32_t consumed=0;
    if (g_prefix_card_root != nullptr) {
        (void)c->dispatch_native_event(g_prefix_card_root,event,param,param2,&consumed);
        return consumed != 0 ? consumed : 1;
    }
    if (g_quantity_card_root != nullptr) {
        (void)c->dispatch_native_event(g_quantity_card_root,event,param,param2,&consumed);
        return consumed != 0 ? consumed : 1;
    }
    if (g_item_card_footer_root != nullptr) {
        (void)c->dispatch_native_event(g_item_card_footer_root,event,param,param2,&consumed);
        if (consumed != 0) return consumed;
    }
    if (g_card_footer_root != nullptr) {
        (void)c->dispatch_native_event(g_card_footer_root,event,param,param2,&consumed);
        if (consumed != 0) return consumed;
    }
    if (g_item_card_root != nullptr) {
        (void)c->dispatch_native_event(g_item_card_root,event,param,param2,&consumed);
        if (consumed != 0) return consumed;
    }
    if (g_card_root != nullptr) {
        (void)c->dispatch_native_event(g_card_root,event,param,param2,&consumed);
        if (consumed != 0) return consumed;
    }
    (void)c->dispatch_native_event(g_root,event,param,param2,&consumed);
    return consumed;
}
void panel_destroy() { destroy(); }
bool panel_is_active() { return g_active.load(std::memory_order_acquire); }
}  // namespace qol::features::item_editor

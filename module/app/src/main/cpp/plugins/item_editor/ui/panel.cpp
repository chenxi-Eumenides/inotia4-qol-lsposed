#include "../plugin.h"

#include "../../../contracts/panels.h"
#include "../../../contracts/ui.h"
#include "../../../contracts/ui_theme.h"
#include "../../../contracts/attribute_quality.h"
#include "../../../contracts/game_ports/item_port.h"
#include "../../../contracts/item_display_name.h"
#include "../model/draft.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace qol::features::item_editor {
namespace {
using namespace contracts;
constexpr int32_t kPanelW = 900;
constexpr int32_t kPanelH = 560;
constexpr int32_t kRightRowH = 75;
constexpr uint32_t kText = 0xFFF2E8D5u;
constexpr uint32_t kGold = 0xFFFFD789u;
constexpr uint32_t kMuted = 0xFF817A72u;
enum class LeftRowKind : uint8_t {
    kCategory,
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
QolUiHandle g_name_row = nullptr;
QolUiHandle g_card_root = nullptr;
QolUiHandle g_card_list = nullptr;
QolUiHandle g_card_confirm = nullptr;
QolUiHandle g_card_cancel = nullptr;
QolUiHandle g_card_buttons[kItemClassCount]{};
int32_t g_card_button_classes[kItemClassCount]{};
ItemEditorDraft g_draft{};
LeftRowKind g_left_rows[11]{};
uint32_t g_left_row_count = 0;
int32_t g_card_selected_class = -1;
bool g_card_open_requested = false;
enum class CategoryCardAction : uint8_t { kNone, kConfirm, kCancel };
CategoryCardAction g_card_pending_action = CategoryCardAction::kNone;
std::atomic<bool> g_active{false};

const QolUiComponents* components() {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* value = nullptr;
    if (!succeeded(sdk->capabilities.require(kUiComponentsCapabilityId,
            {kUiContractMajor, kUiContractMinor}, &value))) return nullptr;
    return static_cast<const QolUiComponents*>(value);
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

void build_left_rows() {
    g_left_row_count = 0;
    g_left_rows[g_left_row_count++] = LeftRowKind::kCategory;
    if (item_class_is_equipment(g_draft.class_index)) {
        constexpr LeftRowKind equipment_rows[] = {
            LeftRowKind::kPrefix,
            LeftRowKind::kEnchantDurability,
            LeftRowKind::kSocketTotal,
            LeftRowKind::kDamage,
            LeftRowKind::kDefense,
            LeftRowKind::kMagicDamage,
            LeftRowKind::kRequiredLevel,
            LeftRowKind::kChaosLevel,
            LeftRowKind::kChaosRate,
        };
        for (const LeftRowKind kind : equipment_rows)
            g_left_rows[g_left_row_count++] = kind;
    } else if (item_class_valid(g_draft.class_index) || draft_has_item(g_draft)) {
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
    int32_t pressed = 0;
    if (adapter->control_pressed != nullptr)
        (void)adapter->control_pressed(g_name_row, &pressed);
    if (adapter->draw_help_option_row != nullptr) {
        const UiRect panel = panel_rect();
        (void)adapter->draw_help_option_row({panel.x + rect.x, panel.y + rect.y,
            rect.w, rect.h}, 0, static_cast<uint8_t>(pressed != 0));
    }
    draw_text(g_root, text, static_cast<int32_t>(rect.x + 16),
              static_cast<int32_t>(rect.y + 14), kText, 0, 0);
}
void draw_left_info_row(void*, uint32_t index, QolUiHandle row, uint8_t) {
    const auto* adapter = ui();
    if (adapter == nullptr || index >= g_left_row_count) return;
    int64_t abs_x = 0, abs_y = 0;
    if (adapter->control_absolute_origin == nullptr ||
        !succeeded(adapter->control_absolute_origin(row, &abs_x, &abs_y))) return;
    if (adapter->draw_help_option_row != nullptr) {
        int32_t pressed = 0;
        if (adapter->control_pressed != nullptr)
            (void)adapter->control_pressed(row, &pressed);
        (void)adapter->draw_help_option_row({abs_x + 4, abs_y + 2, 308, 44}, 0,
                                            static_cast<uint8_t>(pressed != 0));
    }
    char text[160]{};
    switch (g_left_rows[index]) {
        case LeftRowKind::kCategory: {
            if (!draft_has_item(g_draft)) {
                if (item_class_valid(g_draft.class_index)) {
                    const char* class_name = static_text(
                        QolItemStaticTextKind::kItemClassList, g_draft.class_index);
                    std::snprintf(text, sizeof(text), "类别：%s",
                        class_name == nullptr ? "未知类别" : class_name);
                } else {
                    std::snprintf(text, sizeof(text), "类别：");
                }
                break;
            }
            const char* type_name = item_type_name(g_draft.item_type);
            const int32_t type_id = static_cast<int32_t>(g_draft.item_type);
            if (item_class_is_equipment(g_draft.class_index)) {
                const char* class_name = static_text(
                    QolItemStaticTextKind::kItemClassName, g_draft.item_id);
                if (class_name != nullptr && class_name[0] != '\0')
                    std::snprintf(text, sizeof(text), "类别：%s（%s） (id:%d)",
                        type_name, class_name, type_id);
                else
                    std::snprintf(text, sizeof(text), "类别：%s (id:%d)", type_name, type_id);
            } else {
                std::snprintf(text, sizeof(text), "类别：%s (id:%d)", type_name, type_id);
            }
            break;
        }
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
    draw_text(row, text, 16, 14, kText, 0, 0);
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
void left_info_selected(void*, uint32_t index) {
    if (index < g_left_row_count && g_left_rows[index] == LeftRowKind::kCategory)
        g_card_open_requested = true;
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
        if (g_card_root != nullptr) (void)c->destroy(g_card_root);
    }
    g_card_root = nullptr;
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
        c->scroll_list_set_selection_enabled == nullptr) return false;
    const UiPanelDesc card_desc{{130,30,640,500},{0,0,640,500},0,0};
    if (!succeeded(c->panel_create(&card_desc, g_root, &g_card_root))) return false;
    const UiScrollListDesc list{{16,42,608,380},52,16,-1,
        &draw_category_card_row,nullptr,nullptr};
    if (!succeeded(c->scroll_list_create(&list,g_card_root,&g_card_list))) return false;
    if (!succeeded(c->scroll_list_set_selection_enabled(g_card_list,0))) return false;
    g_card_selected_class = item_class_valid(g_draft.class_index) ? g_draft.class_index : -1;
    for (int32_t class_index = 0; class_index < kItemClassCount; ++class_index) {
        uint32_t row = 0;
        int32_t column = 0;
        if (!category_class_location(class_index, &row, &column)) return false;
        g_card_button_classes[class_index] = class_index;
        const UiRect rect{15 + column * 196,2,186,48};
        if (!succeeded(c->scroll_row_button_create(g_card_list,row,rect,
                &category_class_clicked,&g_card_button_classes[class_index],
                &g_card_buttons[class_index]))) return false;
        if (!succeeded(adapter->set_draw_proc(g_card_buttons[class_index],
                &category_card_noop_draw))) return false;
    }
    const UiButtonDesc confirm{{172,436,140,48},"",0,
        &category_card_confirm_clicked,nullptr};
    if (!succeeded(c->button_create(&confirm,g_card_root,&g_card_confirm))) return false;
    if (!succeeded(adapter->set_draw_proc(g_card_confirm,&category_card_noop_draw))) return false;
    const UiButtonDesc cancel{{328,436,140,48},"",0,
        &category_card_cancel_clicked,nullptr};
    if (!succeeded(c->button_create(&cancel,g_card_root,&g_card_cancel))) return false;
    if (!succeeded(adapter->set_draw_proc(g_card_cancel,&category_card_noop_draw))) return false;
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
    draw_text(g_card_root,"类别选择",320,8,kGold,2,1);
    (void)c->scroll_list_process(g_card_list);
    (void)c->scroll_list_draw(g_card_list);
    draw_category_card_footer_button(g_card_confirm,"确认",
        item_class_valid(g_card_selected_class));
    draw_category_card_footer_button(g_card_cancel,"取消",true);
}
void rebuild_left_list() {
    const auto* c = components();
    if (c == nullptr || c->scroll_list_create == nullptr) return;
    if (g_left_list != nullptr && c->destroy != nullptr) (void)c->destroy(g_left_list);
    g_left_list = nullptr;
    const UiScrollListDesc list{{122,210,316,290},48,g_left_row_count,-1,
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
    if (c == nullptr || c->item_slot_create == nullptr) return;
    if (g_icon_slot != nullptr && c->destroy != nullptr) (void)c->destroy(g_icon_slot);
    g_icon_slot = nullptr;
    QolUiItemRef ref{};
    if (draft_has_item(g_draft) && g_draft.source_valid) {
        ref.source = static_cast<uint32_t>(QolUiItemSource::kHandle);
        ref.handle = g_draft.source;
    }
    const UiItemSlotDesc icon{{243,68,74,74},ref,0,kUiItemSlotShowFrame};
    (void)c->item_slot_create(&icon,g_root,&g_icon_slot);
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

void destroy() {
    destroy_category_card();
    const auto* c = components();
    if (c != nullptr && c->destroy != nullptr) {
        if (g_list != nullptr) (void)c->destroy(g_list);
        if (g_left_list != nullptr) (void)c->destroy(g_left_list);
        if (g_icon_slot != nullptr) (void)c->destroy(g_icon_slot);
        if (g_name_row != nullptr) (void)c->destroy(g_name_row);
        if (g_root != nullptr) (void)c->destroy(g_root);
    }
    g_list = nullptr;
    g_left_list = nullptr;
    g_icon_slot = nullptr;
    g_name_row = nullptr;
    g_root = nullptr;
    g_close = nullptr;
    g_card_open_requested = false;
    g_card_pending_action = CategoryCardAction::kNone;
    g_active.store(false, std::memory_order_release);
}
bool create() {
    const auto* c = components();
    if (c == nullptr || c->panel_create == nullptr || c->scroll_list_create == nullptr ||
        c->nav_button_create == nullptr || c->button_create == nullptr) return false;
    const UiRect panel = panel_rect();
    const UiPanelDesc desc{{panel.x,panel.y,panel.w,panel.h},
        {0,0,panel.x*2+panel.w,640},0,0};
    if (!succeeded(c->panel_create(&desc, nullptr, &g_root))) return false;
    rebuild_item_slot();
    if (g_icon_slot == nullptr) return false;
    const UiButtonDesc name_button{{122,152,316,48},"",0,&noop_click,nullptr};
    if (!succeeded(c->button_create(&name_button, g_root, &g_name_row))) return false;
    if (ui() != nullptr && ui()->set_draw_proc != nullptr)
        (void)ui()->set_draw_proc(g_name_row, [](void*) {});
    rebuild_left_list();
    if (g_left_list == nullptr) return false;
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
    if (g_card_open_requested) {
        g_card_open_requested = false;
        if (!create_category_card()) destroy_category_card();
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
    if (g_card_root == nullptr) {
        (void)c->scroll_list_process(g_left_list);
        (void)c->scroll_list_process(g_list);
    }
    (void)c->scroll_list_draw(g_left_list);
    (void)c->scroll_list_draw(g_list);
    if (c->nav_button_draw != nullptr) (void)c->nav_button_draw(g_close);
    if (g_card_root != nullptr) draw_category_card();
    if (adapter->end_frame != nullptr) (void)adapter->end_frame();
}
void panel_f3() { panel_destroy(); }
void panel_f4() {}
uint32_t panel_event(uint64_t event,uint64_t param,uint64_t param2) {
    const auto* c=components(); if(!g_active.load(std::memory_order_acquire)||g_root==nullptr||c==nullptr||c->dispatch_native_event==nullptr)return 1;
    uint32_t consumed=0;
    const QolUiHandle target = g_card_root != nullptr ? g_card_root : g_root;
    (void)c->dispatch_native_event(target,event,param,param2,&consumed);
    return consumed;
}
void panel_destroy() { destroy(); }
bool panel_is_active() { return g_active.load(std::memory_order_acquire); }
}  // namespace qol::features::item_editor

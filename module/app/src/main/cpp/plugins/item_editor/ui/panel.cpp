#include "../plugin.h"

#include "../../../contracts/panels.h"
#include "../../../contracts/ui.h"
#include "../../../contracts/ui_theme.h"
#include "../../../contracts/attribute_quality.h"
#include "../../../contracts/game_ports/item_port.h"
#include "../../../contracts/item_display_name.h"

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
QolItemInventoryView g_view{};
QolItemHandle g_item = kInvalidItemHandle;
int32_t g_category = 0;
QolItemType g_item_type{};
bool g_has_item = false;
LeftRowKind g_left_rows[11]{};
uint32_t g_left_row_count = 0;
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
    if (!g_has_item) return;
    if (g_item_type == QolItemType::kEquipment) {
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
    } else if (g_view.count_supported != 0) {
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
    g_has_item = false;
    g_item = kInvalidItemHandle;
    g_category = 0;
    g_item_type = {};
    g_view = {};
    const auto* adapter = ui();
    int32_t detail_open = 0;
    if (adapter != nullptr && adapter->item_detail_open != nullptr) {
        if (!succeeded(adapter->item_detail_open(&detail_open)) || detail_open == 0) return;
    }
    const auto* port = item_port();
    if (port == nullptr || port->read.detail_item_context == nullptr ||
        port->read.inventory_item_view == nullptr) return;
    QolItemDetailContext detail{};
    if (!succeeded(port->read.detail_item_context(&detail)) || detail.item.slot == 0) return;
    // 1 是袋信息面板，不是物品详情；不能把 cursor 默认 0 当成首格物品。
    if (detail.desc_type != 0 && detail.desc_type != 2) return;
    QolItemInventoryView view{};
    if (!succeeded(port->read.inventory_item_view(detail.item, &view))) return;
    g_item = detail.item;
    g_view = view;
    g_category = view.category;
    g_has_item = true;
    if (port->read.item_type != nullptr) (void)port->read.item_type(g_item, &g_item_type);
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
    if (!g_has_item) return false;
    if (g_item_type == QolItemType::kEquipment) return true;
    return g_item_type == QolItemType::kJewel && section == 1;
}
uint32_t option_count(uint32_t section) {
    if (!g_has_item) return 0;
    if (section == 0) return std::min(g_view.native_option_count, kQolItemInventoryNativeOptionCapacity);
    if (section == 1) {
        if (g_item_type == QolItemType::kJewel) {
            constexpr uint32_t kJewelFields = kQolItemInventoryHasJewelValue |
                                              kQolItemInventoryHasJewelAttributeType;
            return (g_view.fields & kJewelFields) == kJewelFields ? 1u : 0u;
        }
        return std::min(g_view.jewel_option_count, kQolItemInventoryJewelOptionCapacity);
    }
    return std::min(g_view.enchant_option_count, kQolItemInventoryEnchantOptionCapacity);
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
    const QolItemInventoryOptionView* option = section == 0 ? g_view.native_options + index :
        (section == 1 ? g_view.jewel_options + index : g_view.enchant_options + index);
    const char* name = nullptr;
    char value[64]{};
    uint32_t value_color = enabled ? kText : kMuted;
    if (index < count) {
        const bool jewel_self = section == 1 && g_item_type == QolItemType::kJewel;
        const int32_t option_id = jewel_self ? g_view.jewel_attribute_type : option->id;
        const int32_t option_value = jewel_self ? g_view.jewel_value : option->value;
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
        const bool colorable = (section == 0 && g_item_type == QolItemType::kEquipment) ||
                               (section == 1 && g_item_type == QolItemType::kJewel);
        if (colorable && !unknown_name) {
            const auto* api = attribute_quality_api();
            if (api != nullptr) {
                AttributeQualityItem quality{};
                const QolResult result = api->query_option_context(
                    g_item, option_id, option_value, nullptr, &quality);
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
            const char* type_name = item_type_name(g_item_type);
            const int32_t type_id = static_cast<int32_t>(g_item_type);
            if (g_item_type == QolItemType::kEquipment) {
                const char* class_name = static_text(
                    QolItemStaticTextKind::kItemClassName, g_category);
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
            std::snprintf(text, sizeof(text), "数量：%d", g_view.effective_count);
            break;
        case LeftRowKind::kPrefix: {
            const char* prefix_class = static_text(
                QolItemStaticTextKind::kItemPrefixClass, g_category);
            const int32_t class_bits = prefix_class == nullptr ? 0 : std::atoi(prefix_class);
            const char* prefix = contracts::item_rarity_prefix(g_view.variant_series,
                class_bits, g_view.raw_rarity);
            const bool has_prefix = prefix != nullptr && prefix[0] != '\0';
            const char* label = has_prefix ? prefix : "无";
            size_t label_length = std::strlen(label);
            while (has_prefix && label_length > 0 && label[label_length - 1] == ' ')
                --label_length;
            std::snprintf(text, sizeof(text), "前缀：%.*s（raw:%d）",
                static_cast<int>(label_length), label, g_view.raw_rarity);
            break;
        }
        case LeftRowKind::kEnchantDurability:
            std::snprintf(text, sizeof(text), "强化耐久度：%d", g_view.enchant_remaining);
            break;
        case LeftRowKind::kSocketTotal:
            std::snprintf(text, sizeof(text), "宝石孔数：%d", g_view.socket_total);
            break;
        case LeftRowKind::kDamage:
            std::snprintf(text, sizeof(text), "伤害：%d", g_view.damage);
            break;
        case LeftRowKind::kDefense:
            std::snprintf(text, sizeof(text), "防御：%d", g_view.defense);
            break;
        case LeftRowKind::kMagicDamage:
            if (g_view.damage == 0 && g_view.magic_rate == 0) {
                std::snprintf(text, sizeof(text), "法伤：0（0.00x）");
            } else {
                const double multiplier = static_cast<double>(g_view.magic_rate) / 100.0;
                const double magic_damage = static_cast<double>(g_view.damage) * multiplier;
                std::snprintf(text, sizeof(text), "法伤：%.1f（%.2fx）",
                    magic_damage, multiplier);
            }
            break;
        case LeftRowKind::kRequiredLevel:
            std::snprintf(text, sizeof(text), "需要等级：%d", g_view.need_level);
            break;
        case LeftRowKind::kChaosLevel:
            std::snprintf(text, sizeof(text), "混沌等级：%d", g_view.chaos_level);
            break;
        case LeftRowKind::kChaosRate:
            std::snprintf(text, sizeof(text), "混沌数值率：%d%%", g_view.chaos_rate);
            break;
        default:
            return;
    }
    draw_text(row, text, 16, 14, kText, 0, 0);
}

void destroy() {
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
    QolUiItemRef icon_ref{};
    if (g_has_item) {
        icon_ref.source = static_cast<uint32_t>(QolUiItemSource::kHandle);
        icon_ref.handle = g_item;
    }
    const UiItemSlotDesc icon_desc{{243,68,74,74}, icon_ref, 0, kUiItemSlotShowFrame};
    if (c->item_slot_create == nullptr ||
        !succeeded(c->item_slot_create(&icon_desc, g_root, &g_icon_slot))) return false;
    const UiButtonDesc name_button{{122,152,316,48},"",0,&noop_click,nullptr};
    if (!succeeded(c->button_create(&name_button, g_root, &g_name_row))) return false;
    if (ui() != nullptr && ui()->set_draw_proc != nullptr)
        (void)ui()->set_draw_proc(g_name_row, [](void*) {});
    const UiScrollListDesc left_list{{122,210,316,290},48,g_left_row_count,-1,
        &draw_left_info_row,nullptr,nullptr};
    if (!succeeded(c->scroll_list_create(&left_list,g_root,&g_left_list))) return false;
    const uint32_t rows = 3 + option_count(0) + option_count(1) + option_count(2);
    const UiScrollListDesc list{{470,44,400,472},kRightRowH,rows,-1,&draw_right_row,nullptr,nullptr};
    if (!succeeded(c->scroll_list_create(&list,g_root,&g_list))) return false;
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
    if (!g_has_item) draw_text(g_root,"无物品",280,96,kMuted,2,0);
    char text[160]{};
    const char* name = g_has_item ? static_text(QolItemStaticTextKind::kItemName,g_category) : nullptr;
    if (g_has_item) std::snprintf(text,sizeof(text),"名称：%s (id:%d)",name == nullptr ? "" : name,g_category);
    else std::snprintf(text,sizeof(text),"名称：");
    draw_name_row(text);
    (void)c->scroll_list_process(g_left_list);
    (void)c->scroll_list_process(g_list);
    (void)c->scroll_list_draw(g_left_list);
    (void)c->scroll_list_draw(g_list);
    if (c->nav_button_draw != nullptr) (void)c->nav_button_draw(g_close);
    if (adapter->end_frame != nullptr) (void)adapter->end_frame();
}
void panel_f3() { panel_destroy(); }
void panel_f4() {}
uint32_t panel_event(uint64_t event,uint64_t param,uint64_t param2) {
    const auto* c=components(); if(!g_active.load(std::memory_order_acquire)||g_root==nullptr||c==nullptr||c->dispatch_native_event==nullptr)return 1;
    uint32_t consumed=0; (void)c->dispatch_native_event(g_root,event,param,param2,&consumed); return consumed;
}
void panel_destroy() { destroy(); }
bool panel_is_active() { return g_active.load(std::memory_order_acquire); }
}  // namespace qol::features::item_editor

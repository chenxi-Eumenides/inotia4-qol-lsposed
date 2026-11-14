#include "numeric_card.h"

#include "../panel_internal.h"
#include "../../plugin.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace qol::features::item_editor {
using namespace contracts;
namespace {

constexpr int32_t kCardW = 420;
constexpr int32_t kCardH = 250;
constexpr uint32_t kMaxButtons = 8;  // 增量按钮上限；预设按钮额外追加。

QolUiHandle g_card_root = nullptr;
// kMaxButtons 个增量按钮 + 最多 1 个预设按钮。
QolUiHandle g_adjust_buttons[kMaxButtons + 1]{};
QolUiHandle g_confirm = nullptr;
QolUiHandle g_cancel = nullptr;
int32_t g_button_deltas[kMaxButtons]{};
char g_button_labels[kMaxButtons + 1][16]{};
uint32_t g_delta_count = 0;
int32_t g_preset_value = 0;
bool g_has_preset = false;
char g_card_id[32]{};
NumericCardTarget g_target = NumericCardTarget::kEnchantRemaining;
int32_t g_min = 0;
int32_t g_max = 0;
int32_t g_value = 0;
bool g_open_requested = false;
enum class CardAction : uint8_t { kNone, kConfirm, kCancel };
CardAction g_pending_action = CardAction::kNone;

void card_noop_draw(void*) {}

void adjust_clicked(void* user) {
    const auto* delta = static_cast<const int32_t*>(user);
    if (delta == nullptr) return;
    // int64 中间量避免 +step 溢出；越界值夹紧到 [min,max]。
    const int64_t adjusted = static_cast<int64_t>(g_value) + *delta;
    g_value = static_cast<int32_t>(std::clamp<int64_t>(adjusted, g_min, g_max));
}
void preset_clicked(void*) {
    g_value = std::clamp(g_preset_value, g_min, g_max);
}
void confirm_clicked(void*) {
    g_pending_action = CardAction::kConfirm;
}
void cancel_clicked(void*) {
    g_pending_action = CardAction::kCancel;
}

// 混沌改动后按基础属性重算派生值；need_level 按
// min(100, base_need_level + chaos_level) 补混沌加成。读取失败保留当前派生值。
void recalc_chaos_derived() {
    const auto* port = item_port();
    if (port == nullptr || port->read.read_item_base_stats_ex == nullptr) {
        panel_internal::log_panel(
            "fail op=chaos_derived result=failed stage=api code=-1 reason=api_unavailable");
        return;
    }
    if (!draft_has_item(panel_internal::g_draft)) return;
    QolItemBaseStats stats{};
    const QolResult result = port->read.read_item_base_stats_ex(
        panel_internal::g_draft.item_id, panel_internal::g_draft.raw_rarity, &stats);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message, sizeof(message),
            "fail item=%d op=chaos_derived result=failed stage=read code=%d"
            " reason=base_stats_read_failed",
            panel_internal::g_draft.item_id, result.code);
        panel_internal::log_panel(message);
        return;
    }
    panel_internal::g_draft.damage = stats.damage;
    panel_internal::g_draft.defense = stats.defense;
    panel_internal::g_draft.magic_rate = stats.magic_rate;
    panel_internal::g_draft.need_level =
        std::min(100, stats.need_level + panel_internal::g_draft.chaos_level);
}

void draw_card_button(QolUiHandle button, const char* label,
                      int32_t width, int32_t height) {
    const auto* adapter = panel_internal::ui();
    if (adapter == nullptr || button == nullptr) return;
    int64_t abs_x = 0, abs_y = 0;
    if (adapter->control_absolute_origin == nullptr ||
        !succeeded(adapter->control_absolute_origin(button, &abs_x, &abs_y))) return;
    int32_t pressed = 0;
    if (adapter->control_pressed != nullptr)
        (void)adapter->control_pressed(button, &pressed);
    if (adapter->draw_help_option_row != nullptr)
        (void)adapter->draw_help_option_row({abs_x, abs_y, width, height}, 0,
            static_cast<uint8_t>(pressed != 0));
    panel_internal::draw_center_text(button, label, width / 2, height / 2 - 8,
                                     panel_internal::kText);
}

}  // namespace

bool numeric_card_open(const NumericCardSpec& spec) {
    if (spec.min_value > spec.max_value || spec.deltas == nullptr ||
        spec.delta_count == 0 || spec.delta_count > kMaxButtons) {
        panel_internal::log_panel("op=numeric_card_open result=failed reason=invalid_spec");
        return false;
    }
    g_target = spec.target;
    g_min = spec.min_value;
    g_max = spec.max_value;
    g_value = std::clamp(spec.value, spec.min_value, spec.max_value);
    g_card_id[0] = '\0';
    if (spec.id != nullptr)
        std::strncpy(g_card_id, spec.id, sizeof(g_card_id) - 1);
    // 按钮回调通过 user 指针读增量，必须指向生命周期等同卡片的静态存储。
    g_delta_count = spec.delta_count;
    for (uint32_t index = 0; index < spec.delta_count; ++index) {
        g_button_deltas[index] = spec.deltas[index];
        std::snprintf(g_button_labels[index],
            sizeof(g_button_labels[index]), "%+d", g_button_deltas[index]);
    }
    g_has_preset = spec.has_preset;
    g_preset_value = spec.preset_value;
    if (g_has_preset) {
        std::snprintf(g_button_labels[g_delta_count],
            sizeof(g_button_labels[g_delta_count]), "%d", g_preset_value);
    }
    // 同一时间只允许一张数字卡；打开新卡前先关闭旧卡。
    destroy_numeric_card();
    g_open_requested = true;
    return true;
}

bool numeric_card_open_requested() { return g_open_requested; }
void numeric_card_clear_open_request() { g_open_requested = false; }
bool numeric_card_active() { return g_card_root != nullptr; }

void destroy_numeric_card() {
    const auto* c = panel_internal::components();
    if (c != nullptr && c->destroy != nullptr) {
        for (auto& button : g_adjust_buttons) {
            if (button != nullptr) (void)c->destroy(button);
            button = nullptr;
        }
        if (g_confirm != nullptr) (void)c->destroy(g_confirm);
        if (g_cancel != nullptr) (void)c->destroy(g_cancel);
        if (g_card_root != nullptr) (void)c->destroy(g_card_root);
    }
    for (auto& button : g_adjust_buttons) button = nullptr;
    g_confirm = nullptr;
    g_cancel = nullptr;
    g_card_root = nullptr;
    g_open_requested = false;
    g_pending_action = CardAction::kNone;
}

bool create_numeric_card() {
    const auto* c = panel_internal::components();
    const auto* adapter = panel_internal::ui();
    if (c == nullptr || adapter == nullptr || c->panel_create == nullptr ||
        c->button_create == nullptr || adapter->set_draw_proc == nullptr) {
        panel_internal::log_panel("op=numeric_card_create result=failed stage=precondition code=-1");
        return false;
    }
    const UiRect panel = panel_internal::panel_rect();
    const UiRect card_rect{panel.x + (panel.w - kCardW) / 2,
                           panel.y + (panel.h - kCardH) / 2, kCardW, kCardH};
    const UiPanelDesc card{card_rect, {0, 0, kCardW, kCardH}, 0, 0};
    QolResult result = c->panel_create(&card, nullptr, &g_card_root);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message, sizeof(message),
            "op=numeric_card_create result=failed stage=card_root id=%s code=%d",
            g_card_id, result.code);
        panel_internal::log_panel(message);
        return false;
    }
    const uint32_t button_count = g_delta_count + (g_has_preset ? 1u : 0u);
    // 4 个以内保持原数字卡单行布局；更多按钮用四列两行网格（原数量卡布局）。
    const bool grid = button_count > 4;
    for (uint32_t index = 0; index < button_count; ++index) {
        const int32_t column = static_cast<int32_t>(index % 4);
        const int32_t row = static_cast<int32_t>(index / 4);
        const int32_t y = grid ? 96 + row * 50 : 118;
        const bool is_delta = index < g_delta_count;
        const UiButtonDesc button{{22 + column * 96, y, 88, 42}, "", 0,
            is_delta ? &adjust_clicked : &preset_clicked,
            is_delta ? static_cast<void*>(&g_button_deltas[index]) : nullptr};
        result = c->button_create(&button, g_card_root, &g_adjust_buttons[index]);
        if (!succeeded(result)) {
            char message[128]{};
            std::snprintf(message, sizeof(message),
                "op=numeric_card_create result=failed stage=adjust_button id=%s code=%d index=%u",
                g_card_id, result.code, index);
            panel_internal::log_panel(message);
            return false;
        }
        result = adapter->set_draw_proc(g_adjust_buttons[index], &card_noop_draw);
        if (!succeeded(result)) {
            char message[128]{};
            std::snprintf(message, sizeof(message),
                "op=numeric_card_create result=failed stage=adjust_draw id=%s code=%d index=%u",
                g_card_id, result.code, index);
            panel_internal::log_panel(message);
            return false;
        }
    }
    const UiButtonDesc confirm{{100, 200, 100, 42}, "", 0,
        &confirm_clicked, nullptr};
    result = c->button_create(&confirm, g_card_root, &g_confirm);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message, sizeof(message),
            "op=numeric_card_create result=failed stage=confirm_button id=%s code=%d",
            g_card_id, result.code);
        panel_internal::log_panel(message);
        return false;
    }
    result = adapter->set_draw_proc(g_confirm, &card_noop_draw);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message, sizeof(message),
            "op=numeric_card_create result=failed stage=confirm_draw id=%s code=%d",
            g_card_id, result.code);
        panel_internal::log_panel(message);
        return false;
    }
    const UiButtonDesc cancel{{220, 200, 100, 42}, "", 0,
        &cancel_clicked, nullptr};
    result = c->button_create(&cancel, g_card_root, &g_cancel);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message, sizeof(message),
            "op=numeric_card_create result=failed stage=cancel_button id=%s code=%d",
            g_card_id, result.code);
        panel_internal::log_panel(message);
        return false;
    }
    result = adapter->set_draw_proc(g_cancel, &card_noop_draw);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message, sizeof(message),
            "op=numeric_card_create result=failed stage=cancel_draw id=%s code=%d",
            g_card_id, result.code);
        panel_internal::log_panel(message);
        return false;
    }
    return true;
}

void draw_numeric_card() {
    if (g_card_root == nullptr) return;
    const auto* adapter = panel_internal::ui();
    const auto* c = panel_internal::components();
    if (adapter == nullptr || c == nullptr) return;
    const UiRect panel = panel_internal::panel_rect();
    const UiRect card{panel.x + (panel.w - kCardW) / 2,
                      panel.y + (panel.h - kCardH) / 2, kCardW, kCardH};
    if (adapter->fill_rect_alpha != nullptr)
        (void)adapter->fill_rect_alpha({panel.x, panel.y, panel.w, panel.h}, 0x0000, 40);
    if (c->draw_container_styled != nullptr) {
        const UiContainerStyleDesc style{card, 0x0000, 60, 1};
        (void)c->draw_container_styled(&style);
    }
    if (g_target == NumericCardTarget::kCount) {
        // 数量卡沿用原独立实现的标题行与确认文案。
        char quantity[48]{};
        std::snprintf(quantity, sizeof(quantity), "数量：%d", g_value);
        panel_internal::draw_text(g_card_root, quantity, 210, 36,
                                  panel_internal::kText, 2, 2);
    } else {
        char value_text[48]{};
        std::snprintf(value_text, sizeof(value_text), "当前：%d", g_value);
        panel_internal::draw_center_text(g_card_root, value_text, 210, 40,
                                         panel_internal::kText, 1);
        char range_text[64]{};
        std::snprintf(range_text, sizeof(range_text), "范围：%d ~ %d",
            g_min, g_max);
        panel_internal::draw_center_text(g_card_root, range_text, 210, 80,
                                         panel_internal::kMuted);
    }
    const uint32_t button_count = g_delta_count + (g_has_preset ? 1u : 0u);
    for (uint32_t index = 0; index < button_count; ++index)
        draw_card_button(g_adjust_buttons[index], g_button_labels[index], 88, 42);
    draw_card_button(g_confirm, g_target == NumericCardTarget::kCount ? "确定" : "确认",
                     100, 42);
    draw_card_button(g_cancel, "取消", 100, 42);
}

void numeric_card_apply() {
    if (g_pending_action == CardAction::kNone) return;
    const CardAction action = g_pending_action;
    g_pending_action = CardAction::kNone;
    bool rebuilt = false;
    if (action == CardAction::kConfirm) {
        switch (g_target) {
            case NumericCardTarget::kCount: {
                const bool valid = item_class_valid(panel_internal::g_draft.class_index) &&
                    !item_class_is_equipment(panel_internal::g_draft.class_index) &&
                    panel_internal::g_draft.stackable;
                if (valid) {
                    panel_internal::g_draft.count =
                        std::clamp(g_value, 1, stack_limit_max_count());
                    panel_internal::g_draft.dirty = true;
                    rebuilt = true;
                }
                break;
            }
            case NumericCardTarget::kEnchantRemaining:
                panel_internal::g_draft.enchant_remaining =
                    std::clamp(g_value, g_min, g_max);
                panel_internal::g_draft.dirty = true;
                break;
            case NumericCardTarget::kSocketTotal:
                // 孔数不重建列表、不重算派生属性（基础伤害/防御/需要等级不变）。
                panel_internal::g_draft.socket_total =
                    std::clamp(g_value, g_min, g_max);
                panel_internal::g_draft.dirty = true;
                break;
            case NumericCardTarget::kChaosLevel:
            case NumericCardTarget::kChaosRate:
                // 混沌字段改动必须置标记位 2（chaos 生效位），再按基础属性重算派生。
                panel_internal::g_draft.chaos_flags |= 2;
                if (g_target == NumericCardTarget::kChaosLevel)
                    panel_internal::g_draft.chaos_level = std::clamp(g_value, g_min, g_max);
                else
                    panel_internal::g_draft.chaos_rate = std::clamp(g_value, g_min, g_max);
                panel_internal::g_draft.dirty = true;
                recalc_chaos_derived();
                break;
        }
    }
    destroy_numeric_card();
    if (rebuilt) {
        panel_internal::build_left_rows();
        panel_internal::rebuild_left_list();
        panel_internal::rebuild_item_slot();
    }
}

bool numeric_card_handle_event(uint64_t event, uint64_t param, uint64_t param2,
                               uint32_t* consumed) {
    const auto* c = panel_internal::components();
    if (c == nullptr || c->dispatch_native_event == nullptr ||
        g_card_root == nullptr) return false;
    uint32_t local = 0;
    (void)c->dispatch_native_event(g_card_root, event, param, param2, &local);
    if (consumed != nullptr) *consumed = local;
    return true;
}

}  // namespace qol::features::item_editor

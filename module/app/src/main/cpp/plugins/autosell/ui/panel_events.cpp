#include "panel_internal.h"

namespace qol::features::autosell {

// 面板是否激活：主线程写、JVM 线程读（关闭时判断是否需要关闭面板）。
std::atomic<bool> g_panel_active{false};
PressTarget g_pressed_target;       // 仅游戏主线程访问
::autosell::Config g_draft;          // 面板草稿；打开时读取，关闭时一次性提交
bool g_toggle_pressed = false;

int autosell_rule_value(int index) {
    switch (index) {
        case 0: return g_draft.rarity;
        case 1: return g_draft.enhance;
        case 2: return g_draft.socket;
        case 3: return g_draft.gem_tier;
        case 4: return g_draft.gem_range;
        case 5: return g_draft.dice_tier;
        case 6: return g_draft.backpack_tier;
        case 7: return g_draft.badge_tier;
        default: return 0;
    }
}

int autosell_rule_max(int index) {
    switch (index) {
        case 0: return 5;
        case 1: return 32;
        case 2: return 16;
        case 3: return 5;
        case 4: return 5;
        case 5: return 5;
        case 6: return 4;
        case 7: return 4;
        default: return 0;
    }
}

void autosell_set_rule_value(int index, int value) {
    const int clamped = value < 0 ? 0 : (value > autosell_rule_max(index) ? autosell_rule_max(index) : value);
    switch (index) {
        case 0: g_draft.rarity = clamped; break;
        case 1: g_draft.enhance = clamped; break;
        case 2: g_draft.socket = clamped; break;
        case 3: g_draft.gem_tier = clamped; break;
        case 4: g_draft.gem_range = clamped; break;
        case 5: g_draft.dice_tier = clamped; break;
        case 6: g_draft.backpack_tier = clamped; break;
        case 7: g_draft.badge_tier = clamped; break;
        default: break;
    }
}

const char* autosell_rule_name(int index) {
    switch (index) {
        case 0: return "品质";
        case 1: return "耐久度";
        case 2: return "总孔数";
        case 3: return "宝石等级";
        case 4: return "宝石属性";
        case 5: return "骰子";
        case 6: return "背包";
        case 7: return "徽章";
        default: return "";
    }
}

void autosell_rule_value_text(int index, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) return;
    const int raw_value = autosell_rule_value(index);
    const int max_value = autosell_rule_max(index);
    const int value = raw_value < 0 ? 0 : (raw_value > max_value ? max_value : raw_value);
    if (value == 0) {
        std::snprintf(out, out_size, "%s", "关");
        return;
    }
    if (index == 0) {
        std::snprintf(out, out_size, "%s", kRarityLabels[value]);
    } else if (index == 1) {
        std::snprintf(out, out_size, "≤%d", value - 1);
    } else if (index == 2) {
        std::snprintf(out, out_size, "≤%d", value - 1);
    } else if (index == 3) {
        std::snprintf(out, out_size, "%s", kGemTierLabels[value]);
    } else if (index == 4) {
        std::snprintf(out, out_size, "%s", kGemRangeLabels[value]);
    } else if (index == 5) {
        std::snprintf(out, out_size, "%s", kDiceLabels[value]);
    } else if (index == 6) {
        std::snprintf(out, out_size, "%s", kBackpackLabels[value]);
    } else if (index == 7) {
        std::snprintf(out, out_size, "%s", kBadgeLabels[value]);
    } else {
        std::snprintf(out, out_size, "%s", "关");
    }
}

void autosell_commit_draft() {
    const int slot = autosell_active_save_slot();
    const bool persisted = autosell_store_commit_config(g_draft);
    AUTOSELLUI_LOG("panel close applied config; persist slot=%d ok=%d", slot, persisted ? 1 : 0);
}

void autosell_save_before_back(void*) { autosell_commit_draft(); }

static void autosell_panel_enter_impl() {
    AUTOSELLUI_LOG("panel enter");
    g_panel_active.store(false, std::memory_order_release);
    g_draft = autosell_get_runtime_config();
    (void)autosell_components();
    if (const auto* ui = ui_api(); ui != nullptr && ui->load_image_unit != nullptr)
        (void)ui->load_image_unit(0x0f);
    if (!autosell_destroy_panel_components()) {
        AUTOSELLUI_LOG("panel component cleanup deferred");
        g_panel_active.store(false, std::memory_order_release);
        autosell_request_panel_close();
        return;
    }
    if (!autosell_create_panel_components(autosell_panel_rect())) {
        (void)autosell_destroy_panel_components();
        AUTOSELLUI_LOG("panel component creation failed");
        g_panel_active.store(false, std::memory_order_release);
        autosell_request_panel_close();
        return;
    }
    if (g_hit_root == nullptr) g_hit_root = ui_create_root({0, 0, 1, 1});
    g_panel_active.store(true, std::memory_order_release);
    g_pressed_target = {};
    g_toggle_pressed = false;
}

static void autosell_panel_process_impl() {
    if (!g_panel_active.load(std::memory_order_acquire) || ui_api() == nullptr) return;
    ui_begin_frame();
    const UiRect panel = autosell_panel_rect();
    const UiRect mask = autosell_mask_rect(panel);
    ui_fill_rect_alpha(mask, 0xFF000000, kMaskAlpha);
    ui_fill_rect_alpha(panel, kPanelFill565, kPanelAlpha);
    if (g_ui_components != nullptr && g_ui_components->draw_container_ornamented != nullptr) {
        (void)g_ui_components->draw_container_ornamented(panel);
        if (g_ui_components->draw_container != nullptr) {
            (void)g_ui_components->draw_container(autosell_equipment_group(panel));
        }
    }

    autosell_draw_component_text("自动出售", static_cast<int32_t>(panel.w / 2), 20,
                                 kGold, 2, 2);
    autosell_draw_total_toggle(panel);
    if (g_ui_components != nullptr && g_back_button != nullptr)
        (void)g_ui_components->nav_button_draw(g_back_button);

    autosell_draw_component_text("装备", 207, 146, kGold, 2, 1);
    for (int i = 0; i < kRuleCount; ++i) {
        const UiRect row = autosell_rule_row(panel, i);
        const char* label = autosell_rule_name(i);
        const uint32_t color = autosell_rule_value(i) == 0 ? kDisabledText : kGold;
        const int32_t label_x = static_cast<int32_t>(row.x - panel.x + 8);
        const int32_t label_y = static_cast<int32_t>(row.y - panel.y + 10);
        autosell_draw_component_text(label, label_x, label_y, color, 0, 1);
        char value_text[32] = {};
        autosell_rule_value_text(i, value_text, sizeof(value_text));
        const int32_t value_x = static_cast<int32_t>(row.x - panel.x + 236);
        autosell_draw_component_text(value_text, value_x,
            static_cast<int32_t>(row.y - panel.y + 10), color, 2, 1);
        if (g_ui_components != nullptr && g_arrows[i][0] != nullptr && g_arrows[i][1] != nullptr) {
            (void)g_ui_components->arrow_button_draw(g_arrows[i][0]);
            (void)g_ui_components->arrow_button_draw(g_arrows[i][1]);
        }
    }

    autosell_draw_component_text("已强化、镶嵌的物品会被自动保护", 402, 80,
                                 kText, 2, 1);
    ui_end_frame();
}

static void autosell_panel_f3_impl() {
    AUTOSELLUI_LOG("panel f3 (terminate)");
    (void)autosell_destroy_panel_components();
    g_panel_active.store(false, std::memory_order_release);
    g_toggle_pressed = false;
    g_pressed_target = {};
}

static void autosell_panel_f4_impl() {}

static uint32_t autosell_panel_event_impl(uint64_t event, uint64_t param, uint64_t param2) {
    (void)param2;
    if (!g_panel_active.load(std::memory_order_acquire) || param == 0) return 1;
    if (event != 0x17 && event != 0x18) return 1;
    const UiRect panel = autosell_panel_rect();
    int64_t tx = 0;
    int64_t ty = 0;
    if (!autosell_touch_xy(param, &tx, &ty)) return 1;
    if (event == 0x17) {
        g_pressed_target = {};
        if (autosell_hit_test(autosell_back_rect(), tx, ty)) {
            g_pressed_target = {1, -1};
        } else if (autosell_hit_test(autosell_total_rect(), tx, ty)) {
            g_pressed_target = {2, -1};
            g_toggle_pressed = true;
        } else {
            for (int i = 0; i < kRuleCount; ++i) {
                if (autosell_hit_test(autosell_rule_prev(panel, i), tx, ty)) {
                    g_pressed_target = {3, i};
                    break;
                }
                if (autosell_hit_test(autosell_rule_next(panel, i), tx, ty)) {
                    g_pressed_target = {4, i};
                    break;
                }
            }
        }
        if (g_pressed_target.kind != 0 && g_pressed_target.kind != 2 &&
            g_component_root != nullptr && g_ui_components != nullptr &&
            g_ui_components->dispatch_native_event != nullptr) {
            uint32_t consumed = 0;
            (void)g_ui_components->dispatch_native_event(g_component_root, event, param,
                                                          param2, &consumed);
        }
    } else if (event == 0x18) {
        const PressTarget pressed = g_pressed_target;
        bool released_inside = false;
        if (pressed.kind == 1) {
            released_inside = autosell_hit_test(autosell_back_rect(), tx, ty);
        } else if (pressed.kind == 2) {
            released_inside = autosell_hit_test(autosell_total_rect(), tx, ty);
        } else if (pressed.kind == 3) {
            released_inside = autosell_hit_test(autosell_rule_prev(panel, pressed.index), tx, ty);
        } else if (pressed.kind == 4) {
            released_inside = autosell_hit_test(autosell_rule_next(panel, pressed.index), tx, ty);
        }
        if (released_inside) {
            if (pressed.kind == 1) {
                autosell_save_before_back(nullptr);
                AUTOSELLUI_LOG("back released -> save and request host back");
            } else if (pressed.kind == 2) {
                g_draft.enabled = !g_draft.enabled;
            } else if (pressed.kind == 3 || pressed.kind == 4) {
                const int delta = pressed.kind == 3 ? -1 : 1;
                autosell_set_rule_value(pressed.index, autosell_rule_value(pressed.index) + delta);
            }
        }
        if (pressed.kind != 0 && pressed.kind != 2 && g_component_root != nullptr &&
            g_ui_components != nullptr && g_ui_components->dispatch_native_event != nullptr) {
            uint32_t consumed = 0;
            (void)g_ui_components->dispatch_native_event(g_component_root, event, param,
                                                          param2, &consumed);
        }
        if (pressed.kind == 2) g_toggle_pressed = false;
        g_pressed_target = {};
    }
    return 1;
}

void autosell_panel_enter() { autosell_panel_enter_impl(); }
void autosell_panel_process() { autosell_panel_process_impl(); }
void autosell_panel_f3() { autosell_panel_f3_impl(); }
void autosell_panel_f4() { autosell_panel_f4_impl(); }
uint32_t autosell_panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    return autosell_panel_event_impl(event, param, param2);
}

// ============== PanelProviderSpec（panel_id="autosell"）==============

bool autosell_provider_open() {
    // 与旧入口按钮点击同门禁：关闭态不得打开面板（含经 core.ui 面板栈打开）。
    if (!g_enabled.load(std::memory_order_acquire)) return false;
    if (ui_api() == nullptr) {
        AUTOSELLUI_LOG("provider open rejected: UI adapter unavailable");
        return false;
    }
    AUTOSELLUI_LOG("provider open accepted");
    return true;
}

void autosell_provider_close() {
    autosell_save_before_back(nullptr);
    g_panel_active.store(false, std::memory_order_release);
    AUTOSELLUI_LOG("provider closed by host");
}

bool autosell_provider_active() {
    return g_panel_active.load(std::memory_order_acquire);
}

const char* autosell_provider_block_reason() {
    return nullptr;  // 现有实现无占用文案
}

}  // namespace qol::features::autosell

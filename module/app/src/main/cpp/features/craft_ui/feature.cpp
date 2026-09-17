#include "features/craft_ui/feature.h"

#include <atomic>

#include "core/native/qol_log.h"
#include "game/access/craft.h"

namespace qol::features::craft_ui {

namespace {

constexpr QolDomain kDomain = QolDomain::kCustomRecipe;

std::atomic<int> g_verify_log_budget{16};

// 填入格控件缓存（绘制层只做指针比较，见 feature.h 注释）。
void* g_stuff_ctrl[kStuffSlotCount] = {nullptr, nullptr, nullptr};

}  // namespace

bool log_budget_take() {
    return qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0;
}

void finish_three_slot_craft() {
    qol::game::craft_init_mixing_state();
    qol::game::craft_reset_stuff_item_control();
    qol::game::craft_refresh_inven_items();
    qol::game::craft_play_sound(kSoundCraftSuccess);
    qol::game::craft_show_text(kTextCraftSuccess);
}

void select_on_view_enter() {
    if (qol::game::craft_get_ui_type() != 1) return;
    if (qol::game::craft_panel_state() == 0) return;
    qol::game::craft_select_first_empty_stuff_slot();
}

void reset_stuff_and_refresh() {
    // 清空填入格 → 刷新背包网格。
    // `UIMix_ResetStuffItemControl` 已被本模块以函数入口 hook 接管（补「清空后选中第一个
    // 空格」，见 reset_stuff_execute），所以这里调完就会自动恢复选中落点。
    qol::game::craft_reset_stuff_item_control();
    qol::game::craft_refresh_inven_items();
}

void refresh_stuff_ctrls() {
    for (size_t i = 0; i < kStuffSlotCount; ++i) {
        g_stuff_ctrl[i] = qol::game::craft_stuff_slot_control(i);
    }
}

bool is_stuff_ctrl(void* ctrl) {
    if (ctrl == nullptr) return false;
    for (size_t i = 0; i < kStuffSlotCount; ++i) {
        if (g_stuff_ctrl[i] == ctrl) return true;
    }
    return false;
}

}  // namespace qol::features::craft_ui

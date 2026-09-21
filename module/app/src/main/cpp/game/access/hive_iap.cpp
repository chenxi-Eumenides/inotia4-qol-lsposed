#include "game/access/hive_iap.h"

#include "game/access/game_access.h"
#include "game/symbols/game_symbols.h"

// op_ok/op_err 响应信封物理仍在 game/ops/game_ops_common.cpp（过渡期归类 infra）；
// 依赖矩阵禁止 game → infra，故本层仅前置声明两入口、链接期解析（与项目既有
// 前置声明桥先例一致），不引入 include 层违规。信封归位 game 层后改回正式 include。
std::string op_ok();
std::string op_err(const char* msg);

// IAP 恢复：屏蔽 Com2uS Hive 阻断态（常驻指令/全局位读写，不属于常驻 patch 表）。
// D2c-2b-2：由 feature/patch/game_patch.cpp 原样迁入；行为逐字。
std::string data_recover_after_hive_block() {
    if (g_base == 0) return op_err("base not ready");
    if (fn_ui_set_popup_process_info == nullptr) return op_err("symbol not resolved");
    if (fn_networkstore_set_state == nullptr) return op_err("symbol not resolved");
    uint32_t** daily_trigger = reinterpret_cast<uint32_t**>(g_base + G_DAILY_TRIGGER_GOT_VMA);
    if (*daily_trigger != nullptr) **daily_trigger = 1;
    uint8_t** hud_gate = reinterpret_cast<uint8_t**>(g_base + G_HUD_GATE_GOT_VMA);
    if (*hud_gate != nullptr) **hud_gate = 1;
    fn_networkstore_set_state(0);
    fn_ui_set_popup_process_info(4, 0);
    return op_ok();
}

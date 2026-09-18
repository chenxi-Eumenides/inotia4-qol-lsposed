#include "game_patch.h"

#include "core/native/qol_log.h"
#include "contracts/stack_limit.h"
#include "game_access.h"
#include "game_inventory.h"
#include "game_ops_common.h"
#include "game_state.h"
#include "data/native/game_symbols.h"
#include "patch/game_patch_bridge.h"
#include "game/read/stack_codec.h"
#include "plugins/extension_bag/game_ui_virtbag.h"

// D2c-2a：native_inventory_hook 已迁 plugins/inventory_hooks；本 TU 仅需 refresh 入口。
// 前置声明以避免新增 plugins/patch → plugins/inventory_hooks 跨插件层依赖（D2c-2b 收口）。
void inventory_native_hook_call_refresh_item_area_original();

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>

// move_merge 的 GOT 槽指针槽（B2a-3 机制；D2a-3b-1 改声明式）。常驻指令 patch 与
// stack_limit 状态已分别迁 host/core_patches 与 core/native/stack_limit_state；
// 本 TU 只保留 move_merge wrapper 与开关标志。GOT 槽由 plugins/extension_bag 的
// X1 常驻 kPtrSlot 资源声明、宿主装配期安装，wrapper 内自门控。
#define MOVE_LOG(...) QOL_LOG_INFO(QolDomain::kPlatform, __VA_ARGS__)

namespace {

std::mutex g_move_merge_mtx;
std::atomic<bool> g_move_merge_requested{false};
std::atomic<bool> g_extension_source_protection_requested{false};

}  // namespace

#include "game_patch_move_merge.inc"

namespace qol::features::game_patch {

void* game_patch_move_merge_wrapper() {
    return reinterpret_cast<void*>(&ui_equip_inven_item_proc_wrapper);
}

}  // namespace qol::features::game_patch

// IAP 恢复：屏蔽 Com2uS Hive 阻断态（常驻指令/全局位读写，不属于常驻 patch 表）。
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

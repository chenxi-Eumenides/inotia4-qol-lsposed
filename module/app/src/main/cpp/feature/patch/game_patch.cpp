#include "game_patch.h"

#include "core/native/qol_log.h"
#include "core/native/stack_limit_port.h"
#include "game_access.h"
#include "game_inventory.h"
#include "game_ops_common.h"
#include "game_state.h"
#include "data/native/game_symbols.h"
#include "game_ptr_hook.h"
#include "stack_codec.h"
#include "feature/extension_bag/game_ui_virtbag.h"
#include "feature/patch/native_inventory_hook.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sys/mman.h>

// move_merge 的 GOT 槽指针 hook（B2a-3）。常驻指令 patch 与 stack_limit 状态已分别迁
// host/core_patches 与 core/native/stack_limit_state；本 TU 只保留 move_merge wrapper。
#define MOVE_LOG(...) QOL_LOG_INFO(QolDomain::kPlatform, __VA_ARGS__)

namespace {

// GOT 槽指针 hook：目标 F_UIEQUIP_INVEN_ITEM_CONTROL_EVENT_PROC_VMA
//（G_UIEQUIP_INVEN_ITEM_PROC_GOT_VMA 槽）；启用态覆盖为 move wrapper。
PtrHook g_move_merge_hook;
std::mutex g_move_merge_mtx;
std::atomic<bool> g_move_merge_requested{false};
std::atomic<bool> g_extension_source_protection_requested{false};

}  // namespace

#include "game_patch_move_merge.inc"

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

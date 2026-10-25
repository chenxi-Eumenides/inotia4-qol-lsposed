#include "plugins/extension_bag/usecases/unequip.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/model/bag_result.h"
#include "plugins/extension_bag/transaction/transaction.h"
#include "plugins/extension_bag/runtime/entry_guard.h"
#include "plugins/extension_bag/runtime/shared.h"

#include <mutex>

namespace qol::features::extension_bag::usecases {

// P3 袋解除（原版语义）：非空拒绝、无空位拒绝、成功 = 按类型重建袋物品
// 入库 + 装备清零 + 切到接收袋。HTTP 路径不弹窗，仅返回错误码。
std::string unequip_bag(int logical_bag) {
    if (!qol::extension_bag::extension_bag_on_main_thread()) {
        return extension_bag_not_ready_error_locked_bridge();
    }
    if (!extension_bag_ready_locked_bridge()) return extension_bag_not_ready_error_locked_bridge();
    const int internal_bag = extension_internal_bag_bridge(logical_bag);
    if (internal_bag < 0) return op_err("bad extension bag (6-10)");
    ExtensionBagUnequipResult result = ExtensionBagUnequipResult::kNotEquipped;
    {
        ::qol::extension_bag::ReentryLock lock;
        if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked()) {
            return extension_bag_not_ready_error_locked_bridge();
        }
        result = unequip_extension_bag_locked_bridge(lock, internal_bag);
    }
    if (result == ExtensionBagUnequipResult::kOk) return op_ok();
    if (result == ExtensionBagUnequipResult::kNotEmpty) return op_err("bag not empty");
    if (result == ExtensionBagUnequipResult::kNoSpace) return op_err("no space");
    if (result == ExtensionBagUnequipResult::kPersistFailed) return op_err("persist failed");
    if (result == ExtensionBagUnequipResult::kFailed) return op_err("unequip failed");
    return op_err("bag not equipped");
}

}  // namespace qol::features::extension_bag::usecases

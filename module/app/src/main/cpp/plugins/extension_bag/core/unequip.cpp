#include "plugins/extension_bag/usecases/unequip.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/session.h"
#include "plugins/extension_bag/model/bag_result.h"
#include "plugins/extension_bag/core/host_adapter.h"
#include "plugins/extension_bag/runtime/entry_guard.h"
#include "plugins/extension_bag/runtime/shared.h"

namespace qol::features::extension_bag::usecases {

// P3 袋解除（原版语义）：非空拒绝、无空位拒绝、成功 = 按类型重建袋物品
// 入库 + 装备清零 + 切到接收袋。HTTP 路径不弹窗，仅返回错误码。
OpResult unequip_bag(int logical_bag) {
    if (!qol::extension_bag::extension_bag_on_main_thread()) {
        return {false, runtime_state().enabled.load() ? Error::kNotInGame : Error::kDisabled};
    }
    if (!extension_bag_ready_locked()) {
        return {false, runtime_state().enabled.load() ? Error::kNotInGame : Error::kDisabled};
    }
    const int internal_bag = extension_storage_index(logical_bag);
    if (internal_bag < 0) return failure(Error::kBadBag);
    ExtensionBagUnequipResult result = ExtensionBagUnequipResult::kNotEquipped;
    {
        ::qol::extension_bag::ReentryLock lock;
        if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked()) {
            return {false, runtime_state().enabled.load() ? Error::kNotInGame : Error::kDisabled};
        }
        result = unequip_extension_bag_locked(lock, internal_bag);
    }
    if (result == ExtensionBagUnequipResult::kOk) return success();
    if (result == ExtensionBagUnequipResult::kNotEmpty) return failure(Error::kBagNotEmpty);
    if (result == ExtensionBagUnequipResult::kNoSpace) return failure(Error::kNoSpace);
    if (result == ExtensionBagUnequipResult::kPersistFailed) return failure(Error::kPersistFailed);
    if (result == ExtensionBagUnequipResult::kFailed) return failure(Error::kUnequipFailed);
    return failure(Error::kNotEquipped);
}

}  // namespace qol::features::extension_bag::usecases

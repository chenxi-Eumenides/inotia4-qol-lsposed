#include "plugins/extension_bag/usecases/view.h"

#include "plugins/extension_bag/core/ui_state.h"
#include "plugins/extension_bag/core/lifecycle.h"
#include "plugins/extension_bag/core/projection.h"
#include "plugins/extension_bag/core/tabs.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"
#include "plugins/extension_bag/runtime/session.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"

namespace qol::features::extension_bag::usecases {

namespace {

OpResult not_ready_result() {
    return {false, runtime_state().enabled.load() ? Error::kNotInGame : Error::kDisabled};
}

}  // namespace

OpResult enter_view(int logical_bag) {
    if (!extension_bag_ready_locked()) return not_ready_result();
    const int internal_bag = extension_internal_bag(logical_bag);
    if (internal_bag < 0) return failure(Error::kBadBag);
    ::qol::extension_bag::ReentryLock lock;
    if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked()) {
        return not_ready_result();
    }
    // 面板门禁：控件树仅在背包面板打开时有效，面板关闭时不能投影到 stale 树。
    if (!qol::extension_bag::ui_state().ui.inventory_frame_active) return failure(Error::kInventoryPanelNotOpen);
    if (runtime_state().objects.bag.mode == virtual_bag::Mode::kModule) {
        return failure(Error::kAlreadyInExtensionView);
    }
    if (runtime_state().objects.bag.mode == virtual_bag::Mode::kExitingModule) {
        return failure(Error::kExitInProgress);
    }
    handle_extension_tab_click_locked(internal_bag);
    const bool entered = runtime_state().objects.bag.mode == virtual_bag::Mode::kModule;
    return entered ? success() : failure(Error::kEnterViewFailed);
}

OpResult exit_view() {
    if (!extension_bag_ready_locked()) return not_ready_result();
    ::qol::extension_bag::ReentryLock lock;
    if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked()) {
        return not_ready_result();
    }
    if (runtime_state().objects.bag.mode == virtual_bag::Mode::kExitingModule) {
        return failure(Error::kExitInProgress);
    }
    if (runtime_state().objects.bag.mode != virtual_bag::Mode::kModule ||
        !virtual_bag::valid_index(runtime_state().objects.bag.selected)) {
        return failure(Error::kNotInExtensionView);
    }

    const int original_bag = original_bag_locked();
    if (qol::extension_bag::ui_state().objects.module_view_installed) {
        restore_module_view_locked();
    } else {
        clear_original_desc_locked();
    }
    const virtual_bag::ExitViewResult result =
        virtual_bag::exit_module_view(&runtime_state().objects.bag, original_bag);
    if (result == virtual_bag::ExitViewResult::kExitInProgress) {
        return failure(Error::kExitInProgress);
    }
    if (result != virtual_bag::ExitViewResult::kExited) {
        return failure(Error::kNotInExtensionView);
    }
    persist_state_locked();
    return success();
}

OpResult select_bag(int logical_bag) {
    if (!extension_bag_ready_locked()) return not_ready_result();
    const int internal_bag = extension_internal_bag(logical_bag);
    if (internal_bag < 0) return failure(Error::kBadBag);
    ::qol::extension_bag::ReentryLock lock;
    if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked()) {
        return not_ready_result();
    }
    if (runtime_state().objects.bag.mode != virtual_bag::Mode::kModule) {
        return failure(Error::kNotInExtensionView);
    }
    if (runtime_state().objects.bag.selected == internal_bag) {
        return failure(Error::kBagAlreadySelected);
    }
    handle_extension_tab_click_locked(internal_bag);
    const bool selected = runtime_state().objects.bag.selected == internal_bag;
    return selected ? success() : failure(Error::kSelectBagFailed);
}

}  // namespace qol::features::extension_bag::usecases

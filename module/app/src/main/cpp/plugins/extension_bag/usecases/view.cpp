#include "plugins/extension_bag/usecases/view.h"

#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/extension_bag_internal.h"
#include "plugins/extension_bag/runtime/shared.h"

#include <mutex>

namespace qol::features::extension_bag::usecases {

std::string enter_view(int logical_bag) {
    if (!extension_bag_ready_locked_bridge()) return extension_bag_not_ready_error_locked_bridge();
    const int internal_bag = extension_internal_bag_bridge(logical_bag);
    if (internal_bag < 0) return op_err("bad extension bag (6-10)");
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked()) {
        return extension_bag_not_ready_error_locked_bridge();
    }
    ensure_state_loaded_locked();
    // 面板门禁（P3 真机实证）：控件树仅在背包面板打开时有效，面板关闭时
    // enter_view 会投影到 stale 树（点击失效/重建后被 RefreshItemArea 冲掉）。
    if (!runtime_state().ui.inventory_frame_active) return op_err("inventory panel not open");
    if (runtime_state().objects.bag.mode == virtual_bag::Mode::kModule) {
        return op_err("already in extension view");
    }
    if (runtime_state().objects.bag.mode == virtual_bag::Mode::kExitingModule) {
        return op_err("exit in progress");
    }
    handle_extension_tab_click_locked(internal_bag);
    const bool entered = runtime_state().objects.bag.mode == virtual_bag::Mode::kModule;
    return extension_bag_view_result_json_bridge(entered, "enter extension view failed");
}

std::string exit_view() {
    if (!extension_bag_ready_locked_bridge()) return extension_bag_not_ready_error_locked_bridge();
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked()) {
        return extension_bag_not_ready_error_locked_bridge();
    }
    ensure_state_loaded_locked();
    if (runtime_state().objects.bag.mode == virtual_bag::Mode::kExitingModule) {
        return op_err("exit in progress");
    }
    if (runtime_state().objects.bag.mode != virtual_bag::Mode::kModule ||
        !virtual_bag::valid_index(runtime_state().objects.bag.selected)) {
        return op_err("not in extension view");
    }

    const int original_bag = original_bag_locked();
    if (runtime_state().objects.module_view_installed) {
        restore_module_view_locked();
    } else {
        clear_original_desc_locked();
    }
    const virtual_bag::ExitViewResult result =
        virtual_bag::exit_module_view(&runtime_state().objects.bag, original_bag);
    if (result == virtual_bag::ExitViewResult::kExitInProgress) {
        return op_err("exit in progress");
    }
    if (result != virtual_bag::ExitViewResult::kExited) {
        return op_err("not in extension view");
    }
    persist_state_locked();
    return extension_bag_view_result_json_bridge(true, "exit extension view failed");
}

std::string select_bag(int logical_bag) {
    if (!extension_bag_ready_locked_bridge()) return extension_bag_not_ready_error_locked_bridge();
    const int internal_bag = extension_internal_bag_bridge(logical_bag);
    if (internal_bag < 0) return op_err("bad extension bag (6-10)");
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked()) {
        return extension_bag_not_ready_error_locked_bridge();
    }
    ensure_state_loaded_locked();
    if (runtime_state().objects.bag.mode != virtual_bag::Mode::kModule) {
        return op_err("not in extension view");
    }
    if (runtime_state().objects.bag.selected == internal_bag) return op_err("bag already selected");
    handle_extension_tab_click_locked(internal_bag);
    const bool selected = runtime_state().objects.bag.selected == internal_bag;
    return extension_bag_view_result_json_bridge(selected, "select bag failed");
}

}  // namespace qol::features::extension_bag::usecases

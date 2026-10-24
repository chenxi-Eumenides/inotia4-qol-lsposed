#include "plugins/extension_bag/runtime/projection_controller.h"

#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/projection.h"

bool bag_size_word_locked(int bag, uint32_t* out_word) {
    return runtime_state().host.game != nullptr &&
           runtime_state().host.write_port->ui.bag_capacity_word != nullptr &&
           out_word != nullptr && bag >= 0 && bag < 6 &&
           qol::contracts::succeeded(
               runtime_state().host.write_port->ui.bag_capacity_word(bag, out_word));
}

bool set_bag_size_word_locked(int bag, uint32_t word) {
    return runtime_state().host.game != nullptr &&
           runtime_state().host.write_port->ui.set_bag_capacity_word != nullptr &&
           bag >= 0 && bag < 6 &&
           qol::contracts::succeeded(
               runtime_state().host.write_port->ui.set_bag_capacity_word(bag, word));
}

int original_bag_locked() {
    if (runtime_state().host.game == nullptr ||
        runtime_state().host.write_port->ui.current_bag == nullptr) return 0;
    int32_t bag = 0;
    return qol::contracts::succeeded(runtime_state().host.write_port->ui.current_bag(&bag)) &&
                   bag >= 0 && bag < 6
               ? bag
               : 0;
}

int raw_current_bag_locked() {
    const auto* port = runtime_state().host.write_port;
    if (port == nullptr || port->ui.current_bag == nullptr) return -1;
    int32_t bag = -1;
    return qol::contracts::succeeded(port->ui.current_bag(&bag)) ? static_cast<int>(bag) : -1;
}

int native_bag_page_count_locked() {
    void* container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    if (container == nullptr) return 0;
    int child_count = 0;
    if (!extension_bag_ui_child_count(container, &child_count) || child_count <= 0) return 0;
    const int limit = child_count < 32 ? child_count : 32;
    int native_tabs = 0;
    for (int index = 0; index < limit; ++index) {
        void* child = extension_bag_ui_child_at(container, index);
        if (child == nullptr || extension_bag_ui_user_type(child) != 2) continue;
        bool module_tab = false;
        for (const auto& anchor : runtime_state().ui.extension_tab_anchors) {
            if (anchor.mounted != 0 && anchor.handle == child) {
                module_tab = true;
                break;
            }
        }
        if (!module_tab) ++native_tabs;
    }
    return native_tabs;
}

bool native_source_bag_allowed_locked(int bag) {
    if (bag < 0) return false;
    if (bag < virtual_bag::kOriginalTransactionBagCount) return true;
    // 索引 5 是原版任务袋，不能成为任何移动来源。
    if (bag == virtual_bag::kOriginalTaskBag) return false;
    const auto* game = runtime_state().host.game;
    if (game == nullptr || game->target.has_capability == nullptr ||
        game->target.has_capability(
            qol::contracts::kQolTargetCapabilityC18NativeBagLayoutCandidate) == 0) {
        return false;
    }
    const int page_count = native_bag_page_count_locked();
    return page_count > 0 && bag < page_count;
}

void set_original_bag_locked(int bag) {
    if (runtime_state().host.game == nullptr ||
        runtime_state().host.write_port->ui.set_current_bag == nullptr ||
        bag < 0 || bag >= 6) return;
    (void)runtime_state().host.write_port->ui.set_current_bag(bag);
    log_exit_trace_locked("write_set", 0, 0, 0, -1, -1);
}

int projection_proxy_bag_locked() {
    const int current = original_bag_locked();
    if (current >= 0 && current < virtual_bag::kOriginalTransactionBagCount) return current;
    return 0;
}

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

int native_bag_count_locked() {
    // 袋号拓扑来自游戏袋数据（INVEN_GetBagSize 语义），不依赖任何控件树；
    // 未打开背包时同样可用。C18 追加袋上界为 18，原版为 6。
    const auto* extras = runtime_state().host.inventory_extras;
    if (extras == nullptr || extras->read.bag_size == nullptr) return 6;
    int highest = -1;
    for (int bag = 0; bag < 18; ++bag) {
        int32_t size = 0;
        if (!qol::contracts::succeeded(extras->read.bag_size(bag, &size))) continue;
        if (size > 0) highest = bag;
    }
    const int count = highest + 1;
    return count < 6 ? 6 : count;
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
    return bag < native_bag_count_locked();
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

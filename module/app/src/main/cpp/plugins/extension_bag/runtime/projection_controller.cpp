#include "plugins/extension_bag/runtime/projection_controller.h"

#include "contracts/game_ports/item_port.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"

bool bag_size_word_locked(int bag, uint32_t* out_word) {
    return runtime_state().host.game != nullptr &&
           runtime_state().host.write_port->ui.bag_capacity_word != nullptr &&
            out_word != nullptr && bag >= 0 && bag < virtual_bag::kOriginalNativeBagCount &&
           qol::contracts::succeeded(
               runtime_state().host.write_port->ui.bag_capacity_word(bag, out_word));
}

bool set_bag_size_word_locked(int bag, uint32_t word) {
    return runtime_state().host.game != nullptr &&
           runtime_state().host.write_port->ui.set_bag_capacity_word != nullptr &&
            bag >= 0 && bag < virtual_bag::kOriginalNativeBagCount &&
           qol::contracts::succeeded(
               runtime_state().host.write_port->ui.set_bag_capacity_word(bag, word));
}

int original_bag_locked() {
    if (runtime_state().host.game == nullptr ||
        runtime_state().host.write_port->ui.current_bag == nullptr) return 0;
    int32_t bag = 0;
    return qol::contracts::succeeded(runtime_state().host.write_port->ui.current_bag(&bag)) &&
                    bag >= 0 && bag < virtual_bag::kOriginalNativeBagCount
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
    // 未打开背包时同样可用。C18 上界使用 item port 的布局容量信源。
    const auto* extras = runtime_state().host.inventory_extras;
    if (extras == nullptr || extras->read.bag_size == nullptr) {
        return virtual_bag::kOriginalNativeBagCount;
    }
    int highest = -1;
    for (int bag = 0; bag < qol::contracts::kQolMaxNativeBagCount; ++bag) {
        int32_t size = 0;
        if (!qol::contracts::succeeded(extras->read.bag_size(bag, &size))) continue;
        if (size > 0) highest = bag;
    }
    const int count = highest + 1;
    return count < virtual_bag::kOriginalNativeBagCount
               ? virtual_bag::kOriginalNativeBagCount
               : count;
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
        bag < 0 || bag >= virtual_bag::kOriginalNativeBagCount) return;
    (void)runtime_state().host.write_port->ui.set_current_bag(bag);
    QOL_LOG_DEBUG(QolDomain::kExtensionBag, "write_set bag=%d", bag);
}

int projection_proxy_bag_locked() {
    const int current = original_bag_locked();
    if (current >= 0 && current < virtual_bag::kOriginalTransactionBagCount) return current;
    return 0;
}

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

bool native_bag_size_locked(int bag, int32_t* out_size) {
    if (out_size == nullptr || bag < 0) return false;
    const auto* extras = runtime_state().host.inventory_extras;
    if (extras == nullptr) return false;
    if (extras->read.bag_size_ref != nullptr) {
        const qol::contracts::QolBagRef ref{
            bag >= qol::contracts::kQolOriginalBagCount
                ? qol::contracts::kQolBagSpaceVariant
                : qol::contracts::kQolBagSpaceOriginal,
            bag >= qol::contracts::kQolOriginalBagCount
                ? bag - qol::contracts::kQolOriginalBagCount
                : bag};
        return qol::contracts::succeeded(extras->read.bag_size_ref(&ref, out_size));
    }
    // 旧 Host 只提供扁平 bag_size，且仅支持原版 0..5。
    if (bag >= qol::contracts::kQolOriginalBagCount ||
        extras->read.bag_size == nullptr) {
        return false;
    }
    return qol::contracts::succeeded(extras->read.bag_size(bag, out_size));
}

int native_bag_count_locked() {
    // 原版袋恒为 0..5；C18 追加袋使用空间化 bag_size_ref 查询，不能继续用
    // 只接受 0..5 的扁平 bag_size，否则追加袋永远不会被视为合法来源。
    const int original_count = virtual_bag::kOriginalNativeBagCount;
    const auto* game = runtime_state().host.game;
    if (game == nullptr || game->target.has_capability == nullptr ||
        game->target.has_capability(
            qol::contracts::kQolTargetCapabilityC18NativeBagLayoutCandidate) == 0) {
        return original_count;
    }
    // 商店/合成页的袋列每帧都会查询本函数；逐帧扫描全部候选袋会触发
    // native_bag_size_locked → 袋容量/槽占用原语 → game_memory_accessible，
    // 单次可达数十万次地图查询。按 EB 持久化 revision 缓存，仅在袋状态变化时重扫。
    static thread_local uint64_t cached_revision = ~uint64_t{0};
    static thread_local int cached_count = original_count;
    const uint64_t revision = extension_bag_item_revision_locked();
    if (revision == cached_revision) return cached_count;
    int count = original_count;
    for (int bag = qol::contracts::kQolOriginalBagCount;
         bag < qol::contracts::kQolMaxNativeBagCount; ++bag) {
        int32_t size = 0;
        if (!native_bag_size_locked(bag, &size) || size <= 0) continue;
        count = bag + 1;
    }
    cached_revision = revision;
    cached_count = count;
    return count;
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

#include "plugins/core.inventory/transfer/replace_item.h"

#include <cstddef>
#include <cstring>
#include <initializer_list>

#include "plugins/core.inventory/api/bag_source.h"
#include "plugins/core.inventory/runtime/context.h"
#include "plugins/core.inventory/transfer/locate_item.h"
#include "plugins/core.inventory/transfer/native_transport.h"
#include "plugins/core.inventory/usecases/read_inventory.h"

namespace qol::features::core_inventory::transfer::replace_item {
namespace {

QolResult error(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

bool valid_handle(contracts::QolItemHandle item) {
    return item.slot != 0 && item.generation != 0;
}

bool replacement_valid(const contracts::QolBagTransfer* replacement) {
    return replacement != nullptr && replacement->category > 0 && replacement->count > 0 &&
           replacement->payload_size >= 1 &&
           replacement->payload_size <= contracts::kQolBagTransferPayloadMaxSize;
}

QolResult apply_replacement(contracts::QolItemHandle expected,
                            const contracts::QolBagTransfer* replacement) {
    if (g_item_state_write == nullptr || g_item_state_write->apply_payload == nullptr) {
        return error(kQolErrUnsupported, "payload application unavailable");
    }
    return g_item_state_write->apply_payload(expected, replacement->payload,
                                             replacement->payload_size);
}

QolResult replace_native(const contracts::QolBagRef& ref, int32_t slot,
                         contracts::QolItemHandle expected,
                         const contracts::QolBagTransfer* replacement,
                         contracts::QolBagMutationResult* out) {
    (void)slot;
    // 升阶是字段级小改动：原地把 payload 写回既有对象，保留对象地址/句柄，
    // craft UI 与 UseStuff 持有的指针继续有效。
    const QolResult applied = apply_replacement(expected, replacement);
    if (!contracts::succeeded(applied)) return applied;
    out->ref = ref;
    out->item = expected;
    out->count = replacement->count;
    request_inventory_refresh();
    return contracts::ok();
}

QolResult replace_provider(contracts::QolItemHandle expected,
                           const contracts::QolBagTransfer* replacement,
                           contracts::QolBagMutationResult* out) {
    uint8_t old_payload[contracts::kQolBagTransferPayloadCapacity]{};
    uint32_t old_size = 0;
    const bool have_old =
        g_port != nullptr && g_port->read.serialize_item != nullptr &&
        contracts::succeeded(g_port->read.serialize_item(
            expected, old_payload, static_cast<uint32_t>(sizeof(old_payload)), &old_size));
    if (!have_old) return error(kQolErrInternal, "cannot snapshot item payload");
    // 先原地更新对象；provider 只负责把同一 payload 提交为权威 descriptor。
    const QolResult applied = apply_replacement(expected, replacement);
    if (!contracts::succeeded(applied)) return applied;
    auto rollback = [&]() {
        if (!have_old) return;
        contracts::QolBagTransfer old_transfer{};
        old_transfer.category = replacement->category;
        old_transfer.count = replacement->count;
        old_transfer.payload_size = old_size;
        std::memcpy(old_transfer.payload, old_payload, old_size);
        (void)apply_replacement(expected, &old_transfer);
    };
    const uint32_t count = registered_bag_source_count();
    for (uint32_t index = 0; index < count; ++index) {
        const contracts::QolBagSource* source = registered_bag_source_at(index);
        if (source == nullptr || source->replace_item == nullptr) continue;
        const QolResult replaced = source->replace_item(expected, replacement, out);
        if (replaced.code == kQolErrNotFound) continue;
        if (!contracts::succeeded(replaced)) {
            rollback();
            return replaced;
        }
        return contracts::ok();
    }
    rollback();
    return error(kQolErrNotFound, "replacement item not found");
}

QolResult locate_provider(contracts::QolItemHandle expected, contracts::QolBagRef* out_ref,
                          int32_t* out_slot) {
    const uint32_t count = registered_bag_source_count();
    for (uint32_t index = 0; index < count; ++index) {
        const contracts::QolBagSource* source = registered_bag_source_at(index);
        if (source == nullptr || source->identify == nullptr) continue;
        contracts::QolBagRef ref{};
        int32_t slot = -1;
        const QolResult identified = source->identify(expected, &ref, &slot);
        if (!contracts::succeeded(identified)) continue;
        if (ref.space_id != source->space_id || ref.bag < 0 || slot < 0) continue;
        *out_ref = ref;
        *out_slot = slot;
        return contracts::ok();
    }
    return error(kQolErrNotFound, "replacement item not located");
}

bool native_find_available() {
    if (g_game == nullptr) return false;
    constexpr size_t kItemAbiEnd =
        offsetof(contracts::QolGameHost, item_abi) + sizeof(contracts::QolItemAbiDispatchApi);
    return g_game->abi.size >= kItemAbiEnd &&
           g_game->item_abi.original_find_item_slot != nullptr;
}

QolResult locate_native_direct(contracts::QolItemHandle expected, contracts::QolBagRef* out_ref,
                               int32_t* out_slot) {
    if (!native_find_available()) return error(kQolErrNotFound, "native item not located");
    int32_t bag = -1;
    int32_t slot = -1;
    const QolResult found = g_game->item_abi.original_find_item_slot(expected, &bag, &slot);
    if (contracts::succeeded(found) && bag >= 0 && bag < contracts::kQolOriginalBagCount &&
        slot >= 0) {
        *out_ref = contracts::QolBagRef{contracts::kQolBagSpaceOriginal, bag};
        *out_slot = slot;
        return contracts::ok();
    }
    return error(kQolErrNotFound, "native item not located");
}

QolResult locate_native_scan(contracts::QolItemHandle expected, contracts::QolBagRef* out_ref,
                             int32_t* out_slot) {
    for (int32_t space : {contracts::kQolBagSpaceOriginal, contracts::kQolBagSpaceVariant}) {
        contracts::QolBagRef ref{};
        int32_t slot = -1;
        const QolResult located = native_transport::locate(space, expected, &ref, &slot);
        if (contracts::succeeded(located)) {
            *out_ref = ref;
            *out_slot = slot;
            return contracts::ok();
        }
        if (space == contracts::kQolBagSpaceVariant &&
            (located.code == kQolErrNotReady || located.code == kQolErrUnsupported)) {
            continue;
        }
        if (located.code != kQolErrNotFound) return located;
    }
    return error(kQolErrNotFound, "native item not located");
}

}  // namespace

QolResult locate_item(contracts::QolItemHandle expected, contracts::QolBagRef* out_ref,
                      int32_t* out_slot) {
    if (out_ref == nullptr || out_slot == nullptr || !valid_handle(expected)) {
        return error(kQolErrInvalidArgument, "invalid item location request");
    }
    *out_ref = {};
    *out_slot = -1;

    // provider 的 identify 是 32 位定位，优先于原生扫描；EB 等注册源不会因为
    // 原生袋扫描的无关错误被截断。
    const QolResult provider = locate_provider(expected, out_ref, out_slot);
    if (contracts::succeeded(provider)) return contracts::ok();

    const QolResult direct = locate_native_direct(expected, out_ref, out_slot);
    if (contracts::succeeded(direct)) return contracts::ok();

    return locate_native_scan(expected, out_ref, out_slot);
}

QolResult replace_item(contracts::QolItemHandle expected,
                       const contracts::QolBagTransfer* replacement,
                       contracts::QolBagMutationResult* out) {
    if (out == nullptr || !valid_handle(expected) || !replacement_valid(replacement)) {
        return error(kQolErrInvalidArgument, "invalid replacement request");
    }
    *out = {};

    contracts::QolBagRef ref{};
    int32_t slot = -1;
    const QolResult located = locate_item(expected, &ref, &slot);
    if (contracts::succeeded(located)) {
        if (ref.space_id >= contracts::kQolBagSpaceFirstRegistered) {
            return replace_provider(expected, replacement, out);
        }
        if (contracts::qol_bag_ref_is_task_bag(ref)) {
            return error(kQolErrBlocked, "task bag excluded");
        }
        return replace_native(ref, slot, expected, replacement, out);
    }

    // 兼容没有 identify 的注册源：定位失败时仍按旧路径把 expected 交给来源提交。
    const QolResult provider = replace_provider(expected, replacement, out);
    if (provider.code != kQolErrNotFound) return provider;
    return located;
}

}  // namespace qol::features::core_inventory::transfer::replace_item

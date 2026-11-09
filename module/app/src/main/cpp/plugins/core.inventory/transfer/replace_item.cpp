#include "plugins/core.inventory/transfer/replace_item.h"

#include <cstring>
#include <initializer_list>

#include "plugins/core.inventory/api/bag_source.h"
#include "plugins/core.inventory/runtime/context.h"
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

bool is_native_space(int32_t space) {
    return space == contracts::kQolBagSpaceOriginal ||
           space == contracts::kQolBagSpaceVariant;
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

}  // namespace

QolResult replace_item(contracts::QolItemHandle expected,
                       const contracts::QolBagTransfer* replacement,
                       contracts::QolBagMutationResult* out) {
    if (out == nullptr || !valid_handle(expected) || !replacement_valid(replacement)) {
        return error(kQolErrInvalidArgument, "invalid replacement request");
    }
    *out = {};

    // 原生空间优先；expected 不在原生库存时再尝试注册源。
    for (int32_t space : {contracts::kQolBagSpaceOriginal, contracts::kQolBagSpaceVariant}) {
        contracts::QolBagRef ref{};
        int32_t slot = -1;
        const QolResult located = native_transport::locate(space, expected, &ref, &slot);
        if (contracts::succeeded(located)) {
            if (contracts::qol_bag_ref_is_task_bag(ref)) {
                return error(kQolErrBlocked, "task bag excluded");
            }
            return replace_native(ref, slot, expected, replacement, out);
        }
        // C18 追加袋在非 C18 变体下不可用；这只是“不在该空间”，继续尝试注册源。
        if (space == contracts::kQolBagSpaceVariant &&
            (located.code == kQolErrNotReady || located.code == kQolErrUnsupported)) {
            continue;
        }
        if (located.code != kQolErrNotFound) return located;
    }

    return replace_provider(expected, replacement, out);
}

}  // namespace qol::features::core_inventory::transfer::replace_item

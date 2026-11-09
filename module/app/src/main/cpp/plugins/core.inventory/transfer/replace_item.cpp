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

// 详情面板正在展示的物品不能原地换对象：面板控件仍持有旧指针。
bool detail_shows(contracts::QolItemHandle expected) {
    if (g_port == nullptr || g_port->read.detail_item_context == nullptr) return false;
    contracts::QolItemDetailContext context{};
    if (!contracts::succeeded(g_port->read.detail_item_context(&context))) return false;
    return context.item.slot == expected.slot && context.item.generation == expected.generation;
}

QolResult replace_native(const contracts::QolBagRef& ref, int32_t slot,
                         contracts::QolItemHandle expected,
                         const contracts::QolBagTransfer* replacement,
                         contracts::QolBagMutationResult* out) {
    (void)slot;
    // 原生物品没有 provider 权威副本：item.payload.update 已把字段写到原对象上，
    // 这里只确认 expected 仍是槽内物品并返回同一句柄。保留对象身份是必需的：
    // craft UI 与 UseStuff 仍持有该对象指针，换对象会造成悬空引用。
    if (detail_shows(expected)) {
        return error(kQolErrConflict, "item is open in detail view");
    }
    out->ref = ref;
    out->item = expected;
    out->count = replacement->count;
    request_inventory_refresh();
    return contracts::ok();
}

QolResult replace_provider(contracts::QolItemHandle expected,
                           const contracts::QolBagTransfer* replacement,
                           contracts::QolBagMutationResult* out) {
    const uint32_t count = registered_bag_source_count();
    for (uint32_t index = 0; index < count; ++index) {
        const contracts::QolBagSource* source = registered_bag_source_at(index);
        if (source == nullptr || source->replace_item == nullptr) continue;
        if (source->identify != nullptr) {
            contracts::QolBagRef bag_ref{};
            int32_t slot = -1;
            const QolResult located = source->identify(expected, &bag_ref, &slot);
            if (!contracts::succeeded(located)) continue;
            return source->replace_item(expected, replacement, out);
        }
        const QolResult replaced = source->replace_item(expected, replacement, out);
        if (replaced.code == kQolErrNotFound) continue;
        return replaced;
    }
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

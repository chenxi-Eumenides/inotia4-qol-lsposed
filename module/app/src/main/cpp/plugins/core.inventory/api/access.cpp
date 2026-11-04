#include "access.h"

#include <cstring>

#include "runtime/context.h"

namespace qol::features::core_inventory {
namespace {

store::InventoryStore g_store;

QolResult invalid_context() {
    return contracts::error(kQolErrInvalidArgument, "inventory context missing",
                            sizeof("inventory context missing") - 1);
}

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

bool native_port_ready() {
    return g_port != nullptr && g_port->read.native_bag_layout != nullptr &&
           g_port->read.bag_capacity != nullptr && g_port->read.slot_occupied != nullptr &&
           g_port->read.item_ref_at != nullptr &&
           contracts::qol_item_port_native_bag_size_valid(g_port);
}

QolResult native_bag_count(int32_t space_id, int32_t* out_count) {
    if (out_count == nullptr || space_id < contracts::kQolBagSpaceOriginal) {
        return error_result(kQolErrInvalidArgument, "invalid bag count args");
    }
    if (!native_port_ready()) return error_result(kQolErrNotReady, "item port unavailable");
    if (space_id == contracts::kQolBagSpaceOriginal) {
        *out_count = contracts::kQolOriginalBagCount;
        return contracts::ok();
    }
    if (space_id != contracts::kQolBagSpaceVariant) {
        return error_result(kQolErrUnsupported, "bag space unsupported");
    }
    contracts::QolNativeBagLayout layout{};
    const QolResult result = g_port->read.native_bag_layout(&layout);
    if (!contracts::succeeded(result)) return result;
    if (!layout.c18_layout) return error_result(kQolErrNotReady, "c18 bag layout unavailable");
    int32_t count = layout.bag_limit - contracts::kQolOriginalBagCount;
    if (count < 0) count = 0;
    *out_count = count;
    return contracts::ok();
}

QolResult native_bag_info(const contracts::QolBagRef* ref,
                          contracts::QolBagInfo* out_info) {
    if (ref == nullptr || out_info == nullptr ||
        ref->space_id < contracts::kQolBagSpaceOriginal || ref->bag < 0) {
        return error_result(kQolErrInvalidArgument, "invalid bag info args");
    }
    *out_info = {};
    int32_t count = 0;
    QolResult result = native_bag_count(ref->space_id, &count);
    if (!contracts::succeeded(result)) return result;
    if (ref->bag >= count) return error_result(kQolErrInvalidArgument, "bag out of range");
    result = g_port->read.bag_capacity(ref, &out_info->capacity);
    if (!contracts::succeeded(result)) return result;
    for (int32_t slot = 0; slot < 16; ++slot) {
        int32_t occupied = 0;
        result = g_port->read.slot_occupied(ref, slot, &occupied);
        if (!contracts::succeeded(result)) return result;
        out_info->occupied_slots += occupied;
    }
    return contracts::ok();
}

QolResult native_item_at(const contracts::QolBagRef* ref, int32_t slot,
                         contracts::QolInventoryItemRef* out) {
    if (ref == nullptr || out == nullptr ||
        ref->space_id < contracts::kQolBagSpaceOriginal || ref->bag < 0 || slot < 0 ||
        slot >= 16) {
        return error_result(kQolErrInvalidArgument, "invalid item args");
    }
    *out = {};
    int32_t count = 0;
    QolResult result = native_bag_count(ref->space_id, &count);
    if (!contracts::succeeded(result)) return result;
    if (ref->bag >= count) return error_result(kQolErrInvalidArgument, "bag out of range");
    const int32_t flat_bag = ref->bag +
        (ref->space_id == contracts::kQolBagSpaceVariant ? contracts::kQolOriginalBagCount : 0);
    return g_port->read.item_ref_at(flat_bag, slot, out);
}

QolResult native_for_each_space_slot(int32_t space_id, contracts::QolInventoryBagSlotFn fn,
                                     void* ctx) {
    if (fn == nullptr || space_id < contracts::kQolBagSpaceOriginal) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    int32_t bag_count = 0;
    QolResult result = native_bag_count(space_id, &bag_count);
    if (!contracts::succeeded(result)) return result;
    for (int32_t bag = 0; bag < bag_count; ++bag) {
        for (int32_t slot = 0; slot < 16; ++slot) {
            const contracts::QolBagRef ref{space_id, bag};
            contracts::QolInventoryItemRef item{};
            result = native_item_at(&ref, slot, &item);
            if (result.code == kQolErrNotFound) continue;
            if (!contracts::succeeded(result)) return result;
            if (fn(&item, ctx) != 0) return contracts::ok();
        }
    }
    return contracts::ok();
}

QolResult item_at(void* context, const contracts::QolInventorySlotRef* ref,
                  contracts::QolInventoryItemRef* out) {
    if (context == nullptr || ref == nullptr) return invalid_context();
    return static_cast<store::InventoryStore*>(context)->item_at(*ref, out);
}

QolResult locate(void* context, contracts::QolItemHandle item,
                 contracts::QolInventoryItemRef* out) {
    if (context == nullptr) return invalid_context();
    return static_cast<store::InventoryStore*>(context)->locate(item, out);
}

QolResult find_first(void* context, int32_t category,
                     contracts::QolInventoryItemRef* out) {
    if (context == nullptr) return invalid_context();
    return static_cast<store::InventoryStore*>(context)->find_first(category, out);
}

QolResult find_empty(void* context, int32_t space_id,
                     contracts::QolInventorySlotRef* out) {
    if (context == nullptr) return invalid_context();
    return static_cast<store::InventoryStore*>(context)->find_empty(space_id, out);
}

QolResult count_item(void* context, int32_t category, int32_t* out_count) {
    if (context == nullptr) return invalid_context();
    return static_cast<store::InventoryStore*>(context)->count_item(category, out_count);
}

QolResult move(void* context, const contracts::QolInventorySlotRef* from,
               const contracts::QolInventorySlotRef* to) {
    if (context == nullptr || from == nullptr || to == nullptr) return invalid_context();
    return static_cast<store::InventoryStore*>(context)->move(*from, *to);
}

QolResult take(void* context, const contracts::QolInventorySlotRef* ref,
               contracts::QolInventoryItem* out) {
    if (context == nullptr || ref == nullptr) return invalid_context();
    return static_cast<store::InventoryStore*>(context)->take(*ref, out);
}

QolResult place(void* context, const contracts::QolInventorySlotRef* ref,
                const contracts::QolInventoryItem* item) {
    if (context == nullptr || ref == nullptr || item == nullptr) return invalid_context();
    return static_cast<store::InventoryStore*>(context)->place(*ref, *item);
}

QolResult remove_count(void* context, const contracts::QolInventorySlotRef* ref,
                       int32_t count) {
    if (context == nullptr || ref == nullptr) return invalid_context();
    return static_cast<store::InventoryStore*>(context)->remove_count(*ref, count);
}

const contracts::QolInventoryAccessApi kApi = {
    static_cast<uint32_t>(sizeof(contracts::QolInventoryAccessApi)),
    &g_store,
    &item_at,
    &locate,
    &find_first,
    &find_empty,
    &count_item,
    &move,
    &take,
    &place,
    &remove_count,
    &native_bag_count,
    &native_bag_info,
    &native_item_at,
    &native_for_each_space_slot,
};

}  // namespace

store::InventoryStore& inventory_store() { return g_store; }

const contracts::QolInventoryAccessApi& inventory_access_api() { return kApi; }

}  // namespace qol::features::core_inventory

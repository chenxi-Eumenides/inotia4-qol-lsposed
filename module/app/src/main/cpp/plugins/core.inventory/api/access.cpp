#include "access.h"

namespace qol::features::core_inventory {
namespace {

store::InventoryStore g_store;

QolResult invalid_context() {
    return contracts::error(kQolErrInvalidArgument, "inventory context missing",
                            sizeof("inventory context missing") - 1);
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
};

}  // namespace

store::InventoryStore& inventory_store() { return g_store; }

const contracts::QolInventoryAccessApi& inventory_access_api() { return kApi; }

}  // namespace qol::features::core_inventory

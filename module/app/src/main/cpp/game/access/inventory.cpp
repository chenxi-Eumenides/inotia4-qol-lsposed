#include "game/access/inventory.h"

#include "game_state.h"
#include "game_access.h"
#include "game/access/item_class.h"

#include <cstring>

namespace qol::game {
namespace {

constexpr const char* kInvalidArgument = "invalid argument";
constexpr const char* kNotFound = "inventory slot is empty";
constexpr const char* kNotReady = "inventory not ready";

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

void copy_ref(const InventoryItemRef& source, contracts::QolInventoryItemRef* target) {
    target->kind = static_cast<int32_t>(source.kind);
    target->bag = source.bag;
    target->slot = source.slot;
    target->category = source.category;
    target->count = source.count;
    target->native_item = source.native_item;
}

struct BagSlotContext {
    contracts::QolInventoryBagSlotFn callback;
    void* callback_context;
};

bool bridge_bag_slot(void* item, int bag, int slot, void* context) {
    auto* state = static_cast<BagSlotContext*>(context);
    contracts::QolInventoryItemRef ref{
        static_cast<int32_t>(contracts::QolInventoryItemKind::kOriginal),
        bag,
        slot,
        item_class_category(item),
        fn_get_cumulate_count != nullptr ? fn_get_cumulate_count(item) : 1,
        item,
    };
    return state->callback(&ref, state->callback_context) != 0;
}

}  // namespace

QolResult inventory_item_ref_at(int32_t bag, int32_t slot,
                                contracts::QolInventoryItemRef* out) {
    if (out == nullptr || bag < 0 || bag > 10 || slot < 0 || slot >= 16) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    InventoryItemRef source{};
    if (!::inventory_item_ref_at(bag, slot, &source)) {
        if (bag < 6 && g_inven == nullptr) {
            return error_result(kQolErrNotReady, kNotReady);
        }
        return error_result(kQolErrNotFound, kNotFound);
    }
    copy_ref(source, out);
    return contracts::ok();
}

QolResult for_each_bag_slot(contracts::QolInventoryBagSlotFn callback, void* ctx) {
    if (callback == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (g_inven == nullptr) return error_result(kQolErrNotReady, kNotReady);
    BagSlotContext state{callback, ctx};
    ::for_each_bag_slot(&bridge_bag_slot, &state);
    return contracts::ok();
}

}  // namespace qol::game

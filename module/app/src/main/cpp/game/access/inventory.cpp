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

QolResult physical_item_at(int32_t bag, int32_t slot, void** out_item) {
    if (out_item == nullptr || bag < 0 || bag >= 6 || slot < 0 || slot >= 16) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_item = nullptr;
    if (g_inven == nullptr) return error_result(kQolErrNotReady, kNotReady);
    struct Context {
        int32_t bag;
        int32_t slot;
        void** out;
    } context{bag, slot, out_item};
    ::for_each_bag_slot([](void* item, int bag_index, int slot_index, void* raw) -> bool {
        auto* context = static_cast<Context*>(raw);
        if (context->bag == bag_index && context->slot == slot_index) {
            *context->out = item;
            return true;
        }
        return false;
    }, &context);
    return *out_item == nullptr ? error_result(kQolErrNotFound, kNotFound) : contracts::ok();
}

QolResult detail_context(int32_t* out_desc_type, int32_t* out_bag, void** out_control) {
    if (out_desc_type == nullptr || out_bag == nullptr || out_control == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (g_base == 0) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    auto* desc_type = reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_DESC_TYPE_VMA);
    auto* current_bag = reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA);
    auto** panel_control = reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA);
    if (!game_memory_accessible(desc_type, sizeof(*desc_type), 'r') ||
        !game_memory_accessible(current_bag, sizeof(*current_bag), 'r') ||
        !game_memory_accessible(panel_control, sizeof(*panel_control), 'r')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    *out_desc_type = *desc_type;
    *out_bag = *current_bag;
    *out_control = *panel_control;
    return *out_control == nullptr ? error_result(kQolErrNotReady, kNotReady) : contracts::ok();
}

QolResult cursor_index(const void* control, int32_t* out_slot) {
    if (control == nullptr || out_slot == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_control_object_get_cursor_index == nullptr) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    *out_slot = fn_control_object_get_cursor_index(const_cast<void*>(control));
    return *out_slot < 0 ? error_result(kQolErrNotFound, kNotFound) : contracts::ok();
}

}  // namespace qol::game

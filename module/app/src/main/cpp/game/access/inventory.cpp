#include "game/access/inventory.h"

#include "game/state/game_state.h"
#include "game/access/game_access.h"
#include "game/access/item_class.h"
#include "game/access/item_handle_registry.h"
#include "game/adapter_ports/inventory_extension_facade.h"

#include <cstring>

namespace qol::game {
namespace {

constexpr const char* kInvalidArgument = "invalid argument";
constexpr const char* kNotFound = "inventory slot is empty";
constexpr const char* kNotReady = "inventory not ready";

contracts::QolFindItemAbiFn g_find_item_backup = nullptr;
thread_local uint32_t g_original_only_depth = 0;

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

bool handle_for_item(void* item, contracts::QolItemHandle* out_handle) {
    if (item == nullptr || out_handle == nullptr) return false;
    *out_handle = lookup_item(item);
    if (out_handle->slot == 0) {
        *out_handle = register_item(item, contracts::kQolItemHandleOwnerGame);
    }
    return out_handle->slot != 0 && out_handle->generation != 0;
}

bool copy_ref(const InventoryItemRef& source, contracts::QolInventoryItemRef* target) {
    if (!handle_for_item(source.native_item, &target->item)) return false;
    target->kind = static_cast<int32_t>(source.kind);
    target->bag = source.bag;
    target->slot = source.slot;
    target->category = source.category;
    target->count = source.count;
    return true;
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
        {},
    };
    if (!handle_for_item(item, &ref.item)) return false;
    return state->callback(&ref, state->callback_context) != 0;
}

}  // namespace

void* game_inventory_find_item_hook(int32_t category) {
    const contracts::QolFindItemAbiFn backup = g_find_item_backup;
    if (backup == nullptr) return nullptr;
    void* original = backup(category);
    if (original != nullptr) {
        // The pointer remains inside this ABI wrapper and is returned only to the game caller.
        (void)register_item(original, contracts::kQolItemHandleOwnerGame);
        return original;
    }
    if (g_original_only_depth != 0) return nullptr;
    const contracts::QolInventoryExtensionProvider* provider =
        inventory_extension_provider_if_resident();
    if (provider == nullptr || provider->for_each_item == nullptr) return nullptr;
    struct FindContext {
        int32_t category;
        contracts::QolItemHandle item;
    } context{category, contracts::kInvalidItemHandle};
    const QolResult result = provider->for_each_item(
        [](const contracts::QolInventoryExtensionItemRef* ref, void* raw) -> int32_t {
            auto* state = static_cast<FindContext*>(raw);
            if (ref == nullptr || ref->category != state->category || ref->item.slot == 0 ||
                ref->item.generation == 0) return 0;
            state->item = ref->item;
            return 1;
        },
        &context);
    if (!contracts::succeeded(result) || context.item.slot == 0) return nullptr;
    const ItemHandleResolution resolved =
        item_handle_registry().resolve_item(context.item, contracts::kQolItemHandleOwnerAny);
    return resolved.error == ItemHandleError::kNone ? resolved.item : nullptr;
}

QolResult game_inventory_bind_find_item_backup(contracts::QolFindItemAbiFn backup) {
    if (backup == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    g_find_item_backup = backup;
    return contracts::ok();
}

QolResult game_original_find_item_slot(contracts::QolItemHandle handle, int32_t* out_bag,
                                       int32_t* out_slot) {
    if (handle.slot == 0 || handle.generation == 0 || out_bag == nullptr || out_slot == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_inven_find_item_slot == nullptr) return error_result(kQolErrNotReady, kNotReady);
    const ItemHandleResolution resolved =
        item_handle_registry().resolve_item(handle, contracts::kQolItemHandleOwnerAny);
    if (resolved.error == ItemHandleError::kWrongThread) {
        return error_result(kQolErrBlocked, "wrong thread");
    }
    if (resolved.error != ItemHandleError::kNone || resolved.item == nullptr) {
        return error_result(kQolErrNotFound, "stale item handle");
    }
    int8_t encoded = -1;
    if (fn_inven_find_item_slot(resolved.item, &encoded) == 0) {
        return error_result(kQolErrNotFound, kNotFound);
    }
    const uint8_t value = static_cast<uint8_t>(encoded);
    *out_bag = value >> 5;
    *out_slot = value & 0x1F;
    return contracts::ok();
}

QolResult game_item_identity_at(int32_t bag, int32_t slot, contracts::QolItemHandle* out_item) {
    if (out_item == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out_item = contracts::kInvalidItemHandle;
    contracts::QolInventoryItemRef ref{};
    const QolResult result = inventory_item_ref_at(bag, slot, &ref);
    if (!contracts::succeeded(result)) return result;
    *out_item = ref.item;
    return contracts::ok();
}

void game_begin_original_only() { ++g_original_only_depth; }

void game_end_original_only() {
    if (g_original_only_depth != 0) --g_original_only_depth;
}

QolResult game_native_item_at(int32_t bag, int32_t slot, void** out_item) {
    return physical_item_at(bag, slot, out_item);
}

QolResult game_native_item_for_handle(contracts::QolItemHandle handle, void** out_item) {
    if (out_item == nullptr || handle.slot == 0 || handle.generation == 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_item = nullptr;
    const ItemHandleResolution resolved =
        item_handle_registry().resolve_item(handle, contracts::kQolItemHandleOwnerAny);
    if (resolved.error == ItemHandleError::kWrongThread) {
        return error_result(kQolErrBlocked, "wrong thread");
    }
    if (resolved.error != ItemHandleError::kNone || resolved.item == nullptr) {
        return error_result(kQolErrNotFound, "stale item handle");
    }
    *out_item = resolved.item;
    return contracts::ok();
}

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
    if (!copy_ref(source, out)) return error_result(kQolErrNotReady, "item handle unavailable");
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

QolResult set_physical_item(int32_t bag, int32_t slot, void* item) {
    if (bag < 0 || bag >= 6 || slot < 0 || slot >= 16) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (g_inven == nullptr) return error_result(kQolErrNotReady, kNotReady);
    auto* slot_address = reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(g_inven) + static_cast<size_t>(bag) * 0x80 +
        static_cast<size_t>(slot) * sizeof(void*));
    if (!game_memory_accessible(slot_address, sizeof(*slot_address), 'w')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    *slot_address = item;
    return contracts::ok();
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

QolResult detail_item_context(contracts::QolItemDetailContext* out_context) {
    if (out_context == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out_context = {0, -1, -1, contracts::kInvalidItemHandle};
    int32_t desc_type = 0;
    int32_t bag = -1;
    void* control = nullptr;
    QolResult result = detail_context(&desc_type, &bag, &control);
    if (!contracts::succeeded(result)) return result;
    int32_t slot = -1;
    result = cursor_index(control, &slot);
    if (!contracts::succeeded(result)) return result;
    contracts::QolInventoryItemRef item_ref{};
    result = inventory_item_ref_at(bag, slot, &item_ref);
    if (!contracts::succeeded(result)) return result;
    if (item_ref.bag != bag || item_ref.slot != slot || item_ref.item.slot == 0 ||
        item_ref.item.generation == 0) {
        return error_result(kQolErrNotFound, "detail item identity unavailable");
    }
    out_context->desc_type = desc_type;
    out_context->bag = bag;
    out_context->slot = slot;
    out_context->item = item_ref.item;
    return contracts::ok();
}

QolResult bag_size(int32_t bag, int32_t* out_size) {
    if (bag < 0 || bag >= 6 || out_size == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_get_bag_size == nullptr) return error_result(kQolErrNotReady, kNotReady);
    *out_size = fn_get_bag_size(bag);
    return *out_size < 0 ? error_result(kQolErrNotReady, kNotReady) : contracts::ok();
}

QolResult is_empty_bag(int32_t bag, int32_t* out_empty) {
    if (bag < 0 || bag >= 6 || out_empty == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_is_empty_bag == nullptr) return error_result(kQolErrNotReady, kNotReady);
    *out_empty = fn_is_empty_bag(bag) != 0 ? 1 : 0;
    return contracts::ok();
}

QolResult find_save_slot(const void* item, int32_t* out_slot) {
    if (item == nullptr || out_slot == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_inven_find_save_slot == nullptr) return error_result(kQolErrNotReady, kNotReady);
    int8_t slot = -1;
    if (fn_inven_find_save_slot(const_cast<void*>(item), &slot) == 0 || slot < 0) {
        return error_result(kQolErrNotFound, kNotFound);
    }
    *out_slot = slot;
    return contracts::ok();
}

QolResult current_bag(int32_t* out_bag) {
    if (out_bag == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    int32_t desc = 0;
    void* control = nullptr;
    return detail_context(&desc, out_bag, &control);
}

QolResult set_current_bag(int32_t bag) {
    if (bag < 0 || bag >= 7 || g_base == 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    auto* direct = reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA);
    auto** got = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (!game_memory_accessible(direct, sizeof(*direct), 'w')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    *direct = static_cast<uint8_t>(bag);
    if (got != nullptr && game_memory_accessible(got, sizeof(*got), 'r') && *got != nullptr &&
        game_memory_accessible(*got, sizeof(**got), 'w')) {
        **got = static_cast<uint8_t>(bag);
    }
    return contracts::ok();
}

QolResult desc_type(int32_t* out_type) {
    if (out_type == nullptr || g_base == 0) {
        return error_result(out_type == nullptr ? kQolErrInvalidArgument : kQolErrNotReady,
                            out_type == nullptr ? kInvalidArgument : kNotReady);
    }
    auto* value = reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_DESC_TYPE_VMA);
    if (!game_memory_accessible(value, sizeof(*value), 'r')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    *out_type = *value;
    return contracts::ok();
}

QolResult set_desc_type(int32_t type) {
    if (type < 0 || type > UINT8_MAX || g_base == 0) {
        return error_result(type < 0 || type > UINT8_MAX ? kQolErrInvalidArgument : kQolErrNotReady,
                            type < 0 || type > UINT8_MAX ? kInvalidArgument : kNotReady);
    }
    auto* value = reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_DESC_TYPE_VMA);
    if (!game_memory_accessible(value, sizeof(*value), 'w')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    *value = static_cast<uint8_t>(type);
    return contracts::ok();
}

QolResult bag_capacity_word(int32_t bag, uint32_t* out_word) {
    if (bag < 0 || bag >= 6 || out_word == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (g_base == 0) return error_result(kQolErrNotReady, kNotReady);
    auto*** table_slot = reinterpret_cast<void***>(g_base + G_BAG_TABLE_VMA);
    if (!game_memory_accessible(table_slot, sizeof(*table_slot), 'r') || *table_slot == nullptr) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    void* bag_object = (*table_slot)[bag];
    if (bag_object == nullptr) return error_result(kQolErrNotFound, kNotFound);
    auto* word = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(bag_object) + BAG_OBJECT_CAPACITY);
    if (!game_memory_accessible(word, sizeof(*word), 'r')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    *out_word = *word;
    return contracts::ok();
}

QolResult set_bag_capacity_word(int32_t bag, uint32_t word) {
    if (bag < 0 || bag >= 6) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    uint32_t ignored = 0;
    const QolResult ready = bag_capacity_word(bag, &ignored);
    if (!contracts::succeeded(ready)) return ready;
    auto*** table_slot = reinterpret_cast<void***>(g_base + G_BAG_TABLE_VMA);
    void* bag_object = (*table_slot)[bag];
    auto* target = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(bag_object) + BAG_OBJECT_CAPACITY);
    if (!game_memory_accessible(target, sizeof(*target), 'w')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    *target = word;
    return contracts::ok();
}

}  // namespace qol::game

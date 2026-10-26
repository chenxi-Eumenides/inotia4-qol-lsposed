#include "game/ops/item_inventory_state_write.h"

#include <cstring>
#include <initializer_list>
#include <mutex>

#include "contracts/game_cache.h"
#include "game/access/game_access.h"
#include "game/access/inventory.h"
#include "game/access/item_handle_registry.h"
#include "game/adapter_ports/item_count_facade.h"
#include "game/access/item_class.h"
#include "game/read/item_class.h"
#include "game/state/game_state.h"

namespace qol::game::ops {
namespace {

QolResult item_error(contracts::QolItemInventoryStateWriteResult* out, int32_t code,
                     const char* message) {
    if (out == nullptr || message == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid item write result", 24);
    }
    size_t length = std::strlen(message);
    if (length >= sizeof(out->error_message)) length = sizeof(out->error_message) - 1;
    std::memcpy(out->error_message, message, length);
    out->error_message[length] = '\0';
    out->error_message_len = static_cast<uint32_t>(length);
    return contracts::error(code, out->error_message, out->error_message_len);
}

QolResult item_error(contracts::QolItemInventoryStateWriteResult* out,
                     const QolResult& source, const char* fallback) {
    if (out == nullptr || (source.message == nullptr && fallback == nullptr)) {
        return contracts::error(kQolErrInvalidArgument, "invalid item write result", 24);
    }
    const char* message = source.message != nullptr ? source.message : fallback;
    size_t length = source.message != nullptr ? source.message_len : std::strlen(fallback);
    if (length >= sizeof(out->error_message)) length = sizeof(out->error_message) - 1;
    std::memcpy(out->error_message, message, length);
    out->error_message[length] = '\0';
    out->error_message_len = static_cast<uint32_t>(length);
    return contracts::error(source.code, out->error_message, out->error_message_len);
}

QolResult transfer_error(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult handle_error(ItemHandleError error) {
    switch (error) {
        case ItemHandleError::kNone: return contracts::ok();
        case ItemHandleError::kInvalidArgument:
            return transfer_error(kQolErrInvalidArgument, "invalid item handle");
        case ItemHandleError::kWrongThread:
            return transfer_error(kQolErrBlocked, "wrong thread");
        case ItemHandleError::kFull:
            return transfer_error(kQolErrNotReady, "item handle registry full");
        case ItemHandleError::kStale:
            return transfer_error(kQolErrNotFound, "stale item handle");
        case ItemHandleError::kType:
            return transfer_error(kQolErrConflict, "item handle ownership conflict");
        case ItemHandleError::kTransition:
            return transfer_error(kQolErrBlocked, "item handle transition");
    }
    return transfer_error(kQolErrInternal, "item handle registration failed");
}

QolResult resolve_loaded_item(contracts::QolItemHandle handle, void** out_item) {
    if (out_item == nullptr || handle.slot == 0 || handle.generation == 0) {
        return transfer_error(kQolErrInvalidArgument, "invalid item handle");
    }
    const ItemHandleResolution resolution = item_handle_registry().resolve_item(
        handle, contracts::kQolItemHandleOwnerAny);
    if (resolution.error != ItemHandleError::kNone || resolution.item == nullptr) {
        return handle_error(resolution.error);
    }
    *out_item = resolution.item;
    return contracts::ok();
}

QolResult item_stack_count(void* item, uint32_t* out_count) {
    if (item == nullptr || out_count == nullptr) {
        return transfer_error(kQolErrInvalidArgument, "invalid item count arguments");
    }
    const contracts::QolItemCountApi* api = item_count_api_if_resident();
    if (api != nullptr && api->canonical != nullptr) {
        const QolResult result = api->canonical(item, out_count);
        if (contracts::succeeded(result)) return result;
        if (result.code != kQolErrUnsupported) return result;
    }
    if (fn_get_cumulate_count == nullptr) {
        return transfer_error(kQolErrNotReady, "item count unavailable");
    }
    const int count = fn_get_cumulate_count(item);
    if (count <= 0) return transfer_error(kQolErrNotReady, "item count unavailable");
    *out_count = static_cast<uint32_t>(count);
    return contracts::ok();
}

QolResult find_native_item_slot(contracts::QolItemHandle handle, const NativeBagLayout& layout,
                                contracts::QolBagRef* out_ref, int32_t* out_slot) {
    if (out_ref == nullptr || out_slot == nullptr) {
        return transfer_error(kQolErrInvalidArgument, "invalid slot output");
    }
    for (int32_t space_id : {contracts::kQolBagSpaceOriginal, contracts::kQolBagSpaceVariant}) {
        if (space_id == contracts::kQolBagSpaceVariant && layout.bag_limit <= 6) continue;
        int32_t bag_count = 0;
        QolResult result = inventory_bag_count(space_id, &bag_count);
        if (!contracts::succeeded(result)) {
            if (space_id == contracts::kQolBagSpaceVariant && result.code == kQolErrNotReady) continue;
            return result;
        }
        for (int32_t bag = 0; bag < bag_count; ++bag) {
            if (space_id == contracts::kQolBagSpaceOriginal &&
                bag == contracts::kQolTaskBagIndex) continue;
            const contracts::QolBagRef ref{space_id, bag};
            for (int32_t slot = 0; slot < 16; ++slot) {
                contracts::QolInventoryItemRef item_ref{};
                result = inventory_space_item_at(&ref, slot, &item_ref);
                if (result.code == kQolErrNotFound) continue;
                if (!contracts::succeeded(result)) return result;
                if (item_ref.item.slot == handle.slot &&
                    item_ref.item.generation == handle.generation) {
                    *out_ref = ref;
                    *out_slot = slot;
                    return contracts::ok();
                }
            }
        }
    }
    return transfer_error(kQolErrNotFound, "item is not in native inventory");
}

QolResult write_item_canonical_count(void* item, uint32_t count) {
    const contracts::QolItemCountApi* api = item_count_api_if_resident();
    if (api == nullptr || api->read_encoded_field == nullptr ||
        api->field_from_canonical == nullptr || api->write_encoded_field == nullptr) {
        return transfer_error(kQolErrNotReady, "item count writer unavailable");
    }
    uint32_t field = 0;
    QolResult result = api->read_encoded_field(item, &field);
    if (!contracts::succeeded(result)) return result;
    uint32_t updated = 0;
    result = api->field_from_canonical(field, count, 1, &updated);
    if (!contracts::succeeded(result)) return result;
    return api->write_encoded_field(item, updated);
}

QolResult write_item_count_in_mode(void* item, uint32_t count, int32_t extended) {
    const contracts::QolItemCountApi* api = item_count_api_if_resident();
    if (api == nullptr || api->read_encoded_field == nullptr ||
        api->field_from_canonical == nullptr || api->write_encoded_field == nullptr) {
        return transfer_error(kQolErrNotReady, "item count writer unavailable");
    }
    uint32_t field = 0;
    QolResult result = api->read_encoded_field(item, &field);
    if (!contracts::succeeded(result)) return result;
    uint32_t updated = 0;
    result = api->field_from_canonical(field, count, extended, &updated);
    if (!contracts::succeeded(result)) return result;
    return api->write_encoded_field(item, updated);
}

// 原版物理袋数量（0..5，含任务袋）；provider 认领的袋号由 provider 自管，本快照只覆盖物理袋。
constexpr int kOriginalInventoryBagCount = 6;

struct BeforeInventory {
    void* items[96]{};
};

void capture_before_item(void* item, int bag, int slot, void* raw) {
    if (item == nullptr || bag < 0 || bag >= kOriginalInventoryBagCount || slot < 0 ||
        slot >= 16) {
        return;
    }
    static_cast<BeforeInventory*>(raw)->items[bag * 16 + slot] = item;
}

struct GainedContext {
    BeforeInventory* before;
    contracts::QolItemInventoryStateWriteResult* out;
};

bool capture_gained_item(void* item, int bag, int slot, void* raw) {
    auto* context = static_cast<GainedContext*>(raw);
    if (item == nullptr || bag < 0 || bag >= kOriginalInventoryBagCount || slot < 0 ||
        slot >= 16 ||
        context->out->gained_count >= contracts::kQolItemInventoryStateGainedCapacity ||
        fn_get_bit == nullptr || fn_get_cumulate_count == nullptr) {
        return false;
    }
    void* old = context->before->items[bag * 16 + slot];
    int count = fn_get_cumulate_count(item);
    if (old == item) count -= fn_get_cumulate_count(old);
    else if (old != nullptr) return false;
    if (count <= 0) return false;
    auto& gained = context->out->gained[context->out->gained_count++];
    const uint16_t flags = *reinterpret_cast<uint16_t*>(
        reinterpret_cast<uint8_t*>(item) + I_TYPE);
    gained.bag = bag;
    gained.slot = slot;
    gained.category = static_cast<int32_t>(fn_get_bit(flags, 15, 6));
    gained.count = count;
    return false;
}

std::mutex g_shop_buy_mtx;

}  // namespace

QolResult load_item(const uint8_t* payload, uint32_t size,
                    contracts::QolItemHandle* out_item) {
    if (out_item == nullptr) {
        return transfer_error(kQolErrInvalidArgument, "invalid serialized item output");
    }
    *out_item = contracts::kInvalidItemHandle;
    if (payload == nullptr || size < 19 || size > 255 ||
        static_cast<uint32_t>(payload[0]) + 1u != size) {
        return transfer_error(kQolErrInvalidArgument, "invalid serialized item");
    }
    if (!game_in_world() || fn_save_load_item == nullptr || fn_itempool_free == nullptr) {
        return transfer_error(kQolErrNotReady, "item loading not ready");
    }
    void* item = nullptr;
    int32_t consumed = 0;
    const int loaded = fn_save_load_item(payload, &item, &consumed);
    if (loaded == 0 || item == nullptr || consumed != static_cast<int32_t>(size)) {
        if (item != nullptr) fn_itempool_free(item);
        return transfer_error(kQolErrInternal, "item load failed");
    }
    ItemHandleError registry_error = ItemHandleError::kNone;
    contracts::QolItemHandle handle =
        register_item(item, kQolLoadedItemHandleOwner, &registry_error);
    if (handle.slot == 0 || handle.generation == 0) {
        // 钩子或既有所有权可能已为同一对象登记句柄；回查后沿用，不重复注册。
        const contracts::QolItemHandle existing = lookup_item(item, nullptr);
        if (existing.slot == 0 || existing.generation == 0) {
            fn_itempool_free(item);
            return handle_error(registry_error);
        }
        handle = existing;
    }
    *out_item = handle;
    return contracts::ok();
}

QolResult free_loaded_item(contracts::QolItemHandle handle) {
    if (fn_itempool_free == nullptr) {
        return transfer_error(kQolErrNotReady, "item free not ready");
    }
    void* item = nullptr;
    QolResult result = resolve_loaded_item(handle, &item);
    if (!contracts::succeeded(result)) return result;
    if (!game_memory_accessible(item, I_TYPE + sizeof(uint16_t), 'r')) {
        invalidate_item(handle);
        return transfer_error(kQolErrNotFound, "stale item handle");
    }
    if (!invalidate_item(handle)) {
        return transfer_error(kQolErrNotFound, "stale item handle");
    }
    fn_itempool_free(item);
    return contracts::ok();
}

QolResult place_item(const contracts::QolBagRef* ref, int32_t slot,
                     contracts::QolItemHandle handle,
                     contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) return transfer_error(kQolErrInvalidArgument, "invalid item write result");
    *out = {};
    if (ref == nullptr ||
        (ref->space_id != contracts::kQolBagSpaceOriginal &&
         ref->space_id != contracts::kQolBagSpaceVariant) ||
        ref->bag < 0 || slot < -1 || slot >= 16 ||
        (ref->space_id == contracts::kQolBagSpaceOriginal &&
         ref->bag == contracts::kQolTaskBagIndex)) {
        return item_error(out, kQolErrInvalidArgument, "invalid native bag slot");
    }
    if (!game_in_world()) return item_error(out, kQolErrNotReady, "not in game");
    void* item = nullptr;
    QolResult result = resolve_loaded_item(handle, &item);
    if (!contracts::succeeded(result)) {
        return item_error(out, result, "invalid loaded item");
    }
    const int category = item_class_category(item);
    if (category < 0) return item_error(out, kQolErrNotReady, "item category unavailable");
    uint32_t count = 0;
    result = item_stack_count(item, &count);
    if (!contracts::succeeded(result)) {
        return item_error(out, result, "item count unavailable");
    }

    contracts::QolBagInfo info{};
    result = inventory_bag_info(ref, &info);
    if (!contracts::succeeded(result)) {
        return item_error(out, result, "native bag unavailable");
    }
    const int32_t slot_limit = info.capacity < 16 ? info.capacity : 16;
    if (slot_limit <= 0 || (slot >= 0 && slot >= slot_limit)) {
        return item_error(out, kQolErrInvalidArgument, "invalid native bag slot");
    }
    int32_t target_slot = slot;
    bool already_placed = false;
    const int32_t begin = slot == -1 ? 0 : slot;
    const int32_t end = slot == -1 ? slot_limit : slot + 1;
    bool found_empty = false;
    for (int32_t candidate = begin; candidate < end; ++candidate) {
        contracts::QolInventoryItemRef target{};
        result = inventory_space_item_at(ref, candidate, &target);
        if (result.code == kQolErrNotFound) {
            target_slot = candidate;
            found_empty = true;
            break;
        }
        if (!contracts::succeeded(result)) {
            return item_error(out, result, "native slot unavailable");
        }
        if (target.item.slot == handle.slot && target.item.generation == handle.generation) {
            target_slot = candidate;
            already_placed = true;
            found_empty = true;
            break;
        }
        if (slot >= 0) return item_error(out, kQolErrConflict, "native slot occupied");
    }
    if (!found_empty) return item_error(out, kQolErrBlocked, "native bag full");

    if (!rebind_item(item, contracts::kQolItemHandleOwnerGame)) {
        return item_error(out, kQolErrNotFound, "stale item handle");
    }
    if (!already_placed) {
        result = set_native_space_item(ref, target_slot, item);
        if (!contracts::succeeded(result)) {
            (void)rebind_item(item, kQolLoadedItemHandleOwner);
            return item_error(out, result, "native slot write failed");
        }
    }
    auto& gained = out->gained[0];
    gained.bag = ref->bag;
    gained.slot = target_slot;
    gained.category = category;
    gained.count = static_cast<int32_t>(count);
    out->gained_count = 1;
    out->flags |= contracts::kQolItemInventoryStateHasGained;
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult remove_item(contracts::QolItemHandle handle, int32_t count,
                      contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) return transfer_error(kQolErrInvalidArgument, "invalid item write result");
    *out = {};
    if (handle.slot == 0 || handle.generation == 0 || count <= 0) {
        return item_error(out, kQolErrInvalidArgument, "invalid item removal arguments");
    }
    const ItemHandleResolution resolution =
        item_handle_registry().resolve_item(handle, contracts::kQolItemHandleOwnerAny);
    if (resolution.error != ItemHandleError::kNone || resolution.item == nullptr) {
        const QolResult error = handle_error(resolution.error);
        return item_error(out, error, "stale item handle");
    }
    if (!game_memory_accessible(resolution.item, I_TYPE + sizeof(uint16_t), 'r')) {
        return item_error(out, kQolErrNotFound, "stale item handle");
    }
    if (!game_in_world()) return item_error(out, kQolErrNotReady, "not in game");
    const NativeBagLayout layout = native_bag_layout();
    if (layout.slots == nullptr) return item_error(out, kQolErrNotReady, "inventory not ready");
    contracts::QolBagRef ref{};
    int32_t slot = -1;
    QolResult result = find_native_item_slot(handle, layout, &ref, &slot);
    if (!contracts::succeeded(result)) {
        return item_error(out, result, "item not found");
    }
    uint32_t stack_count = 0;
    result = item_stack_count(resolution.item, &stack_count);
    if (!contracts::succeeded(result)) {
        return item_error(out, result, "item count unavailable");
    }
    if (static_cast<uint32_t>(count) > stack_count) {
        return item_error(out, kQolErrInvalidArgument, "item removal exceeds stack count");
    }
    if (static_cast<uint32_t>(count) < stack_count) {
        result = write_item_canonical_count(resolution.item,
                                            stack_count - static_cast<uint32_t>(count));
        if (!contracts::succeeded(result)) {
            return item_error(out, result, "item count update failed");
        }
    } else {
        if (fn_itempool_free == nullptr) {
            return item_error(out, kQolErrNotReady, "item free not ready");
        }
        contracts::QolItemHandle checkpoint{};
        if (!begin_item_invalidation(resolution.item, contracts::kQolItemHandleOwnerAny,
                                     &checkpoint)) {
            return item_error(out, kQolErrBlocked, "item handle transition");
        }
        result = set_native_space_item(&ref, slot, nullptr);
        if (!contracts::succeeded(result)) {
            (void)finish_item_invalidation(checkpoint, false);
            return item_error(out, result, "native slot removal failed");
        }
        if (!finish_item_invalidation(checkpoint, true)) {
            (void)set_native_space_item(&ref, slot, resolution.item);
            (void)finish_item_invalidation(checkpoint, false);
            return item_error(out, kQolErrInternal, "item handle removal failed");
        }
        fn_itempool_free(resolution.item);
    }
    out->value = count;
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult use_item(int32_t bag, int32_t slot,
                   contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid item write result", 25);
    }
    *out = {};
    if (!game_in_world()) return item_error(out, kQolErrNotReady, "not in game");
    if (bag == 5) return item_error(out, kQolErrBlocked, "task bag excluded");

    InventoryItemRef item_ref;
    if (!inventory_item_ref_at(bag, slot, &item_ref)) {
        return item_error(out, kQolErrNotFound, "slot empty");
    }
    if (const char* screen = ui_blocked()) {
        char message[sizeof(out->error_message)]{};
        constexpr char prefix[] = "ui occupied: ";
        std::memcpy(message, prefix, sizeof(prefix) - 1);
        const size_t remaining = sizeof(message) - sizeof(prefix);
        size_t screen_length = std::strlen(screen);
        if (screen_length > remaining) screen_length = remaining;
        std::memcpy(message + sizeof(prefix) - 1, screen, screen_length);
        return item_error(out, kQolErrBlocked, message);
    }
    if (fn_get_bit == nullptr) return item_error(out, kQolErrUnsupported, "symbol not resolved");

    void* item = item_ref.native_item;
    const int category = item_ref.category;
    const bool sealed_or_box =
        (fn_is_sealed != nullptr && fn_is_sealed(category)) ||
        (fn_is_item_box != nullptr && fn_is_item_box(category)) ||
        (fn_is_dice != nullptr && fn_is_dice(category));
    const bool mercenary_seal =
        fn_is_mercenary_seal != nullptr && fn_is_mercenary_seal(category);
    if (!sealed_or_box && !mercenary_seal && fn_is_use != nullptr && !fn_is_use(category)) {
        return item_error(out, kQolErrUnsupported, "item not usable");
    }

    void* leader = member_or_null(0);
    if (fn_is_dice != nullptr && fn_is_dice(category)) {
        if (fn_status_dice_roll == nullptr || fn_get_stat_base == nullptr) {
            return item_error(out, kQolErrUnsupported, "symbol not resolved");
        }
        if (leader == nullptr) return item_error(out, kQolErrNotFound, "no leader");
        uint8_t* flag = *reinterpret_cast<uint8_t**>(g_base + G_STATUSDICE_FLAG_GOT_VMA);
        if (flag != nullptr && (*flag & 1u) != 0) {
            return item_error(out, kQolErrConflict, "dice result pending, accept or reject first");
        }
        const int8_t char_index = *reinterpret_cast<int8_t*>(
            reinterpret_cast<uint8_t*>(leader) + C_CLASS);
        const int dice_type = category - 0x34;
        if (char_index < 0 || char_index > 5) return item_error(out, kQolErrInvalidArgument, "bad char");
        if (dice_type < 0 || dice_type > 4) {
            return item_error(out, kQolErrInvalidArgument, "bad dice type");
        }
        if (!fn_status_dice_roll(char_index, dice_type)) {
            return item_error(out, kQolErrInternal, "dice roll failed");
        }
        int8_t* pending = *reinterpret_cast<int8_t**>(g_base + G_STATUSDICE_PENDING_GOT_VMA);
        if (pending == nullptr) return item_error(out, kQolErrInternal, "dice result missing");
        if (fn_consume_item != nullptr) fn_consume_item(item);
        if (flag != nullptr) *flag |= 1u;
        frame_cache_force_refresh();
        return contracts::ok();
    }

    if (fn_is_sealed != nullptr && fn_is_sealed(category) && fn_release_sealed != nullptr) {
        BeforeInventory before{};
        ::for_each_bag_slot(
            [](void* candidate, int candidate_bag, int candidate_slot, void* raw) -> bool {
                capture_before_item(candidate, candidate_bag, candidate_slot, raw);
                return false;
            },
            &before);
        if (fn_release_sealed(category) == 0) {
            return item_error(out, kQolErrInternal, "release sealed failed");
        }
        if (fn_consume_item != nullptr) fn_consume_item(item);
        out->flags |= contracts::kQolItemInventoryStateHasGained;
        GainedContext context{&before, out};
        ::for_each_bag_slot(&capture_gained_item, &context);
        return contracts::ok();
    }

    if (fn_is_item_box != nullptr && fn_is_item_box(category) && fn_open_item_box != nullptr) {
        BeforeInventory before{};
        ::for_each_bag_slot(
            [](void* candidate, int candidate_bag, int candidate_slot, void* raw) -> bool {
                capture_before_item(candidate, candidate_bag, candidate_slot, raw);
                return false;
            },
            &before);
        if (fn_open_item_box(category) == 0) {
            return item_error(out, kQolErrInternal, "open box failed");
        }
        if (fn_consume_item != nullptr) fn_consume_item(item);
        out->flags |= contracts::kQolItemInventoryStateHasGained;
        GainedContext context{&before, out};
        ::for_each_bag_slot(&capture_gained_item, &context);
        return contracts::ok();
    }

    if (mercenary_seal) {
        if (fn_save_is_ok == nullptr || fn_mercenary_is_empty_manager_slot == nullptr ||
            fn_mercenary_make_mercenary == nullptr) {
            return item_error(out, kQolErrUnsupported, "symbol not resolved");
        }
        if (fn_save_is_ok() == 0) return item_error(out, kQolErrBlocked, "save busy");
        if (fn_mercenary_is_empty_manager_slot() == 0) {
            return item_error(out, kQolErrBlocked, "mercenary slots full");
        }
        if (fn_mercenary_make_mercenary(item) == nullptr) {
            return item_error(out, kQolErrInternal, "make mercenary failed");
        }
        frame_cache_force_refresh();
        return contracts::ok();
    }

    if (leader == nullptr) return item_error(out, kQolErrNotFound, "no leader");
    if (fn_char_use_item_ex == nullptr) {
        return item_error(out, kQolErrUnsupported, "symbol not resolved");
    }
    const int used = fn_char_use_item_ex(leader, item, 0);
    if (used != 0 && tutorial_state() == 6) tutorial_cancel();
    if (used != 0) {
        frame_cache_force_refresh();
        return contracts::ok();
    }
    return item_error(out, kQolErrBlocked, "on cooldown");
}

QolResult shop_buy(int32_t slot, contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) return contracts::error(kQolErrInvalidArgument, "invalid item write result", 25);
    *out = {};
    if (!game_in_world()) return item_error(out, kQolErrNotReady, "not in game");
    if (fn_item_get_buy_price == nullptr || fn_get_money == nullptr || fn_minus_money == nullptr ||
        fn_inven_find_save_slot == nullptr || fn_inven_save_item == nullptr ||
        fn_dealsystem_clear_item == nullptr) {
        return item_error(out, kQolErrUnsupported, "symbol not resolved");
    }
    if (slot < 0 || slot >= 48) return item_error(out, kQolErrInvalidArgument, "bad slot");

    std::lock_guard<std::mutex> lock(g_shop_buy_mtx);
    uint8_t* sale_list = reinterpret_cast<uint8_t*>(
        *reinterpret_cast<void**>(g_base + G_DEALSYSTEM_SALE_LIST_VMA));
    if (sale_list == nullptr) return item_error(out, kQolErrNotFound, "no shop");
    uint8_t* slot_ptr = sale_list + slot * 16;
    const uint64_t flags = *reinterpret_cast<uint64_t*>(slot_ptr);
    if ((flags & 1u) != 0) return item_error(out, kQolErrNotFound, "item sold out");
    void* item = *reinterpret_cast<void**>(slot_ptr + 8);
    if (item == nullptr) return item_error(out, kQolErrNotFound, "item not found");
    const int price = fn_item_get_buy_price(item);
    if (fn_get_money() < price) return item_error(out, kQolErrBlocked, "not enough money");
    int8_t save_slot = -1;
    if (!fn_inven_find_save_slot(item, &save_slot)) {
        return item_error(out, kQolErrBlocked, "inventory full");
    }
    if (!fn_inven_save_item(item, nullptr)) return item_error(out, kQolErrInternal, "buy failed");
    if (!fn_minus_money(price)) return item_error(out, kQolErrInternal, "money update failed");
    fn_dealsystem_clear_item(item);
    out->value = price;
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult move_item(int32_t bag, int32_t slot, int32_t count, int32_t to_bag,
                    int32_t to_slot,
                    contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid item write result", 25);
    }
    *out = {};
    if (!game_in_world()) return item_error(out, kQolErrNotReady, "not in game");
    if (bag == 5 || to_bag == 5) return item_error(out, kQolErrBlocked, "task bag excluded");
    const NativeBagLayout layout = native_bag_layout();
    if (bag < 0 || to_bag < 0 || bag >= layout.bag_limit || to_bag >= layout.bag_limit) {
        return item_error(out, kQolErrNotFound, "native bag not found");
    }
    if (fn_inven_move_item == nullptr) {
        return item_error(out, kQolErrUnsupported, "symbol not resolved");
    }
    InventoryItemRef item_ref;
    if (!inventory_item_ref_at(bag, slot, &item_ref)) {
        return item_error(out, kQolErrNotFound, "slot empty");
    }
    if (count <= 0) return item_error(out, kQolErrInvalidArgument, "bad count");
    if (to_slot < 0 || to_slot >= 16 || to_bag >= native_bag_layout().bag_limit) {
        return item_error(out, kQolErrInvalidArgument, "bad target");
    }
    if (bag == to_bag && slot == to_slot) {
        return item_error(out, kQolErrInvalidArgument, "same slot");
    }
    if (fn_inven_move_item(item_ref.native_item, count, to_bag, to_slot) == 0) {
        return item_error(out, kQolErrInternal, "move failed");
    }
    return contracts::ok();
}

QolResult move_handle(contracts::QolItemHandle handle, int32_t count,
                      const contracts::QolBagRef* ref, int32_t slot,
                      contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) return transfer_error(kQolErrInvalidArgument, "invalid item write result");
    *out = {};
    if (!game_in_world()) return item_error(out, kQolErrNotReady, "not in game");
    if (ref == nullptr ||
        (ref->space_id != contracts::kQolBagSpaceOriginal &&
         ref->space_id != contracts::kQolBagSpaceVariant) ||
        ref->bag < 0 || slot < -1 || slot >= 16 ||
        (ref->space_id == contracts::kQolBagSpaceOriginal &&
         ref->bag == contracts::kQolTaskBagIndex)) {
        return item_error(out, kQolErrInvalidArgument, "invalid native bag slot");
    }
    if (count <= 0) return item_error(out, kQolErrInvalidArgument, "bad count");
    void* item = nullptr;
    QolResult result = resolve_loaded_item(handle, &item);
    if (!contracts::succeeded(result)) return item_error(out, result, "invalid loaded item");
    if (!game_memory_accessible(item, I_TYPE + sizeof(uint16_t), 'r')) {
        return item_error(out, kQolErrNotFound, "stale item handle");
    }
    const int32_t category = item_class_category(item);
    if (category < 0) return item_error(out, kQolErrNotReady, "item category unavailable");
    uint32_t source_count = 0;
    result = item_stack_count(item, &source_count);
    if (!contracts::succeeded(result)) return item_error(out, result, "item count unavailable");
    const contracts::QolItemCountEncoding encoding = item_count_encoding(item);
    // SAVE_LoadItem 重建的单件装备/特殊物品可能没有数量位段，canonical 返回 0；
    // 非堆叠单件仍按 1 处理，否则跨介质移动会在放置前被误判为数量超限。
    if (source_count == 0 && encoding != contracts::QolItemCountEncoding::kEncoded &&
        count == 1) {
        source_count = 1;
    }
    if (static_cast<uint32_t>(count) > source_count) {
        return item_error(out, kQolErrInvalidArgument, "move count exceeds stack count");
    }

    uint32_t stack_limit = 1;
    const contracts::QolItemCountApi* count_api = item_count_api_if_resident();
    int32_t extended = 0;
    if (encoding == contracts::QolItemCountEncoding::kEncoded) {
        if (count_api == nullptr || count_api->read_extended_mode == nullptr ||
            count_api->view_limit == nullptr) {
            return item_error(out, kQolErrNotReady, "item count writer unavailable");
        }
        uint8_t extended_mode = 0;
        result = count_api->read_extended_mode(&extended_mode);
        if (result.code == kQolErrNotReady) {
            // An unapplied stack-limit group uses the legacy 99-item encoding.
            extended = 0;
        } else if (!contracts::succeeded(result)) {
            return item_error(out, result, "item count mode unavailable");
        } else {
            extended = extended_mode != 0 ? 1 : 0;
        }
        result = count_api->view_limit(extended, &stack_limit);
        if (!contracts::succeeded(result)) {
            return item_error(out, result, "item stack limit unavailable");
        }
        if (static_cast<uint32_t>(count) > stack_limit) {
            return item_error(out, kQolErrBlocked, "move count exceeds native stack limit");
        }
        if (static_cast<uint32_t>(count) != source_count) {
            result = write_item_count_in_mode(item, static_cast<uint32_t>(count), extended);
            if (!contracts::succeeded(result)) {
                return item_error(out, result, "item count update failed");
            }
        }
    } else if (encoding == contracts::QolItemCountEncoding::kUnknown) {
        // 未知编码只能证明“单件”是安全的；多件仍拒绝，避免把可堆叠物拆坏。
        if (count != 1) {
            return item_error(out, kQolErrUnsupported, "item stack encoding unavailable");
        }
    } else if (encoding != contracts::QolItemCountEncoding::kNotEncoded || count != 1) {
        return item_error(out, kQolErrUnsupported, "item stack encoding unavailable");
    }

    contracts::QolBagInfo bag_info{};
    result = inventory_bag_info(ref, &bag_info);
    if (!contracts::succeeded(result)) return item_error(out, result, "native bag unavailable");
    const int32_t slot_limit = bag_info.capacity < 16 ? bag_info.capacity : 16;
    if (slot_limit <= 0 || (slot >= 0 && slot >= slot_limit)) {
        return item_error(out, kQolErrInvalidArgument, "invalid native bag slot");
    }

    int32_t target_slot = slot;
    bool target_empty = false;
    bool target_occupied = false;
    contracts::QolInventoryItemRef target_item_ref{};
    const int32_t first = slot == -1 ? 0 : slot;
    const int32_t last = slot == -1 ? slot_limit : slot + 1;
    for (int32_t candidate = first; candidate < last; ++candidate) {
        contracts::QolInventoryItemRef target{};
        result = inventory_space_item_at(ref, candidate, &target);
        if (result.code == kQolErrNotFound) {
            target_slot = candidate;
            target_empty = true;
            break;
        }
        if (!contracts::succeeded(result)) {
            return item_error(out, result, "native slot unavailable");
        }
        if (slot == -1) continue;
        if (target.category != category) {
            return item_error(out, kQolErrConflict, "native slot contains another item");
        }
        target_item_ref = target;
        target_occupied = true;
        break;
    }
    if (target_slot < 0 || (slot == -1 && !target_empty)) {
        return item_error(out, kQolErrBlocked, "native bag full");
    }

    const NativeBagLayout layout = native_bag_layout();
    const int32_t native_bag =
        ref->bag + (ref->space_id == contracts::kQolBagSpaceVariant
                        ? contracts::kQolOriginalBagCount
                        : 0);
    if (layout.slots == nullptr || native_bag >= layout.bag_limit) {
        return item_error(out, kQolErrNotReady, "native bag unavailable");
    }
    auto** target_address = reinterpret_cast<void**>(
        layout.slots + static_cast<size_t>(native_bag) * 0x80 +
        static_cast<size_t>(target_slot) * sizeof(void*));
    if (!game_memory_accessible(target_address, sizeof(*target_address), 'r')) {
        return item_error(out, kQolErrNotReady, "native slot unavailable");
    }

    if (target_occupied) {
        if (*target_address == nullptr) {
            return item_error(out, kQolErrConflict, "native target changed");
        }
        const ItemHandleResolution target_resolution = item_handle_registry().resolve_item(
            target_item_ref.item, contracts::kQolItemHandleOwnerGame);
        if (target_resolution.error != ItemHandleError::kNone) {
            return item_error(out, handle_error(target_resolution.error),
                              "native target item unavailable");
        }
        if (target_resolution.item == nullptr || target_resolution.item != *target_address) {
            return item_error(out, kQolErrNotFound, "native target item unavailable");
        }
        void* target_item = target_resolution.item;
        if (item_class_category(target_item) != category ||
            item_count_encoding(target_item) != contracts::QolItemCountEncoding::kEncoded) {
            return item_error(out, kQolErrConflict, "native target is not stackable");
        }
        uint32_t target_count = 0;
        result = item_stack_count(target_item, &target_count);
        if (!contracts::succeeded(result)) {
            return item_error(out, result, "native target count unavailable");
        }
        if (target_count > stack_limit - static_cast<uint32_t>(count)) {
            return item_error(out, kQolErrBlocked, "native target stack full");
        }
        if (count_api == nullptr) {
            return item_error(out, kQolErrNotReady, "item count writer unavailable");
        }
        result = write_item_count_in_mode(target_item,
                                          target_count + static_cast<uint32_t>(count), extended);
        if (!contracts::succeeded(result)) {
            return item_error(out, result, "native target count update failed");
        }
        result = free_loaded_item(handle);
        if (!contracts::succeeded(result)) {
            const QolResult restored =
                write_item_count_in_mode(target_item, target_count, extended);
            if (!contracts::succeeded(restored)) {
                return item_error(out, kQolErrInternal, "native merge rollback failed");
            }
            return item_error(out, result, "loaded item cleanup failed");
        }

        auto& gained = out->gained[0];
        gained.bag = ref->bag;
        gained.slot = target_slot;
        gained.category = category;
        gained.count = count;
        out->gained_count = 1;
        out->flags |= contracts::kQolItemInventoryStateHasGained;
        frame_cache_force_refresh();
        return contracts::ok();
    }

    if (!target_empty || *target_address != nullptr) {
        return item_error(out, kQolErrConflict, "native target changed");
    }
    if (!rebind_item(item, contracts::kQolItemHandleOwnerGame)) {
        return item_error(out, kQolErrNotFound, "stale item handle");
    }
    result = set_native_space_item(ref, target_slot, item);
    if (!contracts::succeeded(result)) {
        (void)rebind_item(item, kQolLoadedItemHandleOwner);
        return item_error(out, result, "native slot write failed");
    }

    auto& gained = out->gained[0];
    gained.bag = ref->bag;
    gained.slot = target_slot;
    gained.category = category;
    gained.count = count;
    out->gained_count = 1;
    out->flags |= contracts::kQolItemInventoryStateHasGained;
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult add_item(int32_t category, int32_t count, int32_t socket_total,
                   int32_t enhance_remaining, int32_t rarity_group,
                   const contracts::QolItemCountApi* item_count,
                   contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid item write result", 25);
    }
    *out = {};
    if (!game_in_world()) return item_error(out, kQolErrNotReady, "not in game");
    if (count <= 0) return item_error(out, kQolErrInvalidArgument, "bad count");
    if (fn_create_item == nullptr || fn_inven_save_item == nullptr ||
        fn_inven_find_save_slot == nullptr || fn_itempool_free == nullptr) {
        return item_error(out, kQolErrUnsupported, "symbol not resolved");
    }

    // CreateItem does not apply MakeItem's search-table validation.
    void* item = fn_create_item(category, 0, 0, 0);
    if (item == nullptr) return item_error(out, kQolErrInternal, "create item failed");

    socket_total = socket_total < 0 ? 0 : (socket_total > 15 ? 15 : socket_total);
    enhance_remaining = enhance_remaining < 0 ? 0 : (enhance_remaining > 15 ? 15 : enhance_remaining);
    // Preserve filled sockets and all enchant fields other than remaining durability.
    uint8_t* socket = reinterpret_cast<uint8_t*>(item) + I_SOCKET;
    *socket = static_cast<uint8_t>((*socket & 0x0F) | ((socket_total & 0x0F) << 4));
    uint16_t* enchant = reinterpret_cast<uint16_t*>(
        reinterpret_cast<uint8_t*>(item) + I_ENCHANT);
    *enchant = static_cast<uint16_t>((*enchant & ~(0x0F << 2)) |
                                     ((enhance_remaining & 0x0F) << 2));

    // Preserve the category and unrelated type flags while setting the requested raw grade.
    static constexpr uint8_t kRarityRawForGroup[5] = {0, 4, 7, 10, 11};
    rarity_group = rarity_group < -1 ? -1 : (rarity_group > 4 ? 4 : rarity_group);
    if (rarity_group >= 0) {
        const uint8_t raw = kRarityRawForGroup[rarity_group];
        uint16_t* type = reinterpret_cast<uint16_t*>(
            reinterpret_cast<uint8_t*>(item) + I_TYPE);
        *type = static_cast<uint16_t>((*type & ~(0x0F << 2)) | ((raw & 0x0F) << 2));
    }

    const contracts::QolItemCountEncoding encoding = item_count_encoding(item);
    if (count > 1 && encoding != contracts::QolItemCountEncoding::kEncoded) {
        invalidate_item(item);
        fn_itempool_free(item);
        return item_error(out, encoding == contracts::QolItemCountEncoding::kUnknown
                                   ? kQolErrNotReady
                                   : kQolErrUnsupported,
                          encoding == contracts::QolItemCountEncoding::kUnknown
                              ? "item category unavailable"
                              : "item not stackable");
    }
    if (count > 1) {
        if (item_count == nullptr || item_count->write_effective == nullptr) {
            invalidate_item(item);
            fn_itempool_free(item);
            return item_error(out, kQolErrNotReady, "item count provider unavailable");
        }
        const QolResult write_result = item_count->write_effective(item, static_cast<uint32_t>(count));
        if (!contracts::succeeded(write_result)) {
            if (write_result.code != kQolErrNotReady && write_result.code != kQolErrUnsupported) {
                invalidate_item(item);
                fn_itempool_free(item);
                return item_error(out, kQolErrInternal, "item count write failed");
            }
            if (item_count->field_from_canonical == nullptr) {
                invalidate_item(item);
                fn_itempool_free(item);
                return item_error(out, kQolErrNotReady, "item count provider not ready");
            }
            const uint32_t field = *reinterpret_cast<uint32_t*>(
                reinterpret_cast<uint8_t*>(item) + I_COUNT);
            uint32_t fallback_field = 0;
            // The provider owns count-field encoding, including the non-expanded fallback.
            const QolResult fallback_result = item_count->field_from_canonical(
                field, static_cast<uint32_t>(count), 0, &fallback_field);
            if (!contracts::succeeded(fallback_result)) {
                invalidate_item(item);
                fn_itempool_free(item);
                return item_error(out, kQolErrInternal, "item count fallback failed");
            }
            *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT) = fallback_field;
        }
    }

    int8_t save_slot = -1;
    if (!fn_inven_find_save_slot(item, &save_slot)) {
        invalidate_item(item);
        fn_itempool_free(item);
        return item_error(out, kQolErrBlocked, "inventory full");
    }
    if (!fn_inven_save_item(item, nullptr)) {
        invalidate_item(item);
        fn_itempool_free(item);
        return item_error(out, kQolErrInternal, "save item failed");
    }
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult equip_handle(int32_t role, contracts::QolItemHandle handle, int32_t equip_slot,
                       contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) return transfer_error(kQolErrInvalidArgument, "invalid item write result");
    *out = {};
    if (!game_in_world()) return item_error(out, kQolErrNotReady, "not in game");
    if (role < 0 || role > 2 || equip_slot < 0 || equip_slot >= C_EQUIP_SLOTS) {
        return item_error(out, kQolErrInvalidArgument, "bad role or equip slot");
    }
    void* character = member_or_null(role);
    if (character == nullptr) return item_error(out, kQolErrNotFound, "role not found");
    if (fn_can_equip == nullptr || fn_find_equip_slot == nullptr || fn_equip_item == nullptr ||
        fn_set_equip_item == nullptr) {
        return item_error(out, kQolErrUnsupported, "symbol not resolved");
    }
    void* item = nullptr;
    QolResult result = resolve_loaded_item(handle, &item);
    if (!contracts::succeeded(result)) return item_error(out, result, "invalid loaded item");
    if (!game_memory_accessible(item, I_TYPE + sizeof(uint16_t), 'r')) {
        return item_error(out, kQolErrNotFound, "stale item handle");
    }
    auto** equipped_slot = reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(character) + C_EQUIP + static_cast<size_t>(equip_slot) * 8);
    if (!game_memory_accessible(equipped_slot, sizeof(*equipped_slot), 'r')) {
        return item_error(out, kQolErrNotReady, "equip slot unavailable");
    }
    if (*equipped_slot != nullptr) return item_error(out, kQolErrConflict, "equip slot occupied");
    if (!fn_can_equip(character, item) || fn_find_equip_slot(character, item) != equip_slot) {
        return item_error(out, kQolErrBlocked, "cannot equip");
    }

    const int equipped = fn_equip_item(character, item);
    if (equipped == 0) {
        if (*equipped_slot == item) fn_set_equip_item(character, equip_slot, nullptr);
        return item_error(out, kQolErrInternal, "equip failed");
    }
    if (*equipped_slot != item) {
        for (int32_t slot = 0; slot < C_EQUIP_SLOTS; ++slot) {
            auto** candidate = reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(character) + C_EQUIP + static_cast<size_t>(slot) * 8);
            if (game_memory_accessible(candidate, sizeof(*candidate), 'r') && *candidate == item) {
                fn_set_equip_item(character, slot, nullptr);
            }
        }
        return item_error(out, kQolErrInternal, "equip slot mismatch");
    }
    if (!rebind_item(item, contracts::kQolItemHandleOwnerGame)) {
        fn_set_equip_item(character, equip_slot, nullptr);
        return item_error(out, kQolErrInternal, "item ownership transfer failed");
    }
    out->value = equip_slot;
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult unequip_handle(int32_t role, int32_t equip_slot,
                         contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) return transfer_error(kQolErrInvalidArgument, "invalid item write result");
    *out = {};
    if (!game_in_world()) return item_error(out, kQolErrNotReady, "not in game");
    if (role < 0 || role > 2 || equip_slot < 0 || equip_slot >= C_EQUIP_SLOTS) {
        return item_error(out, kQolErrInvalidArgument, "bad role or equip slot");
    }
    void* character = member_or_null(role);
    if (character == nullptr) return item_error(out, kQolErrNotFound, "role not found");
    if (fn_set_equip_item == nullptr) {
        return item_error(out, kQolErrUnsupported, "symbol not resolved");
    }
    auto** equipped_slot = reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(character) + C_EQUIP + static_cast<size_t>(equip_slot) * 8);
    if (!game_memory_accessible(equipped_slot, sizeof(*equipped_slot), 'r')) {
        return item_error(out, kQolErrNotReady, "equip slot unavailable");
    }
    void* item = *equipped_slot;
    if (item == nullptr) return item_error(out, kQolErrNotFound, "equip slot empty");
    if (!game_memory_accessible(item, I_TYPE + sizeof(uint16_t), 'r')) {
        return item_error(out, kQolErrNotFound, "stale equip item");
    }
    if (fn_can_unequip != nullptr && fn_can_unequip(character, item) == 0) {
        return item_error(out, kQolErrBlocked, "cannot unequip");
    }

    contracts::QolItemHandle handle = lookup_item(item);
    if (handle.slot == 0 || handle.generation == 0) {
        ItemHandleError registry_error = ItemHandleError::kNone;
        handle = register_item(item, contracts::kQolItemHandleOwnerGame, &registry_error);
        if (handle.slot == 0 || handle.generation == 0) {
            return item_error(out, handle_error(registry_error), "item handle registration failed");
        }
    }

    fn_set_equip_item(character, equip_slot, nullptr);
    if (*equipped_slot != nullptr) {
        fn_set_equip_item(character, equip_slot, item);
        return item_error(out, kQolErrInternal, "unequip failed");
    }
    if (!rebind_item(item, kQolLoadedItemHandleOwner)) {
        fn_set_equip_item(character, equip_slot, item);
        return item_error(out, kQolErrInternal, "item ownership transfer failed");
    }
    out->value = equip_slot;
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult put_jewel_handles(int32_t role, contracts::QolItemHandle equip_handle,
                            contracts::QolItemHandle jewel_handle,
                            contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) return transfer_error(kQolErrInvalidArgument, "invalid item write result");
    *out = {};
    if (!game_in_world()) return item_error(out, kQolErrNotReady, "not in game");
    if (role < 0 || role > 2) return item_error(out, kQolErrInvalidArgument, "bad role");
    void* character = member_or_null(role);
    if (character == nullptr) return item_error(out, kQolErrNotFound, "role not found");
    if (fn_put_jewel == nullptr || fn_is_jewel == nullptr || fn_itempool_free == nullptr) {
        return item_error(out, kQolErrUnsupported, "symbol not resolved");
    }

    const ItemHandleResolution equip_resolution =
        item_handle_registry().resolve_item(equip_handle, contracts::kQolItemHandleOwnerAny);
    if (equip_resolution.error != ItemHandleError::kNone || equip_resolution.item == nullptr) {
        return item_error(out, handle_error(equip_resolution.error), "stale equip item handle");
    }
    void* equipped = equip_resolution.item;
    if (!game_memory_accessible(equipped, I_TYPE + sizeof(uint16_t), 'r')) {
        return item_error(out, kQolErrNotFound, "stale equip item handle");
    }
    bool is_equipped = false;
    for (int32_t slot = 0; slot < C_EQUIP_SLOTS; ++slot) {
        auto** equipped_slot = reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(character) + C_EQUIP + static_cast<size_t>(slot) * 8);
        if (!game_memory_accessible(equipped_slot, sizeof(*equipped_slot), 'r')) {
            return item_error(out, kQolErrNotReady, "equip slot unavailable");
        }
        if (*equipped_slot == equipped) {
            is_equipped = true;
            break;
        }
    }
    if (!is_equipped) return item_error(out, kQolErrConflict, "equip item is not equipped");

    void* jewel = nullptr;
    QolResult result = resolve_loaded_item(jewel_handle, &jewel);
    if (!contracts::succeeded(result)) return item_error(out, result, "invalid loaded jewel");
    if (!game_memory_accessible(jewel, I_TYPE + sizeof(uint16_t), 'r')) {
        return item_error(out, kQolErrNotFound, "stale jewel handle");
    }
    const int32_t jewel_category = item_class_category(jewel);
    if (jewel_category < 0) return item_error(out, kQolErrNotReady, "jewel category unavailable");
    if (fn_is_jewel(jewel_category) == 0) {
        return item_error(out, kQolErrInvalidArgument, "not jewel");
    }
    uint32_t jewel_count = 0;
    result = item_stack_count(jewel, &jewel_count);
    if (!contracts::succeeded(result)) return item_error(out, result, "jewel count unavailable");
    if (jewel_count != 1) {
        return item_error(out, kQolErrUnsupported, "stacked jewel unsupported");
    }

    contracts::QolItemHandle checkpoint{};
    if (!begin_item_invalidation(jewel, kQolLoadedItemHandleOwner, &checkpoint)) {
        return item_error(out, kQolErrBlocked, "item handle transition");
    }
    const int put_result = fn_put_jewel(equipped, jewel);
    if (put_result != 0) {
        (void)finish_item_invalidation(checkpoint, false);
        return item_error(out, kQolErrInternal, put_result == 2 ? "no socket" : "not jewel");
    }
    if (!finish_item_invalidation(checkpoint, true)) {
        return item_error(out, kQolErrInternal, "jewel handle consumption failed");
    }
    fn_itempool_free(jewel);
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult equip_item(int32_t role, int32_t bag, int32_t slot,
                     contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid item write result", 25);
    }
    *out = {};
    if (!game_in_world()) return item_error(out, kQolErrNotReady, "not in game");
    void* character = member_or_null(role);
    if (character == nullptr) return item_error(out, kQolErrNotFound, "role not found");
    if (g_inven == nullptr || fn_equip_item == nullptr || fn_can_equip == nullptr) {
        return item_error(out, kQolErrUnsupported, "symbol not resolved");
    }
    if (bag < 0 || bag >= 5 || slot < 0 || slot >= 16) {
        return item_error(out, kQolErrInvalidArgument, "bad slot");
    }
    auto* bag_slots = reinterpret_cast<uint8_t*>(g_inven) + bag * 0x80;
    void* item = *reinterpret_cast<void**>(bag_slots + slot * 8);
    if (item == nullptr) return item_error(out, kQolErrNotFound, "slot empty");
    if (!fn_can_equip(character, item)) {
        return item_error(out, kQolErrBlocked, "cannot equip");
    }
    if (fn_find_equip_slot == nullptr || fn_equip_item_from_inven_to_slot == nullptr) {
        return item_error(out, kQolErrUnsupported, "symbol not resolved");
    }
    const int target = fn_find_equip_slot(character, item);
    if (target < 0 || target >= C_EQUIP_SLOTS) {
        return item_error(out, kQolErrBlocked, "cannot equip");
    }
    // This entry remains hooked by Stage4; it owns the original exchange and special_equip chain.
    const int result = fn_equip_item_from_inven_to_slot(character, bag, slot, target);
    if (result == 0) return item_error(out, kQolErrInternal, "equip failed");
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult unequip_item(int32_t role, int32_t equip_slot,
                       contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid item write result", 25);
    }
    *out = {};
    if (!game_in_world()) return item_error(out, kQolErrNotReady, "not in game");
    void* character = member_or_null(role);
    if (character == nullptr) return item_error(out, kQolErrNotFound, "role not found");
    if (fn_unequip == nullptr) return item_error(out, kQolErrUnsupported, "symbol not resolved");
    if (equip_slot < 0 || equip_slot >= C_EQUIP_SLOTS) {
        return item_error(out, kQolErrInvalidArgument, "bad slot");
    }
    void* item = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(character) + C_EQUIP + equip_slot * 8);
    if (item == nullptr) return item_error(out, kQolErrNotFound, "equip slot empty");
    if (fn_can_unequip != nullptr && fn_can_unequip(character, item) == 0) {
        return item_error(out, kQolErrBlocked, "cannot unequip");
    }
    const int result = fn_unequip(character, equip_slot);
    if (result == 0) return item_error(out, kQolErrInternal, "unequip failed");
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult put_jewel(int32_t role, int32_t bag, int32_t slot, int32_t equip_slot,
                    contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid item write result", 25);
    }
    *out = {};
    if (!game_in_world()) return item_error(out, kQolErrNotReady, "not in game");
    void* character = member_or_null(role);
    if (character == nullptr) return item_error(out, kQolErrNotFound, "role not found");
    if (fn_put_jewel == nullptr || fn_is_jewel == nullptr || fn_consume_item == nullptr) {
        return item_error(out, kQolErrUnsupported, "symbol not resolved");
    }
    if (bag < 0 || bag >= 5 || slot < 0 || slot >= 16) {
        return item_error(out, kQolErrInvalidArgument, "bad slot");
    }
    if (equip_slot < 0 || equip_slot >= C_EQUIP_SLOTS) {
        return item_error(out, kQolErrInvalidArgument, "bad equip slot");
    }
    InventoryItemRef jewel_ref{};
    if (!inventory_item_ref_at(bag, slot, &jewel_ref)) {
        return item_error(out, kQolErrNotFound, "jewel not found");
    }
    void* jewel = jewel_ref.native_item;
    void* equipped = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(character) + C_EQUIP + equip_slot * 8);
    if (equipped == nullptr) return item_error(out, kQolErrNotFound, "equip slot empty");
    const int result = fn_put_jewel(equipped, jewel);
    if (result != 0) {
        return item_error(out, kQolErrInternal, result == 2 ? "no socket" : "not jewel");
    }
    // ConsumeItem is the original ApplyStuff consumption primitive and decrements one item.
    fn_consume_item(jewel);
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult enchant(int32_t role, int32_t bag, int32_t slot, int32_t equip_slot,
                  contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid item write result", 25);
    }
    *out = {};
    if (!game_in_world()) return item_error(out, kQolErrNotReady, "not in game");
    void* character = member_or_null(role);
    if (character == nullptr) return item_error(out, kQolErrNotFound, "role not found");
    if (fn_enchant_item == nullptr || fn_is_enchant_scroll == nullptr ||
        fn_consume_item == nullptr || fn_get_bit == nullptr) {
        return item_error(out, kQolErrUnsupported, "symbol not resolved");
    }
    if (bag < 0 || bag >= 5 || slot < 0 || slot >= 16) {
        return item_error(out, kQolErrInvalidArgument, "bad slot");
    }
    if (equip_slot < 0 || equip_slot >= C_EQUIP_SLOTS) {
        return item_error(out, kQolErrInvalidArgument, "bad equip slot");
    }
    void* equipped = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(character) + C_EQUIP + equip_slot * 8);
    if (equipped == nullptr) return item_error(out, kQolErrNotFound, "equip slot empty");
    void* scroll = inventory_item_at(bag, slot);
    if (scroll == nullptr) return item_error(out, kQolErrNotFound, "scroll not found");
    const uint16_t flags = *reinterpret_cast<uint16_t*>(
        reinterpret_cast<uint8_t*>(scroll) + I_TYPE);
    const int scroll_category = fn_get_bit(flags, 15, 6);
    if (!fn_is_enchant_scroll(scroll_category)) {
        return item_error(out, kQolErrInvalidArgument, "not enchant scroll");
    }
    const int result = fn_enchant_item(equipped, scroll_category);
    if (result != 0) {
        return item_error(out, kQolErrInternal, result == 7 ? "not enchantable" : "enchant failed");
    }
    fn_consume_item(scroll);
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult remove_item(int32_t category, contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid item write result", 25);
    }
    *out = {};
    if (!game_in_world()) return item_error(out, kQolErrNotReady, "not in game");
    if (fn_remove_item == nullptr || fn_get_bit == nullptr) {
        return item_error(out, kQolErrUnsupported, "symbol not resolved");
    }
    InventoryItemRef ref;
    if (!find_inventory_item_ref(category, &ref)) {
        return item_error(out, kQolErrNotFound, "item not found");
    }
    if (ref.native_item == nullptr) {
        return item_error(out, kQolErrNotFound, "native item unavailable");
    }
    if (ref.bag == 5) return item_error(out, kQolErrBlocked, "task bag excluded");
    contracts::QolItemHandle checkpoint{};
    const bool suspended = begin_item_invalidation(
        ref.native_item, contracts::kQolItemHandleOwnerAny, &checkpoint);
    const int result = fn_remove_item(ref.native_item);
    if (suspended) finish_item_invalidation(checkpoint, result != 0);
    else if (result) invalidate_item(ref.native_item);
    if (!result) return item_error(out, kQolErrNotFound, "item not found");
    frame_cache_force_refresh();
    return contracts::ok();
}

}  // namespace qol::game::ops

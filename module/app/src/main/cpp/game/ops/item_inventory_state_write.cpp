#include "game/ops/item_inventory_state_write.h"

#include <cstring>
#include <mutex>

#include "contracts/game_cache.h"
#include "contracts/extension_bag_port.h"
#include "game/access/game_access.h"
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

struct BeforeInventory {
    void* items[96]{};
};

void capture_before_item(void* item, int bag, int slot, void* raw) {
    if (item == nullptr || bag < 0 || bag >= 6 || slot < 0 || slot >= 16) return;
    static_cast<BeforeInventory*>(raw)->items[bag * 16 + slot] = item;
}

struct GainedContext {
    BeforeInventory* before;
    contracts::QolItemInventoryStateWriteResult* out;
};

bool capture_gained_item(void* item, int bag, int slot, void* raw) {
    auto* context = static_cast<GainedContext*>(raw);
    if (item == nullptr || bag < 0 || bag >= 6 || slot < 0 || slot >= 16 ||
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
    if (item_ref.kind == InventoryItemKind::kExtension) {
        return item_error(out, kQolErrUnsupported, "extension item use requires provider");
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
        const auto* eb = extension_bag_capability();
        if (eb != nullptr && eb->sync_projected_slot != nullptr) eb->sync_projected_slot(bag, slot);
        frame_cache_force_refresh();
        return contracts::ok();
    }

    if (fn_is_sealed != nullptr && fn_is_sealed(category) && fn_release_sealed != nullptr) {
        BeforeInventory before{};
        for_each_bag_slot(
            [](void* candidate, int candidate_bag, int candidate_slot, void* raw) -> bool {
                capture_before_item(candidate, candidate_bag, candidate_slot, raw);
                return false;
            },
            &before);
        if (fn_release_sealed(category) == 0) {
            return item_error(out, kQolErrInternal, "release sealed failed");
        }
        if (fn_consume_item != nullptr) fn_consume_item(item);
        const auto* eb = extension_bag_capability();
        if (eb != nullptr && eb->sync_projected_slot != nullptr) eb->sync_projected_slot(bag, slot);
        out->flags |= contracts::kQolItemInventoryStateHasGained;
        GainedContext context{&before, out};
        for_each_bag_slot(&capture_gained_item, &context);
        return contracts::ok();
    }

    if (fn_is_item_box != nullptr && fn_is_item_box(category) && fn_open_item_box != nullptr) {
        BeforeInventory before{};
        for_each_bag_slot(
            [](void* candidate, int candidate_bag, int candidate_slot, void* raw) -> bool {
                capture_before_item(candidate, candidate_bag, candidate_slot, raw);
                return false;
            },
            &before);
        if (fn_open_item_box(category) == 0) {
            return item_error(out, kQolErrInternal, "open box failed");
        }
        if (fn_consume_item != nullptr) fn_consume_item(item);
        const auto* eb = extension_bag_capability();
        if (eb != nullptr && eb->sync_projected_slot != nullptr) eb->sync_projected_slot(bag, slot);
        out->flags |= contracts::kQolItemInventoryStateHasGained;
        GainedContext context{&before, out};
        for_each_bag_slot(&capture_gained_item, &context);
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
        const auto* eb = extension_bag_capability();
        if (eb != nullptr && eb->sync_projected_slot != nullptr) eb->sync_projected_slot(bag, slot);
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
        const auto* eb = extension_bag_capability();
        if (eb != nullptr && eb->sync_projected_slot != nullptr) eb->sync_projected_slot(bag, slot);
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

    const qol::contracts::ExtensionBagApi* eb = extension_bag_capability();
    const bool extension_bag_requested =
        eb != nullptr && eb->is_logical_bag != nullptr &&
        (eb->is_logical_bag(bag) || eb->is_logical_bag(to_bag));
    if (extension_bag_requested) {
        if (count <= 0) return item_error(out, kQolErrInvalidArgument, "bad count");
        if (to_slot < 0 || to_slot >= 16) return item_error(out, kQolErrInvalidArgument, "bad target");
        if (eb->enabled == nullptr || eb->enabled() == 0) {
            return item_error(out, kQolErrNotReady, "extension bag disabled");
        }
        return item_error(out, kQolErrUnsupported, "extension bag move capability unavailable");
    }
    if (eb != nullptr && eb->module_view_installed != nullptr && eb->module_view_installed()) {
        return item_error(out, kQolErrBlocked, "extension view open; movement disabled (P2)");
    }
    if (fn_inven_move_item == nullptr) {
        return item_error(out, kQolErrUnsupported, "symbol not resolved");
    }
    InventoryItemRef item_ref;
    if (!inventory_item_ref_at(bag, slot, &item_ref)) {
        return item_error(out, kQolErrNotFound, "slot empty");
    }
    if (item_ref.kind == InventoryItemKind::kExtension) {
        return item_error(out, kQolErrUnsupported,
                          "extension item move not supported by original API");
    }
    if (count <= 0) return item_error(out, kQolErrInvalidArgument, "bad count");
    if (to_bag < 0 || to_bag >= 5 || to_slot < 0 || to_slot >= 16) {
        return item_error(out, kQolErrInvalidArgument, "bad target");
    }
    if (bag == to_bag && slot == to_slot) {
        return item_error(out, kQolErrInvalidArgument, "same slot");
    }
    if (fn_inven_move_item(item_ref.native_item, count, to_bag, to_slot) == 0) {
        return item_error(out, kQolErrInternal, "move failed");
    }
    if (eb != nullptr && eb->sync_projected_slot != nullptr) {
        eb->sync_projected_slot(bag, slot);
        eb->sync_projected_slot(to_bag, to_slot);
    }
    return contracts::ok();
}

}  // namespace qol::game::ops

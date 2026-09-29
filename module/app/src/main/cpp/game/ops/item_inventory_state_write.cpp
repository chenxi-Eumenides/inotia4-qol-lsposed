#include "game/ops/item_inventory_state_write.h"

#include <cstring>
#include <mutex>

#include "contracts/game_cache.h"
#include "game/access/game_access.h"
#include "game/adapter_ports/inventory_extension_facade.h"
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

// 中立扩展 provider：每次现取，不缓存跨帧指针。provider 仅在其可用（已注册且未停用）时参与
// 路由；停用后路由等同无 provider，保留原 typed 路径。provider 内部再按自身门禁两态处理。
const contracts::QolInventoryExtensionProvider* extension_provider() {
    return qol::game::inventory_extension_available()
               ? qol::game::inventory_extension_provider_if_resident()
               : nullptr;
}

void sync_projected_slot_if_available(int bag, int slot) {
    const auto* provider = extension_provider();
    if (provider != nullptr && provider->sync_projected_slot != nullptr) {
        provider->sync_projected_slot(bag, slot);
    }
}

void sync_projected_bag_if_available() {
    const auto* provider = extension_provider();
    if (provider != nullptr && provider->sync_projected_bag != nullptr) {
        provider->sync_projected_bag();
    }
}

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
    if (item_ref.kind == InventoryItemKind::kProvider) {
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
        sync_projected_slot_if_available(bag, slot);
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
        sync_projected_slot_if_available(bag, slot);
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
        sync_projected_slot_if_available(bag, slot);
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
        sync_projected_slot_if_available(bag, slot);
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
        sync_projected_slot_if_available(bag, slot);
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

    const auto* provider = extension_provider();
    const bool provider_requested =
        provider != nullptr && provider->owns_bag != nullptr &&
        (provider->owns_bag(bag) != 0 || provider->owns_bag(to_bag) != 0);
    if (provider_requested) {
        if (count <= 0) return item_error(out, kQolErrInvalidArgument, "bad count");
        if (to_slot < 0 || to_slot >= 16) return item_error(out, kQolErrInvalidArgument, "bad target");
        // provider 认领的袋号在 core.item 路由经 provider->move_item 处理；本分支仅覆盖
        // provider 已注册但 move 不可用的直接调用，报文不含扩展袋实现细节。
        return item_error(out, kQolErrUnsupported, "provider move unavailable");
    }
    if (provider != nullptr && provider->module_view_installed != nullptr &&
        provider->module_view_installed() != 0) {
        return item_error(out, kQolErrBlocked, "extension view open; movement disabled (P2)");
    }
    if (fn_inven_move_item == nullptr) {
        return item_error(out, kQolErrUnsupported, "symbol not resolved");
    }
    InventoryItemRef item_ref;
    if (!inventory_item_ref_at(bag, slot, &item_ref)) {
        return item_error(out, kQolErrNotFound, "slot empty");
    }
    if (item_ref.kind == InventoryItemKind::kProvider) {
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
    sync_projected_slot_if_available(bag, slot);
    sync_projected_slot_if_available(to_bag, to_slot);
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
            fn_itempool_free(item);
            return item_error(out, kQolErrNotReady, "item count provider unavailable");
        }
        const QolResult write_result = item_count->write_effective(item, static_cast<uint32_t>(count));
        if (!contracts::succeeded(write_result)) {
            if (write_result.code != kQolErrNotReady && write_result.code != kQolErrUnsupported) {
                fn_itempool_free(item);
                return item_error(out, kQolErrInternal, "item count write failed");
            }
            if (item_count->field_from_canonical == nullptr) {
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
                fn_itempool_free(item);
                return item_error(out, kQolErrInternal, "item count fallback failed");
            }
            *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT) = fallback_field;
        }
    }

    int8_t save_slot = -1;
    if (!fn_inven_find_save_slot(item, &save_slot)) {
        fn_itempool_free(item);
        return item_error(out, kQolErrBlocked, "inventory full");
    }
    if (!fn_inven_save_item(item, nullptr)) {
        fn_itempool_free(item);
        return item_error(out, kQolErrInternal, "save item failed");
    }
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult equip_item(int32_t role, int32_t bag, int32_t slot,
                     const contracts::QolInventoryExtensionProvider* extension_provider_arg,
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
    if (extension_provider_arg != nullptr && extension_provider_arg->sync_projected_bag != nullptr) {
        extension_provider_arg->sync_projected_bag();
    }
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult unequip_item(int32_t role, int32_t equip_slot,
                       const contracts::QolInventoryExtensionProvider* extension_provider_arg,
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
    if (extension_provider_arg != nullptr && extension_provider_arg->sync_projected_bag != nullptr) {
        extension_provider_arg->sync_projected_bag();
    }
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult put_jewel(int32_t role, int32_t bag, int32_t slot, int32_t equip_slot,
                    const contracts::QolInventoryExtensionProvider* extension_provider_arg,
                    contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid item write result", 25);
    }
    *out = {};
    if (!game_in_world()) return item_error(out, kQolErrNotReady, "not in game");
    if (extension_provider_arg != nullptr && extension_provider_arg->owns_bag != nullptr &&
        extension_provider_arg->owns_bag(bag) != 0) {
        if (extension_provider_arg->put_jewel == nullptr) {
            return item_error(out, kQolErrUnsupported, "extension jewel unavailable");
        }
        const QolResult result =
            extension_provider_arg->put_jewel(role, bag, slot, equip_slot);
        if (!contracts::succeeded(result)) {
            return item_error(out, result.code, result.message != nullptr
                                                    ? result.message
                                                    : "extension jewel failed");
        }
        frame_cache_force_refresh();
        return contracts::ok();
    }
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
    void* jewel = inventory_item_at(bag, slot);
    if (jewel == nullptr) return item_error(out, kQolErrNotFound, "jewel not found");
    // provider 命中（物品属于 provider）时按其自带逻辑袋号下发；调用方不推导袋号偏移。
    int32_t provider_bag = -1;
    int32_t provider_slot = -1;
    if (extension_provider_arg != nullptr && extension_provider_arg->identify_native_item != nullptr &&
        extension_provider_arg->identify_native_item(jewel, &provider_bag, &provider_slot) != 0) {
        if (extension_provider_arg->put_jewel == nullptr) {
            return item_error(out, kQolErrUnsupported, "extension jewel unavailable");
        }
        const QolResult result =
            extension_provider_arg->put_jewel(role, provider_bag, provider_slot, equip_slot);
        if (!contracts::succeeded(result)) {
            return item_error(out, result.code, result.message != nullptr
                                                    ? result.message
                                                    : "extension jewel failed");
        }
        frame_cache_force_refresh();
        return contracts::ok();
    }
    void* equipped = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(character) + C_EQUIP + equip_slot * 8);
    if (equipped == nullptr) return item_error(out, kQolErrNotFound, "equip slot empty");
    const int result = fn_put_jewel(equipped, jewel);
    if (result != 0) {
        return item_error(out, kQolErrInternal, result == 2 ? "no socket" : "not jewel");
    }
    // ConsumeItem is the original ApplyStuff consumption primitive and decrements one item.
    fn_consume_item(jewel);
    if (extension_provider_arg != nullptr && extension_provider_arg->sync_projected_slot != nullptr) {
        extension_provider_arg->sync_projected_slot(bag, slot);
    }
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult enchant(int32_t role, int32_t bag, int32_t slot, int32_t equip_slot,
                  const contracts::QolInventoryExtensionProvider* extension_provider_arg,
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
    if (extension_provider_arg != nullptr && extension_provider_arg->sync_projected_slot != nullptr) {
        extension_provider_arg->sync_projected_slot(bag, slot);
    }
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
        return item_error(out, kQolErrNotFound, "extension item unavailable");
    }
    if (ref.kind == InventoryItemKind::kProvider) {
        // 中立 provider：provider 缺失（未注册/停用）时按删除失败处理，不落入原版删除链。
        const auto* provider = extension_provider();
        if (provider == nullptr || provider->remove_native_item == nullptr ||
            provider->remove_native_item(ref.native_item) == 0) {
            return item_error(out, kQolErrInternal, "extension item removal failed");
        }
        sync_projected_slot_if_available(ref.bag, ref.slot);
        frame_cache_force_refresh();
        return contracts::ok();
    }
    if (ref.bag == 5) return item_error(out, kQolErrBlocked, "task bag excluded");
    const int result = fn_remove_item(ref.native_item);
    if (result) sync_projected_slot_if_available(ref.bag, ref.slot);
    if (!result) return item_error(out, kQolErrNotFound, "item not found");
    frame_cache_force_refresh();
    return contracts::ok();
}

}  // namespace qol::game::ops

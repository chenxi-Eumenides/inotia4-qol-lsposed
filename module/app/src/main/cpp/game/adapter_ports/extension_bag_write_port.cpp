#include "game/adapter_ports/extension_bag_write_port.h"

#include <cstring>

#include "game/access/game_access.h"
#include "game/access/item_handle_registry.h"
#include "game/access/inventory.h"
#include "game/access/symbols.h"

namespace qol::game {
namespace {

using QolResult = ::QolResult;
constexpr const char* kInvalidArgument = "invalid argument";
constexpr const char* kNotReady = "not ready";

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult not_ready() { return error_result(kQolErrNotReady, kNotReady); }

QolResult write_extension_save_item_on_empty(void* item, int32_t bag, int32_t* out_result) {
    if (item == nullptr || out_result == nullptr || bag < 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_inven_save_item_on_empty == nullptr) return not_ready();
    *out_result = fn_inven_save_item_on_empty(item, bag);
    return contracts::ok();
}

QolResult write_extension_save_item(void* item, void* param, int32_t* out_result) {
    if (item == nullptr || out_result == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_inven_save_item == nullptr) return not_ready();
    *out_result = fn_inven_save_item(item, param);
    return contracts::ok();
}

QolResult write_extension_serialize_item(uint8_t* payload, int32_t payload_capacity, void* item,
                                         int32_t* out_size) {
    if (payload == nullptr || payload_capacity <= 0 || item == nullptr || out_size == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_save_save_item == nullptr) return not_ready();
    *out_size = fn_save_save_item(payload, item);
    if (*out_size < 0 || *out_size > payload_capacity) {
        return error_result(kQolErrBlocked, "serialized item exceeds buffer");
    }
    return contracts::ok();
}

QolResult write_extension_load_item(const uint8_t* payload, int32_t payload_size, void** out_item,
                                    int32_t* out_consumed) {
    if (payload == nullptr || payload_size <= 0 || out_item == nullptr || out_consumed == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_item = nullptr;
    *out_consumed = 0;
    if (fn_save_load_item == nullptr) return not_ready();
    const int result = fn_save_load_item(payload, out_item, out_consumed);
    return result != 0 && *out_item != nullptr ? contracts::ok()
                                                 : error_result(kQolErrInternal, "load item failed");
}

QolResult write_extension_create_item(int32_t category, int32_t arg1, int32_t arg2, int32_t arg3,
                                      void** out_item) {
    if (category < 0 || out_item == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_item = nullptr;
    if (fn_create_item == nullptr) return not_ready();
    *out_item = fn_create_item(category, arg1, arg2, arg3);
    return *out_item == nullptr ? not_ready() : contracts::ok();
}

QolResult write_extension_free_item(void* item) {
    if (item == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (fn_itempool_free == nullptr) return not_ready();
    invalidate_item(item);
    fn_itempool_free(item);
    return contracts::ok();
}

QolResult write_extension_remove_item_direct(int32_t bag, int32_t slot, int32_t* out_result) {
    if (bag < 0 || bag >= 6 || slot < 0 || slot >= 16 || out_result == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_remove_item_direct == nullptr) return not_ready();
    *out_result = fn_remove_item_direct(bag, slot);
    return contracts::ok();
}

QolResult write_extension_move_item(void* item, int32_t count, int32_t bag, int32_t slot,
                                    int32_t* out_result) {
    if (item == nullptr || count < 0 || bag < 0 || slot < 0 || out_result == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_inven_move_item == nullptr) return not_ready();
    *out_result = fn_inven_move_item(item, count, bag, slot);
    return contracts::ok();
}

QolResult write_extension_set_physical_item(int32_t bag, int32_t slot, void* item) {
    return qol::game::set_physical_item(bag, slot, item);
}

QolResult write_extension_equip_from_inventory(void* character, int32_t bag, int32_t slot,
                                               int32_t equip_slot, int32_t* out_result) {
    if (character == nullptr || bag < 0 || slot < 0 || equip_slot < 0 || out_result == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_equip_item_from_inven_to_slot == nullptr) return not_ready();
    *out_result = fn_equip_item_from_inven_to_slot(character, bag, slot, equip_slot);
    return contracts::ok();
}

QolResult write_extension_apply_stuff(void* target_item, void* material) {
    if (target_item == nullptr || material == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_ui_equip_apply_stuff == nullptr) return not_ready();
    fn_ui_equip_apply_stuff(target_item, material);
    return contracts::ok();
}

QolResult write_extension_use_item(void* character, void* item, int32_t arg, int32_t* out_result) {
    if (character == nullptr || item == nullptr || out_result == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_char_use_item_ex == nullptr) return not_ready();
    *out_result = fn_char_use_item_ex(character, item, arg);
    return contracts::ok();
}

QolResult write_extension_make_mercenary(void* item, void** out_mercenary) {
    if (item == nullptr || out_mercenary == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_mercenary = nullptr;
    if (fn_mercenary_make_mercenary == nullptr) return not_ready();
    *out_mercenary = fn_mercenary_make_mercenary(item);
    return *out_mercenary == nullptr ? error_result(kQolErrInternal, "make mercenary failed")
                                      : contracts::ok();
}

QolResult write_extension_add_money(int64_t amount, int32_t* out_result) {
    if (amount < 0 || out_result == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_add_money == nullptr) return not_ready();
    *out_result = fn_add_money(amount);
    return contracts::ok();
}

QolResult write_extension_minus_money(int64_t amount, int32_t* out_result) {
    if (amount < 0 || out_result == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_minus_money == nullptr) return not_ready();
    *out_result = fn_minus_money(amount);
    return contracts::ok();
}

QolResult write_extension_put_jewel(void* equip_item, void* jewel_item, int32_t* out_result) {
    if (equip_item == nullptr || jewel_item == nullptr || out_result == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_put_jewel == nullptr) return not_ready();
    *out_result = fn_put_jewel(equip_item, jewel_item);
    return contracts::ok();
}

QolResult write_extension_void_call(void (*fn)()) {
    if (fn == nullptr) return not_ready();
    fn();
    return contracts::ok();
}

QolResult write_extension_refresh_item_area() {
    return write_extension_void_call(fn_ui_equip_refresh_item_area);
}
QolResult write_extension_refresh_bag_area() {
    return write_extension_void_call(fn_ui_equip_refresh_bag_area);
}
QolResult write_extension_refresh_store_inventory() {
    return write_extension_void_call(fn_ui_store_refresh_inven_bag);
}
QolResult write_extension_refresh_store_item() {
    return write_extension_void_call(fn_ui_store_refresh_inven_item);
}
QolResult write_extension_refresh_mix_item() {
    return write_extension_void_call(fn_ui_mix_refresh_inven_item);
}
QolResult write_extension_update_character_equip() {
    return write_extension_void_call(fn_ui_equip_update_char_equip);
}
QolResult write_extension_description_set_off() {
    return write_extension_void_call(fn_ui_desc_set_off);
}
QolResult write_extension_play_switch_sound() {
    if (fn_sound_system_play == nullptr || g_snd_fx == nullptr) return not_ready();
    fn_sound_system_play(0x11);
    return contracts::ok();
}
QolResult write_extension_make_item_description(void* control) {
    if (control == nullptr || fn_ui_equip_make_desc == nullptr) return not_ready();
    fn_ui_equip_make_desc(control, nullptr);
    return contracts::ok();
}
QolResult write_extension_current_bag(int32_t* out_bag) {
    return qol::game::current_bag(out_bag);
}
QolResult write_extension_set_current_bag(int32_t bag) {
    return qol::game::set_current_bag(bag);
}
QolResult write_extension_desc_type(int32_t* out_type) {
    return qol::game::desc_type(out_type);
}
QolResult write_extension_set_desc_type(int32_t type) {
    return qol::game::set_desc_type(type);
}
QolResult write_extension_bag_capacity_word(int32_t bag, uint32_t* out_word) {
    return qol::game::bag_capacity_word(bag, out_word);
}
QolResult write_extension_set_bag_capacity_word(int32_t bag, uint32_t word) {
    return qol::game::set_bag_capacity_word(bag, word);
}
QolResult write_extension_draw_original_bag() {
    if (g_base == 0) return not_ready();
    const uintptr_t offset = fn_resolve("F_UIEQUIP_DRAW_INVEN_BAG_VMA",
                                        F_UIEQUIP_DRAW_INVEN_BAG_VMA);
    if (offset == 0) return not_ready();
    using DrawFn = void (*)();
    reinterpret_cast<DrawFn>(g_base + offset)();
    return contracts::ok();
}
QolResult write_extension_draw_original_store_bag_group() {
    if (g_base == 0) return not_ready();
    const uintptr_t offset = fn_resolve("F_UISTORE_DRAW_INVEN_BAG_GROUP_VMA",
                                        F_UISTORE_DRAW_INVEN_BAG_GROUP_VMA);
    if (offset == 0) return not_ready();
    using DrawFn = void (*)();
    reinterpret_cast<DrawFn>(g_base + offset)();
    return contracts::ok();
}
QolResult write_extension_draw_original_mix_bag_group() {
    if (g_base == 0) return not_ready();
    const uintptr_t offset = fn_resolve("F_UIMIX_DRAW_INVEN_BAG_GROUP_VMA",
                                        F_UIMIX_DRAW_INVEN_BAG_GROUP_VMA);
    if (offset == 0) return not_ready();
    using DrawFn = void (*)();
    reinterpret_cast<DrawFn>(g_base + offset)();
    return contracts::ok();
}
QolResult write_extension_create_item_count_popup(int32_t type, void* item, int32_t input_count,
                                                  int32_t total_count, void* callback) {
    if (g_base == 0 || item == nullptr || callback == nullptr) return not_ready();
    const uintptr_t offset = fn_resolve("F_UIINPUT_ITEM_COUNT_CREATE_VMA",
                                        F_UIINPUT_ITEM_COUNT_CREATE_VMA);
    if (offset == 0) return not_ready();
    using CreateFn = void (*)(int32_t, void*, int32_t, int32_t, void*);
    reinterpret_cast<CreateFn>(g_base + offset)(type, item, input_count, total_count, callback);
    return contracts::ok();
}
void* write_extension_sell_ok_input_item_count() {
    if (g_base == 0) return nullptr;
    const uintptr_t offset = fn_resolve("F_UISTORE_SELL_OK_INPUT_ITEM_COUNT_VMA",
                                        F_UISTORE_SELL_OK_INPUT_ITEM_COUNT_VMA);
    return offset == 0 ? nullptr : reinterpret_cast<void*>(g_base + offset);
}
QolResult write_extension_make_store_item_description(void* control, void* desc_control) {
    if (control == nullptr || g_base == 0) return not_ready();
    const uintptr_t offset = fn_resolve("F_UISTORE_MAKE_DESC_VMA", F_UISTORE_MAKE_DESC_VMA);
    if (offset == 0) return not_ready();
    using MakeDescFn = void (*)(void*, void*);
    reinterpret_cast<MakeDescFn>(g_base + offset)(control, desc_control);
    return contracts::ok();
}
QolResult write_extension_draw_original_items() {
    if (g_base == 0) return not_ready();
    const uintptr_t offset = fn_resolve("F_UIEQUIP_DRAW_INVEN_ITEM_VMA",
                                        F_UIEQUIP_DRAW_INVEN_ITEM_VMA);
    if (offset == 0) return not_ready();
    using DrawFn = void (*)();
    reinterpret_cast<DrawFn>(g_base + offset)();
    return contracts::ok();
}

QolResult write_extension_create_confirmation(int32_t variant, uint32_t price, void* ok_wrapper,
                                              void* cancel_wrapper, void** out_original_ok,
                                              void** out_original_cancel) {
    if ((variant != 0 && variant != 1) || ok_wrapper == nullptr || cancel_wrapper == nullptr ||
        out_original_ok == nullptr || out_original_cancel == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_original_ok = nullptr;
    *out_original_cancel = nullptr;
    if (fn_popup_create_yesno_from_textdata == nullptr || g_base == 0) return not_ready();
    const char* symbol = variant != 0 ? "F_UIEQUIP_OK_DESTROY_ITEM_VMA"
                                      : "F_UISTORE_SELL_ITEM_VMA";
    const uintptr_t fallback = variant != 0 ? F_UIEQUIP_OK_DESTROY_ITEM_VMA
                                            : F_UISTORE_SELL_ITEM_VMA;
    const uintptr_t original = g_base + fn_resolve(symbol, fallback);
    if (original == g_base) return not_ready();
    using PopupCallback = void (*)(void*);
    fn_popup_create_yesno_from_textdata(
        9, 0, 2, reinterpret_cast<void*>(original), nullptr,
        reinterpret_cast<void*>(static_cast<uintptr_t>(price)));
    auto* ok_slot = reinterpret_cast<PopupCallback*>(g_base + G_POPUP_FPOK_VMA);
    auto* cancel_slot = reinterpret_cast<PopupCallback*>(g_base + G_POPUP_FPCANCEL_VMA);
    *out_original_ok = reinterpret_cast<void*>(*ok_slot);
    *out_original_cancel = reinterpret_cast<void*>(*cancel_slot);
    *ok_slot = reinterpret_cast<PopupCallback>(ok_wrapper);
    *cancel_slot = reinterpret_cast<PopupCallback>(cancel_wrapper);
    return contracts::ok();
}

QolResult write_extension_restore_confirmation(int32_t variant, void* ok_wrapper,
                                               void* cancel_wrapper, void* original_ok,
                                               void* original_cancel) {
    if ((variant != 0 && variant != 1) || ok_wrapper == nullptr || cancel_wrapper == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (g_base == 0) return not_ready();
    using PopupCallback = void (*)(void*);
    auto* ok_slot = reinterpret_cast<PopupCallback*>(g_base + G_POPUP_FPOK_VMA);
    auto* cancel_slot = reinterpret_cast<PopupCallback*>(g_base + G_POPUP_FPCANCEL_VMA);
    if (*ok_slot == reinterpret_cast<PopupCallback>(ok_wrapper)) {
        *ok_slot = reinterpret_cast<PopupCallback>(original_ok);
    }
    if (*cancel_slot == reinterpret_cast<PopupCallback>(cancel_wrapper)) {
        *cancel_slot = reinterpret_cast<PopupCallback>(original_cancel);
    }
    return contracts::ok();
}

QolResult write_extension_original_bag_item(int32_t bag, void** out_item) {
    if (bag < 0 || bag >= 6 || out_item == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (g_base == 0) return not_ready();
    auto* table = reinterpret_cast<void**>(g_base + G_BAG_TABLE_VMA);
    if (table == nullptr) return not_ready();
    *out_item = table[bag];
    return contracts::ok();
}

QolResult write_extension_set_original_bag_item(int32_t bag, void* item) {
    if (bag < 0 || bag >= 6) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (g_base == 0) return not_ready();
    auto* table = reinterpret_cast<void**>(g_base + G_BAG_TABLE_VMA);
    if (table == nullptr) return not_ready();
    table[bag] = item;
    return contracts::ok();
}

const contracts::QolExtensionBagWritePort kExtensionBagWritePort{
    contracts::kQolPortExtensionBagWriteVersion,
    {&write_extension_save_item_on_empty, &write_extension_save_item,
     &write_extension_serialize_item, &write_extension_load_item, &write_extension_create_item,
     &write_extension_free_item, &write_extension_remove_item_direct, &write_extension_move_item,
     &write_extension_set_physical_item},
    {&write_extension_equip_from_inventory, &write_extension_apply_stuff,
     &write_extension_use_item, &write_extension_make_mercenary, &write_extension_add_money,
     &write_extension_minus_money, &write_extension_put_jewel},
    {&write_extension_refresh_item_area, &write_extension_refresh_bag_area,
     &write_extension_refresh_store_inventory, &write_extension_refresh_store_item,
     &write_extension_refresh_mix_item, &write_extension_update_character_equip,
     &write_extension_description_set_off, &write_extension_play_switch_sound,
     &write_extension_make_item_description, &write_extension_current_bag,
     &write_extension_set_current_bag, &write_extension_desc_type, &write_extension_set_desc_type,
     &write_extension_bag_capacity_word, &write_extension_set_bag_capacity_word,
     &write_extension_draw_original_bag, &write_extension_draw_original_items,
     &write_extension_create_confirmation, &write_extension_restore_confirmation,
     &write_extension_original_bag_item, &write_extension_set_original_bag_item,
     &write_extension_draw_original_store_bag_group, &write_extension_draw_original_mix_bag_group,
     &write_extension_create_item_count_popup, &write_extension_sell_ok_input_item_count,
     &write_extension_make_store_item_description},
};

}  // namespace

const contracts::QolExtensionBagWritePort& extension_bag_write_port() {
    return kExtensionBagWritePort;
}

}  // namespace qol::game

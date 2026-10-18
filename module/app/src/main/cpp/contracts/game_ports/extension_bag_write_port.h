#pragma once

#include <cstdint>

#include "../result.h"
#include "ports.h"
#include "../version.h"

namespace qol::contracts {

// QolResult reports whether the adapter accepted the arguments and could invoke the game API.
// For functions with an out_result parameter, that output carries the game's int result
// independently: 0 means game-level success and non-zero means game-level failure. A game-level
// failure therefore still returns a successful QolResult when the adapter call was executable.
struct QolExtensionBagInventoryWriteApi {
    QolResult (*save_item_on_empty)(void* item, int32_t bag, int32_t* out_result);
    QolResult (*save_item)(void* item, void* param, int32_t* out_result);
    QolResult (*serialize_item)(uint8_t* payload, int32_t payload_capacity, void* item,
                                int32_t* out_size);
    QolResult (*load_item)(const uint8_t* payload, int32_t payload_size, void** out_item,
                           int32_t* out_consumed);
    QolResult (*create_item)(int32_t category, int32_t arg1, int32_t arg2, int32_t arg3,
                             void** out_item);
    QolResult (*free_item)(void* item);
    QolResult (*remove_item_direct)(int32_t bag, int32_t slot, int32_t* out_result);
    QolResult (*move_item)(void* item, int32_t count, int32_t bag, int32_t slot,
                           int32_t* out_result);
    QolResult (*set_physical_item)(int32_t bag, int32_t slot, void* item);
};

struct QolExtensionBagEquipmentWriteApi {
    QolResult (*equip_from_inventory)(void* character, int32_t bag, int32_t slot,
                                      int32_t equip_slot, int32_t* out_result);
    QolResult (*apply_stuff)(void* target_item, void* material);
    QolResult (*use_item)(void* character, void* item, int32_t arg, int32_t* out_result);
    QolResult (*make_mercenary)(void* item, void** out_mercenary);
    QolResult (*add_money)(int64_t amount, int32_t* out_result);
    QolResult (*minus_money)(int64_t amount, int32_t* out_result);
    QolResult (*put_jewel)(void* equip_item, void* jewel_item, int32_t* out_result);
};

struct QolExtensionBagUiWriteApi {
    QolResult (*refresh_item_area)();
    QolResult (*refresh_bag_area)();
    QolResult (*refresh_store_inventory)();
    QolResult (*refresh_store_item)();
    QolResult (*refresh_mix_item)();
    QolResult (*update_character_equip)();
    QolResult (*description_set_off)();
    QolResult (*play_switch_sound)();
    QolResult (*make_item_description)(void* control);
    QolResult (*current_bag)(int32_t* out_bag);
    QolResult (*set_current_bag)(int32_t bag);
    QolResult (*desc_type)(int32_t* out_type);
    QolResult (*set_desc_type)(int32_t type);
    QolResult (*bag_capacity_word)(int32_t bag, uint32_t* out_word);
    QolResult (*set_bag_capacity_word)(int32_t bag, uint32_t word);
    QolResult (*draw_original_bag)();
    QolResult (*draw_original_items)();
    QolResult (*create_confirmation)(int32_t variant, uint32_t price, void* ok_wrapper,
                                     void* cancel_wrapper, void** out_original_ok,
                                     void** out_original_cancel);
    QolResult (*restore_confirmation)(int32_t variant, void* ok_wrapper, void* cancel_wrapper,
                                      void* original_ok, void* original_cancel);
    QolResult (*original_bag_item)(int32_t bag, void** out_item);
    QolResult (*set_original_bag_item)(int32_t bag, void* item);
    // Store scene owns a different bag-group draw entry from UIEquip.
    QolResult (*draw_original_store_bag_group)();
    // Forces the native quantity dialog for an EB store item. callback must be
    // the native UIStore_SellOKInputItemCount pointer returned below.
    QolResult (*create_item_count_popup)(int32_t type, void* item, int32_t input_count,
                                         int32_t total_count, void* callback);
    void* (*sell_ok_input_item_count)();
    // UIStore_MakeDesc(control, desc_control): store detail uses UIStore's own
    // menu (UIStore_ButtonSellExe), not the UIEquip detail hooks.
    QolResult (*make_store_item_description)(void* control, void* desc_control);
};

struct QolExtensionBagWritePort {
    QolVersion abi_version;
    QolExtensionBagInventoryWriteApi inventory;
    QolExtensionBagEquipmentWriteApi equipment;
    QolExtensionBagUiWriteApi ui;
};

}  // namespace qol::contracts

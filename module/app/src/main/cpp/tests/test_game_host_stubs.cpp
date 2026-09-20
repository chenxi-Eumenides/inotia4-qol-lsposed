#include "game/access/inventory.h"
#include "game/access/item_class.h"
#include "game_access.h"
#include "patch/ptr_slot.h"

uint8_t* g_snd_fx = nullptr;
UiEquipMakeDescFn fn_ui_equip_make_desc = nullptr;

namespace qol::game {

int item_sell_price(const void*) { return -1; }

QolResult physical_item_at(int32_t, int32_t, void** out_item) {
    if (out_item != nullptr) *out_item = nullptr;
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

QolResult current_bag(int32_t* out_bag) {
    if (out_bag != nullptr) *out_bag = -1;
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

QolResult set_current_bag(int32_t) {
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

QolResult desc_type(int32_t* out_type) {
    if (out_type != nullptr) *out_type = -1;
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

QolResult set_desc_type(int32_t) {
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

QolResult bag_capacity_word(int32_t, uint32_t* out_word) {
    if (out_word != nullptr) *out_word = 0;
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

QolResult set_bag_capacity_word(int32_t, uint32_t) {
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

QolResult detail_context(int32_t* out_desc_type, int32_t* out_bag, void** out_control) {
    if (out_desc_type != nullptr) *out_desc_type = 0;
    if (out_bag != nullptr) *out_bag = 0;
    if (out_control != nullptr) *out_control = nullptr;
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

QolResult cursor_index(const void*, int32_t* out_slot) {
    if (out_slot != nullptr) *out_slot = -1;
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

contracts::QolItemCountEncoding item_count_encoding_for_category(int) {
    return contracts::QolItemCountEncoding::kUnknown;
}

bool extension_desc_button_slot(uint32_t, PtrSlotSite* out) {
    if (out == nullptr) return false;
    out->slot_addr = 0;
    out->expected_value = nullptr;
    return false;
}

bool extension_desc_button_instance(uint32_t, void** out_button) {
    if (out_button != nullptr) *out_button = nullptr;
    return false;
}

bool extension_desc_button_original_matches(void*, const char*) { return false; }

// P2/G5 语义面在 host 测试中的桩（真实实现位于 game/access/*.cpp，测试目标不编译它们）。
void* char_party_member(int32_t) { return nullptr; }

QolResult set_physical_item(int32_t, int32_t, void*) {
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

int32_t craft_is_apply_stuff(void*, void*) { return -1; }

}  // namespace qol::game

namespace qol::patch {

bool ptr_slot_write_raw(uintptr_t, void*) { return false; }
bool ptr_slot_restore_raw(uintptr_t, void*) { return false; }

}  // namespace qol::patch

AddMoneyFn fn_add_money = nullptr;
RemoveItemDirectFn fn_remove_item_direct = nullptr;

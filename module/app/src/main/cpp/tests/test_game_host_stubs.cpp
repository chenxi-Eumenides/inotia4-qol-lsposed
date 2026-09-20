#include "game/access/inventory.h"
#include "game/access/item_class.h"
#include "game_access.h"

namespace qol::game {

int item_sell_price(const void*) { return -1; }

QolResult physical_item_at(int32_t, int32_t, void** out_item) {
    if (out_item != nullptr) *out_item = nullptr;
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

}  // namespace qol::game

AddMoneyFn fn_add_money = nullptr;
RemoveItemDirectFn fn_remove_item_direct = nullptr;

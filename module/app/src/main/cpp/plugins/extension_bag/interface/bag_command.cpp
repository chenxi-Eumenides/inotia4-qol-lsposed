#include "plugins/extension_bag/interface/bag_command.h"

#include "plugins/extension_bag/game_ui_virtbag.h"

namespace qol::features::extension_bag {

bool dispatch_bag_command(int32_t kind, int32_t a, int32_t b, int32_t c, int32_t d,
                          std::string* out) {
    if (out == nullptr) return false;

    switch (static_cast<BagCommandKind>(kind)) {
        case BagCommandKind::kEnterView:
            *out = data_op_extension_bag_enter_view(a);
            return true;
        case BagCommandKind::kExitView:
            *out = data_op_extension_bag_exit_view();
            return true;
        case BagCommandKind::kUnequip:
            *out = data_op_extension_bag_unequip(a);
            return true;
        case BagCommandKind::kSelectBag:
            *out = data_op_extension_bag_select_bag(a);
            return true;
        case BagCommandKind::kClickItem:
            *out = data_op_extension_bag_click_item(a, b);
            return true;
        case BagCommandKind::kEquip:
            *out = data_virtual_bag_test_equip(a, b);
            return true;
        case BagCommandKind::kItem:
            *out = data_virtual_bag_test_item(a, b, c, d);
            return true;
        case BagCommandKind::kSetBagCount:
            *out = data_op_extension_bag_set_bag_count(a);
            return true;
        default:
            return false;
    }
}

}  // namespace qol::features::extension_bag

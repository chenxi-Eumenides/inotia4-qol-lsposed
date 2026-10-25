#include "game_ui_virtbag.h"
#include "plugins/extension_bag/runtime/state_access.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/model/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/core/lifecycle.h"
#include "plugins/extension_bag/core/ownership.h"
#include "plugins/extension_bag/core/projection.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/core/tabs.h"
#include "plugins/extension_bag/transaction/transaction.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/usecases/set_bag_count.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>



std::string data_op_extension_bag_set_bag_count(int count) {
    return qol::features::extension_bag::usecases::usecase_set_bag_count(count);
}

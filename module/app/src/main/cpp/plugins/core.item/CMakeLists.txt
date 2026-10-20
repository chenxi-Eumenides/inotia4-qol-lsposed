add_library(qol_plugin_core.item STATIC
    plugin.cpp
    json_lite.cpp
    response.cpp
    inventory/native_inventory_hook.cpp
    inventory/inventory_find_item_poc.cpp
    inventory/inventory_hook_stage4.cpp
    inventory/inventory_logging.cpp
    inventory/inventory_slot.cpp
    inventory/plugin.cpp
    merge/plugin.cpp
)

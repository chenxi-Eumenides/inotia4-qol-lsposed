add_library(qol_plugin_core.item STATIC
    plugin.cpp
    http/json_lite.cpp
    http/response.cpp
    hooks/inventory_hooks.cpp
    inventory/inventory_find_item_poc.cpp
    usecases/consume_item.cpp
    hooks/inventory_logging.cpp
    usecases/item_slot.cpp
    usecases/sell_item.cpp
    hooks/merge_hook.cpp
)

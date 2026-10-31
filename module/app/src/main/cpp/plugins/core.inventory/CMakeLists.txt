add_library(qol_plugin_core.inventory STATIC
    plugin.cpp
    api/access.cpp
    api/bag_source.cpp
    bags/snapshot_store.cpp
    hooks/inventory_logging.cpp
    hooks/storage_hooks.cpp
    http/json_lite.cpp
    http/query_handler.cpp
    http/response.cpp
    http/routes.cpp
    runtime/context.cpp
    store/slot_table.cpp
    store/inventory_store.cpp
    usecases/item_helpers.cpp
    usecases/read_inventory.cpp
)

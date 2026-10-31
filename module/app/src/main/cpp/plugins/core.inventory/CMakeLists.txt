add_library(qol_plugin_core.inventory STATIC
    plugin.cpp
    api/access.cpp
    api/bag_source.cpp
    hooks/inventory_logging.cpp
    hooks/stage4.cpp
    hooks/storage_hooks.cpp
    runtime/context.cpp
    store/slot_table.cpp
    store/inventory_store.cpp
)

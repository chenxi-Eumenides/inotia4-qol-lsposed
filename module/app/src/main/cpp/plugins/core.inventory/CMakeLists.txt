add_library(qol_plugin_core.inventory STATIC
    plugin.cpp
    api/access.cpp
    api/bag_source.cpp
    hooks/inventory_logging.cpp
    hooks/storage_hooks.cpp
    store/slot_table.cpp
    store/inventory_store.cpp
)

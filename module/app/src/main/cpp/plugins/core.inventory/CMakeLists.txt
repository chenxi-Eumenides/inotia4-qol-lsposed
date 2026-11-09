add_library(qol_plugin_core.inventory STATIC
    plugin.cpp
    api/access.cpp
    api/bag_source.cpp
    bags/snapshot_store.cpp
    hooks/inventory_logging.cpp
    hooks/placement.cpp
    hooks/storage_hooks.cpp
    http/json_lite.cpp
    http/query_handler.cpp
    http/response.cpp
    http/routes.cpp
    http/write_handler.cpp
    http/write_results.cpp
    runtime/context.cpp
    store/slot_table.cpp
    store/inventory_store.cpp
    transfer/api.cpp
    transfer/move_item.cpp
    transfer/native_transport.cpp
    transfer/replace_item.cpp
    transfer/source_transport.cpp
    usecases/item_helpers.cpp
    usecases/read_inventory.cpp
)
# 宿主测试直接编译 plugin.cpp 并内嵌 transfer 与 HTTP 写侧实现；插件 target 单独编译各实现。
target_compile_definitions(qol_plugin_core.inventory PRIVATE QOL_CORE_INVENTORY_TRANSFER_SEPARATE=1)
target_compile_definitions(qol_plugin_core.inventory PRIVATE QOL_CORE_INVENTORY_WRITE_SEPARATE=1)

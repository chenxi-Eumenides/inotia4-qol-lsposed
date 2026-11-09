add_library(qol_plugin_core.item STATIC
    plugin.cpp
    runtime/context.cpp
    runtime/lifecycle.cpp
    bags/snapshot_store.cpp
    usecases/item_helpers.cpp
    usecases/read_inventory.cpp
    shop/snapshot.cpp
    shop/query.cpp
    shop/buy.cpp
    api/bag_source.cpp
    api/item_payload.cpp
    api/native_inventory.cpp
    api/projection.cpp
    api/facts.cpp
    api/state.cpp
    http/query_handler.cpp
    http/write_handler.cpp
    http/write_results.cpp
    http/routes.cpp
    http/json_lite.cpp
    http/response.cpp
    hooks/inventory_hooks.cpp
    usecases/consume_item.cpp
    hooks/inventory_logging.cpp
    usecases/item_slot.cpp
    usecases/sell_item.cpp
    usecases/item_action.cpp
)
target_compile_definitions(qol_plugin_core.item PRIVATE QOL_CORE_ITEM_BAG_SOURCE_SEPARATE=1)
target_compile_definitions(qol_plugin_core.item PRIVATE QOL_CORE_ITEM_NATIVE_INVENTORY_SEPARATE=1)
target_compile_definitions(qol_plugin_core.item PRIVATE QOL_CORE_ITEM_PROJECTION_SEPARATE=1)
target_compile_definitions(qol_plugin_core.item PRIVATE QOL_CORE_ITEM_ITEM_PAYLOAD_SEPARATE=1)
target_compile_definitions(qol_plugin_core.item PRIVATE QOL_CORE_ITEM_TYPED_READ_SOURCES_SEPARATELY=1)
target_compile_definitions(qol_plugin_core.item PRIVATE QOL_CORE_ITEM_SHOP_SEPARATE=1)

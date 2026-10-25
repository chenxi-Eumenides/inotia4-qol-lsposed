add_library(qol_plugin_core.item STATIC
    plugin.cpp
    runtime/context.cpp
    runtime/lifecycle.cpp
    bags/native_transport.cpp
    bags/source_transport.cpp
    bags/snapshot_store.cpp
    usecases/item_helpers.cpp
    usecases/move_item.cpp
    usecases/read_inventory.cpp
    usecases/transfer_journal.cpp
    shop/snapshot.cpp
    shop/query.cpp
    shop/buy.cpp
    api/bag_source.cpp
    api/facts.cpp
    api/state.cpp
    http/query_handler.cpp
    http/write_handler.cpp
    http/write_results.cpp
    http/routes.cpp
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
target_compile_definitions(qol_plugin_core.item PRIVATE QOL_CORE_ITEM_BAG_SOURCE_SEPARATE=1)
target_compile_definitions(qol_plugin_core.item PRIVATE QOL_CORE_ITEM_TYPED_READ_SOURCES_SEPARATELY=1)
target_compile_definitions(qol_plugin_core.item PRIVATE QOL_CORE_ITEM_SHOP_SEPARATE=1)
target_compile_definitions(qol_plugin_core.item PRIVATE
    QOL_CORE_ITEM_TRANSFER_JOURNAL_SEPARATE=1
    QOL_CORE_ITEM_SOURCE_TRANSPORT_SEPARATE=1
)

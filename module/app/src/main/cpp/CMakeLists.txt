cmake_minimum_required(VERSION 3.22.1)
project(gamebridge)

add_library(gamebridge SHARED
    bridge/native/gamebridge.cpp
    bridge/native/gamebridge_data.cpp
    bridge/native/gamebridge_operations.cpp
    bridge/native/gamebridge_ui.cpp
    bridge/native/gamebridge_domain_operations.cpp
    bridge/native/gamebridge_settings.cpp
    bridge/native/gamebridge_extension_bag.cpp
    game_access.cpp
    data/native/symbol_resolver.cpp
    core/native/game_json.cpp
    core/native/game_ops_common.cpp
    core/native/grid_geometry.cpp
    game_state.cpp
    core/native/game_nav.cpp
    core/native/game_cache.cpp
    core/native/module_save.cpp
    api/native/game_world.cpp
    api/native/game_quest.cpp
    api/native/game_ui.cpp
    game_dialog.cpp
    api/native/game_shop.cpp
    api/native/game_save.cpp
    api/native/game_save_preflight.cpp
    game_system.cpp
    core/native/game_motion.cpp
    data/native/game_tiles.cpp
    api/native/game_character.cpp
    api/native/game_party.cpp
    api/native/game_inventory.cpp
    feature/patch/game_patch.cpp
    feature/patch/native_inventory_hook.cpp
    feature/patch/inventory_find_item_poc.cpp
    feature/patch/inventory_hook_stage4.cpp
    feature/ui/game_ui_exp.cpp
    feature/ui/game_ui_kit.cpp
    feature/ui/game_ui_components.cpp
    feature/ui/game_ui_settings.cpp
    feature/extension_bag/game_ui_virtbag.cpp
    feature/extension_bag/extension_bag_port.cpp
    feature/extension_bag/extension_bag_persistence.cpp
    feature/extension_bag/extension_bag_geometry.cpp
    feature/extension_bag/extension_bag_api.cpp
    feature/extension_bag/extension_bag_logging.cpp
)

find_library(log-lib log)
target_include_directories(gamebridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/api/native
    ${CMAKE_CURRENT_SOURCE_DIR}/core/native
    ${CMAKE_CURRENT_SOURCE_DIR}/data/native
)
target_link_libraries(gamebridge ${log-lib} dl)

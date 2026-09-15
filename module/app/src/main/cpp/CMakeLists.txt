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
    feature/save_backup/save_backup_bundle.cpp
    feature/save_backup/save_backup.cpp
    game_system.cpp
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
    feature/ui/game_ui_savebackup.cpp
    feature/ui/native_choice.cpp
    feature/extension_bag/game_ui_virtbag.cpp
    feature/extension_bag/extension_bag_port.cpp
    feature/extension_bag/extension_bag_persistence.cpp
    feature/extension_bag/extension_bag_geometry.cpp
    feature/extension_bag/extension_bag_api.cpp
    feature/attribute_range/attribute_range.cpp
    feature/attribute_range/game_ui_attr_range.cpp
    feature/world_teleport/world_teleport.cpp
    feature/world_teleport/world_teleport_rules.cpp
)

find_library(log-lib log)
target_include_directories(gamebridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/api/native
    ${CMAKE_CURRENT_SOURCE_DIR}/core/native
    ${CMAKE_CURRENT_SOURCE_DIR}/data/native
)
target_link_libraries(gamebridge ${log-lib} dl)

# 自动出售阶段 A：core 基础设施 + 数据层下沉（末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    core/native/call_patch.cpp
    core/native/frame_task.cpp
    core/native/frame_host.cpp
    core/native/save_enter.cpp
    core/native/save_exit.cpp
    core/native/inventory_trade.cpp
    data/native/item_class.cpp
)

# 自动出售阶段 B：规则/视图/扫描/宿主 + JNI 薄层（末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    feature/autosell/autosell_rules.cpp
    feature/autosell/autosell_config.cpp
    feature/autosell/autosell_view.cpp
    feature/autosell/autosell_scan.cpp
    bridge/native/gamebridge_autosell.cpp
)

# 自动出售按存档配置 sidecar（末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    feature/autosell/autosell_store.cpp
)

# 自动出售 UI 原型：背包页入口按钮 + 只读占位面板（末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    feature/ui/game_ui_autosell.cpp
)

# 合成器宝石合成操作优化（gem-craft-optimization 阶段1，末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    feature/gemcraft/gemcraft_rules.cpp
    feature/ui/game_ui_gemcraft.cpp
)

# 状态转换派发（go_main_menu/enter_slot/create_slot 跨线程收敛到逻辑帧点，末尾追加）
target_sources(gamebridge PRIVATE
    core/native/transition_dispatch.cpp
)

# 任务完成度进档重算（quest-resync，末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    feature/quest/quest_resync.cpp
)

# 统一日志系统 P0 基础设施（末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    core/native/qol_log.cpp
    bridge/native/gamebridge_log.cpp
)

# libgame.so 变体对照表运行时查表（末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    core/native/game_variant.cpp
)

# 功能抽象层：构建能力 → 功能可用性（末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    core/native/game_feature.cpp
)

# 自定义合成配方（custom-craft-recipe：目录/规则/表注入/UI hook，末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    feature/custom_recipe/custom_recipe_catalog.cpp
    feature/custom_recipe/custom_recipe_rules.cpp
    feature/custom_recipe/custom_recipe_table.cpp
    feature/custom_recipe/game_ui_custom_recipe.cpp
)

# 角色属性面板战斗属性行标签中文化（character-info-zh-labels，末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    feature/ui/game_ui_charinfo_zh.cpp
)

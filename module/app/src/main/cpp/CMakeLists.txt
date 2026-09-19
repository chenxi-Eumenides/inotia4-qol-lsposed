cmake_minimum_required(VERSION 3.22.1)
project(gamebridge)

set(QOL_BRIDGE_PLUGIN_SOURCES
    plugins/save_backup/bundle.cpp
    plugins/save_backup/feature.cpp
    plugins/inventory_hooks/native_inventory_hook.cpp
    plugins/inventory_hooks/inventory_find_item_poc.cpp
    plugins/inventory_hooks/inventory_hook_stage4.cpp
    plugins/extension_bag/game_ui_virtbag.cpp
    plugins/extension_bag/extension_bag_port.cpp
    plugins/extension_bag/extension_bag_persistence.cpp
    plugins/extension_bag/extension_bag_geometry.cpp
    plugins/extension_bag/extension_bag_api.cpp
    plugins/attribute_quality/rules.cpp
    plugins/attribute_quality/feature.cpp
    plugins/attribute_quality/plugin.cpp
    plugins/attribute_range_display/feature.cpp
    plugins/attribute_range_display/plugin.cpp
    plugins/world_teleport/feature.cpp
    plugins/world_teleport/rules.cpp
    plugins/autosell/rules.cpp
    plugins/autosell/config.cpp
    plugins/autosell/view.cpp
    plugins/autosell/scan.cpp
    plugins/autosell/plugin.cpp
    plugins/autosell/store.cpp
    plugins/autosell/ui/panel.cpp
    plugins/quest/feature.cpp
    plugins/quest/plugin.cpp
    plugins/special_equip/rules.cpp
    plugins/special_equip/feature.cpp
    plugins/special_equip/plugin.cpp
    plugins/custom_recipe/catalog.cpp
    plugins/custom_recipe/rules.cpp
    plugins/custom_recipe/table.cpp
    plugins/custom_recipe/feature.cpp
    plugins/custom_recipe/plugin.cpp
    plugins/text_layer/charinfo_scope.cpp
    plugins/text_layer/feature.cpp
    plugins/text_layer/plugin.cpp
    plugins/craft_ui/feature.cpp
    plugins/craft_ui/plugin.cpp
    plugins/push_all/rules.cpp
    plugins/push_all/feature.cpp
    plugins/push_all/plugin.cpp
    plugins/inventory_hooks/plugin.cpp
    plugins/stack_limit/plugin.cpp
    plugins/save_backup/plugin.cpp
    plugins/world_teleport/plugin.cpp
    plugins/native_choice/feature.cpp
    plugins/native_choice/plugin.cpp
    plugins/core.ui/plugin.cpp
    plugins/core.ui/feature.cpp
    plugins/core.ui/ui_components.cpp
    plugins/core.ui/panels/settings.cpp
    plugins/core.ui/panels/stack.cpp
    plugins/save_backup/ui/panel.cpp
    plugins/extension_bag/plugin.cpp
)

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
    data/native/game_tiles.cpp
    api/native/game_character.cpp
    api/native/game_party.cpp
    api/native/game_inventory.cpp
    game/access/hive_iap.cpp
    game/access/ui_kit.cpp
    game/access/ui_components.cpp
    game/access/attr_range_ui.cpp
    game/access/inventory.cpp
)

find_library(log-lib log)
target_include_directories(gamebridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/api/native
    ${CMAKE_CURRENT_SOURCE_DIR}/core/native
    ${CMAKE_CURRENT_SOURCE_DIR}/data/native
)
add_subdirectory(plugins)
target_sources(gamebridge PRIVATE ${QOL_BRIDGE_PLUGIN_SOURCES})
target_link_libraries(gamebridge PRIVATE ${log-lib} dl)

# 自动出售阶段 A：core 基础设施 + 数据层下沉（末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    core/native/call_patch.cpp
    core/native/frame_task.cpp
    core/native/save_enter.cpp
    core/native/save_exit.cpp
    core/native/inventory_trade.cpp
    data/native/item_class.cpp
)

# 自动出售阶段 B：规则/视图/扫描/宿主 + JNI 薄层（P4-B3 迁移至 plugins/autosell）
target_sources(gamebridge PRIVATE
    bridge/native/gamebridge_autosell.cpp
)

# 自动出售按存档配置 sidecar（P4-B3 迁移至 plugins/autosell）
target_sources(gamebridge PRIVATE
)

# 自动出售 UI：背包页入口按钮 + 配置面板（D1-b3 迁入 plugins/autosell/ui）
target_sources(gamebridge PRIVATE
)

# 合成器宝石合成操作优化（gem-craft-optimization 阶段1，末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
)

# 状态转换派发（go_main_menu/enter_slot/create_slot 跨线程收敛到逻辑帧点，末尾追加）
target_sources(gamebridge PRIVATE
    core/native/transition_dispatch.cpp
)

# 任务完成度进档重算（quest 插件迁移：零资源常驻 + infra/scheduler 帧节拍；旧 quest_resync 删除）
target_sources(gamebridge PRIVATE
    game/access/quest.cpp
)

# P4 阶段 B1：special_equip 迁移为插件（game/access/item_class 收口 ITEMCLASS/GOT 读取，
# 入口 hook 声明式化 + 出售门禁谓词过渡依赖；末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    game/access/item_class.cpp
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

# 自定义合成配方（custom-craft-recipe：目录/规则/表注入/结果层 provider，末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
)

# 角色属性面板战斗属性行标签中文化（character-info-zh-labels；作用域标记归入 text_layer 插件）
target_sources(gamebridge PRIVATE
    hooks/dispatch.cpp
)

# 合成器界面层（craft_ui 插件）：UIMix 控件写入收口 game/access/craft + 12 条入口 hook 声明。
# 宝石合成与自定义配方共用；「界面归界面、结果归结果」（末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    game/access/craft.cpp
)

# 模块替换 PopupState enter 回调后的 UI 态反查表（末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    core/native/popup_enter_alias.cpp
)

# 推怪（推动增强）全局开关：JNI 薄层（插件实现已迁 plugins/push_all，末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    bridge/native/gamebridge_push_all.cpp
)

# P1 最小骨架：infra 运行时（命令队列/事件/能力/状态）+ game 时钟适配（末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    infra/command_queue.cpp
    infra/runtime.cpp
    infra/events.cpp
    infra/capabilities.cpp
    infra/status.cpp
    game/clock.cpp
)

# L2：插件 ABI 表的 host/game adapter（固定宽度契约转发至内部实现）。
target_sources(gamebridge PRIVATE
    host/host_sdk.cpp
    host/game_host.cpp
    host/item_count_facade.cpp
    host/ui_adapter.cpp
)

# P1 最小诊断接口：功能状态 JSON（末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    host/status_json.cpp
    bridge/native/gamebridge_status.cpp
)

# P1 诊断：游戏变体 JSON（末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    host/variant_json.cpp
    bridge/native/gamebridge_variant.cpp
)

# P2 纵向样板基础层：hooks 资源登记/安装引擎 + infra 配置与命令端口（末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    hooks/registry.cpp
    hooks/install.cpp
    infra/config.cpp
    infra/commands.cpp
)

# P2 纵向样板插件：simple_mode（入口 hook 声明 + 配置门禁 + 状态 + 命令端口）。
# game/access 收口符号解析与角色读写；host 插件注册表声明式安装资源（末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    game/access/symbols.cpp
    game/access/char.cpp
    host/plugin_registry.cpp
    host/assembly.cpp
    bridge/native/gamebridge_simple_mode.cpp
)

# P3 Lane A：资源事务引擎（运行期记录/在途计数/隔离/物理释放/帧步进事务，末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    hooks/transaction.cpp
)

# P4 阶段 A1：patch/ 机制层 + push_all 迁移（指令 patch 占用声明/原字校验/回滚，末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    game/access/patches.cpp
    patch/claims.cpp
    patch/instr_patch.cpp
)

# D2a-3a：extension_bag 指针槽站点解析（UIEquip 事件 proc GOT，末尾追加）
target_sources(gamebridge PRIVATE
    game/access/ui_equip.cpp
)

# P4 阶段 A2a：infra/scheduler 形式化 + 帧源声明式 core 资源（末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    infra/scheduler.cpp
    host/frame_source.cpp
)

# P3 Lane B：生命周期 JNI 薄层（控制端点经命令端口投递，末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    bridge/native/gamebridge_lifecycle.cpp
)

# B2a-1：库存行为域声明式化（24 条入口 hook 的插件 owner；wrapper 业务体已随 D2c-2a
# 迁入 plugins/inventory_hooks，同目录 claims.h 直接引用。末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
)

# B2a-2/P1-B1：宿主仅保留 Hive 兼容 patch；S2 12 条与 6 条 clamp 归 stack_limit 999
# 原子组。末尾追加，不改上方既有行
target_sources(gamebridge PRIVATE
    host/core_patches.cpp
)

# B4-1：infra/store MSAV 容器层（冻结核心件；零行为改动，不切换消费者）
target_sources(gamebridge PRIVATE
    infra/store.cpp
)

# B4-2：infra/store JNI 薄层（Kotlin ModuleSaveStore 委托 native；native 成为
# module-saves/ 唯一文件读写者）
target_sources(gamebridge PRIVATE
    bridge/native/gamebridge_store.cpp
)

# B4-5：save_backup 本体迁移为插件（零资源常驻 + 存档域原语收口 game/access/save）
target_sources(gamebridge PRIVATE
    game/access/save.cpp
)

# C1：world_teleport 迁移为插件（零资源常驻 + 世界/地图访问收口 game/access/world）
target_sources(gamebridge PRIVATE
    game/access/world.cpp
)

# B2a-3：指针槽声明式机制（kPtrSlot）+ save_backup 删档 PtrHook 迁移（末尾追加，不改上方既有行）
target_sources(gamebridge PRIVATE
    game/access/ptr_slots.cpp
    patch/ptr_slot.cpp
)

# D1-a2：PopupState state 表注入机制（hooks/popup_state 原语 + game/access 条目访问收口；
# 本 lane 只落机制，生产注入点转换在 D1-b/D1-c）
target_sources(gamebridge PRIVATE
    game/access/popup_state.cpp
    hooks/popup_state.cpp
)

# D1-b1：native_choice 迁移为插件（provider `ui_choice`）+ 选择框语义收口 game/access/ui_choice
target_sources(gamebridge PRIVATE
    game/access/ui_choice.cpp
)

# D1-b0：面板注册表（纯数据面，零 game 依赖）+ PopupState 按名定位入口
target_sources(gamebridge PRIVATE
    infra/panels.cpp
)

# D1-b2：core.ui 出口面插件（设置页容器 + 面板栈 + ui_panels 能力；own:popup.settings）
target_sources(gamebridge PRIVATE
)

# D1-b2：save_backup 游戏内面板迁入插件（provider save_backup + own:popup.savebackup）
target_sources(gamebridge PRIVATE
)

# D2a-3b-1 / D2c-2b-2：extension_bag 声明式资源插件（29 条 claim；wrapper 业务体随
# D2c-2b-2 全部收口在 plugins/extension_bag，features/patch 已删除）
target_sources(gamebridge PRIVATE
)

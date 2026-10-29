# 当前实现结构地图

> 目标职责、依赖方向与边界以根目录 `ARCHITECTURE.md` 为准。本文件仅呈现当前源码结构统计，不定义架构目标。

<!-- BEGIN GENERATED:stats -->
## 生成统计

> 本节由 `scripts/maintenance/gen_map_doc.py` 生成，**禁止手工编辑**；
> 手工改动会在 `--check` 下被判定为不一致。

- 数据源：`module/app/src/main/cpp/{contracts,game,infra,hooks,patch,plugins,host}`、`module/app/src/main/cpp/plugins/*`、`module/app/src/main/cpp/tests/CMakeLists.txt`
- 口径：行数按 `wc -l`（换行符计数，含空行与注释）；文件限 `.cpp`/`.h`/`.inc`；不含 `bridge/`、`tests/` 与 legacy 路径
- CTest 分类规则：`gen_map_doc.py` 的显式白名单（测试源文件 → 机制/宿主或已迁移功能）确定新架构/已迁移 target，其余归 legacy；白名单条目须存在于 `tests/CMakeLists.txt`，两类互斥

### 新架构目录

| 目录 | 文件数 | 行数 |
| --- | --- | --- |
| `contracts/` | 94 | 7040 |
| `game/` | 182 | 24615 |
| `infra/` | 40 | 4882 |
| `hooks/` | 10 | 1784 |
| `patch/` | 13 | 1880 |
| `plugins/` | 461 | 81648 |
| `host/` | 63 | 18364 |
| **合计** | **863** | **140213** |

### 已迁移插件（`plugins/*`）

- `plugins/attribute_quality/`：6 文件 / 793 行
- `plugins/attribute_range_display/`：4 文件 / 448 行
- `plugins/autosell/`：31 文件 / 3654 行
- `plugins/character_panel_i18n/`：6 文件 / 388 行
- `plugins/core.api/`：6 文件 / 731 行
- `plugins/core.character/`：6 文件 / 2663 行
- `plugins/core.craft/`：3 文件 / 223 行
- `plugins/core.custom_ui/`：29 文件 / 8401 行
- `plugins/core.data/`：5 文件 / 1089 行
- `plugins/core.feature/`：3 文件 / 104 行
- `plugins/core.item/`：76 文件 / 12423 行
- `plugins/core.quest/`：6 文件 / 922 行
- `plugins/core.save/`：13 文件 / 3552 行
- `plugins/core.system/`：4 文件 / 404 行
- `plugins/core.ui_state/`：3 文件 / 1550 行
- `plugins/core.world/`：4 文件 / 871 行
- `plugins/craft_lottery/`：8 文件 / 340 行
- `plugins/custom_map/`：8 文件 / 1998 行
- `plugins/custom_recipe/`：19 文件 / 5575 行
- `plugins/eight_way_move/`：4 文件 / 494 行
- `plugins/extension_bag/`：131 文件 / 20860 行
- `plugins/module_health/`：2 文件 / 233 行
- `plugins/popup_blocker/`：2 文件 / 132 行
- `plugins/push_all/`：8 文件 / 358 行
- `plugins/save_backup/`：34 文件 / 7875 行
- `plugins/settings/`：14 文件 / 2194 行
- `plugins/special_equip/`：6 文件 / 705 行
- `plugins/stack_limit/`：4 文件 / 138 行
- `plugins/world_teleport/`：6 文件 / 982 行
- `plugins/world_tuning/`：10 文件 / 1548 行

### CTest target 分类

- 全仓 target 总数：**158**
- 新架构/已迁移：**33**（机制/宿主 17 + 已迁移功能 16）
- legacy：**125**

新架构/已迁移（33）：
- 机制/宿主（17）：`capability_lifecycle_tests`、`hooks_dispatch_tests`、`hooks_popup_state_tests`、`hooks_registry_tests`、`hooks_transaction_tests`、`infra_capabilities_status_tests`、`infra_command_queue_tests`、`infra_config_commands_tests`、`infra_events_tests`、`infra_panels_tests`、`infra_scheduler_tests`、`infra_snapshot_tests`、`infra_store_tests`、`patch_layer_tests`、`plugin_lifecycle_tests`、`plugin_registry_tests`、`store_sell_rules_tests`
- 已迁移功能（16）：`attribute_quality_tests`、`attribute_range_display_tests`、`autosell_plugin_tests`、`autosell_rules_tests`、`autosell_store_tests`、`character_panel_i18n_tests`、`core_custom_ui_host_tests`、`core_data_plugin_tests`、`custom_recipe_tests`、`module_text_tests`、`native_choice_tests`、`push_all_rules_tests`、`quest_lifecycle_tests`、`save_warehouse_validate_tests`、`special_equip_rules_tests`、`world_tuning_rates_tests`

legacy（125）：`attribute_quality_adapter_tests`、`attribute_quality_provider_tests`、`autosell_scan_gates_tests`、`autosell_scan_task_tests`、`autosell_slot_data_tests`、`autosell_view_diagnostic_tests`、`character_facts_dispatch_tests`、`character_facts_plugin_tests`、`character_http_port_tests`、`character_json_slice_tests`、`close_button_component_tests`、`command_queue_overflow_tests`、`core_api_lifecycle_tests`、`core_feature_plugin_tests`、`core_frame_overlay_tests`、`core_patches_shape_tests`、`core_settings_config_service_effect_failures_tests`、`core_settings_config_service_tests`、`core_settings_config_service_writeback_tests`、`core_settings_config_store_tests`、`core_settings_config_tests`、`core_settings_panel_migration_tests`、`core_ui_components_tests`、`core_ui_confirmation_tests`、`core_ui_panels_tests`、`core_ui_state_tests`、`craft_facts_plugin_tests`、`craft_ui_guard_tests`、`craft_ui_handle_identity_tests`、`custom_recipe_handle_chain_tests`、`custom_recipe_owner_fixture_tests`、`custom_recipe_table_readiness_tests`、`debug_two_column_tests`、`eight_way_move_plugin_tests`、`eight_way_move_tests`、`extension_bag_bag_source_mutation_tests`、`extension_bag_bag_state_store_tests`、`extension_bag_catalog_tests`、`extension_bag_deferred_free_tests`、`extension_bag_entry_guard_tests`、`extension_bag_equip_tab_layout_tests`、`extension_bag_sale_transaction_tests`、`extension_bag_slot_data_tests`、`extension_bag_transaction_coordinator_tests`、`feature_reconciler_tests`、`game_cache_tests`、`game_feature_tests`、`game_variant_tests`、`help_page_tests`、`host_adapter_tests`、`host_feature_controller_tests`、`host_tests`、`http_dispatch_tests`、`http_endpoint_binder_tests`、`http_registry_tests`、`infra_data_tests`、`infra_snapshot_handle_tests`、`inventory_action_lifecycle_tests`、`inventory_action_sell_tests`、`inventory_remove_wrapper_tests`、`inventory_trade_sell_tests`、`item_count_codec_tests`、`item_count_facade_tests`、`item_facts_dispatch_tests`、`item_facts_plugin_tests`、`item_handle_custody_tests`、`item_handle_registry_tests`、`item_transfer_journal_tests`、`lifecycle_participant_tests`、`module_health_tests`、`move_merge_plugin_tests`、`native_close_button_render_tests`、`native_scroll_list_tests`、`native_text_button_render_tests`、`native_toggle_render_tests`、`nav_bfs_tests`、`nav_box_consistency_tests`、`nav_button_component_tests`、`popup_blocker_plugin_tests`、`popup_enter_alias_tests`、`production_feature_facade_tests`、`push_all_feature_tests`、`qol_log_seam_tests`、`qol_log_tests`、`quest_dispatch_tests`、`runtime_hooks_contract_tests`、`save_backup_gate_tests`、`save_backup_rollback_tests`、`save_coordinator_tests`、`save_facts_plugin_tests`、`save_journal_tests`、`save_pre_save_tests`、`save_route_tests`、`save_transaction_tests`、`save_transition_tests`、`settings_catalog_abi_tests`、`settings_catalog_adapter_tests`、`settings_catalog_tests`、`settings_config_bridge_tests`、`settings_contributions_tests`、`settings_panel_model_tests`、`settings_panel_widget_tests`、`slot_data_contract_tests`、`slot_data_provider_tests`、`slot_operation_gate_tests`、`special_equip_feature_tests`、`stack_limit_plugin_tests`、`stage4_hook_tests`、`symbol_resolver_tests`、`system_events_builder_tests`、`system_facts_dispatch_tests`、`system_port_identity_tests`、`transition_dispatch_tests`、`ui_adapter_tests`、`ui_choice_save_lcd_tests`、`ui_control_item_identity_tests`、`ui_state_dialog_tests`、`ui_state_port_tests`、`world_facts_dispatch_tests`、`world_facts_plugin_tests`、`world_legacy_readers_tests`、`world_teleport_feature_tests`、`world_teleport_install_tests`、`world_teleport_status_tests`、`world_tuning_feature_tests`
<!-- END GENERATED:stats -->

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
| `contracts/` | 84 | 5667 |
| `game/` | 160 | 19429 |
| `infra/` | 39 | 4611 |
| `hooks/` | 10 | 1660 |
| `patch/` | 13 | 1563 |
| `plugins/` | 250 | 55551 |
| `host/` | 59 | 13865 |
| **合计** | **615** | **102346** |

### 已迁移插件（`plugins/*`）

- `plugins/attribute_quality/`：6 文件 / 445 行
- `plugins/attribute_range_display/`：4 文件 / 266 行
- `plugins/autosell/`：20 文件 / 3538 行
- `plugins/character_panel_i18n/`：6 文件 / 376 行
- `plugins/core.api/`：5 文件 / 561 行
- `plugins/core.character/`：5 文件 / 2535 行
- `plugins/core.craft/`：2 文件 / 205 行
- `plugins/core.custom_ui/`：20 文件 / 5126 行
- `plugins/core.data/`：4 文件 / 977 行
- `plugins/core.feature/`：2 文件 / 101 行
- `plugins/core.item/`：7 文件 / 2936 行
- `plugins/core.quest/`：5 文件 / 896 行
- `plugins/core.save/`：10 文件 / 3360 行
- `plugins/core.system/`：3 文件 / 390 行
- `plugins/core.ui_state/`：2 文件 / 906 行
- `plugins/core.world/`：3 文件 / 855 行
- `plugins/craft_ui/`：4 文件 / 1094 行
- `plugins/custom_recipe/`：13 文件 / 2391 行
- `plugins/device_probe_export_failure/`：2 文件 / 88 行
- `plugins/extension_bag/`：51 文件 / 16055 行
- `plugins/inventory_hooks/`：12 文件 / 2754 行
- `plugins/move_merge/`：4 文件 / 477 行
- `plugins/push_all/`：8 文件 / 353 行
- `plugins/save_backup/`：15 文件 / 4974 行
- `plugins/settings/`：13 文件 / 1889 行
- `plugins/simple_mode/`：8 文件 / 574 行
- `plugins/special_equip/`：6 文件 / 645 行
- `plugins/stack_limit/`：4 文件 / 140 行
- `plugins/world_teleport/`：6 文件 / 644 行

### CTest target 分类

- 全仓 target 总数：**133**
- 新架构/已迁移：**34**（机制/宿主 18 + 已迁移功能 16）
- legacy：**99**

新架构/已迁移（34）：
- 机制/宿主（18）：`capability_lifecycle_tests`、`extension_bag_contract_tests`、`hooks_dispatch_tests`、`hooks_popup_state_tests`、`hooks_registry_tests`、`hooks_transaction_tests`、`infra_capabilities_status_tests`、`infra_command_queue_tests`、`infra_config_commands_tests`、`infra_events_tests`、`infra_panels_tests`、`infra_scheduler_tests`、`infra_snapshot_tests`、`infra_store_tests`、`patch_layer_tests`、`plugin_lifecycle_tests`、`plugin_registry_tests`、`store_sell_rules_tests`
- 已迁移功能（16）：`attribute_quality_tests`、`attribute_range_display_tests`、`autosell_plugin_tests`、`autosell_rules_tests`、`autosell_store_tests`、`character_panel_i18n_tests`、`core_custom_ui_host_tests`、`core_data_plugin_tests`、`custom_recipe_tests`、`module_text_tests`、`native_choice_tests`、`push_all_rules_tests`、`quest_lifecycle_tests`、`save_warehouse_validate_tests`、`simple_mode_rules_tests`、`special_equip_rules_tests`

legacy（99）：`autosell_slot_data_tests`、`autosell_view_diagnostic_tests`、`character_facts_dispatch_tests`、`character_facts_plugin_tests`、`character_http_port_tests`、`character_json_slice_tests`、`close_button_component_tests`、`command_queue_overflow_tests`、`core_api_lifecycle_tests`、`core_feature_plugin_tests`、`core_patches_shape_tests`、`core_settings_config_service_effect_failures_tests`、`core_settings_config_service_tests`、`core_settings_config_service_writeback_tests`、`core_settings_config_store_tests`、`core_settings_config_tests`、`core_settings_panel_migration_tests`、`core_ui_components_tests`、`core_ui_panels_tests`、`core_ui_state_tests`、`craft_facts_plugin_tests`、`craft_ui_guard_tests`、`craft_ui_handle_identity_tests`、`custom_recipe_handle_chain_tests`、`debug_two_column_tests`、`extension_bag_draw_lock_tests`、`extension_bag_move_merge_shim_tests`、`extension_bag_sale_transaction_tests`、`extension_bag_slot_data_tests`、`extension_bag_status_query_tests`、`feature_reconciler_tests`、`game_feature_tests`、`game_variant_tests`、`help_page_tests`、`host_adapter_tests`、`host_feature_controller_tests`、`host_tests`、`http_dispatch_tests`、`http_registry_tests`、`infra_data_tests`、`infra_snapshot_handle_tests`、`inventory_action_sell_tests`、`inventory_extension_facade_tests`、`inventory_trade_provider_sell_tests`、`item_count_codec_tests`、`item_count_facade_tests`、`item_facts_dispatch_tests`、`item_facts_plugin_tests`、`item_handle_custody_tests`、`item_handle_registry_tests`、`lifecycle_participant_tests`、`move_merge_plugin_tests`、`native_close_button_render_tests`、`native_scroll_list_tests`、`native_text_button_render_tests`、`native_toggle_render_tests`、`nav_bfs_tests`、`nav_box_consistency_tests`、`nav_button_component_tests`、`popup_enter_alias_tests`、`production_feature_facade_tests`、`qol_log_seam_tests`、`qol_log_tests`、`quest_dispatch_tests`、`runtime_hooks_contract_tests`、`save_backup_gate_tests`、`save_backup_rollback_tests`、`save_coordinator_tests`、`save_facts_plugin_tests`、`save_journal_tests`、`save_route_tests`、`save_transaction_tests`、`save_transition_tests`、`settings_catalog_abi_tests`、`settings_catalog_adapter_tests`、`settings_catalog_tests`、`settings_config_bridge_tests`、`settings_contributions_tests`、`settings_panel_model_tests`、`settings_panel_widget_tests`、`slot_data_contract_tests`、`slot_data_provider_tests`、`slot_operation_gate_tests`、`stack_limit_plugin_tests`、`stage4_hook_tests`、`symbol_resolver_tests`、`system_events_builder_tests`、`system_facts_dispatch_tests`、`system_port_identity_tests`、`transition_dispatch_tests`、`ui_adapter_tests`、`ui_control_item_identity_tests`、`ui_state_dialog_tests`、`ui_state_port_tests`、`world_facts_dispatch_tests`、`world_facts_plugin_tests`、`world_legacy_readers_tests`、`world_teleport_install_tests`、`world_teleport_status_tests`
<!-- END GENERATED:stats -->

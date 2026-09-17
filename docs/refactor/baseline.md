<!-- 本文件由脚本生成，禁止手工编辑。 -->

# 重构现状基线清单

本文由脚本生成，**禁止手工编辑**；手工改动会在 `--check` 下被判定为不一致。

- 生成命令：`uv run python scripts/maintenance/gen_baseline_doc.py`
- 一致性校验：`uv run python scripts/maintenance/gen_baseline_doc.py --check`
- 机器事实源：
  - `scripts/maintenance/arch_baseline.json`
  - `scripts/maintenance/arch_common.py`
  - `scripts/maintenance/check_layers.py`
  - `scripts/maintenance/check_resources.py`
  - `scripts/maintenance/check_jni.py`
  - `module/app/src/main/cpp/feature/*/`

## 1. 层分布与迁移期映射

### 1.1 层分布

| 目标层 | 文件数 |
| --- | --- |
| `contracts` | 5 |
| `domains/character` | 2 |
| `domains/dialog` | 4 |
| `domains/inventory` | 9 |
| `domains/party` | 2 |
| `domains/quest` | 2 |
| `domains/save` | 5 |
| `domains/shop` | 2 |
| `domains/system` | 2 |
| `domains/ui` | 5 |
| `domains/world` | 9 |
| `features/attribute_range` | 4 |
| `features/autosell` | 10 |
| `features/craft_ui` | 4 |
| `features/custom_recipe` | 8 |
| `features/extension_bag` | 32 |
| `features/patch` | 12 |
| `features/push_all` | 2 |
| `features/quest` | 2 |
| `features/save_backup` | 4 |
| `features/simple_mode` | 4 |
| `features/special_equip` | 2 |
| `features/ui` | 33 |
| `features/world_teleport` | 4 |
| `game` | 21 |
| `host` | 18 |
| `infra` | 35 |
| `patch` | 3 |
| **合计** | **245** |

### 1.2 迁移期映射摘要

映射常量直接 `import arch_common` 读取私有表（同目录模块，脚本只读、不修改该库），
避免在本文档复制一份映射形成第二事实源。

**新架构目标目录（`arch_common._TARGET_PREFIXES`，按序匹配）**

| 当前路径前缀 | 目标层 |
| --- | --- |
| `contracts/` | contracts |
| `domains/` | domains |
| `features/` | features |
| `game/` | game |
| `hooks/` | hooks |
| `host/` | host |
| `infra/` | infra |
| `patch/` | patch |
| `products/` | products |

**重构期根目录覆盖（`arch_common._ROOT_OVERRIDES`）**

| 当前文件（cpp 根） | 目标层 |
| --- | --- |
| `game_access.cpp` | game |
| `game_access.h` | game |
| `game_access_globals.inc` | game |
| `game_dialog.cpp` | domains/dialog |
| `game_dialog.h` | domains/dialog |
| `game_dialog_content.inc` | domains/dialog |
| `game_dialog_operations.inc` | domains/dialog |
| `game_ptr_hook.h` | patch |
| `game_state.cpp` | game |
| `game_state.h` | game |
| `game_system.cpp` | domains/system |
| `game_system.h` | domains/system |

**重构期 `core/native/` 覆盖（`arch_common._CORE_OVERRIDES`）**

| 当前文件名 | 目标层 |
| --- | --- |
| `call_patch.cpp` | patch |
| `call_patch.h` | patch |
| `extension_bag_port.h` | contracts |
| `game_feature.cpp` | game |
| `game_feature.h` | game |
| `game_nav.cpp` | domains/world |
| `game_nav.h` | domains/world |
| `game_variant.cpp` | game |
| `game_variant.h` | game |
| `inventory_trade.cpp` | domains/inventory |
| `inventory_trade.h` | domains/inventory |
| `module_save.cpp` | domains/save |
| `module_save.h` | domains/save |
| `module_save_port.h` | contracts |
| `sell_price.h` | domains/inventory |
| `stack_codec.h` | game |
| `stack_limit_port.h` | contracts |

**重构期 `api/native/` 文件名前缀 → 域（`arch_common._DOMAIN_BY_STEM`）**

| 当前文件名前缀 | 目标层 |
| --- | --- |
| `game_character*` | domains/character |
| `game_inventory*` | domains/inventory |
| `game_party*` | domains/party |
| `game_quest*` | domains/quest |
| `game_save*` | domains/save |
| `game_save_preflight*` | domains/save |
| `game_shop*` | domains/shop |
| `game_ui*` | domains/ui |
| `game_world*` | domains/world |

> 说明：`_TARGET_PREFIXES` 为有序元组，`features/` 必须先于 `feature/` 匹配；
> 其余无法映射的路径落入 `unknown`（由 `check_layers` 以警告上报）。

## 2. 跨层依赖与 include 环

### 2.1 跨层依赖对 Top 15

跨层依赖对共 130 个；下表按计数降序、同计数按层名升序取前 15 个。

| 源层 | 目标层 | 计数 |
| --- | --- | --- |
| `features/ui` | `game` | 24 |
| `features/ui` | `infra` | 12 |
| `infra` | `game` | 12 |
| `features/patch` | `game` | 11 |
| `features/extension_bag` | `game` | 10 |
| `host` | `infra` | 10 |
| `domains/inventory` | `game` | 9 |
| `features/autosell` | `game` | 8 |
| `domains/world` | `game` | 7 |
| `features/extension_bag` | `contracts` | 7 |
| `host` | `features/ui` | 7 |
| `features/ui` | `patch` | 6 |
| `host` | `features/autosell` | 6 |
| `domains/save` | `infra` | 5 |
| `domains/world` | `infra` | 5 |

### 2.2 include 环

当前无 include 环。

## 3. 基线债务

基线共 3 组、124 条。

### layer_direction（71 条）

- `layer_direction|api/native/game_character.cpp|api/native/game_inventory.h|domains/character → domains/inventory`
- `layer_direction|api/native/game_character.cpp|api/native/game_world.h|domains/character → domains/world`
- `layer_direction|api/native/game_party.cpp|api/native/game_character.h|domains/party → domains/character`
- `layer_direction|api/native/game_party.cpp|api/native/game_world.h|domains/party → domains/world`
- `layer_direction|api/native/game_save.cpp|feature/ui/game_ui_savebackup.h|domains/save → features/ui`
- `layer_direction|api/native/game_ui.cpp|api/native/game_world.h|domains/ui → domains/world`
- `layer_direction|core/native/frame_host.cpp|core/native/call_patch.h|infra → patch`
- `layer_direction|core/native/frame_host.cpp|data/native/game_symbols.h|infra → game`
- `layer_direction|core/native/frame_host.cpp|game_access.h|infra → game`
- `layer_direction|core/native/frame_task.cpp|game_system.h|infra → domains/system`
- `layer_direction|core/native/game_cache.cpp|api/native/game_character.h|infra → domains/character`
- `layer_direction|core/native/game_cache.cpp|api/native/game_inventory.h|infra → domains/inventory`
- `layer_direction|core/native/game_cache.cpp|api/native/game_party.h|infra → domains/party`
- `layer_direction|core/native/game_cache.cpp|api/native/game_world.h|infra → domains/world`
- `layer_direction|core/native/game_cache.cpp|data/native/game_symbols.h|infra → game`
- `layer_direction|core/native/game_cache.cpp|game_access.h|infra → game`
- `layer_direction|core/native/game_cache.cpp|game_state.h|infra → game`
- `layer_direction|core/native/game_cache.cpp|game_system.h|infra → domains/system`
- `layer_direction|core/native/game_json.cpp|data/native/game_symbols.h|infra → game`
- `layer_direction|core/native/game_json.cpp|game_access.h|infra → game`
- …（其余 51 条省略）

### offsets_symbols_header（34 条）

- `offsets_symbols_header|core/native/frame_host.cpp|data/native/game_symbols.h|infra 层引用符号头`
- `offsets_symbols_header|core/native/game_cache.cpp|data/native/game_symbols.h|infra 层引用符号头`
- `offsets_symbols_header|core/native/game_json.cpp|data/native/game_symbols.h|infra 层引用符号头`
- `offsets_symbols_header|core/native/game_nav.cpp|data/native/game_symbols.h|domains/world 层引用符号头`
- `offsets_symbols_header|core/native/inventory_trade.cpp|data/native/game_symbols.h|domains/inventory 层引用符号头`
- `offsets_symbols_header|core/native/save_enter.cpp|data/native/game_symbols.h|infra 层引用符号头`
- `offsets_symbols_header|core/native/save_exit.cpp|data/native/game_symbols.h|infra 层引用符号头`
- `offsets_symbols_header|feature/attribute_range/game_ui_attr_range.cpp|data/native/game_symbols.h|features/attribute_range 层引用符号头`
- `offsets_symbols_header|feature/autosell/autosell_scan.cpp|data/native/game_symbols.h|features/autosell 层引用符号头`
- `offsets_symbols_header|feature/autosell/autosell_view.cpp|data/native/game_symbols.h|features/autosell 层引用符号头`
- `offsets_symbols_header|feature/craft_ui/craft_ui.cpp|data/native/game_symbols.h|features/craft_ui 层引用符号头`
- `offsets_symbols_header|feature/craft_ui/craft_ui_hooks.cpp|data/native/game_symbols.h|features/craft_ui 层引用符号头`
- `offsets_symbols_header|feature/custom_recipe/custom_recipe_api.cpp|data/native/game_symbols.h|features/custom_recipe 层引用符号头`
- `offsets_symbols_header|feature/custom_recipe/custom_recipe_table.cpp|data/native/game_symbols.h|features/custom_recipe 层引用符号头`
- `offsets_symbols_header|feature/extension_bag/extension_bag_geometry.cpp|data/native/game_symbols.h|features/extension_bag 层引用符号头`
- `offsets_symbols_header|feature/extension_bag/game_ui_virtbag.cpp|data/native/game_symbols.h|features/extension_bag 层引用符号头`
- `offsets_symbols_header|feature/patch/game_patch.cpp|data/native/game_symbols.h|features/patch 层引用符号头`
- `offsets_symbols_header|feature/patch/inventory_hook_stage4.cpp|data/native/game_symbols.h|features/patch 层引用符号头`
- `offsets_symbols_header|feature/push_all/push_all.cpp|data/native/game_symbols.h|features/push_all 层引用符号头`
- `offsets_symbols_header|feature/save_backup/save_backup.cpp|data/native/game_symbols.h|features/save_backup 层引用符号头`
- …（其余 14 条省略）

### resource_unknown_symbol（19 条）

- `resource_unknown_symbol|bridge/native/gamebridge.cpp:69||call patch 无法归属符号`
- `resource_unknown_symbol|bridge/native/gamebridge.cpp:70||call patch 无法归属符号`
- `resource_unknown_symbol|feature/extension_bag/extension_bag_equip.inc:362||ptr slot 无法归属符号`
- `resource_unknown_symbol|feature/extension_bag/extension_bag_store.inc:20||ptr slot 无法归属符号`
- `resource_unknown_symbol|feature/extension_bag/game_ui_virtbag.cpp:161||ptr slot 无法归属符号`
- `resource_unknown_symbol|feature/patch/game_patch.cpp:36||ptr slot 无法归属符号`
- `resource_unknown_symbol|feature/patch/game_patch.cpp:41||ptr slot 无法归属符号`
- `resource_unknown_symbol|feature/patch/game_patch_core.inc:75||call patch 无法归属符号`
- `resource_unknown_symbol|feature/patch/native_inventory_hook.cpp:1021||entry hook 无法归属符号`
- `resource_unknown_symbol|feature/patch/native_inventory_hook.cpp:1162||entry hook 无法归属符号`
- `resource_unknown_symbol|feature/save_backup/save_backup.cpp:58||ptr slot 无法归属符号`
- `resource_unknown_symbol|feature/simple_mode/simple_mode.cpp:240||entry hook 无法归属符号`
- `resource_unknown_symbol|feature/special_equip/special_equip.cpp:186||entry hook 无法归属符号`
- `resource_unknown_symbol|feature/ui/game_ui_custom.cpp:43||ptr slot 无法归属符号`
- `resource_unknown_symbol|feature/ui/game_ui_exp.cpp:25||ptr slot 无法归属符号`
- `resource_unknown_symbol|feature/ui/game_ui_settings.cpp:80||ptr slot 无法归属符号`
- `resource_unknown_symbol|feature/ui/native_choice.cpp:232||entry hook 无法归属符号`
- `resource_unknown_symbol|feature/ui/native_choice.cpp:255||entry hook 无法归属符号`
- `resource_unknown_symbol|feature/ui/native_choice.cpp:25||ptr slot 无法归属符号`

基线只允许收缩，新增违规即失败

## 4. 资源安装点清单

扫描合计：安装点 43 个 / 文件 24 个 / 去重符号 14 个 / 无法归属 19 个。

| 机制 | 文件数 | 安装点数 | 去重符号数 |
| --- | --- | --- | --- |
| entry hook | 9 | 12 | 6 |
| call patch | 7 | 20 | 9 |
| ptr slot | 9 | 11 | 2 |

去重符号 → 文件（前 14 条，共 14 条；`--verbose` 列全部）：

- `F_CHAR_MOVE_VMA` → `feature/push_all/push_all.cpp`
- `F_GAMESTATE_DRAWPLAY_VMA` → `core/native/frame_host.cpp`
- `F_GAMESTATE_SET_STATE_VMA` → `core/native/save_exit.cpp`
- `F_INVEN_FIND_SAVE_SLOT_VMA` → `feature/patch/game_patch_core.inc`
- `F_MAINPROCESS_VMA` → `core/native/frame_host.cpp`
- `F_MATH_GET_RANDOM_VMA` → `feature/attribute_range/game_ui_attr_range.cpp`
- `F_MEMORYTEXT_GET_TEXT_VMA` → `feature/ui/module_text.cpp`
- `F_SAVESLOT_SLOT_BUTTON_EXE_VMA` → `core/native/save_enter.cpp`
- `F_SAVE_SAVE_INVENTORY_VMA` → `feature/extension_bag/extension_bag_lifecycle.inc`
- `F_SCENE_DRAW_EQUIP_VMA` → `feature/ui/game_ui_autosell.cpp`
- `F_SCENE_DRAW_POPUP_SC_CHARACTER_INFO_VMA` → `feature/ui/game_ui_charinfo_zh.cpp`
- `F_SELECTCHAR_BUTTON_START_EXE_VMA` → `core/native/save_enter.cpp`
- `F_UIEQUIP_BUTTON_USE_EXE_VMA` → `feature/extension_bag/extension_bag_equip.inc`
- `F_UIMIX_BUTTON_INVEN_ITEM_SELECT_EXE_VMA` → `feature/craft_ui/craft_ui_hooks.cpp`

## 5. JNI 面

- Kotlin `external fun`：139 个
- native 导出：139 个
- 差异条数：0 条

差异样例（前 0 条，共 0 条）：

（无）

## 6. 功能域清单

`feature/*` 共 13 个域。

| 域 | 目标层 | 文件数 |
| --- | --- | --- |
| `attribute_range` | `features/attribute_range` | 4 |
| `autosell` | `features/autosell` | 10 |
| `craft_ui` | `features/craft_ui` | 4 |
| `custom_recipe` | `features/custom_recipe` | 8 |
| `extension_bag` | `features/extension_bag` | 32 |
| `patch` | `features/patch` | 12 |
| `push_all` | `features/push_all` | 2 |
| `quest` | `features/quest` | 2 |
| `save_backup` | `features/save_backup` | 4 |
| `simple_mode` | `features/simple_mode` | 4 |
| `special_equip` | `features/special_equip` | 2 |
| `ui` | `features/ui` | 33 |
| `world_teleport` | `features/world_teleport` | 4 |

## 校验入口

```
uv run python scripts/maintenance/check_arch.py
```

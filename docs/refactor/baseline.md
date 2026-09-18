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
  - `module/app/src/main/cpp/plugins/*/`

## 1. 层分布与迁移期映射

### 1.1 层分布

| 目标层 | 文件数 |
| --- | --- |
| `contracts` | 15 |
| `domains/attribute_range` | 2 |
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
| `game` | 51 |
| `hooks` | 10 |
| `host` | 28 |
| `infra` | 47 |
| `patch` | 9 |
| `plugins/attribute_range` | 4 |
| `plugins/autosell` | 12 |
| `plugins/craft_ui` | 4 |
| `plugins/custom_recipe` | 10 |
| `plugins/extension_bag` | 32 |
| `plugins/inventory_hooks` | 2 |
| `plugins/native_choice` | 4 |
| `plugins/patch` | 9 |
| `plugins/push_all` | 6 |
| `plugins/quest` | 4 |
| `plugins/save_backup` | 6 |
| `plugins/simple_mode` | 6 |
| `plugins/special_equip` | 6 |
| `plugins/stack_limit` | 2 |
| `plugins/text_layer` | 4 |
| `plugins/ui` | 17 |
| `plugins/world_teleport` | 6 |
| **合计** | **338** |

### 1.2 迁移期映射摘要

映射常量直接 `import arch_common` 读取私有表（同目录模块，脚本只读、不修改该库），
避免在本文档复制一份映射形成第二事实源。

**新架构目标目录（`arch_common._TARGET_PREFIXES`，按序匹配）**

| 当前路径前缀 | 目标层 |
| --- | --- |
| `contracts/` | contracts |
| `domains/` | domains |
| `game/` | game |
| `hooks/` | hooks |
| `host/` | host |
| `infra/` | infra |
| `patch/` | patch |
| `plugins/` | plugins |

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

> 说明：`_TARGET_PREFIXES` 为有序元组，`plugins/` 为目标插件层前缀；
> legacy `feature/` 经重构期映射归入 `plugins/<id>`（待迁移分列）；
> 其余无法映射的路径落入 `unknown`（由 `check_layers` 以警告上报）。

## 2. 跨层依赖与 include 环

### 2.1 跨层依赖对 Top 15

跨层依赖对共 146 个；下表按计数降序、同计数按层名升序取前 15 个。

| 源层 | 目标层 | 计数 |
| --- | --- | --- |
| `host` | `infra` | 32 |
| `plugins/ui` | `game` | 16 |
| `host` | `game` | 14 |
| `host` | `contracts` | 12 |
| `plugins/extension_bag` | `game` | 11 |
| `plugins/patch` | `game` | 11 |
| `infra` | `game` | 10 |
| `plugins/autosell` | `infra` | 10 |
| `domains/inventory` | `game` | 9 |
| `host` | `patch` | 9 |
| `domains/world` | `game` | 7 |
| `host` | `hooks` | 7 |
| `plugins/custom_recipe` | `contracts` | 7 |
| `plugins/custom_recipe` | `infra` | 7 |
| `plugins/extension_bag` | `contracts` | 7 |

### 2.2 include 环

当前无 include 环。

## 3. 基线债务

基线共 3 组、68 条。

### layer_direction（43 条）

- `layer_direction|api/native/game_character.cpp|api/native/game_inventory.h|domains/character → domains/inventory`
- `layer_direction|api/native/game_character.cpp|api/native/game_world.h|domains/character → domains/world`
- `layer_direction|api/native/game_party.cpp|api/native/game_character.h|domains/party → domains/character`
- `layer_direction|api/native/game_party.cpp|api/native/game_world.h|domains/party → domains/world`
- `layer_direction|api/native/game_save.cpp|feature/ui/game_ui_savebackup.h|domains/save → plugins/ui`
- `layer_direction|api/native/game_ui.cpp|api/native/game_world.h|domains/ui → domains/world`
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
- `layer_direction|core/native/game_variant.cpp|core/native/qol_log.h|game → infra`
- `layer_direction|core/native/save_enter.cpp|core/native/call_patch.h|infra → patch`
- `layer_direction|core/native/save_enter.cpp|data/native/game_symbols.h|infra → game`
- …（其余 23 条省略）

### offsets_symbols_header（19 条）

- `offsets_symbols_header|core/native/game_cache.cpp|data/native/game_symbols.h|infra 层引用符号头`
- `offsets_symbols_header|core/native/game_json.cpp|data/native/game_symbols.h|infra 层引用符号头`
- `offsets_symbols_header|core/native/game_nav.cpp|data/native/game_symbols.h|domains/world 层引用符号头`
- `offsets_symbols_header|core/native/inventory_trade.cpp|data/native/game_symbols.h|domains/inventory 层引用符号头`
- `offsets_symbols_header|core/native/save_enter.cpp|data/native/game_symbols.h|infra 层引用符号头`
- `offsets_symbols_header|core/native/save_exit.cpp|data/native/game_symbols.h|infra 层引用符号头`
- `offsets_symbols_header|feature/extension_bag/extension_bag_geometry.cpp|data/native/game_symbols.h|plugins/extension_bag 层引用符号头`
- `offsets_symbols_header|feature/extension_bag/game_ui_virtbag.cpp|data/native/game_symbols.h|plugins/extension_bag 层引用符号头`
- `offsets_symbols_header|feature/patch/game_patch.cpp|data/native/game_symbols.h|plugins/patch 层引用符号头`
- `offsets_symbols_header|feature/patch/inventory_hook_stage4.cpp|data/native/game_symbols.h|plugins/patch 层引用符号头`
- `offsets_symbols_header|feature/ui/game_ui_autosell.cpp|data/native/game_symbols.h|plugins/ui 层引用符号头`
- `offsets_symbols_header|feature/ui/game_ui_charinfo_zh.cpp|data/native/game_symbols.h|plugins/ui 层引用符号头`
- `offsets_symbols_header|feature/ui/game_ui_components.cpp|data/native/game_symbols.h|plugins/ui 层引用符号头`
- `offsets_symbols_header|feature/ui/game_ui_kit.cpp|data/native/game_symbols.h|plugins/ui 层引用符号头`
- `offsets_symbols_header|feature/ui/game_ui_savebackup.cpp|data/native/game_symbols.h|plugins/ui 层引用符号头`
- `offsets_symbols_header|feature/ui/game_ui_settings.cpp|data/native/game_symbols.h|plugins/ui 层引用符号头`
- `offsets_symbols_header|feature/ui/native_choice.cpp|data/native/game_symbols.h|plugins/ui 层引用符号头`
- `offsets_symbols_header|plugins/custom_recipe/feature.cpp|data/native/game_symbols.h|plugins/custom_recipe 层引用符号头`
- `offsets_symbols_header|plugins/custom_recipe/table.cpp|data/native/game_symbols.h|plugins/custom_recipe 层引用符号头`

### resource_unknown_symbol（6 条）

- `resource_unknown_symbol|feature/extension_bag/extension_bag_equip.inc:362||ptr slot 无法归属符号`
- `resource_unknown_symbol|feature/extension_bag/game_ui_virtbag.cpp:161||ptr slot 无法归属符号`
- `resource_unknown_symbol|feature/ui/game_ui_settings.cpp:80||ptr slot 无法归属符号`
- `resource_unknown_symbol|feature/ui/native_choice.cpp:232||entry hook 无法归属符号`
- `resource_unknown_symbol|feature/ui/native_choice.cpp:255||entry hook 无法归属符号`
- `resource_unknown_symbol|feature/ui/native_choice.cpp:25||ptr slot 无法归属符号`

基线只允许收缩，新增违规即失败

## 4. 资源安装点清单

扫描合计：安装点 17 个 / 文件 10 个 / 去重符号 9 个 / 无法归属 3 个。

| 机制 | 文件数 | 安装点数 | 去重符号数 |
| --- | --- | --- | --- |
| entry hook | 3 | 5 | 3 |
| call patch | 3 | 7 | 4 |
| ptr slot | 4 | 5 | 3 |

去重符号 → 文件（前 9 条，共 9 条；`--verbose` 列全部）：

- `F_GAMESTATE_SET_STATE_VMA` → `core/native/save_exit.cpp`
- `F_SAVESLOT_SLOT_BUTTON_EXE_VMA` → `core/native/save_enter.cpp`
- `F_SAVE_SAVE_INVENTORY_VMA` → `feature/extension_bag/extension_bag_lifecycle.inc`
- `F_SCENE_DRAW_EQUIP_VMA` → `feature/ui/game_ui_autosell.cpp`
- `F_SCENE_DRAW_POPUP_SC_CHARACTER_INFO_VMA` → `feature/ui/game_ui_charinfo_zh.cpp`
- `F_SELECTCHAR_BUTTON_START_EXE_VMA` → `core/native/save_enter.cpp`
- `F_UIEQUIP_BUTTON_USE_EXE_VMA` → `feature/extension_bag/extension_bag_equip.inc`
- `F_UIEQUIP_INVEN_ITEM_CONTROL_EVENT_PROC_VMA` → `feature/patch/game_patch.cpp`
- `F_UISTORE_SELL_ITEM_VMA` → `feature/extension_bag/extension_bag_store.inc`

## 5. JNI 面

- Kotlin `external fun`：143 个
- native 导出：143 个
- 差异条数：0 条

差异样例（前 0 条，共 0 条）：

（无）

## 6. 功能域清单

旧 `feature/*`（待迁移 3 个）与已迁移 `plugins/*`（14 个）共 17 个域。

### 6.1 旧 `feature/*`（待迁移，3）

| 域 | 目标层 | 文件数 |
| --- | --- | --- |
| `extension_bag` | `plugins/extension_bag` | 32 |
| `patch` | `plugins/patch` | 9 |
| `ui` | `plugins/ui` | 17 |

### 6.2 已迁移 `plugins/*`（14）

| 域 | 目标层 | 文件数 |
| --- | --- | --- |
| `attribute_range` | `plugins/attribute_range` | 4 |
| `autosell` | `plugins/autosell` | 12 |
| `craft_ui` | `plugins/craft_ui` | 4 |
| `custom_recipe` | `plugins/custom_recipe` | 10 |
| `inventory_hooks` | `plugins/inventory_hooks` | 2 |
| `native_choice` | `plugins/native_choice` | 4 |
| `push_all` | `plugins/push_all` | 6 |
| `quest` | `plugins/quest` | 4 |
| `save_backup` | `plugins/save_backup` | 6 |
| `simple_mode` | `plugins/simple_mode` | 6 |
| `special_equip` | `plugins/special_equip` | 6 |
| `stack_limit` | `plugins/stack_limit` | 2 |
| `text_layer` | `plugins/text_layer` | 4 |
| `world_teleport` | `plugins/world_teleport` | 6 |

## 校验入口

```
uv run python scripts/maintenance/check_arch.py
```

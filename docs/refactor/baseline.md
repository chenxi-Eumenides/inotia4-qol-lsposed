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
| `contracts` | 54 |
| `game` | 105 |
| `hooks` | 10 |
| `host` | 44 |
| `infra` | 39 |
| `legacy/character` | 2 |
| `legacy/dialog` | 4 |
| `legacy/inventory` | 8 |
| `legacy/party` | 2 |
| `legacy/quest` | 2 |
| `legacy/save` | 3 |
| `legacy/shop` | 2 |
| `legacy/system` | 2 |
| `legacy/ui` | 5 |
| `legacy/world` | 7 |
| `patch` | 9 |
| `plugins/attribute_quality` | 6 |
| `plugins/attribute_range_display` | 4 |
| `plugins/autosell` | 17 |
| `plugins/core.api` | 5 |
| `plugins/core.character` | 5 |
| `plugins/core.choice` | 4 |
| `plugins/core.craft` | 2 |
| `plugins/core.item` | 7 |
| `plugins/core.save` | 2 |
| `plugins/core.system` | 3 |
| `plugins/core.ui` | 17 |
| `plugins/core.world` | 3 |
| `plugins/craft_ui` | 4 |
| `plugins/custom_recipe` | 10 |
| `plugins/device_probe_consumer` | 2 |
| `plugins/device_probe_provider` | 2 |
| `plugins/extension_bag` | 41 |
| `plugins/inventory_hooks` | 11 |
| `plugins/push_all` | 6 |
| `plugins/quest` | 5 |
| `plugins/save_backup` | 13 |
| `plugins/simple_mode` | 6 |
| `plugins/special_equip` | 6 |
| `plugins/stack_limit` | 3 |
| `plugins/text_layer` | 6 |
| `plugins/world_teleport` | 6 |
| **合计** | **494** |

### 1.2 迁移期映射摘要

映射常量直接 `import arch_common` 读取私有表（同目录模块，脚本只读、不修改该库），
避免在本文档复制一份映射形成第二事实源。

**新架构目标目录（`arch_common._TARGET_PREFIXES`，按序匹配）**

| 当前路径前缀 | 目标层 |
| --- | --- |
| `contracts/` | contracts |
| `game/` | game |
| `hooks/` | hooks |
| `host/` | host |
| `infra/` | infra |
| `patch/` | patch |
| `plugins/` | plugins |

**重构期根目录覆盖（`arch_common._ROOT_OVERRIDES`）**

| 当前文件（cpp 根） | 目标层 |
| --- | --- |
| `game_dialog.cpp` | legacy/dialog |
| `game_dialog.h` | legacy/dialog |
| `game_dialog_content.inc` | legacy/dialog |
| `game_dialog_operations.inc` | legacy/dialog |
| `game_system.cpp` | legacy/system |
| `game_system.h` | legacy/system |

**重构期 `api/native/` 文件名前缀 → 迁移期标签（`arch_common._LEGACY_API_BY_STEM`）**

| 当前文件名前缀 | 迁移期标签 |
| --- | --- |
| `game_character*` | legacy/character |
| `game_inventory*` | legacy/inventory |
| `game_party*` | legacy/party |
| `game_quest*` | legacy/quest |
| `game_save*` | legacy/save |
| `game_shop*` | legacy/shop |
| `game_ui*` | legacy/ui |
| `game_world*` | legacy/world |

> 说明：`_TARGET_PREFIXES` 为有序元组，`plugins/` 为目标插件层前缀；
> legacy `feature/` 经重构期映射归入 `plugins/<id>`（待迁移分列）；
> 七个目标目录内无法映射的路径由 `check_layers` 硬失败；其他 legacy 路径落入 `unknown` 并以警告上报。

## 2. 跨层依赖与 include 环

### 2.1 跨层依赖对 Top 15

跨层依赖对共 120 个；下表按计数降序、同计数按层名升序取前 15 个。

| 源层 | 目标层 | 计数 |
| --- | --- | --- |
| `host` | `game` | 49 |
| `game` | `contracts` | 40 |
| `host` | `infra` | 40 |
| `host` | `contracts` | 38 |
| `plugins/extension_bag` | `contracts` | 34 |
| `plugins/autosell` | `contracts` | 23 |
| `plugins/custom_recipe` | `contracts` | 20 |
| `plugins/inventory_hooks` | `contracts` | 16 |
| `plugins/save_backup` | `contracts` | 14 |
| `plugins/core.ui` | `contracts` | 12 |
| `host` | `hooks` | 10 |
| `host` | `patch` | 10 |
| `legacy/system` | `game` | 9 |
| `infra` | `contracts` | 8 |
| `infra` | `game` | 8 |

### 2.2 include 环

当前无 include 环。

## 3. 基线债务

基线共 3 组、45 条。

### layer_direction（32 条）

- `layer_direction|api/native/game_character.cpp|api/native/game_inventory.h|legacy/character → legacy/inventory`
- `layer_direction|api/native/game_character.cpp|api/native/game_world.h|legacy/character → legacy/world`
- `layer_direction|api/native/game_party.cpp|api/native/game_character.h|legacy/party → legacy/character`
- `layer_direction|api/native/game_party.cpp|api/native/game_world.h|legacy/party → legacy/world`
- `layer_direction|api/native/game_ui.cpp|api/native/game_world.h|legacy/ui → legacy/world`
- `layer_direction|core/native/game_cache.cpp|api/native/game_character.h|infra → legacy/character`
- `layer_direction|core/native/game_cache.cpp|api/native/game_inventory.h|infra → legacy/inventory`
- `layer_direction|core/native/game_cache.cpp|api/native/game_party.h|infra → legacy/party`
- `layer_direction|core/native/game_cache.cpp|api/native/game_world.h|infra → legacy/world`
- `layer_direction|core/native/game_cache.cpp|game/access/game_access.h|infra → game`
- `layer_direction|core/native/game_cache.cpp|game/state/game_state.h|infra → game`
- `layer_direction|core/native/game_cache.cpp|game/symbols/game_symbols.h|infra → game`
- `layer_direction|core/native/game_cache.cpp|game_system.h|infra → legacy/system`
- `layer_direction|game/access/game_access.cpp|game_system.h|game → legacy/system`
- `layer_direction|game/access/game_access.cpp|infra/log/qol_log.h|game → infra`
- `layer_direction|game/json/game_json.cpp|game/access/game_access.h|infra → game`
- `layer_direction|game/json/game_json.cpp|game/symbols/game_symbols.h|infra → game`
- `layer_direction|game/read/game_tiles.cpp|infra/log/qol_log.h|game → infra`
- `layer_direction|game/state/game_state.cpp|api/native/game_ui.h|game → legacy/ui`
- `layer_direction|game/state/game_state.cpp|infra/frame/frame_task.h|game → infra`
- …（其余 12 条省略）

### offsets_symbols_header（11 条）

- `offsets_symbols_header|core/native/game_cache.cpp|game/symbols/game_symbols.h|infra 层引用符号头`
- `offsets_symbols_header|core/native/inventory_trade.cpp|game/symbols/game_symbols.h|legacy/inventory 层引用符号头`
- `offsets_symbols_header|game/json/game_json.cpp|game/symbols/game_symbols.h|infra 层引用符号头`
- `offsets_symbols_header|game/nav/game_nav.cpp|game/symbols/game_symbols.h|legacy/world 层引用符号头`
- `offsets_symbols_header|infra/save/save_enter.cpp|game/symbols/game_symbols.h|infra 层引用符号头`
- `offsets_symbols_header|infra/save/save_exit.cpp|game/symbols/game_symbols.h|infra 层引用符号头`
- `offsets_symbols_header|plugins/custom_recipe/feature.cpp|game/symbols/game_symbols.h|plugins/custom_recipe 层引用符号头`
- `offsets_symbols_header|plugins/custom_recipe/table.cpp|game/symbols/game_symbols.h|plugins/custom_recipe 层引用符号头`
- `offsets_symbols_header|plugins/extension_bag/extension_bag_geometry.cpp|game/symbols/game_symbols.h|plugins/extension_bag 层引用符号头`
- `offsets_symbols_header|plugins/extension_bag/game_ui_virtbag.cpp|game/symbols/game_symbols.h|plugins/extension_bag 层引用符号头`
- `offsets_symbols_header|plugins/inventory_hooks/inventory_hook_stage4.cpp|game/symbols/game_symbols.h|plugins/inventory_hooks 层引用符号头`

### resource_unknown_symbol（2 条）

- `resource_unknown_symbol|plugins/extension_bag/extension_bag_equip.inc:374||ptr slot 无法归属符号`
- `resource_unknown_symbol|plugins/extension_bag/game_ui_virtbag.cpp:150||ptr slot 无法归属符号`

基线只允许收缩，新增违规即失败

## 4. 资源安装点清单

扫描合计：安装点 6 个 / 文件 2 个 / 去重符号 3 个 / 无法归属 0 个。

| 机制 | 文件数 | 安装点数 | 去重符号数 |
| --- | --- | --- | --- |
| entry hook | 0 | 0 | 0 |
| call patch | 2 | 6 | 3 |
| ptr slot | 0 | 0 | 0 |

去重符号 → 文件（前 3 条，共 3 条；`--verbose` 列全部）：

- `F_GAMESTATE_SET_STATE_VMA` → `infra/save/save_exit.cpp`
- `F_SAVESLOT_SLOT_BUTTON_EXE_VMA` → `infra/save/save_enter.cpp`
- `F_SELECTCHAR_BUTTON_START_EXE_VMA` → `infra/save/save_enter.cpp`

## 5. JNI 面

- Kotlin `external fun`：150 个
- native 导出：150 个
- 差异条数：0 条

差异样例（前 0 条，共 0 条）：

（无）

## 6. 功能域清单

旧 `feature/*`（待迁移 0 个）与已迁移 `plugins/*`（26 个）共 26 个域。

### 6.1 旧 `feature/*`（待迁移，0）

| 域 | 目标层 | 文件数 |
| --- | --- | --- |

### 6.2 已迁移 `plugins/*`（26）

| 域 | 目标层 | 文件数 |
| --- | --- | --- |
| `attribute_quality` | `plugins/attribute_quality` | 6 |
| `attribute_range_display` | `plugins/attribute_range_display` | 4 |
| `autosell` | `plugins/autosell` | 17 |
| `core.api` | `plugins/core.api` | 5 |
| `core.character` | `plugins/core.character` | 5 |
| `core.choice` | `plugins/core.choice` | 4 |
| `core.craft` | `plugins/core.craft` | 2 |
| `core.item` | `plugins/core.item` | 7 |
| `core.save` | `plugins/core.save` | 2 |
| `core.system` | `plugins/core.system` | 3 |
| `core.ui` | `plugins/core.ui` | 17 |
| `core.world` | `plugins/core.world` | 3 |
| `craft_ui` | `plugins/craft_ui` | 4 |
| `custom_recipe` | `plugins/custom_recipe` | 10 |
| `device_probe_consumer` | `plugins/device_probe_consumer` | 2 |
| `device_probe_provider` | `plugins/device_probe_provider` | 2 |
| `extension_bag` | `plugins/extension_bag` | 41 |
| `inventory_hooks` | `plugins/inventory_hooks` | 11 |
| `push_all` | `plugins/push_all` | 6 |
| `quest` | `plugins/quest` | 5 |
| `save_backup` | `plugins/save_backup` | 13 |
| `simple_mode` | `plugins/simple_mode` | 6 |
| `special_equip` | `plugins/special_equip` | 6 |
| `stack_limit` | `plugins/stack_limit` | 3 |
| `text_layer` | `plugins/text_layer` | 6 |
| `world_teleport` | `plugins/world_teleport` | 6 |

## 校验入口

```
uv run python scripts/maintenance/check_arch.py
```

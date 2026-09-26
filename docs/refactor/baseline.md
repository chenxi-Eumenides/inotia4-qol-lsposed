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
| `contracts` | 66 |
| `game` | 137 |
| `hooks` | 10 |
| `host` | 51 |
| `infra` | 39 |
| `legacy/inventory` | 2 |
| `legacy/save` | 1 |
| `legacy/system` | 2 |
| `patch` | 9 |
| `plugins/attribute_quality` | 6 |
| `plugins/attribute_range_display` | 4 |
| `plugins/autosell` | 17 |
| `plugins/character_panel_i18n` | 6 |
| `plugins/core.api` | 5 |
| `plugins/core.character` | 5 |
| `plugins/core.craft` | 2 |
| `plugins/core.custom_ui` | 10 |
| `plugins/core.data` | 4 |
| `plugins/core.feature` | 2 |
| `plugins/core.item` | 7 |
| `plugins/core.quest` | 5 |
| `plugins/core.save` | 2 |
| `plugins/core.settings` | 15 |
| `plugins/core.system` | 3 |
| `plugins/core.ui_state` | 2 |
| `plugins/core.world` | 3 |
| `plugins/craft_ui` | 4 |
| `plugins/custom_recipe` | 11 |
| `plugins/device_probe_consumer` | 2 |
| `plugins/device_probe_export_failure` | 2 |
| `plugins/device_probe_provider` | 2 |
| `plugins/extension_bag` | 41 |
| `plugins/inventory_hooks` | 11 |
| `plugins/push_all` | 6 |
| `plugins/save_backup` | 13 |
| `plugins/simple_mode` | 6 |
| `plugins/special_equip` | 6 |
| `plugins/stack_limit` | 3 |
| `plugins/world_teleport` | 6 |
| **合计** | **528** |

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

> 说明：`_TARGET_PREFIXES` 为有序元组，`plugins/` 为目标插件层前缀；
> legacy `feature/` 经重构期映射归入 `plugins/<id>`（待迁移分列）；
> 七个目标目录内无法映射的路径由 `check_layers` 硬失败；其他 legacy 路径落入 `unknown` 并以警告上报。

## 2. 跨层依赖与 include 环

### 2.1 跨层依赖对 Top 15

跨层依赖对共 89 个；下表按计数降序、同计数按层名升序取前 15 个。

| 源层 | 目标层 | 计数 |
| --- | --- | --- |
| `host` | `game` | 89 |
| `game` | `contracts` | 71 |
| `host` | `contracts` | 56 |
| `host` | `infra` | 47 |
| `plugins/extension_bag` | `contracts` | 35 |
| `plugins/autosell` | `contracts` | 23 |
| `plugins/custom_recipe` | `contracts` | 21 |
| `legacy/system` | `game` | 17 |
| `plugins/inventory_hooks` | `contracts` | 16 |
| `plugins/core.item` | `contracts` | 14 |
| `plugins/save_backup` | `contracts` | 14 |
| `plugins/core.settings` | `contracts` | 13 |
| `plugins/core.custom_ui` | `contracts` | 11 |
| `host` | `hooks` | 10 |
| `host` | `patch` | 10 |

### 2.2 include 环

当前无 include 环。

## 3. 基线债务

基线共 2 组、21 条。

### layer_direction（17 条）

- `layer_direction|core/native/game_cache.cpp|game/access/game_access.h|infra → game`
- `layer_direction|core/native/game_cache.cpp|game/state/game_state.h|infra → game`
- `layer_direction|core/native/game_cache.cpp|game/symbols/game_symbols.h|infra → game`
- `layer_direction|core/native/game_cache.cpp|game_system.h|infra → legacy/system`
- `layer_direction|game/access/game_access.cpp|game_system.h|game → legacy/system`
- `layer_direction|game/access/game_access.cpp|infra/log/qol_log.h|game → infra`
- `layer_direction|game/read/game_tiles.cpp|infra/log/qol_log.h|game → infra`
- `layer_direction|game/state/game_state.cpp|infra/frame/frame_task.h|game → infra`
- `layer_direction|game/variants/game_variant.cpp|infra/log/qol_log.h|game → infra`
- `layer_direction|infra/frame/frame_task.cpp|game_system.h|infra → legacy/system`
- `layer_direction|infra/save/save_enter.cpp|game/access/game_access.h|infra → game`
- `layer_direction|infra/save/save_enter.cpp|game/state/game_state.h|infra → game`
- `layer_direction|infra/save/save_enter.cpp|game/symbols/game_symbols.h|infra → game`
- `layer_direction|infra/save/save_enter.cpp|patch/call_patch.h|infra → patch`
- `layer_direction|infra/save/save_exit.cpp|game/access/game_access.h|infra → game`
- `layer_direction|infra/save/save_exit.cpp|game/symbols/game_symbols.h|infra → game`
- `layer_direction|infra/save/save_exit.cpp|patch/call_patch.h|infra → patch`

### offsets_symbols_header（4 条）

- `offsets_symbols_header|core/native/game_cache.cpp|game/symbols/game_symbols.h|infra 层引用符号头`
- `offsets_symbols_header|core/native/inventory_trade.cpp|game/symbols/game_symbols.h|legacy/inventory 层引用符号头`
- `offsets_symbols_header|infra/save/save_enter.cpp|game/symbols/game_symbols.h|infra 层引用符号头`
- `offsets_symbols_header|infra/save/save_exit.cpp|game/symbols/game_symbols.h|infra 层引用符号头`

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

- Kotlin `external fun`：156 个
- native 导出：156 个
- 差异条数：0 条

差异样例（前 0 条，共 0 条）：

（无）

## 6. 功能域清单

旧 `feature/*`（待迁移 0 个）与已迁移 `plugins/*`（30 个）共 30 个域。

### 6.1 旧 `feature/*`（待迁移，0）

| 域 | 目标层 | 文件数 |
| --- | --- | --- |

### 6.2 已迁移 `plugins/*`（30）

| 域 | 目标层 | 文件数 |
| --- | --- | --- |
| `attribute_quality` | `plugins/attribute_quality` | 6 |
| `attribute_range_display` | `plugins/attribute_range_display` | 4 |
| `autosell` | `plugins/autosell` | 17 |
| `character_panel_i18n` | `plugins/character_panel_i18n` | 6 |
| `core.api` | `plugins/core.api` | 5 |
| `core.character` | `plugins/core.character` | 5 |
| `core.craft` | `plugins/core.craft` | 2 |
| `core.custom_ui` | `plugins/core.custom_ui` | 10 |
| `core.data` | `plugins/core.data` | 4 |
| `core.feature` | `plugins/core.feature` | 2 |
| `core.item` | `plugins/core.item` | 7 |
| `core.quest` | `plugins/core.quest` | 5 |
| `core.save` | `plugins/core.save` | 2 |
| `core.settings` | `plugins/core.settings` | 15 |
| `core.system` | `plugins/core.system` | 3 |
| `core.ui_state` | `plugins/core.ui_state` | 2 |
| `core.world` | `plugins/core.world` | 3 |
| `craft_ui` | `plugins/craft_ui` | 4 |
| `custom_recipe` | `plugins/custom_recipe` | 11 |
| `device_probe_consumer` | `plugins/device_probe_consumer` | 2 |
| `device_probe_export_failure` | `plugins/device_probe_export_failure` | 2 |
| `device_probe_provider` | `plugins/device_probe_provider` | 2 |
| `extension_bag` | `plugins/extension_bag` | 41 |
| `inventory_hooks` | `plugins/inventory_hooks` | 11 |
| `push_all` | `plugins/push_all` | 6 |
| `save_backup` | `plugins/save_backup` | 13 |
| `simple_mode` | `plugins/simple_mode` | 6 |
| `special_equip` | `plugins/special_equip` | 6 |
| `stack_limit` | `plugins/stack_limit` | 3 |
| `world_teleport` | `plugins/world_teleport` | 6 |

## 校验入口

```
uv run python scripts/maintenance/check_arch.py
```

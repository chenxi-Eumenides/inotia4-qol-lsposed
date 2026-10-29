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
| `contracts` | 94 |
| `game` | 182 |
| `hooks` | 10 |
| `host` | 86 |
| `infra` | 42 |
| `legacy/inventory` | 2 |
| `legacy/save` | 1 |
| `legacy/system` | 2 |
| `patch` | 13 |
| `plugins/attribute_quality` | 6 |
| `plugins/attribute_range_display` | 4 |
| `plugins/autosell` | 31 |
| `plugins/character_panel_i18n` | 6 |
| `plugins/core.api` | 6 |
| `plugins/core.character` | 6 |
| `plugins/core.craft` | 3 |
| `plugins/core.custom_ui` | 29 |
| `plugins/core.data` | 5 |
| `plugins/core.feature` | 3 |
| `plugins/core.item` | 76 |
| `plugins/core.quest` | 6 |
| `plugins/core.save` | 13 |
| `plugins/core.system` | 4 |
| `plugins/core.ui_state` | 3 |
| `plugins/core.world` | 4 |
| `plugins/craft_lottery` | 8 |
| `plugins/custom_map` | 8 |
| `plugins/custom_recipe` | 19 |
| `plugins/eight_way_move` | 4 |
| `plugins/extension_bag` | 131 |
| `plugins/module_health` | 2 |
| `plugins/popup_blocker` | 2 |
| `plugins/push_all` | 8 |
| `plugins/save_backup` | 34 |
| `plugins/settings` | 14 |
| `plugins/special_equip` | 6 |
| `plugins/stack_limit` | 4 |
| `plugins/world_teleport` | 6 |
| `plugins/world_tuning` | 10 |
| **合计** | **893** |

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

跨层依赖对共 82 个；下表按计数降序、同计数按层名升序取前 15 个。

| 源层 | 目标层 | 计数 |
| --- | --- | --- |
| `plugins/extension_bag` | `contracts` | 183 |
| `host` | `game` | 118 |
| `game` | `contracts` | 105 |
| `plugins/core.item` | `contracts` | 95 |
| `host` | `contracts` | 73 |
| `host` | `infra` | 62 |
| `plugins/custom_recipe` | `contracts` | 44 |
| `plugins/save_backup` | `contracts` | 40 |
| `plugins/core.custom_ui` | `contracts` | 38 |
| `plugins/autosell` | `contracts` | 31 |
| `plugins/core.save` | `contracts` | 19 |
| `legacy/system` | `game` | 18 |
| `plugins/settings` | `contracts` | 18 |
| `host` | `patch` | 14 |
| `plugins/custom_map` | `contracts` | 13 |

### 2.2 include 环

当前无 include 环。

## 3. 基线债务

基线共 0 组、0 条。

（基线为空）

基线只允许收缩，新增违规即失败

## 4. 资源安装点清单

扫描合计：安装点 6 个 / 文件 2 个 / 去重符号 3 个 / 无法归属 0 个。

| 机制 | 文件数 | 安装点数 | 去重符号数 |
| --- | --- | --- | --- |
| entry hook | 0 | 0 | 0 |
| call patch | 2 | 6 | 3 |
| ptr slot | 0 | 0 | 0 |

去重符号 → 文件（前 3 条，共 3 条；`--verbose` 列全部）：

- `F_GAMESTATE_SET_STATE_VMA` → `patch/save_exit.cpp`
- `F_SAVESLOT_SLOT_BUTTON_EXE_VMA` → `patch/save_enter.cpp`
- `F_SELECTCHAR_BUTTON_START_EXE_VMA` → `patch/save_enter.cpp`

## 5. JNI 面

- Kotlin `external fun`：158 个
- native 导出：158 个
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
| `autosell` | `plugins/autosell` | 31 |
| `character_panel_i18n` | `plugins/character_panel_i18n` | 6 |
| `core.api` | `plugins/core.api` | 6 |
| `core.character` | `plugins/core.character` | 6 |
| `core.craft` | `plugins/core.craft` | 3 |
| `core.custom_ui` | `plugins/core.custom_ui` | 29 |
| `core.data` | `plugins/core.data` | 5 |
| `core.feature` | `plugins/core.feature` | 3 |
| `core.item` | `plugins/core.item` | 76 |
| `core.quest` | `plugins/core.quest` | 6 |
| `core.save` | `plugins/core.save` | 13 |
| `core.system` | `plugins/core.system` | 4 |
| `core.ui_state` | `plugins/core.ui_state` | 3 |
| `core.world` | `plugins/core.world` | 4 |
| `craft_lottery` | `plugins/craft_lottery` | 8 |
| `custom_map` | `plugins/custom_map` | 8 |
| `custom_recipe` | `plugins/custom_recipe` | 19 |
| `eight_way_move` | `plugins/eight_way_move` | 4 |
| `extension_bag` | `plugins/extension_bag` | 131 |
| `module_health` | `plugins/module_health` | 2 |
| `popup_blocker` | `plugins/popup_blocker` | 2 |
| `push_all` | `plugins/push_all` | 8 |
| `save_backup` | `plugins/save_backup` | 34 |
| `settings` | `plugins/settings` | 14 |
| `special_equip` | `plugins/special_equip` | 6 |
| `stack_limit` | `plugins/stack_limit` | 4 |
| `world_teleport` | `plugins/world_teleport` | 6 |
| `world_tuning` | `plugins/world_tuning` | 10 |

## 校验入口

```
uv run python scripts/maintenance/check_arch.py
```

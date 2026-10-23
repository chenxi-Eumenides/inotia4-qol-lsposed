# core.ui_state 统一状态接口（最终设计）

## 1. 模型

| 字段 | 说明 |
|---|---|
| `name` | 唯一状态名，程序的主键；含来源前缀 |
| `type` | 0 unknown / 1 boot / 2 menu / 3 world / 4 dialog / 5 panel |
| `source` | 0 unknown / 1 native / 2 module / 3 external / 4 android |
| `info` | 数据：`title` / `content`，由 `name` 决定是否存在 |
| `options` | 动作：`[{id,label}]`，无动作 `[]` |

- 不设 per-state `id`，`name` 即稳定标识。
- 不设 `sequence` / `frame` / `flags`；当前没有消费者需要版本或时间戳。
- `info` 与 `options` 同级：一个管“能获取什么”，一个管“能做什么”。

## 2. C++ capability（`ui.state` 1.2）

```cpp
constexpr uint32_t kUiStateMaxNameBytes = 64;

struct QolUiStateSummary {
    uint32_t struct_size;  // = sizeof(QolUiStateSummary)
    uint32_t type;
    uint32_t source;
    char     name[kUiStateMaxNameBytes];
};

struct QolUiStateTableRow {
    const char* name;      // 仅在回调期间有效
    uint32_t type;
    uint32_t source;
    uint32_t stability;    // 0 static, 1 session
};

struct QolUiStateRegistration {
    uint32_t struct_size;
    uint32_t type;         // 4 dialog / 5 panel
    char     key[64];      // panel_id 或 dialog_type_id
    char     plugin_id[64]; // 由插件从 PluginContext::plugin_id 复制（Host 注入）
};

struct QolUiExternalSnapshot {
    uint32_t struct_size;
    uint32_t active;
    char     name[kUiStateMaxNameBytes];  // "android_agreement"
};

using QolUiStateReadFn = QolResult (*)(QolUiStateSummary* out);
using QolUiStateTableVisitFn = void (*)(const QolUiStateTableRow* row, void* user);

struct UiStateApi {
    // v1.0 / v1.1 成员保持不变
    QolResult (*read_state)(QolUiStateSummary* out);
    QolResult (*visit_states)(QolUiStateTableVisitFn visit, void* user);
    QolResult (*register_state)(const QolUiStateRegistration* reg);
    QolResult (*unregister_state)(uint32_t type, const char* key);
    QolResult (*publish_external_ui)(const QolUiExternalSnapshot* snapshot);
};
```

语义：

- `read_state`：任意线程可调，返回当前顶层状态；原生快照不可用时返回
  `name="unknown"`、`type=0`、`source=0`。
- `visit_states`：枚举当前可用状态（原生 + 已注册模块/外部），按注册/表顺序；
  未安装插件不出现。
- `register_state`：插件 install 阶段调用；`key` 为 panel_id/dialog_type_id，`plugin_id` 由插件从
  `PluginContext::plugin_id`（Host 注入）复制，不接受自报未知身份。当前实现所有注册状态
  `source=2`（module）；`ext_*` 命名与 external 来源保留给外部插件加载器。重复 `name` 或
  非法 key → 注册失败。
- `unregister_state`：插件 deactivate 时调用，按 `type + key` 移除。
- `publish_external_ui`：native 能力已提供；当前 HTTP 层由 Kotlin `UiActivityTracker` 直接覆盖
  `android_agreement`，C++ `read_state` 尚未接入 JVM→native 发布入口（外部 UI 仍以 HTTP 为准）。
- `struct_size` 不匹配 → `kQolErrInvalidArgument`。
- 不返回 “not ready”：能力不可用由 Host 能力协商处理。

## 3. HTTP API

### 3.1 `GET /api/ui/state`

owner `core.ui_state`，`op_gated=0`，`lease_required=1`。

```json
{
  "name": "game_story",
  "type": 4,
  "source": 1,
  "info": {
    "title": "凯恩",
    "content": "……"
  },
  "options": [
    {"id": "next", "label": "下一句"},
    {"id": "skip", "label": "跳过"}
  ]
}
```

- `info` 始终是对象；无字段时为 `{}`。
- `options` 始终是数组；无动作时为 `[]`。
- 不输出不存在的键，不使用 `null` 字段。
- 原生读取失败：`{"name":"unknown","type":0,"source":0,"info":{},"options":[]}`。
- 模块未就绪：503 `not ready`（既有 `ControllerGuard`）。

### 3.2 `GET /api/ui/state/table`

```json
{
  "states": [
    {"name": "world", "type": 3, "source": 1, "stability": "static"},
    {"name": "module_panel_autosell", "type": 5, "source": 2, "stability": "static"},
    {"name": "ext_panel_vendor_inventory", "type": 5, "source": 3, "stability": "session"}
  ]
}
```

- `stability`：`static`（原生/内置模块，跨构建不变）/ `session`（外部插件，仅本进程）。
- 程序启动时拉一次，按 `name` 建立可用集合；不依赖 id。
- 未安装插件不出现。

### 3.3 旧端点迁移

删除 `GET /api/ui`、`GET /api/ui/screen`、`GET /api/ui/panel`、`GET /api/ui/dialog`；不保留 legacy 路由。当前状态统一使用 `GET /api/ui/state`，状态枚举使用 `GET /api/ui/state/table`。五个 POST action 保持原路径、请求与响应/票据契约。

## 4. 状态表

### 4.1 基础态与 Android

| name | type | source | info | options |
|---|---:|---:|---|---|
| `unknown` | 0 | 0 | `{}` | `[]` |
| `loading` | 1 | 1 | `{}` | `[]` |
| `main_menu` | 2 | 1 | `{}` | `[]` |
| `world` | 3 | 1 | `{}` | `[]` |
| `tutorial_pause` | 3 | 1 | `{}` | `[]`（保留，当前不产生） |
| `android_agreement` | 4 | 4 | `{}` | `ok` |

### 4.2 原生对话

| name | type | source | info | 典型 options |
|---|---:|---:|---|---|
| `game_popup` | 4 | 1 | `content` | `ok` / `cancel` |
| `game_story` | 4 | 1 | `title`（说话人）、`content` | `next` / `skip` |
| `game_wipeout` | 4 | 1 | `{}` | `game_over` |
| `game_npc_quest` | 4 | 1 | `{}` | `complete` / `close` |
| `game_npc_dialog` | 4 | 1 | `title`（说话人）、`content` | NPC 选项、`close` |
| `game_choice` | 4 | 1 | `{}` | 事件选项 |
| `game_input_count` | 4 | 1 | `{}` | `close` |

### 4.3 原生面板

| name | type | source | info | options |
|---|---:|---:|---|---|
| `panel_character_info` | 5 | 1 | `{}` | `close` |
| `panel_inventory` | 5 | 1 | `{}` | `close` |
| `panel_skills` | 5 | 1 | `{}` | `close` |
| `panel_mercenary` | 5 | 1 | `{}` | `close` |
| `panel_quests` | 5 | 1 | `{}` | `close` |
| `panel_settings` | 5 | 1 | `{}` | `close` |
| `panel_shop` | 5 | 1 | `{}` | `close` |
| `panel_craft` | 5 | 1 | `{}` | `close` |
| `panel_npc_rest` | 5 | 1 | `{}` | `close` |
| `panel_npc_revive` | 5 | 1 | `{}` | `close` |
| `panel_save_slot` | 5 | 1 | `{}` | `save` / `close` |
| `panel_character_select` | 5 | 1 | `{}` | `close` |
| `panel_options` | 5 | 1 | `{}` | `close` |
| `panel_shortcut` | 5 | 1 | `{}` | `close` |
| `panel_world_map` | 5 | 1 | `{}` | `close` |
| `panel_daily_reward` | 5 | 1 | `{}` | `close` |
| `panel_in_app` | 5 | 1 | `{}` | `close` |
| `panel_ui_panel` | 5 | 1 | `{}` | `close`（未知原生面板兜底） |

### 4.4 主菜单面板

| name | type | source | info | options |
|---|---:|---:|---|---|
| `main_menu_save_slot` | 5 | 1 | `{}` | `[]` |
| `main_menu_character_select` | 5 | 1 | `{}` | `[]` |
| `main_menu_daily_reward` | 5 | 1 | `{}` | `[]` |
| `main_menu_options` | 5 | 1 | `{}` | `[]` |
| `main_menu_settings` | 5 | 1 | `{}` | `[]` |

> 主菜单面板当前没有统一 HTTP 关闭路径，所以 `options` 为空。

### 4.5 模块面板

| name | type | source | info | options |
|---|---:|---:|---|---|
| `module_panel_autosell` | 5 | 2 | `{}` | `[]` |
| `module_panel_settings` | 5 | 2 | `{}` | `[]` |
| `module_panel_save_backup` | 5 | 2 | `{}` | `[]` |
| `module_panel_custom_map` | 5 | 2 | `{}` | `[]` |
| `module_panel_debug_components` | 5 | 2 | `{}` | `[]` |
| `module_panel_debug_scroll` | 5 | 2 | `{}` | `[]` |
| `module_panel_debug_help` | 5 | 2 | `{}` | `[]` |
| `module_panel_other` | 5 | 2 | `{}` | `[]`（未注册兜底） |

### 4.6 模块对话

| name | type | source | info | options |
|---|---:|---:|---|---|
| `module_dialog_world_teleport_confirm` | 4 | 2 | `title`、`content` | provider 选项 |
| `module_dialog_other` | 4 | 2 | `title`、`content` | provider 选项（未注册兜底） |

### 4.7 外部插件

| 模式 | type | source | stability |
|---|---:|---:|---|
| `ext_panel_<plugin_id>_<panel_id>` | 5 | 3 | session |
| `ext_dialog_<plugin_id>_<dialog_type_id>` | 4 | 3 | session |

`plugin_id` 匹配 `[a-z0-9_.-]+`；`.` 在 dialog type 中替换为 `_`。

## 5. info / options 规则

- `title`：story/npc 的说话人；module dialog 的标题；其余状态不存在。
- `content`：对话/剧情/弹窗正文；其余状态不存在。
- `options`：当前状态可执行的 UI 动作；动作通道由 `type` + `source` 决定。
- `type=dialog`：`info` 可能有 title/content，`options` 是对话选项。
- `type=panel`：`info` 为空，`options` 通常是 `close`（`panel_save_slot` 另有 `save`）。
- `type=world/menu/boot`：`info={}`、`options=[]`。
- HTTP 返回完整文本，不做 capability 截断。

## 6. 注册与命名

- **面板 provider**：Host SDK v1.29 尾追加 `QolPanelEnumerationApi`（`count`/`at`）。`core.ui_state`
  直接枚举 `infra/panels` 已注册 provider，按 `panel_id` 派生 `module_panel_<key>`，无需逐个插件
  注册；provider 注销后自动从状态表消失。
- **对话框与未来来源**：插件在 install 阶段 `register_state({type, key, plugin_id})`；不提交 name/id。
  `plugin_id` 由插件从 `PluginContext::plugin_id` 复制；`core.ui_state` 派生 name：

  | 来源 | panel | dialog |
  |---|---|---|
  | 当前实现（module） | `module_panel_<key>` | `module_dialog_<key>` |
  | 预留 external | `ext_panel_<plugin_id>_<key>` | `ext_dialog_<plugin_id>_<key>` |

- 重复 name、非法 key、key/plugin_id 为空 → 注册失败。
- 插件 deactivate 时 `unregister_state(type, key)`。
- 不保存插件清单；不分配 id；不维护 `preferred_id`。
- CI 扫描对话框注册 key，检查与自身 dialog_type_id 一致、无重复。

## 7. 优先级

```
android_agreement
  > game_popup
  > module_dialog_*
  > game_story
  > game_wipeout / game_npc_quest / game_npc_dialog / game_choice / game_input_count
  > module_panel_*
  > panel_* / main_menu_*
  > main_menu
  > world / tutorial_pause
  > loading
```

- 同一时刻只发布一个 `name`；高优先级覆盖低优先级。
- 按输入归属排序：`game_popup` 即使被模块面板遮住也优先于 `module_panel_*`。

## 8. 兼容与版本

- `ui.state` 1.1 → 1.2：尾部追加 5 个函数，1.1 消费方不受影响。
- 新增状态名是兼容变更；改名/删除状态名是破坏性变更。
- C++ summary 新增字段只能尾部追加；不改变已有字段语义。
- 新路由 2 条，`check_http_routes` 计数与 smoke 基线同步。
- 删除四个旧 GET 路由，新增两个统一状态 GET 路由；不保留旧路由的 legacy 别名。

## 9. 验收

Host：

- `read_state` 在 loading/main_menu/world/dialog/panel 五态返回正确 name/type/source。
- 原生读取失败 → `unknown`，不崩溃。
- `register_state` 重复/非法 key → 注册失败；`unregister_state` 后表移除。
- `visit_states` 内容 = 原生表 + 面板 provider 枚举 + 已注册状态 + 已见模块状态 + 兜底；
  不含未安装插件。
- `publish_external_ui` 出现/清除 overlay。
- ABI：1.1 消费方可运行；1.2 可取新函数。

HTTP：

- `/api/ui/state` 的 `name/type/source/info/options` 正确；`info` 仅含 `title`/`content`，`options` 为 `[{id,label}]`。
- `/api/ui/state/table` 与 `visit_states` 一致。
- 未安装插件不出现；外部插件安装/卸载后表变化。
- 路由计数同步。

真机：

- 标题页 `main_menu`、世界 `world`、剧情 `game_story`、NPC `game_npc_dialog`、
  背包 `panel_inventory`、自研面板 `module_panel_*`、同意页 `android_agreement`。
- 关闭后回到正确 name；`info.title`/`info.content` 与游戏文本一致。

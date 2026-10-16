# `character_panel_i18n` 插件参考

## 摘要

功能插件（纯内容贡献者、叶子）：把角色属性面板（`POPUP_SC_CHARACTER_INFO`）的战斗属性行标签（游戏 text id `35185..35196`，共 12 个）汉化。它不提供能力、无 HTTP 路由、**不自己挂全局文本钩子**、不写游戏内存。

事实来源：`MEMORYTEXT_GetText` 是全游戏唯一文本热点，且 LSPosed NativeHook 对同一地址二次挂载会失败，故该 hook 的唯一 owner 是 `core.custom_ui` 的文本机制（经 `text_layer` 能力暴露注册接口）；本插件是该能力的消费者，只贡献文案内容，并额外持有一个角色面板绘制窗口的入口 hook，把该绘制窗口标记为 `kCharacterPanel` 作用域，使文本机制**只在面板绘制期间**替换这 12 个 id（ADR 0001 D3/D4）。

除特别说明，文中 `plugins/...`、`contracts/...`、`infra/...`、`host/...` 路径均相对 `module/app/src/main/cpp/`。

## 定位与版本

`character_panel_i18n`，插件版本 `0.1.0`（`plugins/character_panel_i18n/plugin.cpp:11-12`）。descriptor（`plugin.cpp:80-96`）：

- `requires_count=1` → 唯一 required 依赖 `text_layer` v1.0（`plugin.cpp:33-36,83`）；`optional_count=0`。
- `provides_count=0`（纯内容贡献者，`plugin.cpp:85`）；`resources_count=1`；`jni_count=0`（`plugin.cpp:87`）。
- 无 `routes.inc`、无 `http_queries`/port、无快照/事件/命令端口。

注册于 `host/assembly.cpp:67`（include）与 `host/assembly.cpp:233`（`plugin_descriptor` 登记）。

## 公开能力与路由

- 不 `provides` 任何 capability。
- 不注册 HTTP 路由，无 `routes.inc`。
- 无对外快照、事件或命令端口。
- 唯一用户可见 feature：`character_panel_i18n.translation`，version `1`，default `1`，分类 `kBasicOptimization`，类型 `kToggle`，名称「角色属性汉化」，描述「在角色面板绘制作用域内提供中文文案。」（`plugin.cpp:74-78`）。其 `on_toggle` 为 no-op：运行时门禁由 Host 的 feature 布尔值 → `enable` 派生驱动生命周期（ARCHITECTURE.md §8.2），回调本身不需动作。当前分类/默认值登记见 `docs/reference/plugins/settings.md`。

## 游戏内行为

### 资源（入口 hook）

一个声明式入口 hook（`plugin.cpp:16-31`），`kEntryHook` / `kPrivate` / `kRequired` / `kFailSoft`、`resumable=true`、`step_budget=120`、`gate=nullptr`（常驻）、`owner=character_panel_i18n`：

| 资源 id | 符号 | 职责 |
|---|---|---|
| `hook:Scene_Draw_Popup_SC_CharacterInfo` | `F_SCENE_DRAW_POPUP_SC_CHARACTER_INFO_VMA` | 面板每帧绘制入口 wrapper：进入时 push `kCharacterPanel` 作用域 → 转发原绘制 → 退出时恢复上一层作用域并 pop |

### 标签表

12 条记录覆盖 `35185..35196` 连续升序，`scope` 全为 `kCharacterPanel`（`feature.h:46-59`）：

| key | text id | 文案 | 说明 |
|---|---|---|---|
| `charinfo.dmg` | 35185 | 物攻 | DMG |
| `charinfo.magic_dmg` | 35186 | 法攻 | M. DMG |
| `charinfo.def` | 35187 | 防御力 | DEF |
| `charinfo.magic_def` | 35188 | `nullptr` | 面板未显示，显式占位以维持区间连续、保持原文 |
| `charinfo.crit_rate` | 35189 | 暴击率 | |
| `charinfo.hit_rate` | 35190 | 命中率 | |
| `charinfo.crit_dmg` | 35191 | 爆伤 | |
| `charinfo.phys_res` | 35192 | 物抗 | P.RES = 物理抗性 |
| `charinfo.magic_res` | 35193 | 法抗 | |
| `charinfo.evade` | 35194 | 闪避率 | |
| `charinfo.weapon_block` | 35195 | 武器格挡 | |
| `charinfo.shield_block` | 35196 | 盾牌格挡 | |

编译期 `static_assert(charinfo_block_ok(), ...)` 保证条目数 == 区间长度、逐项升序无空洞、scope 全为 1（`feature.h:63-77`）。`35181..35184`（LV/EXP/HP/MP）游戏原文保留，不在本表内。

作用域枚举本体在基础设施：`kNone=0`、`kCharacterPanel=1`、`kUimixRecipeButton=2`、`kUimixPanelTitle=3`、`kUimixPageTab=4`（`infra/text_scope.h:14-20`）。

## 使用的 Host SDK 与 Game 面

- `ctx.sdk->capabilities.require(contracts::kTextLayerCapabilityId, {v1.0}, &iface)`：取得 `TextLayerApi`（`feature.cpp:39-45`；能力 id/版本与函数表见 `contracts/text_layer.h:10-22`）。
- `text_layer->subscribe(phase, kind, fn, ctx)` / `unsubscribe(id)`：`phase=after=1`、`kind=modify=1`，回调体 `owner_table_modify`（`feature.cpp:50-64`；常量 `feature.h:125-128`）。
- `ctx.sdk->text_scope.push(scope)` / `pop()`：绘制 wrapper 进出时 push/pop 作用域（`charinfo_scope.cpp:22-29`；SDK 面 `contracts/host_sdk.h:306-311,423`；实现 `host/host_sdk.cpp:1078-1095`——16 层 thread_local 栈，`scope` 范围校验 `[0, kUimixPageTab]`，push 保存上一层、pop 恢复）。
- **不使用**任何 Game Host 面：`plugin_deactivate` 不保存 `ctx.game`，插件不持有游戏裸指针、不直接调用游戏函数；无 log/scheduler/commands/ticket_result 使用。
- 注意：插件另持有**一份线程局部作用域镜像** `g_active_scope`（`feature.cpp:11-12,23-25`），供 `owner_table_modify` 使用；而查表纯逻辑 `owner_table_lookup` 读 `qol::infra::active_text_scope()`（`feature.h:117-119`）。两条路径的取值来源在阶段审计中需核对一致性。

## 消费者与职责边界

本插件不作为 provider，**无下游消费者**（叶子）。级联影响面仅在依赖方向：

- required 依赖 `text_layer`：provider（`core.custom_ui`）缺失时本插件 `blocked`；provider 撤销时作为 required consumer 被级联停用（`cascade_stop`），provider 恢复后按 Host 生命周期重新激活（消费者不自动恢复，需逐个 activate）。
- 本插件停用不影响任何其他插件。

边界归其他 owner：

- `MEMORYTEXT_GetText` 的唯一 hook、注册与分派**机制** → `core.custom_ui`（提供 `text_layer` 能力，见 `docs/reference/plugins/core.custom_ui.md`）。
- 文本作用域枚举与 thread_local 载体 → `infra/text_scope.h`（L2 基础设施，feature 之间不得互相 include）。
- 角色/队伍事实 → `core.character`；游戏 UI 状态与操作 → `core.ui_state`；本插件的汉化属可裁剪的**呈现功能**，不并入二者（ADR 0001 D4）。

## 失败语义（fail-soft）

- 资源声明 `kFailSoft`（`plugin.cpp:23`）：安装失败记 `degraded`，不使插件整体失败，文本机制与其他内容贡献者继续。
- `install`：`bind_sdk` 后调 `register_consumers`；后者在「无 SDK / 无 `capabilities.require` / require 失败 / `subscribe` 返回 0」时返回 false，`install` 随即返回 1（宿主回滚并把插件置 `failed`）（`plugin.cpp:38-44`；`feature.cpp:35-59`）。
- `on_resource_ready`：`resource_id` 匹配时**无条件**缓存 backup（含 `nullptr`）并返回 0（`charinfo_scope.cpp:32-35`），wrapper 侧自行判空（`charinfo_scope.cpp:25`）；未匹配 id 返回 0（`plugin.cpp:62-66`）。
- `scene_draw_charinfo_wrapper`：`sdk`/`text_scope.push` 不可用或 push 失败时，只透传 backup、**不标记作用域**（`charinfo_scope.cpp:22-25`）；push 成功才写镜像、调用后恢复上一层并 pop（`charinfo_scope.cpp:24-29`）。
- `fill_status` 恒输出 `enabled=true, error=nullptr`（`plugin.cpp:68-71`）。

## 生命周期

- `install` / `on_enabled` → `register_consumers()`：幂等，句柄 `g_consumer != 0` 时直接返回 true（`plugin.cpp:38-55`；`feature.cpp:36`）。
- `deactivate` / `on_disabled` → `unregister_consumers()`（有句柄且 `unsubscribe` 非空时注销并清空），`deactivate` 另执行 `bind_sdk(nullptr)`（`plugin.cpp:46-60`；`feature.cpp:61-66`）。
- 禁用期间的 entry hook 由 Loader 保留为空转 wrapper（`resumable=true`）。

## 文件地图

| 文件 | 事实位置 |
|---|---|
| `plugins/character_panel_i18n/plugin.cpp:11-12,16-36,38-96` | 版本、资源声明（入口 hook）、requires（text_layer）、install/deactivate/on_enabled/on_disabled/on_resource_ready/fill_status、feature、descriptor。 |
| `plugins/character_panel_i18n/feature.h:10-18,30-61,63-77,79-93,95-128` | 前向声明、`Entry`/`kEntries`（12 条）、区间 `static_assert`、`entries`/`lookup`/`entry_by_key`、SDK 绑定与镜像访问、`owner_table_modify`/`owner_table_lookup`、phase/kind 常量。 |
| `plugins/character_panel_i18n/feature.cpp:11-15,19-25,27-33,35-64,66` | 全局状态、SDK 绑定与镜像访问、`owner_table_modify`、`register_consumers`/`unregister_consumers`。 |
| `plugins/character_panel_i18n/charinfo_scope.h:19,23` | wrapper 与 `on_resource_ready` 对外声明。 |
| `plugins/character_panel_i18n/charinfo_scope.cpp:13,19-30,32-35` | backup 缓存、绘制窗口 wrapper（push→转发→pop）、无条件 backup 交付。 |
| `plugins/character_panel_i18n/CMakeLists.txt:1-7` | 静态库 target（feature.cpp / plugin.cpp / charinfo_scope.cpp）。 |
| `host/assembly.cpp:67,233` | include 与插件注册。 |
| `contracts/text_layer.h:10-22` | `text_layer` 能力 id/版本、`TextLayerApi`（subscribe/unsubscribe）。 |
| `contracts/host_sdk.h:306-311,423` | `QolTextScopeApi` push/pop 面。 |
| `host/host_sdk.cpp:1078-1095` | 16 层 thread_local 作用域栈实现与范围校验。 |
| `infra/text_scope.h:14-20,40-57` | `TextScope` 枚举、`t_active_text_scope`、`TextScopeGuard`。 |
| `tests/test_character_panel_i18n.cpp` | host 侧 descriptor/资源声明/标签表（含 35188 占位与作用域不匹配）/backup 转发用例。 |

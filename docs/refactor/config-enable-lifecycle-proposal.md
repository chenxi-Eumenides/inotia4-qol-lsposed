# 提案：Host lifecycle `enable` 与插件功能配置

> **已取代（superseded）**：本提案描述的评审期模型已被实现模型取代，事实来源为 `.slim/deepwork/module-settings-features.md` 的已批准目标（Host 独占 `enable`/`features`/`config` 三层 schema、注册元数据不持久化、`want_run = enable && any(feature=true)`、依赖级联不改 desired），并已落地在 `ARCHITECTURE.md` §2.2/§7.1/§7.2/§8.2 与 `docs/reference/api-reference.md` §7.6。本文仅作历史评审记录；其中旧平铺字段到 `*.enable` 的映射、`recipesEnabled`/`clampEnabled` 等候选功能 key 名均未采用（最终模型见上述文档）。

> **状态（历史）：根据用户最新约束编写的评审稿。** Host 对 `plugin_id.enable` 的所有权及其 desired-state 语义已明确；旧功能开关如何迁移、依赖恢复策略和设置页入口仍待审阅。批准前不据此删除旧 runtime keys。

## 1. 用户已明确的规则

1. `plugin_id.enable` 是 **Host 保留的 lifecycle 键**，不是插件自定义功能配置。
2. 插件代码不得直接读取或写入该键。Host 启动时从 `config.json` 读取已注册插件的值，构建期望启用/停用清单。
3. `enable=true` 仅表示“希望运行”。实际状态还取决于依赖、目标版本、descriptor 注册、资源安装和错误情况；不能把 desired 状态报告成实际运行成功。
4. 必须处理 required dependency 的级联行为。
5. 除保留的 `enable` 外，插件仍可按已有 owner-token single-key API 提交修改其他合法 JSON key/value；Host 验证后统一持久化。

这些规则替代初版提案中“enable 是插件功能配置、不是 lifecycle”的建议，也替代“旧 12 个布尔值直接迁移成各插件 `.enable`”的映射。此前 Phase1 Store 与 update API 仍可复用原子 JSON/权限实现，但 enable 权限和 legacy mapping 必须重开。

## 2. 要区分的状态

| 概念 | 含义 | 事实来源 |
|---|---|---|
| Registered | 当前 build 的 descriptor 已向 Host 注册 | Plugin registry |
| Desired enabled | `config.json` 中 Host 保留的 `plugin_id.enable` | Host ConfigStore |
| Actual lifecycle state | `registered`、`enabled`、`blocked`、`unsupported`、`failed`、`inactive` 等 | Host registry/lifecycle |
| Resource state | hook/patch 等声明资源的 `live/disabled/free` 状态 | Host resource registry |
| Feature config | 插件自己的普通 namespace key，例如推怪功能开关 | Plugin namespace / single-key API |

`enable=true` 不等于插件已运行；插件运行也不等于其每个游戏内行为当前可触发。API/UI 应分别展示 desired enable 与 actual lifecycle 状态及原因。

## 3. 推荐的 Host desired-state 协调方案

### 启动

1. Loader 初始化 resident Store，注册本次构建实际包含的 plugin descriptors，并批量 seed 缺少的 namespace/defaults。
2. Host 只读取当前 registered IDs 的 `plugin_id.enable` 来生成 desired list。未注册 namespace 保留在文件，但不进入当前启用清单、runtime projection 或公开 registered view。
3. 对 desired-enabled 插件按 required dependency 拓扑尝试启动。依赖未注册、被禁用、版本不兼容或 install 失败时，实际状态明确报告 `blocked/unsupported/failed`；不把 `enable` 自动改为 false。
4. 对 `enable=false` 的插件不进行 lifecycle activate；需要停止已运行插件时由 Host lifecycle transaction 处理，清理其 routes、capabilities、subscriptions 与 resources。

### 依赖级联（建议）

- Required provider 被 Host 停用或启动失败时，依赖它且正在运行的 consumer cascade-deactivate / 标记 `cascade_stopped`；consumer 自身 `enable=true` 保留，表示仍希望运行。
- Provider 后续恢复并达到可用状态时，Host 按依赖拓扑自动重试所有仍为 `enable=true` 的 consumer；失败者继续报告 actual error，而不篡改 desired state。
- Optional dependency 不阻断启动；由现有 optional/degraded 状态反映缺失。
- 显式用户 lifecycle deactivate 建议同时将该插件 `enable=false` 持久化；因依赖级联造成的内部停用不改 consumer desired 值。

这样将“用户想运行什么”存于 Host lifecycle 键，将“现在到底能不能运行”留给 Host 生命周期状态机；依赖恢复可重新收敛到 desired list。

## 4. `enable` 的访问边界与设置页入口

- Owner-token plugin single-key API 对 `key == "enable"` 一律拒绝；无论 caller 写自己还是写其他 plugin，都不能绕过 Host lifecycle owner。自有 namespace 读取也不向插件代码暴露 `enable`。
- Host 提供独立的 lifecycle control/view 接口：取得 desired/actual 状态，提交 enable/disable 请求，并由 Host 更新保留键、持久化后运行 lifecycle transaction。插件只能请求 Host 操作，不直接读写 Store 中的 `enable`。
- `core.settings` 的设置页需要使用该 Host lifecycle 接口，而不是把 `enable` 作为普通 namespace toggle；HTTP 的 enable 操作也应由 Host 生命周期适配器处理。其他普通插件 key 仍走已有 single-key update。
- `/api/config/list` 是 user-facing registered-config projection，可以由 Host 序列化 lifecycle desired 值；不能让普通插件通过 `read_json/read_registered` 获取保留字段。`/api/system/features` 继续表达 actual state。
- `core.settings.enable` 有 self-lockout 风险：若它为 false，原设置页和配置路由可能不可用。**建议 Host 固定保护 `core.settings` 的恢复能力**（例如拒绝禁用核心配置服务，或保留独立 Loader recovery surface），由评审确认具体选项。

## 5. 旧七个功能开关的迁移选择

此前七个平铺开关多为**资源/功能 gate**，不一定等价于插件整体 lifecycle。不能在未确认语义前机械映射到 `plugin_id.enable`。

### 方案 A（推荐）：分离 lifecycle 与功能设置，保留原行为

- `plugin_id.enable` 由 Host 管理，通常默认为 true；它负责插件整体生命周期。
- 原七个功能开关迁移为 plugin namespace 中非保留 `enable` 的功能 key，沿用此前默认值及 Host/runtime 映射：

| 旧平铺字段 | 建议的新功能 key | 旧默认 | 备注 |
|---|---|---:|---|
| `stackLimitIncrease` | `stack_limit.clampEnabled` | false | 只控制数量 clamp 行为 |
| `moveMergeEnabled` | `move_merge.autoMergeEnabled` | false | 只控制拖拽合并 gate |
| `extensionBagEnabled` | `extension_bag.featureEnabled` | true | 只控制扩展背包功能 gate |
| `customRecipeEnabled` | `custom_recipe.recipes`（最终采用名） | false | 唯一可见开关；与 hidden 的 `craft_ui.custom_crafting` 级联 |
| `simpleModeEnabled` | `simple_mode.simpleModeEnabled` | false | 控制简单模式行为，不停用插件本体 |
| `pushAllEnabled` | `push_all.pushEnabled` | false | 控制推动资源，不停用插件本体 |
| `autoSellEnabled` | `autosell.scanEnabled` | false | 模块级扫描 gate；存档内筛选配置仍独立 |

具体命名可在实施前按现有 descriptor/contribution 统一，但不得复用保留字段名 `enable`。

### 方案 B：旧功能开关完全由 lifecycle `enable` 取代

旧开关不再保留功能 key，开/关意味着整个插件 activate/deactivate，包括它的 routes、capabilities、callbacks 和全部资源。该方案更少字段，但改变现有行为和默认可用性；`customRecipeEnabled` 原来涉及 `craft_ui` 与 `custom_recipe` 两个 owner，必须另定双插件 lifecycle/依赖映射。

**建议选择方案 A**，因为它保持“插件希望运行”与“插件某项功能开启”的语义不同；如用户/评审决定不再需要某项功能，再单独删该功能 key。

## 6. 两个具体插件例子

### `push_all`

- `push_all.enable=true`：Host 尝试注册/安装 push_all；若依赖或游戏版本不满足，实际状态可能是 blocked/unsupported/failed。
- `push_all.pushEnabled=false`（若采用方案 A）：插件仍可处于 lifecycle enabled，但推怪 patch/resources 关闭。
- 依赖 provider 被禁用时，push_all 可 cascade-stopped；`push_all.enable=true` 保留。provider 恢复后 Host 按推荐策略自动重试。

### `quest`

- `quest.enable=false`：Host lifecycle 停用 quest 插件，因此 save-enter 回调、任务完成检查等整个插件行为不运行。
- `quest.enable=true`：表示 Host 应尝试运行；实际完成检查仍需进档/save-enter/world ready 等既有条件。
- 若需求只是暂停任务完成检查但保留 quest 的其他读取/API能力，应新增类似 `quest.completionCheckEnabled` 的普通功能 key，而不是借用 lifecycle `enable`。

## 7. 持久化与并发规则

- Host 对保留 `enable` 的读取、更新与 lifecycle desired map 构建是唯一权威路径；plugin owner update API 和插件自读 API均不能访问该 key。
- Host lifecycle 请求与普通 plugin key 更新共用 resident Store 的原子写入、revision/effect 顺序和未知字段保护，但 `enable` 更新触发的是 lifecycle reconciler，不是普通 feature gate projection。
- 持久化失败不修改内存 desired map、不改变 lifecycle/resource state、不发 runtime effect；安装/依赖失败发生在 durable commit 之后，保持 desired enable 值并报告 actual failure。
- `infra::config` 可以作为旧 runtime gate 的派生投影，不能继续提供第二个 public writer。普通功能 key变化经 Host durable commit 后投影；Host lifecycle `enable` 走 lifecycle transaction。

## 8. 等待评审的决策

1. 旧七个功能开关选择方案 A（迁移为普通 namespaced feature keys）还是方案 B（完全以插件整体 lifecycle 取代）？推荐 A。
2. dependency provider 恢复后，`enable=true` 的 cascade-stopped dependents 是否自动按拓扑重试？推荐自动重试。
3. 用户显式 deactivate 是否持久化 `enable=false`；内部 dependency cascade 是否保留 dependent `enable=true`？推荐“用户操作更新 desired，级联不改 desired”。
4. `core.settings.enable` 是否由 Host 永久保护，或需要独立恢复界面？推荐保护核心配置恢复路径。
5. `/api/config/list` 是否由 Host 输出 registered objects 的 desired `enable`，同时 `/api/system/features` 返回 actual state？推荐分开呈现两者。

## 9. 实施/验收条件（评审通过后）

- Host 启动时按实际注册 descriptor IDs 批量读取 desired enable；插件 API 自读与单 key update 均拒绝 `enable`。
- Host desired-state list 与 actual lifecycle status 独立；依赖、版本、注册和 install 错误不会伪装为 enabled。
- 依赖 cascade/恢复行为按获批策略测试；`push_all` 与 `quest` 用例覆盖 lifecycle 与 feature switch 的区别。
- Runtime feature projection 只消费 durable Store 更新；无旧 `infra::config`/旧 JNI public writer 旁路。
- 更新 `ARCHITECTURE.md`、API reference、core.settings/plugin docs、verification/issues/audit records；没有设备/EB证据的项目标记 `NOT_ACCEPTED`，不报告完整验收。
- 定向 Host tests、`check_arch.py`、`check_http_routes.py` 和 Debug build通过后再做Oracle Gate；用户未授权前不跑设备/EB、不提交、不安装中间APK。

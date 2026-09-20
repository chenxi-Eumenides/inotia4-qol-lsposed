# 分层与组件说明（**临时过程性文件**）

> ⚠️ **本文是临时过程性文件，不是权威文档，随时可能过时或被删除。**
> - 架构权威：根目录 `ARCHITECTURE.md`（当前 v0.3.14，待外部复审签署）
> - 阶段与验收状态：`docs/refactor/ledger.md`、`docs/refactor/verification.md`
> - 结构基线：`docs/refactor/baseline.md`、`docs/refactor/map.md`（脚本生成）
> - 生成时点：2026-09-19（**P0 事实修正版**），仓库 HEAD `6fc6098`；来源＝`module/app/src/main/cpp/` 物理文件清单 + `scripts/maintenance/` 脚本实测输出
> - 口径：文件数/行数为 `wc -l`（含空行与注释）；只列**当前实际存在**的模块，设计提及但未落地的在「收口项」标注
> - **三态标记（全篇适用）**：**当前事实**＝可由源码/脚本输出直接定位；**迁移债**＝当前存在但已有目标归属与清除阶段；**目标设计问题**＝目标边界本身过宽或缺少硬约束
> - 用途：给人快速理解现状（含**依赖链**：插件↔插件 / 插件↔组件 / 组件↔组件）；**任何决策请回到权威文档**，本文件不得作为落地依据
> - **基线可信度**：`check_arch.py` 全部通过只代表「无新增违规」（基线棘轮），**不代表七目录合规**；基线内仍有层方向 32 条、符号/裸地址 9 条、资源 3 条（2026-09-19 实测）——均属**迁移债**

## 0. 编号口径

**架构分层只有一套：L0–L5**（L0 contracts / L1 game / L2 infra+hooks+patch / L3 plugins / L4 出口面 / L5 host）；七目录为 `contracts/game/infra/hooks/patch/plugins/host`。

> 施工阶段（工作分解顺序）编号 **C1–C7** 仅记录在 `ledger.md` §7，**不是分层编号**。

---

# 一、分层编号（L0–L5）

| 层 | 目录 | 文件/行数 | 职责 |
|---|---|---|---|
| L0 | `contracts/` | 18 / 1,564 | 跨层稳定接口：POD、能力契约、宿主 SDK / Game Host 函数表；**不含游戏逻辑** |
| L1 | `game/` | 36 / 2,725 | 唯一承接游戏版本耦合（VMA/偏移/ABI/函数调用）；**禁止业务规则** |
| L2 | `infra/` + `hooks/` + `patch/` | 40 / 4,203 | 运行时机制：调度/队列/事件/能力/状态/配置/存储 + 资源登记/事务/分派 + 指令改写 |
| L3 | `plugins/` | 154 / 30,974 | 17 个内置插件（功能实现 + 能力 provider/consumer + 面板）；目录形态受「可外置插件」约束（ADR-0008，只允许 include `contracts/` 与自身目录） |
| L4 | （原 `products/`，已取消） | — | 出口面职责现由 `plugins/core.api`（路由内核，待 C6）与 `plugins/core.ui`（UI 容器与通用组件）承担 |
| L5 | `host/` | 17 / 4,229 | 宿主：装配、插件生命周期执行器、Host SDK/Game Host 实现、帧源、core 常驻补丁、状态 JSON |

## 1.1 `contracts/`（19）

| 文件 | 用途 |
|---|---|
| `plugin.h` | 插件契约总纲：`PluginDescriptor`、`ResourceClaim`（机制/策略/必要性/失败模式/gate/step_budget/handler/owner）、`PluginContext`（`sdk`/`game`/`plugin_id`）、能力声明与状态输出 |
| `host_sdk.h` | **宿主 SDK**（插件可用宿主能力）：`log`/`config`/`scheduler`/`commands`/`events`/`capabilities`(含 lease)/`store`(section + 容器级)/`panels`/`hooks`(文本分派 + 布尔门 + PopupState)/`text_scope`/`ui` |
| `game_host.h` | **Game Host ABI**（宿主内核子表）：`target`(变体/能力位/符号可用)/`thread`(主线程/帧号)/`read`(角色/物品/库存/世界/合成/存档/任务/属性)/`write`(库存/合成)/`call`(登记调用)/`ui_primitive` |
| `ui.h` | UI 标准契约：6 描述符（Panel/Button/Label/Toggle/Select/ScrollView）+ `QolUiComponents`（由 `core.ui` 提供）+ `QolUiAdapter`（由 host 注入的低层绘制/命中/贴图面） |
| `result.h` / `version.h` | 统一返回信封 / 版本与兼容判定（major 相等且 minor ≥ min） |
| `events.h` / `commands.h` | 事件类型（配置变更等）/ 命令端口（票据 id、状态、kind） |
| `ownership.h` | 所有权机制契约（`acquire/release`）——表注入、弹窗状态占用等「所有权型」资源 |
| `panels.h` | 设置页项与面板 provider 注册契约（`UiPanelsApi`：open/close/active/block_reason） |
| `attribute_quality.h` | `attribute.quality` 能力（档位/百分位/区间探测，**不输出颜色码**） |
| `craft_result.h` | 合成结果能力（12 函数：模式判定/放料/准备/执行/材料需求/产物/描述替换） |
| `extension_bag.h` | 扩展背包能力（`ExtensionBagApi` 37 函数 7 组 + `ExtensionBagMoveObservation` + 3 枚举） |
| `inventory_refresh.h` | 刷新 trampoline 能力（绕开被 hook 入口直调原函数，防重入） |
| `text_layer.h` | 文本层能力（subscribe/unsubscribe） |
| `ui_choice.h` | 原生选择框能力（`UiChoiceSpec` + open/active/close） |
| `autosell.h` | 自动出售端口（配置/特殊类型 + 运行时读取/提交/持久化声明） |
| P1 收口 | `contracts/stack_codec.h`、`contracts/stack_limit.h` 已删除（P1） |

**收口项**：`contracts/ui.h` 已落地但部分 raw 面（角标贴图、扩展背包页签/拖放等）仍走 adapter；`extension_bag` 的 JSON 面 `write_json` 为 v1.1 预留。

## 1.2 `game/`（36）

| 模块 | 用途 |
|---|---|
| `access/symbols.*` | **符号解析唯一入口**：入口 hook 白名单 + `resolve_hook_entry`（monster 跳板跟随、可执行段校验） |
| `access/patches.*` | 指令 patch 站点表：字替换 / BL / BL→符号 / B 尾调用（常量全部来自 `game_symbols.h`） |
| `access/ptr_slots.*` | 指针槽站点表（槽地址 + 期望值） |
| `access/char.*` | 角色读取：类型/HP/可换装/等级/佣兵槽/菜单主角/解析状态 |
| `access/item_class.*` | 物品类别：ITEMCLASS 记录、no-equip 保护位、类别/装备判定/装备等级 |
| `access/quest.*` | 任务状态：活跃槽数/槽 id/总数/状态读写/完成判定 |
| `access/world.*` | 世界与地图：世界态、地图名/记录数、切图请求、朝向、金币、YesNo 弹窗/HUD、入口 patch 元信息 |
| `access/save.*` | 存档域：槽结构与 hero、`Save_LoadData`/`mem_free`/hub key/`Encrypt_Process2` 链、删档 GOT 目标 |
| `access/craft.*` | 合成 UI：UIMix 槽与配方、填入格读写、界面动作（刷新/换型/音效/文案/YesNo） |
| `access/attr_range_ui.*` | 属性区间 UI 的 VMA/GOT 原语 + 随机数捕获 |
| `access/popup_state.*` | 27×0x40 弹窗状态表定位与条目字段读写 |
| `access/ui_kit.*`（+`ui_kit_controls.inc`、`ui_kit_render.inc`） | UI 原语：建控件、矩形运算、命中检测、文本/图像绘制、面板装饰、文本弹窗 |
| `access/ui_components.*` | 原版风格复合控件（返回键/开关/物品图标/自绘按钮） |
| `access/ui_choice.*` | 原生选择框状态 id 与按钮槽目标解析 |
| `access/ui_equip.*` | 装备面板槽（move_merge GOT 解析） |
| `access/hive_iap.*` | IAP 拦截后恢复（GOT 改写 + 调原版恢复状态） |
| `clock.*` | 帧计数（GOT 取值，供宿主注入帧时钟） |

**收口项**：`game/write`、`game/ops`、`game/variant` 未独立落地；变体表、`qol_log`、`module_save`、`save_enter/exit`、`stack_limit_state` 仍在遗留 `core/native`；`data/native/game_symbols.h` 待 C7 收口。

## 1.3 `infra/`（24）

| 模块 | 用途 |
|---|---|
| `runtime.*` | 帧时钟注入（未注入时 `frame_count()==-1`） |
| `scheduler.*` | 帧任务：点位（逻辑前/渲染前）、间隔帧、次数（0=常驻）、句柄、按帧去重、自动注销 |
| `command_queue.*` | 通用命令队列：单调票据、四态、取消、**锁外执行**、终态环表 |
| `commands.*` | 命令端口实现：POD 槽位表 + 内建分派（写配置、插件生命周期）+ `drain` |
| `events.*` | 事件总线（订阅/注销/快照派发） |
| `capabilities.*` | 能力注册表（注册/注销/`require` + 版本校验） |
| `status.*` | 功能状态：10 生命周期态 + `degraded` 正交健康标记 + 快照 |
| `config.*` | 键值配置（int32/bool），值变更才广播 |
| `panels.*` | 面板注册表数据面（item/provider，一 id 一 owner，保序注销） |
| `store.*` | sidecar 存储：MSAV 编解码、原子写、last-good、坏件隔离、section/容器读写 |
| `snapshot.h` | 只读快照原语（seqlock；仅平凡可复制类型） |
| `text_scope.h` | 文本作用域上下文（角色面板/合成窗口标记） |
| `test_reset.*` | host 测试复位 |

## 1.4 `hooks/`（10）

| 模块 | 用途 |
|---|---|
| `registry.*` | 资源登记与仲裁：**一资源一安装者** + 运行期记录（`resolved_entry`/`backup`/`in_flight`/`isolated`/`retry_pending`） |
| `install.*` | 入口 hook 安装与物理归还（原语由宿主注入；失败锚点固定文案） |
| `transaction.*` | 资源事务引擎：帧步进安装（声明序）→ 逆序还原；失败回滚；在途等待/超时；帧预算；tombstone+隔离 |
| `dispatch.*` | 共享资源分派：文本三段式（before 顺序 / original / after 逆序；每回调单次）+ **布尔 Intercept 门**（如出售拦截） |
| `popup_state.*` | 弹窗状态条目占用原语（按名/VMA 解析、一 entry 一 owner、部分 wrap、整段写回） |

## 1.5 `patch/`（6）

| 模块 | 用途 |
|---|---|
| `claims.*` | 地址占用声明（一地址一所有者、幂等、释放可复用） |
| `instr_patch.*` | 指令改写：站点校验、±128MB 预检、BL 与 B 尾调用（宿主近址 thunk 池）、字替换、**先还原成功后才释放占用** |
| `ptr_slot.*` | 指针槽：槽值校验 → 占用 → 写 handler；归还不匹配则保留占用 |

## 1.6 `plugins/`（17 个插件 / 154 文件）

| 插件 | 功能/用途 |
|---|---|
| `core.ui` | 出口面 UI 容器：设置页容器与分页、面板栈（push/兜底/占用）、`QolUiComponents` 实现与发布（`ui_components`） |
| `simple_mode` | 半血/伤害倍率（2 入口 hook；gate=`plugin.simple_mode.enabled`） |
| `push_all` | 推怪（2 call patch：队伍过滤 + NOP） |
| `quest` | 任务状态补正（零资源常驻 + 进档一次性调度任务，带句柄） |
| `attribute_quality` | 属性档位/百分位 provider（发布 `attribute.quality`） |
| `attribute_range_display` | 属性区间显示消费者（颜色映射 + 有界文本写入） |
| `text_layer` | 文本层：`MEMORYTEXT_GetText` 共享资源 owner + 内置文本表（声明式消费者）+ 角色面板 scope |
| `native_choice` | 原生选择框 provider（`ui_choice`：ptr slot + 入口 hook） |
| `stack_limit` | 数量上限 99/999（6 条 clamp；gate=`plugin.stack_limit.enabled`） |
| `inventory_hooks` | 库存 hook 宿主：24 条入口 hook 声明 + S2 写侧/读侧业务 + `inventory_refresh` provider |
| `autosell` | 自动出售：规则/视图/扫描/持久化 + 背包入口与面板（零资源常驻，命令端口门禁） |
| `save_backup` | 存档备份：导出/导入两段事务/回滚/仓库 + 三栏面板 + 删档指针槽 |
| `special_equip` | 特殊装备穿脱放行（2 入口 hook）+ 出售门禁消费者 |
| `craft_ui` | 合成器界面（12 条声明式资源：5 核心 required + 6 装饰 optional + reset 常驻）；消费 `craft_result` |
| `custom_recipe` | 新合成系统：目录/3 格匹配/表注入；发布 `craft_result` + 表所有权（`own:RECIPEBASE_MIXTUREBASE`） |
| `world_teleport` | 地图传送（零资源 + 声明式双指纹入口 patch；消费 `ui_choice`） |
| `extension_bag` | 扩展背包整域（29 条声明式资源；状态机/事务/投影/出售/合成/装备/持久化；发布 `extension_bag`） |

**插件边界合规（P0 修正：两种口径必须分开陈述）**

| 口径 | 结果 | 三态 |
|---|---|---|
| 带目录前缀 `(game\|infra\|hooks\|patch\|host)/` 与跨插件 include | **0** 条 | 当前事实（新架构前缀已无直引） |
| legacy 路径：`core/native/**`、`data/native/**`、根部裸名头（`game_access.h` / `game_state.h` / `game_symbols.h` / `qol_log.h` / `game_ops_common.h` / `game_feature.h` / `inventory_trade.h` / `sell_price.h` / `save_enter.h` / `save_exit.h` / `game_ptr_hook.h` / `item_class.h`） | **19 个文件**（12 个去重头） | **迁移债**（宿主/legacy 私有面直引，待 P3 清除） |

**当前事实（逐文件，共 19 个）**：`extension_bag/game_ui_virtbag.cpp`(10)、`inventory_hooks/native_inventory_hook.cpp`(6)、`core.ui/panels/settings.cpp`(6)、`autosell/scan.cpp`(6)、`save_backup/ui/panel.cpp`(5)、`save_backup/feature.cpp`(3)、`extension_bag/extension_bag_port.cpp`(3)、`extension_bag/extension_bag_geometry.cpp`(3)、`autosell/ui/panel.cpp`(3)、`inventory_hooks/inventory_hook_stage4.cpp`(2)、`autosell/view.cpp`(2)、`save_backup/plugin.cpp`(1)、`extension_bag/game_ui_virtbag.h`(1)、`extension_bag/extension_bag_store.inc`(1)、`extension_bag/extension_bag_persistence.cpp`(1)、`extension_bag/extension_bag_logging.h`(1)、`autosell/view.h`(1)、`autosell/store.cpp`(1)、`autosell/plugin.cpp`(1)。

**结论**：此前"17/17 合规"是**口径漏检**的结论；真实结论＝"目录前缀口径 0、legacy 口径 19 文件未清"。真机 smoke（VM-BOUNDARY）只证明启动/门禁/生命周期面，**不构成边界合规证据**。

## 1.7 `host/`（17）

| 模块 | 用途 |
|---|---|
| `assembly.*` | 装配：插件注册顺序（**provider 先于消费者**）、机制原语注入、帧回调链 `commands_drain → lifecycle_step → sync_gates` |
| `plugin_registry.*` | 插件注册表与执行器：安装编排、生命周期（activate/deactivate 帧步进）、门禁同步、能力注册与级联撤销、隔离与 tombstone |
| `host_sdk.cpp` / `game_host.cpp` | 两张 ABI 表的真实实现（逐成员转发到 infra/hooks/patch/game） |
| `ui_adapter.*` | `QolUiAdapter` 实现（背后 `game/access/ui_kit|ui_components|ui_choice`） |
| `frame_source.*` | 帧源：2 条常驻 core 资源派发 `infra::scheduler` + legacy `frame_task` |
| `core_patches.*` | core 常驻补丁（S2 布局/位段缩窄/读重定向/hub 跳过/monster 兼容扫描） |
| `status_json.*` | 状态 JSON：features / resources / tickets |
| `variant_json.*` | 变体 JSON（`/api/health` 的 `variant` 段） |
| `adapters.h` | 适配层内部声明 |

## 1.8 出口面（不属于六层）

- `bridge/native/`：16 个 JNI 薄层（名/签名冻结；`check_jni` 143↔143）。
- Kotlin（`ModuleConfig` / `ApiServer` / controllers / services）：HTTP 传输（AndServer）、配置、sidecar 桥——C6/C7 收敛点。
- **遗留目录**：`core/`(37 文件)、`api/`(30)、`data/`(9)——待 C6（api→API provider）与 C7（data→game 符号收口、core→infra/game）吸收。

---

# 二、插件边界迁移施工阶段（C1–C7，**非分层编号**）

| 阶段 | 内容 | 状态 |
|---|---|---|
| C1 | ABI 骨架：`contracts/host_sdk.h`、`contracts/game_host.h`、`PluginContext` 注入 | ✅ 完成 |
| C2 | 宿主 adapter：SDK / Game Host 函数体与注入 | ✅ 完成 |
| C3 | `domains/` 取消：拆 `attribute_quality`(provider) + `attribute_range_display`(消费者) | ✅ 完成 |
| C4 | 小中插件边界化（9 个）+ U11-1 契约/adapter + U11-2 `core.ui` 组件实现 | ✅ 完成 |
| C5 | `inventory_hooks` + `extension_bag` 边界化（含 `save_backup`/`autosell`；`stack_codec` 迁 contracts、store 容器级 ABI、SDK `intercept_blocks`） | ✅ 完成 |
| C6 | `core.api` 路由内核与 API provider 拆分（149 路由 owner 表、U10） | ⬜ 待办 |
| C7 | native config 早期自读、脚本切七目录、**边界违规硬失败**（`check_layers` 收紧、删过渡映射） | ⬜ 待办 |

**验证状态**：C1–C5 已通过真机 smoke（启动状态 + 门禁两态 + 生命周期往返，见 VM-BOUNDARY）；各插件的**功能细则**仍按各自 VM 卡待补。C6/C7 未开工。

---

# 三、依赖链

> 数据源：插件 descriptor（`plugins/*/plugin.cpp`）、全仓 `require_capability` 调用点、`arch_common.IncludeGraph` 实测 include 边（提取脚本 `.tmp/dep-graph/extract.py`，HEAD `7f93fa5` 时点）。

## 3.1 插件 ↔ 插件：**无 include 边**，全部经 capability

插件目录之间 include 边 = **0 条**（脚本核验）。

**提供的 capability（8 个）**

| capability | 提供者 |
|---|---|
| `attribute.quality` | `attribute_quality` |
| `craft_result` | `custom_recipe` |
| `extension_bag` | `extension_bag` |
| `inventory_refresh` | `inventory_hooks` |
| `text_layer` | `text_layer` |
| `ui_choice` | `native_choice` |
| `ui_components` | `core.ui` |
| `ui_panels` | `core.ui` |

**声明式依赖（descriptor 的 `requires` / `optional_requires`）**

| 消费者 | capability | 类型 | 提供者 |
|---|---|---|---|
| `autosell` | `attribute.quality` | **required** | `attribute_quality` |
| `extension_bag` | `inventory_refresh` | **required** | `inventory_hooks` |
| `autosell` | `extension_bag` | **required** | `extension_bag` |
| `save_backup` | `ui_panels` | **required** | `core.ui` |
| `attribute_range_display` | `attribute.quality` | optional | `attribute_quality` |
| `custom_recipe` | `attribute.quality` | optional | `attribute_quality` |
| `craft_ui` | `craft_result` | optional | `custom_recipe` |
| `world_teleport` | `ui_choice` | optional | `native_choice` |

**运行期消费但不属于插件 descriptor 依赖**（宿主内部消费者；不参与插件 capability DAG）

| 消费者 | capability | 位置 |
|---|---|---|
| `api/native/game_save.cpp` | `ui_panels` | 宿主侧 API 消费者（非插件） |
| `core/native/inventory_trade.cpp` | `extension_bag` | legacy 宿主侧消费者（待 C6） |

**提供但当前无消费者**：`text_layer`（消费者已并入自身）、`ui_components`（消费者迁移属 U11-3）。

## 3.2 插件 → 组件（Host SDK 组 / Game Host 组 / contracts 头）

| 插件 | Host SDK（`sdk->…`） | Game Host（`game->…`） | contracts 头 |
|---|---|---|---|
| `attribute_quality` | — | `read` | attribute_quality, plugin |
| `attribute_range_display` | capabilities | `read, ui_primitive` | attribute_quality, plugin |
| `autosell` | capabilities, config, events, hooks, panels, scheduler, store | — | attribute_quality, autosell, events, extension_bag, ownership, panels, plugin, ui |
| `core.ui` | hooks, panels, scheduler | — | host_sdk, ownership, panels, plugin, stack_limit, ui |
| `craft_ui` | capabilities, config, log | `extra, read, write` | craft_result, game_host, host_sdk, plugin |
| `custom_recipe` | capabilities, log | `extra, read, write` | attribute_quality, craft_result, events, game_host, host_sdk, ownership, plugin |
| `extension_bag` | capabilities, config, events, hooks, scheduler（+ `abi`） | `abi` | events, extension_bag, game_host, host_sdk, inventory_refresh, ownership, plugin, stack_codec, stack_limit, ui |
| `inventory_hooks` | hooks | — | host_sdk, inventory_refresh, plugin, stack_codec, stack_limit |
| `native_choice` | — | — | plugin, ui_choice |
| `push_all` | config, events, log | `read` | events, plugin |
| `quest` | log, scheduler | `extra, read` | plugin |
| `save_backup` | capabilities, hooks, panels, store | `extra` | game_host, host_sdk, ownership, panels, plugin, ui |
| `simple_mode` | config, events, log | `extra, read` | events, plugin |
| `special_equip` | hooks, log | `extra, read` | plugin |
| `stack_limit` | — | — | plugin, stack_limit |
| `text_layer` | hooks, log, text_scope | — | plugin, text_layer |
| `world_teleport` | capabilities, log, scheduler | — | plugin, ui_choice |

**读法**：插件对宿主设施的**新架构访问面**（Host SDK / Game Host / contracts）见上表；但**尚有 19 个文件保留 legacy 私有 include**（`core/native/**`、`data/native/**`、根部裸名头，逐文件见 §1.6），故"全部插件只 include `contracts/` 与自身目录"**当前不成立**，属 P3（≈C7）清除目标。

## 3.3 组件 ↔ 组件（编译期 include 边，实测）

**依赖方向规则**（设计 §2.2）：`contracts` ← 所有层；`infra` / `hooks` / `patch` ← `host`（及 hooks/patch 内部）；`game` ← `host` / `patch`（且 `game` 只被宿主实现使用）；`plugins` ← `contracts`（其余经运行期注册表）；`host` ← `plugins` **仅组合根**；`bridge` 属宿主侧，可跨 `host/infra/contracts/game`。

**层 → 层边（计数，Top）**

| 源 → 目标 | 边数 | 说明 |
|---|---|---|
| `host` → `infra` | 26 | 宿主驱动运行时机制 |
| `host` → `game` | 21 | `game_host.cpp` 逐访问器转发 |
| `host` → `contracts` | 17 | 契约类型/函数表 |
| `host` → `plugins/*` | 17 | **组合根**（assembly/plugin_registry 引用描述符 + SDK 分发） |
| `host` → `hooks` | 9 | registry/transaction/install/dispatch/popup_state |
| `host` → `patch` | 6 | instr_patch/ptr_slot |
| `host` → `host` | 27 | 宿主内部 |
| `infra` → `contracts` | 4 | version / commands / events / panels |
| `hooks` → `contracts` | 3 | `plugin.h`（资源声明类型） |
| `hooks` → `game` | 2 | `popup_state.cpp` → `game/access/{popup_state,symbols}`（game 层收口） |
| `patch` → `contracts` | 2 | `plugin.h` |
| `patch` → `game` | 2 | 站点表：`game/access/{patches,ptr_slots}` |
| `bridge` → `host` / `infra` / `contracts` / `game` | 见下 | 宿主侧入口 |
| `plugins/*` → `contracts` | 多数 | 唯一跨层出口 |
| **`plugins/*` → `plugins/*`（其他插件）** | **0** | 插件边界要求 |

**关键链路（逐条，实测文件边）**

- 契约聚合：`contracts/plugin.h → {host_sdk.h, game_host.h, version.h}`；`host_sdk.h → {commands.h, panels.h, result.h, version.h}`；`game_host.h → host_sdk.h`
- 装配根：`host/assembly.cpp → host/{plugin_registry,frame_source,ui_adapter,adapters} + infra/{commands,config,runtime,scheduler} + game/{patches,ptr_slots,symbols,clock} + hooks/transaction + patch/{instr_patch,ptr_slot} + contracts/{host_sdk,game_host,ownership}`
- 插件执行器：`host/plugin_registry.cpp → hooks/{registry,transaction,install} + patch/{instr_patch,ptr_slot} + infra/{capabilities,config,status} + game/{patches,ptr_slots} + contracts/{plugin,commands,ownership}`
- SDK 实现：`host/host_sdk.cpp → infra/{capabilities,commands,config,events,panels,scheduler,store,text_scope} + hooks/{dispatch,popup_state} + host/adapters.h`
- Game Host 实现：`host/game_host.cpp → game/access/{char,item_class,world,craft,save,quest,attr_range_ui,symbols,ui_choice} + game/clock`
- UI adapter：`host/ui_adapter.cpp → game/access/{ui_kit,ui_components}`
- 资源机制：`hooks/transaction.cpp → hooks/{registry,install} + contracts/plugin.h`；`hooks/popup_state.cpp → game/access/{popup_state,symbols}`；`patch/instr_patch.cpp → patch/claims.h + game/access/patches.h`；`patch/ptr_slot.cpp → patch/{claims,instr_patch} + game/access/ptr_slots.h`
- 运行时机制：`infra/commands.cpp → infra/{command_queue,config}`；`infra/config.cpp → contracts/events.h`；`infra/capabilities.h → contracts/version.h`；`infra/panels.h → contracts/panels.h`
- 入口桥：`bridge/gamebridge.cpp → host/{assembly,frame_source,core_patches} + bridge/native/gamebridge_internal.h`；`bridge/gamebridge_{autosell,lifecycle,push_all,simple_mode,domain_operations}.cpp → contracts/commands.h + infra/commands.h`；`bridge/gamebridge_store.cpp → infra/store.h`；`bridge/gamebridge_status.cpp → host/status_json.h`；`bridge/gamebridge_variant.cpp → host/variant_json.h`

## 3.4 三条运行期通道（编译期边之外）

| 通道 | 用途 | 关键设施 |
|---|---|---|
| **capability** | 插件/宿主间功能面（3.1 表） | `contracts/host_sdk.h` 的 `capabilities.require/lease_*` → `host/host_sdk.cpp` → `infra/capabilities` |
| **命令端口** | 跨线程副作用请求（HTTP/JVM → 主线程） | `contracts/commands.h` → `host/host_sdk.cpp` 的 `commands` → `infra/{commands,command_queue}` |
| **事件总线** | 配置变更等低频通知 | `contracts/events.h` → `infra/events`（订阅经 `sdk->events`） |

# 四、当前收口项与已知缺口

1. **C6/C7 未完成**：`core.api` 路由内核与 API provider 未落地（149 路由仍在 Kotlin controllers）；native config 早期自读未实现；架构脚本仍是八目录过渡映射，边界约束未硬失败。
2. **遗留目录**：`core/`、`api/`、`data/` 共 76 文件未吸收。
3. **引擎缺口**：
   - required consumer 在 provider 之后注册会**永久 `blocked`**（无重试）——已在真机复现一次（autosell），当前靠装配顺序规避；建议 C7 的 `check_plugins` 加静态校验或引擎补重试。
   - `in_flight_enter/leave` 未在插件 wrapper 接线（级联撤销的等待暂无真实计数来源）。
   - thunk 池在门禁关闭时不回收（约 51 次开关往返耗尽）；`g_patched_calls` 去重表容量 16 < 21 站点。
4. **契约缺口**：`contracts/ui.h` 部分 raw 面仍由 adapter 提供；`extension_bag` JSON 面 `write_json` 为 v1.1 预留。
5. **过渡依赖**：`stack_limit.h` 实现、`game/access/ui_*` 背后实现、`core/native/*` 多处仍在遗留位置（见 `map.md` 过渡依赖区）。

---

# 五、2026-09-19 架构审阅：已核验的阻断事实（P0 冻结）

> 审阅记录：`docs/refactor/architecture-review-2026-09-19.md`（结论 **NOT_ACCEPTED**）。以下 6 条已逐条核验**成立**（核验方式＝源码定位 + 脚本实测），未作任何代码改动。

| # | 事实 | 三态 | 证据 |
|---|---|---|---|
| 1 | 插件存在 19 个文件的 legacy 私有 include（目录前缀口径为 0） | 迁移债 | §1.6 双口径表；`check_layers` 层方向 32 条 |
| 2 | `arch_common.py` 过渡规则仍允许 `plugins → game/infra/hooks/patch/domains`，`BASE_LAYERS` 含 `domains`，另有 `_ROOT_OVERRIDES`/`_CORE_OVERRIDES`/`_TARGET_PREFIXES` 三张映射 | 迁移债 + **目标设计问题** | `arch_common.py:36,67-72,95,110,129` |
| 3 | 校验器"通过"＝无新增而非合规（层方向 32 / 符号·裸地址 9 / 资源 3 落在基线内，豁免 1） | 迁移债 | `check_layers/offsets/resources` 实测输出 |
| 4 | 单一 `gamebridge` target，无独立插件 target（无法证明插件可独立编译/不链接宿主私有符号） | 迁移债 | `CMakeLists.txt:4` + 144 条源文件 |
| 5 | `QolGameHostExtraApi` 过宽（`host/game_host.cpp` 15 处 `extra.*` 注入；跨域业务聚合） | **目标设计问题** | `contracts/game_host.h:233,259`；`host/game_host.cpp` |
| 6 | `contracts` 存在绕过 capability 的全局业务函数（`stack_limit.h`、`autosell.h`）；`host/core_patches` 把 S2 与 hub 混在同 owner/reason | **目标设计问题** | `contracts/stack_limit.h:5,9`、`contracts/autosell.h:44-46`、`host/core_patches.cpp:26-34` |

**审阅裁决（本轮目标，非当前事实）**：`core.*` 保持 **2 个**（`core.api` + `core.ui`，插件总数 17→18）；S2（codec / getter / 写路径 / 99–999 策略）最终归 `stack_limit`；Hub 兼容补丁不再与 S2 同 owner；`core.game` / `core.inventory` / `core.save` / `core.diagnostics` / `core.compat` / `core.config` / `core.log` / `core.registry` **一律不创建**（正确归属见审阅 §5）。

# 六、收口计划（P0–P6，2026-09-19 审阅版）

> **编号区分**：`C1–C7`＝已完成的插件边界迁移施工阶段（见 `ledger.md` §7）；`P0–P6`＝本轮审阅后的收口顺序。映射关系：P2≈C7 的 ABI 补齐、P3≈C7 的脚本硬失败与 include 清零、P5≈C6。

| 阶段 | 目标 | 关键交付 | 依赖 |
|---|---|---|---|
| **P0** | 冻结事实与词汇 | 本文双口径事实表 + 三态标记；`ledger.md` §7 记录审阅裁决与 P0–P6 | — |
| **P1** | 公共契约纯度 + S2 seam | 冻结 Game Host `item_count` facade；`stack_codec.h` 下沉为 `stack_limit` 实现头；消除 `stack_limit.h`/`autosell.h` 全局函数（改 provider 函数表/SDK）；明确 marker 与库存写回归属 | P0 |
| **P2** | Game Host / SDK ABI 面补齐 | 逐项冻结缺口面（log/config、save/quest/world、item_class、craft、EB 端口、UI 原语），配 fake 注入 host 测试 | P1 |
| **P3** | 插件 include 清零 + 独立构建 + 脚本硬失败 | 19 文件 legacy include 清除；per-plugin target（或等价链接矩阵）；`check_layers` 删过渡规则并硬失败 | P2 |
| **P4** | provider 声明 + capability DAG + extra 拆分 | 7 provider 补 provides/requires；`install_all` 改 DAG 执行；拆 `QolGameHostExtraApi`；修 5 条未声明消费 | P3 |
| **P5** | `core.api` / `core.ui` 出口收口 | 149 路由 owner 表 + `http.registry`；两 core 走普通生命周期，停用只降级出口 | P4 |
| **P6** | S2 residency + 收口与签署门禁 | S2 claims 迁 `stack_limit`（含 boot 时序论证）；provider residency × feature enabled 双态；清过渡映射与豁免 | P5 |

**单写者纪律**：`contracts/**`、`arch_common.py`、`host/` 内核、`ARCHITECTURE.md` 全程单一写者；并行仅限 `plugins/<id>/`。

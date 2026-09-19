# 分层与组件说明（**临时过程性文件**）

> ⚠️ **本文是临时过程性文件，不是权威文档，随时可能过时或被删除。**
> - 架构权威：根目录 `ARCHITECTURE.md`（当前 v0.3.14，待外部复审签署）
> - 阶段与验收状态：`docs/refactor/ledger.md`、`docs/refactor/verification.md`
> - 结构基线：`docs/refactor/baseline.md`、`docs/refactor/map.md`（脚本生成）
> - 生成时点：2026-09-19，仓库 HEAD `7e77af0`；来源＝`module/app/src/main/cpp/` 物理文件清单
> - 口径：文件数/行数为 `wc -l`（含空行与注释）；只列**当前实际存在**的模块，设计提及但未落地的在「收口项」标注
> - 用途：给人快速理解现状；**任何决策请回到权威文档**，本文件不得作为落地依据

## 0. 编号口径

**架构分层只有一套：L0–L5**（L0 contracts / L1 game / L2 infra+hooks+patch / L3 plugins / L4 出口面 / L5 host）。
「胶囊」（可外置插件）**不是新层**，而是既有各层内的组件与约束：

| 胶囊要素 | 所属层 | 具体组件 |
|---|---|---|
| Host SDK / Game Host / UI 契约 | **L0** | `contracts/host_sdk.h`、`contracts/game_host.h`、`contracts/ui.h` |
| 宿主 ABI 实现 | **L5** | `host/host_sdk.cpp`、`host/game_host.cpp`、`host/ui_adapter.cpp` |
| 插件边界约束 | **L3** | `plugins/<id>/` 只允许 include `contracts/` 与自身目录（17/17 已合规） |

> 施工阶段（工作分解顺序）另有编号 **C1–C7**，仅记录在 `ledger.md` §7，**不是分层编号**。

---

# 一、分层编号（L0–L5）

| 层 | 目录 | 文件/行数 | 职责 |
|---|---|---|---|
| L0 | `contracts/` | 19 / 1,630 | 跨层稳定接口：POD、能力契约、宿主 SDK / Game Host 函数表；**不含游戏逻辑** |
| L1 | `game/` | 36 / 2,725 | 唯一承接游戏版本耦合（VMA/偏移/ABI/函数调用）；**禁止业务规则** |
| L2 | `infra/` + `hooks/` + `patch/` | 40 / 4,203 | 运行时机制：调度/队列/事件/能力/状态/配置/存储 + 资源登记/事务/分派 + 指令改写 |
| L3 | `plugins/` | 154 / 30,974 | 17 个内置插件（功能实现 + 能力 provider/consumer + 面板）；胶囊约束：只能 include `contracts/` 与自身目录 |
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
| `stack_codec.h` | S2 数量编码（canonical ↔ a/b 段、视图计数/钳制/回写判定）——原 `game/read/` 迁入，供多插件共享 |
| `stack_limit.h` | 上限状态只读端口声明（实现在宿主过渡侧） |
| `attribute_quality.h` | `attribute.quality` 能力（档位/百分位/区间探测，**不输出颜色码**） |
| `craft_result.h` | 合成结果能力（12 函数：模式判定/放料/准备/执行/材料需求/产物/描述替换） |
| `extension_bag.h` | 扩展背包能力（`ExtensionBagApi` 37 函数 7 组 + `ExtensionBagMoveObservation` + 3 枚举） |
| `inventory_refresh.h` | 刷新 trampoline 能力（绕开被 hook 入口直调原函数，防重入） |
| `text_layer.h` | 文本层能力（subscribe/unsubscribe） |
| `ui_choice.h` | 原生选择框能力（`UiChoiceSpec` + open/active/close） |
| `autosell.h` | 自动出售端口（配置/特殊类型 + 运行时读取/提交/持久化声明） |

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

**收口项**：`game/write`、`game/ops`、`game/variant` 未独立落地；变体表、`qol_log`、`module_save`、`save_enter/exit`、`stack_limit_state` 仍在遗留 `core/native`；`data/native/game_symbols.h` 待 L7 收口。

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

**胶囊合规**：17/17 插件对 `game/ infra/ hooks/ patch/ host/` 及跨插件 include 计数为 **0**；真机 smoke 通过（VM-CAPSULE）。

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

# 二、胶囊迁移施工阶段（C1–C7，**非分层编号**）

| 阶段 | 内容 | 状态 |
|---|---|---|
| C1 | ABI 骨架：`contracts/host_sdk.h`、`contracts/game_host.h`、`PluginContext` 注入 | ✅ 完成 |
| C2 | 宿主 adapter：SDK / Game Host 函数体与注入 | ✅ 完成 |
| C3 | `domains/` 取消：拆 `attribute_quality`(provider) + `attribute_range_display`(消费者) | ✅ 完成 |
| C4 | 小中插件胶囊化（9 个）+ U11-1 契约/adapter + U11-2 `core.ui` 组件实现 | ✅ 完成 |
| C5 | `inventory_hooks` + `extension_bag` 胶囊化（含 `save_backup`/`autosell`；`stack_codec` 迁 contracts、store 容器级 ABI、SDK `intercept_blocks`） | ✅ 完成 |
| C6 | `core.api` 路由内核与 API provider 拆分（149 路由 owner 表、U10） | ⬜ 待办 |
| C7 | native config 早期自读、脚本切七目录、**胶囊硬失败**（`check_layers` 收紧、删过渡映射） | ⬜ 待办 |

**验证状态**：C1–C5 已通过真机 smoke（启动状态 + 门禁两态 + 生命周期往返，见 VM-CAPSULE）；各插件的**功能细则**仍按各自 VM 卡待补。C6/C7 未开工。

---

# 三、当前收口项与已知缺口

1. **C6/C7 未完成**：`core.api` 路由内核与 API provider 未落地（149 路由仍在 Kotlin controllers）；native config 早期自读未实现；架构脚本仍是八目录过渡映射，胶囊约束未硬失败。
2. **遗留目录**：`core/`、`api/`、`data/` 共 76 文件未吸收。
3. **引擎缺口**：
   - required consumer 在 provider 之后注册会**永久 `blocked`**（无重试）——已在真机复现一次（autosell），当前靠装配顺序规避；建议 C7 的 `check_plugins` 加静态校验或引擎补重试。
   - `in_flight_enter/leave` 未在插件 wrapper 接线（级联撤销的等待暂无真实计数来源）。
   - thunk 池在门禁关闭时不回收（约 51 次开关往返耗尽）；`g_patched_calls` 去重表容量 16 < 21 站点。
4. **契约缺口**：`contracts/ui.h` 部分 raw 面仍由 adapter 提供；`extension_bag` JSON 面 `write_json` 为 v1.1 预留。
5. **过渡依赖**：`stack_limit.h` 实现、`game/access/ui_*` 背后实现、`core/native/*` 多处仍在遗留位置（见 `map.md` 过渡依赖区）。

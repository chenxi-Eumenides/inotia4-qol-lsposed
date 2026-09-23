# 重构迁移台账

> 本文件是紧凑的迁移状态记录，不定义最终架构。
>
> - **最终目标唯一权威**：根目录 `ARCHITECTURE.md`。
> - **当前优化唯一权威**：`current-optimization-review.md`。
> - `ledger.md`、`verification.md`、`baseline.md`、`map.md`、`api-route-map.md` 仅记录迁移过程、现状映射和验证材料。

## 1. 阶段状态

| 阶段 | 当前范围 | 状态 | 主要证据或出口 |
|---|---|---|---|
| P0 | 校验脚本族与基线棘轮 | ✅ 完成 | `check_arch.py` 可运行；基线只允许收缩 |
| P1 | `contracts/`、`infra/`、`game/` 骨架与最小诊断接口 | ✅ 完成 | VM-P1-02、VM-P1-03 |
| K1 | kernel-complete：零业务插件 Debug 启动 + Debug device probe 机制验收 | ✅ Debug/probe 真机证据已取得；release 隔离未执行 | VM-K1-01、VM-K1-02；`docs/history/kernel-complete-20260922/` |
| K2 | provider 与功能插件并行：`world_facts`（`world.facts`，5 消费者）、`item_facts`（`item.facts`，7 消费者）、`character_facts`（`character.facts`，3 消费者）三批 + cascade_stopped activate 修复 | ✅ 三批真机闭环（VM-K2-01/02/03）；剩余复用语义（craft/save/quest/choice）多为单消费者，随阶段 4/5 处理 | VM-K2-01、VM-K2-02、VM-K2-03；`docs/history/provider-{world,item,character}-facts-20260922/` |
| K4 | GameHost 收窄（Z2：插件域语义 + 受限 Adapter Port）：批 1 Port 注入 + typed command + quest 试点；批 2 三 provider 脱离 + typed handler 幂等修复；批 3 EB 写面 36 函数内化；批 4a choice 内化；批 4b save 收口；批 4c craft 读面 provider + owner 写面；批 5 ABI major=2 + extras ports + facade 迁移 + 负例 | ✅ 七批全部完成并真机闭环（VM-K4-01..05，4c/5 为调用通道+门禁口径）；`QolGameHost` 只剩 item_count facade 与宿主设施，实现全归 `game/adapter_ports`；深度行为卡（VM-43/46/P4-07/D2 游戏内操作）待人工配合时统一重跑 | VM-K4-01..05；`docs/history/gamehost-narrow-b{1,2,3,4a,4b,4c,5}-20260922/` |
| P2 | `simple_mode` 纵向样板 | ✅ 完成 | VM-P2-01 |
| P3 | 资源事务、异步步进、回滚、在途归零、生命周期状态 | ✅ 完成 | VM-P3-01、VM-P3-02、VM-P3-03 |
| P4 | 内置静态插件批量迁移 | 🔄 进行中；阶段 A、B、C 已完成，D1 已完成待收尾 | VM-P4-01..08、VM-43..46、VM-D1、VM-D2-01..04 |
| P5 | `core.api`、`core.ui`、动态 capability、JNI 与边界脚本收口 | 🔄 进行中；批 1、批 2、批 3、批 4-0、批 4-1、批 4-2、批 4-3、批 4-4 已完成：14 条静态/配置查询路由 owner 真实化，18 条 debug/feature 路由完成 owner 审计与真机验证，HTTP dispatch 接缝及 `game_frame` 试点完成，7 条 world 实时查询由 `world_facts` 接管，6 条 item GET 由 `item_facts`、4 条 quest GET 由 `quest` 接管，4 条系统快照查询（snapshot/game/info/game_frame）由新 `system_facts` 接管（新增 `QOL_PORT_SYSTEM` adapter port + 启动期身份 JNI，快照重排/class_name 注入下沉 native；`/api/system/events` 因请求驱动有状态 diff、4 条 `/api/ui*` GET 因 ActivityThread 同意页检测留在 Kotlin），15 条 character GET（party 12 + mercenary 3）由 `character_facts` 接管（新增 `QOL_PORT_CHARACTER_HTTP` adapter port，成员注入/restructureRole/装备 restructureItem/技能名注入与 leader·status 多源组合下沉 native，技能名随启动期静态文本表 `S` 行下发；19 条 POST 含 `/api/op/*` 留批 5）；启动期静态名称/片段表 JNI 下发；批 4-2 顺带修复 lifecycle deactivate 不摘 http handler 的引擎缺陷，无 provides owner 停用语义与墓碑恢复已真机闭环；阶段 5 写面按用户裁决 A 转入票据契约（先修订 ARCHITECTURE 冻结规则，再在公共 ABI 尾部追加结果通道、保留旧 handler 签名另增结果变体、unknown/expired 与 failed 三态区分、16 KB/512 KB 尽力保留窗口）；批 5-2 已完成 EB 7 条回补为主线程 owner command + 票据；批 5-3 完成 quit_quest 迁移并修复其恒 405 不可达缺陷；批 5-4 完成 save/enter_slot/create_slot 迁 `save_backup` owner（新增**有界延迟结算**内核能力：转场类写在游戏世界态提交后结算，票据结算前保持 pending）；批 5-5 完成世界写 5 条迁 owner；批 5-6a/5-6b 完成世界域 `core.world` 命名统一与读写一体（写侧由 `world_teleport` 并入，并修复 world GET 忽略状态码的既有缺陷）；批 5-6c 完成其余领域插件改名为 `core.<domain>`（character/item/save/system/craft/choice/quest；能力头与能力 id 不变；save/craft 写面并入留后续）；批 5-3 完成 quit_quest 迁移并修复其恒 405 不可达缺陷；批 5-4 完成 save/enter_slot/create_slot 迁 save_backup owner（新增有界延迟结算内核能力，转场类写在世界态提交后结算）；批 5-0、批 5-1 已完成：`http.dispatch` command handler 机制补全（`register_command` 真实实现 + `dispatch` POST 分支 + `op_gated` 门禁 + 墓碑/并发保护；新增 host seam `http_dispatch_set_op_enabled` + JNI `nativeSetOpEnabled`，由 `ModuleConfig.opEnabled` 同步）+ `POST /api/op/features/lifecycle` 试点迁移（owner `core.api`，Kotlin 改为 `nativeHttpDispatch` 透传；成功/失败/op 拒绝三类响应逐字节冻结，真机 403/ticket/unknown action/缺字段四态验证通过）；批 5-1 完成已有真实 owner 的 11 条 POST（`extension_bag` 7 + `core.ui` 4，插件内自注册 command handler 直调既有业务函数；body 合法性校验留 Kotlin 传输层保 400 冻结；批量注册按 method 分流；顺带修复自注册 handler 停用不摘除缺陷），真机 11 条响应/校验文本/双向 lifecycle/smoke 全过）；批 5-S1：`save_backup` 以声明式 http 路由接管备份四端点（export/import/delete/list，VM-K5-05c，真机 4 条冻结/功能路径/生命周期/smoke 全过）；后续批 5-2 至 5-6d 已完成：批 5-2 EB 7 条回补为主线程命令+票据；批 5-3 `quit_quest` 迁 owner（并修复其恒 405 的不可达缺陷）；批 5-4 save 三写迁票据契约 + **有界延迟结算内核能力**；批 5-5 世界写 5 条迁 owner；批 5-6a/b/c 领域架构统一为 `core.<domain>`（ARCHITECTURE 规则先行、读写一体）；批 5-6d 按裁决 A 将 save 核心写入并入 `core.save`（backup 留 `save_backup`）。剩余：ui 5、item 13、character 20 与 12 条 501 占位文档标注 | `VM-K5-01`、`VM-K5-02`、`VM-K5-03`、`VM-K5-04a`、`VM-K5-04b`、`VM-K5-04c`、`VM-K5-04d`、`VM-K5-04e`、`VM-K5-05a`、`VM-K5-05b`、`VM-K5-05c`、`VM-K5-06a`、`VM-K5-06b`、`VM-K5-06c`、`VM-K5-06d`、`VM-K5-06e`、`VM-K5-06f`、`VM-K5-06g`、`VM-K5-06h`、`VM-K5-06i`；`scripts/verification/smoke_baseline_v0.7.13.json`；`docs/refactor/api-route-map.md`；`docs/history/stage5-b{2,3}-20260922/`、`docs/history/stage5-b{4-0,4-1}-20260922/`、`docs/history/stage5-b42-20260923/`、`docs/history/stage5-b43-20260923/`、`docs/history/stage5-b44-20260923/`、`docs/history/stage5-b50-20260923/`、`docs/history/stage5-b51-20260923/`、`docs/history/stage5-b5s1-20260923/`、`docs/history/stage5-ticket-contract-20260923/`、`docs/history/stage5-b52-20260923/`、`docs/history/stage5-b54-20260923/`、`docs/history/stage5-b55-20260923/`、`docs/history/stage5-b53-20260923/`、`docs/history/stage5-b56a-20260923/`、`docs/history/stage5-b56b-20260923/`、`docs/history/stage5-b56c-20260923/`、`docs/history/stage5-b56d-20260923/` |
| P6 | S2 `stack_limit` 归属、provider 常驻与功能启用双态、收口门禁 | 🔄 进行中 | host 已通过；VM-P6-01 真机证据待补 |
| P7（可选） | 外部插件加载与分发 | ⬜ 未开始 | 仅在前六阶段完成后进入 |

## 2. 当前迁移面

| 迁移面 | 当前状态 | 未闭合项 / 清除条件 | 真机验收指针 |
|---|---|---|---|
| 插件目录 | 大部分功能已进入 `plugins/<id>/`；`feature/` 已清空，`feature/patch` 已拆除 | 清除插件私有 legacy include、独立构建与边界负例；通过架构检查并收缩基线 | VM-BOUNDARY；各插件行为卡 |
| `extension_bag` | D2a–D2c 已完成主要契约、资源声明、消费者切换和物理迁移 | 完成余下 hook/ptr/UI/生命周期收口、独立 target 与边界检查 | VM-D2-01..04；受影响 VM-43、VM-P4-07 |
| UI | D1 的基础原语、文本层和共享组件已归位；面板已分配到对应插件 | U11-3：迁移 `settings`、`autosell`、`save_backup` 面板消费者；动态 UI 绑定器清除临时豁免 | VM-D1 |
| S2 / `stack_limit` | codec、策略和 999 有效态由 `plugins/stack_limit` 持有；provider residency 与 `feature_enabled` 已分离 | 完成 getter/写路径全量切换、持久数据解释保证、过渡入口清除 | VM-P1-S2、VM-P6-01 |
| HTTP | 143 条路径已有 route registry、owner 表和诊断出口 | 逐路由迁移真实 owner；`core.api` 只保留传输、注册、门控、lease 和诊断职责 | 出口插件卡；`api-route-map.md` |

## 3. 仍有效的债务与清除条件

通用清除条件：完成目标职责迁移，相关 host 测试与架构检查通过，受影响行为卡取得真机日志；基线条目和临时映射随迁移收缩。

| 债务 | 当前事实 | 清除条件 |
|---|---|---|
| Loader / bootstrap 仍执行业务 | Loader 入口和 bootstrap 仍无条件触发部分 IAP、协议页、存档、UI、扩展背包等行为 | 业务行为改由插件注册和生命周期管理；零业务插件组合可启动、ready、无业务状态和副作用 |
| 宿主兼容补丁混合业务 | `host/core_patches` 仍同时包含 Hive 登录跳过与 monster 数量兼容逻辑 | 将业务补丁归对应 owner；宿主只保留通用机制和版本适配；受影响卡通过 |
| Game Host 过宽 | `QolGameHost` 仍聚合角色、物品、库存、世界和扩展背包读面 | 按 provider / 功能 owner 拆分高层语义；保留低层受控适配；ABI、线程和错误语义回归通过 |
| 官方 provider 不完整 | 角色、世界、库存等稳定复用语义仍有部分调用依赖宽 Game Host 或 legacy 面 | 每个 provider 有真实消费者、窄 capability / snapshot / owner command 与对应回归卡 |
| Kotlin HTTP 仍承载业务 | Controller、Service 和 `NativeBridge` 调用链仍包含业务参数处理、状态拼装和复合查询 | 路由逐条绑定 owner；Kotlin 只做 AndServer 传输适配；冻结路径、方法和成功响应保持不变 |
| legacy 目录和过渡映射 | `api/native`、`core/native`、根目录聚合文件及脚本 legacy 映射仍存在 | 各文件职责吸收到 `game/`、`infra/`、`plugins/` 或 `contracts/`；删除映射；边界负例确实失败；基线收缩 |
| 插件私有越层与单一目标 | 仍有 legacy 私有 include；独立 target 不能覆盖全部宿主私有依赖 | 插件只 include `contracts/` 与自身目录；每个 target 独立编译并通过负例检查 |
| `contracts` 业务入口 | `contracts/autosell.h` 仍有全局业务函数，部分消费者尚未转 capability / owner command | 改为 provider 函数表、Host SDK 或 owner capability 后删除全局入口；host 与架构检查通过 |
| 内核装配证据不足 | K1 的零业务插件 Debug 与 device probe 真机证据已取得；release 构建及 release 隔离验收尚未执行 | 用户明确许可后执行 release 构建，确认 probe 未链接、未注册、未进入 release 清单，并补充 release 隔离证据 |
| 引擎未闭合项 | VM-FIX-01 仍登记生产 `in_flight_enter/leave` wrapper 未接线；required 消费者 `cascade_stopped` 后 activate 不执行的缺口已修复（`plugin_registry.cpp`，host 用例覆盖恢复与缺依赖 blocked），VM-K2-01 真机闭环 | 接线并验证在途等待；更新受影响验收卡 |
| 生命周期票据与状态不同步 | VM-K2-01 实测：消费者 activate 时依赖方尚未 enable 则票据 `failed`，随后 `sync_gates` 重评估自动拉起 enabled，票据不追认；autosell `degraded:true` 残留（error 已 None） | 票据终态追认自动恢复路径；`sync_gates` 恢复时同步清除 `degraded` 残留；以 VM-K2-01 后续回归卡验证 |

## 4. 临时豁免与保留项

### 4.1 资源豁免

`plugins/core.ui/panels/settings.cpp` 的主菜单「更多游戏」运行时控件实例槽属于 `resource_unknown_symbol`。当前由主线程帧任务管理，不能按稳定 GOT 槽静态归属。

清除条件：P5/U11 动态 UI 绑定管理器建立后，登记 `own:ui.main_menu.more_games_entry` 与运行期 lease，并通过资源检查。

### 4.2 legacy 实现保留

- `core/native/game_cache.{h,cpp}`：仍横跨 `api/native`、`game/access` 和 `game/state`，目标映射为 `infra`。
- `core/native/module_save.cpp` 与 `module_save_port.h`：实现仍依赖旧游戏访问面；稳定声明暂留 `contracts`，实现映射为 legacy/save。
- `core/native/inventory_trade.{h,cpp}`：仍依赖游戏访问面、状态面和符号头，映射为 legacy/inventory。
- `game_system.{h,cpp}`、`game_dialog*`：直接迁入目标层会引入 legacy 依赖，须先拆出低层原语与上层聚合。

清除条件：完成对应访问面吸收和目标层适配，迁入目标目录，删除过渡映射，并通过 `check_arch.py`、`check_offsets.py` 及相关 host 测试。

## 5. 验收指针

- 已有阶段证据：VM-P1-02/03、VM-P2-01、VM-P3-01/02/03、VM-P4-01..08、VM-43..46、VM-FIX-01、VM-BOUNDARY。
- 阶段 1 kernel-complete：VM-K1-01（零业务插件 Debug）与 VM-K1-02（Debug device probe）已取得 monster v29 真机证据；普通 Debug 已恢复。release 构建未执行，K1 不宣称 release PASS。
- 待补行为证据：VM-P1-S2、VM-P6-01、VM-D1、VM-D2-01..04，以及 P5 出口插件卡。
- `verification.md` 定义 PASS、NOT_ACCEPTED、BLOCKED 和证据要求；缺少真机日志时，不以 host、静态检查、Debug 构建或 API 成功替代。
- `baseline.md`、`map.md` 为生成材料；`api-route-map.md` 为路由过程核对材料。本台账只引用其结果，不改写其生成口径。

## 6. 更新边界

迁移实现、当前债务和优化顺序先按 `ARCHITECTURE.md` 与 `current-optimization-review.md` 判断，再在本台账记录状态和证据指针。本文件不得新增目标职责、改变 API 契约或以过程材料取代目标架构。

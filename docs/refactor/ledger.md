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
| K1 | kernel-complete：零业务插件 Debug 启动 + Debug device probe 机制验收 | ✅ Debug/probe 真机证据已取得；按项目口径不构建、不发布零业务插件 Release 包，kernel-only 组合仅在 Debug 通道验证 | VM-K1-01、VM-K1-02；`docs/history/kernel-complete-20260922/` |
| K2 | provider 与功能插件并行：`world_facts`（`world.facts`，5 消费者）、`item_facts`（`item.facts`，7 消费者）、`character_facts`（`character.facts`，3 消费者）三批 + cascade_stopped activate 修复 | ✅ 三批真机闭环（VM-K2-01/02/03）；craft/save/quest/choice 语义后续已由 K4 对应 provider/owner 与 Adapter Port 收口 | VM-K2-01、VM-K2-02、VM-K2-03、VM-K4-01、VM-K4-04a、VM-K4-04b、VM-K4-04c；`docs/history/provider-{world,item,character}-facts-20260922/` |
| K4 | GameHost 收窄（Z2：插件域语义 + 受限 Adapter Port）：批 1 Port 注入 + typed command + quest 试点；批 2 三 provider 脱离 + typed handler 幂等修复；批 3 EB 写面 36 函数内化；批 4a choice 内化；批 4b save 收口；批 4c craft 读面 provider + owner 写面；批 5 ABI major=2 + extras ports + facade 迁移 + 负例 | ✅ 七批全部完成并真机闭环（VM-K4-01..05，4c/5 为调用通道+门禁口径）；`QolGameHost` 只剩 item_count facade 与宿主设施，实现全归 `game/adapter_ports`；深度行为卡（VM-43/46/P4-07/D2 游戏内操作）待人工配合时统一重跑 | VM-K4-01..05；`docs/history/gamehost-narrow-b{1,2,3,4a,4b,4c,5}-20260922/` |
| P2 | `simple_mode` 纵向样板 | ✅ 完成 | VM-P2-01 |
| P3 | 资源事务、异步步进、回滚、在途归零、生命周期状态 | ✅ 完成 | VM-P3-01、VM-P3-02、VM-P3-03 |
| P4 | 内置静态插件批量迁移 | 🔄 进行中；阶段 A、B、C 已完成，D1 已完成待收尾 | VM-P4-01..08、VM-43..46、VM-D1、VM-D2-01..04 |
| P5 | `core.api` 出口、领域 HTTP owner、动态 capability、JNI 与边界脚本收口 | 🔄 进行中；item 写面 13 条归 `core.item`，character 已实现写面 20 条归 `core.character`；Kotlin 400 校验保持、成功投递返回票据并经 §7.9 回取原结果，party 4 条 POST 与通配映射同 controller 且前置声明；唯一已证实未实现的 `set_skill_usage` 由临时 `core.feature` 保持 501。路由/契约/VM-K5-08h 真机证据已归档；本批真机卡按批次延后，P5 汇总收尾仍按出口插件卡执行 | VM-K5-01..08h；VM-P5-CORE-FEATURE-01；`docs/history/stage5-item-character-writes-20260924/README.md`；`docs/refactor/api-route-map.md`；`docs/reference/api-reference.md` |
| P6 | S2 `stack_limit` 归属、provider 常驻与功能启用双态、收口门禁 | 🔄 进行中 | host 已通过；VM-P6-01 真机证据待补 |
| P7（可选） | 外部插件加载与分发 | ⬜ 未开始 | 仅在前六阶段完成后进入 |

## 2. 当前迁移面

| 迁移面 | 当前状态 | 未闭合项 / 清除条件 | 真机验收指针 |
|---|---|---|---|
| 插件目录 | 大部分功能已进入 `plugins/<id>/`；`feature/` 已清空，`feature/patch` 已拆除 | 清除插件私有 legacy include、独立构建与边界负例；通过架构检查并收缩基线 | VM-BOUNDARY；各插件行为卡 |
| `extension_bag` | D2a–D2c 已完成主要契约、资源声明、消费者切换和物理迁移。D2d 代码已接入 `contracts/extension_bag.h` v2.0 固定宽度 move capability：`game_system.cpp` 经 registry 取用，能力缺失时回退 `data_op_move_item`；`contracts/extension_bag_port.h` 的 legacy 全局声明及 api/native 直调仍待阶段 6 删除批清退 | D2d 代码已收口；legacy 面清退并入阶段 6。完成余下 hook/ptr/UI/生命周期收口、独立 target 与边界检查 | VM-D2D-01/02；VM-K5-08h；受影响 VM-43、VM-P4-07 |
| UI | D1 的基础原语、文本层和共享组件已归位；面板已分配到对应插件 | U11-3：迁移 `settings`、`autosell`、`save_backup` 面板消费者；动态 UI 绑定器清除临时豁免 | VM-D1 |
| S2 / `stack_limit` | codec、策略和 999 有效态由 `plugins/stack_limit` 持有；provider residency 与 `feature_enabled` 已分离 | 完成 getter/写路径全量切换、持久数据解释保证、过渡入口清除 | VM-P1-S2、VM-P6-01 |
| HTTP | 143 条路径均登记在 route registry；静态数据 12 条 owner=`core.data`（`static.data=0`），item/character 写面归 `core.item`、`core.character`；`set_skill_usage` owner 为 `core.feature` | `core.data` 批1已接管静态数据路由与分批资产 ingress；enrichment/非 HTTP 消费者留批2。`core.feature` 是开发迁移期临时插件，仅暂存已证实未实现端点；实现后迁入对应领域，清空后移除，不属于最终发布插件。保持 `core.api` 的传输、注册、门控、lease 和诊断职责；`GET /api/system/events` 与 4 条 `GET /api/ui*` 的 Kotlin 处理器按现状保留；扩展袋 move 的 string 面见 D2d | VM-K5-08h；VM-P5-CORE-FEATURE-01；VM-P5-CORE-DATA-01/02；`api-route-map.md` |

## 3. 仍有效的债务与清除条件

通用清除条件：完成目标职责迁移，相关 host 测试与架构检查通过，受影响行为卡取得真机日志；基线条目和临时映射随迁移收缩。

| 债务 | 当前事实 | 清除条件 |
|---|---|---|
| Loader / bootstrap 仍执行业务 | Loader 入口和 bootstrap 仍无条件触发部分 IAP、协议页、存档、UI、扩展背包等行为 | 业务行为改由插件注册和生命周期管理；零业务插件组合可启动、ready、无业务状态和副作用 |
| 宿主兼容补丁混合业务 | `host/core_patches` 仍同时包含 Hive 登录跳过与 monster 数量兼容逻辑 | 将业务补丁归对应 owner；宿主只保留通用机制和版本适配；受影响卡通过 |
| Game Host 过宽 | `QolGameHost` 仍聚合角色、物品、库存、世界和扩展背包读面 | 按 provider / 功能 owner 拆分高层语义；保留低层受控适配；ABI、线程和错误语义回归通过 |
| Kotlin HTTP 仍承载业务 | Controller、Service 和 `NativeBridge` 调用链仍包含业务参数处理、状态拼装和复合查询 | 路由逐条绑定 owner；Kotlin 只做 AndServer 传输适配；冻结路径、方法和成功响应保持不变 |
| legacy 目录和过渡映射 | `api/native`、`core/native`、根目录聚合文件及脚本 legacy 映射仍存在 | 各文件职责吸收到 `game/`、`infra/`、`plugins/` 或 `contracts/`；删除映射；边界负例确实失败；基线收缩 |
| 插件私有越层与单一目标 | 仍有 legacy 私有 include；独立 target 不能覆盖全部宿主私有依赖 | 插件只 include `contracts/` 与自身目录；每个 target 独立编译并通过负例检查 |
| `contracts` 业务入口 | `contracts/autosell.h` 仍有全局业务函数，部分消费者尚未转 capability / owner command | 改为 provider 函数表、Host SDK 或 owner capability 后删除全局入口；host 与架构检查通过 |
| 引擎未闭合项 | 生产 `in_flight_enter/leave` wrapper 已接线：Host SDK 导出、registry 实现及 inventory_hooks/extension_bag 生产调用均已具备。VM-K2-01 仍观察到自动恢复路径的票据终态未追认、`degraded` 未同步清除 | 后续真机回归恢复路径，验证票据终态与 degraded 同步；证据沿用 VM-K2-01 |
| 生命周期票据与状态不同步 | VM-K2-01 实测：消费者 activate 时依赖方尚未 enable 则票据 `failed`，随后 `sync_gates` 重评估自动拉起 enabled，票据不追认；autosell `degraded:true` 残留（error 已 None） | 票据终态追认自动恢复路径；`sync_gates` 恢复时同步清除 `degraded` 残留；以 VM-K2-01 后续回归卡验证 |
| `item_count_api` legacy 访问器（#10） | 已收口（d90918c2）：`contracts::item_count_api()` 旧全局访问器删除；state builder、add-item 编码写入、`inventory_trade` 售价数量、`game/read/item_class.cpp` 编码判定四组消费者改走 typed API 表/owner serializer；`QolItemCountApi` typed 表与 `QolGameHost.item_count` 注入面保留（inventory_hooks/autosell 消费者合法使用） | api/native `data_op_set_money` 等薄转发与 `build_inventory_json` 残余消费者的清退并入阶段 6 删除批；扩展袋 >99 真机数量视图卡待插件独立测试批补 |
| bridge 直调 api/native（G-item/T2 序列后剩余 14 符号） | character/item 写面 JNI 已全部改 typed/既有原语（20 character + 12 item-thin + 4 item-legacy + 2 character 保留 = 38 符号清零，T2d/T2e/T2f/T2g-1）；bridge 剩余直调 14 符号：world 5（`teleport`/`move`/`walk`/`walk_stop`/`interact`）、save 3（`save`/`enter_slot`/`create_slot`）、ui 5（`npc_interact`/`dialog_select`/`main_menu`/`panel_open`/`panel_close`）、quest 1（`quest_quit`）；X 类 EB 全局 2（`extension_bag_use_item`/`extension_bag_move_item`，另 `extension_bag_put_jewel` 0 消费者） | 各域先立 G 前置批（owner/typed 化，`game_system.cpp`/dialog 消费者同接）后 bridge 才可转接；X 类退役需 capability 扩展（`use_item` 无对应成员）与全局声明清除（触碰 contracts，需专项授权） |
| B2 门槛未通过：owner raw inventory 投影未暴露 | inventory 读面切换的硬前置未满足（4562f929 B0 取证）：core.item 的 raw `serialize_inventory_json`（`plugins/core.item/plugin.cpp:490`）位于匿名 namespace，未经任何公开接口暴露；`item.facts` 无 inventory JSON 字段；`GET /api/item/inventory` 为富化 schema，与 legacy raw `build_inventory_json` 不同字节。B0 已冻结 legacy 输出（`frozen_inventory_json`，4 fixture 等值），但 owner-raw==legacy 无法在 tests-only 下构造 | owner（core.item 插件）暴露 raw inventory 投影（新 capability 字段或 host 注入面），并以 B0 fake 环境建立 owner-raw golden 通过后，方可执行 B2 inventory 槽切换；期间不得切换或改写 JNI/HTTP 字节 |
| api/native 残余 TU 与 kernel stub | `game_inventory.cpp` 只剩 `!QOL_KERNEL_ONLY` 的 `game_inventory_read.inc` 与 3 个 kernel stub（`append_item_attrs`/`build_inventory_json`/`data_debug_item_raw_json`）；`game_character.cpp`/`game_party.cpp` 只剩读 builder（`member_json`/`build_player_json`/`build_skills_json`/`build_party_json`/`build_mercenaries_json`） | 读面 owner 快照路径（T2h B1/B2/B3）与 `data_debug_item_raw_json` 落点确定后，TU/stub 随物理删除；K1 下 TU 保留 |

## 4. 临时豁免与保留项

### 4.1 资源豁免

`plugins/core.ui/panels/settings.cpp` 的主菜单「更多游戏」运行时控件实例槽属于 `resource_unknown_symbol`。当前由主线程帧任务管理，不能按稳定 GOT 槽静态归属。

清除条件：P5/U11 动态 UI 绑定管理器建立后，登记 `own:ui.main_menu.more_games_entry` 与运行期 lease，并通过资源检查。

### 4.2 legacy 实现保留

- `core/native/game_cache.{h,cpp}`：仍横跨 `game/access`/`game/state`/`game/symbols` 与 `game_world.h`/`game_system.h`，目标映射为 `infra`。T2h-B'（9cfbe382）后 player/party/inventory/skills/mercenaries 五个槽的 legacy builder 改经 host 装配期注入（`frame_cache_set_json_builder`，A1 host↔infra 内部面），已删除对 `api/native/{game_character,game_party,game_inventory}.h` 的 include（layers 30→27）；world/system 槽与 cache 机制的 5 条 infra→game/legacy 边留批⑥。
- `core/native/module_save.cpp` 与 `module_save_port.h`：实现仍依赖旧游戏访问面；稳定声明暂留 `contracts`，实现映射为 legacy/save。
- `core/native/inventory_trade.{h,cpp}`：仍依赖游戏访问面、状态面和符号头，映射为 legacy/inventory。
- `game_system.{h,cpp}`、`game_dialog*`：直接迁入目标层会引入 legacy 依赖，须先拆出低层原语与上层聚合。

清除条件：完成对应访问面吸收和目标层适配，迁入目标目录，删除过渡映射，并通过 `check_arch.py`、`check_offsets.py` 及相关 host 测试。

## 5. 验收指针

- 已有阶段证据：VM-P1-02/03、VM-P2-01、VM-P3-01/02/03、VM-P4-01..08、VM-43..46、VM-FIX-01、VM-BOUNDARY。
- 阶段 1 kernel-complete：VM-K1-01（零业务插件 Debug）与 VM-K1-02（Debug device probe）已取得 monster v29 真机证据；普通 Debug 已恢复。项目口径是不构建、不发布零业务插件的 Release 包；kernel-only 组合仅在 Debug 通道验证，Release 不适用（见 `verification.md` VM-K1-01/02）。
- 待补行为证据：VM-P1-S2、VM-P6-01、VM-D1、VM-D2-01..04，以及 P5 出口插件卡。
- `verification.md` 定义 PASS、NOT_ACCEPTED、BLOCKED 和证据要求；缺少真机日志时，不以 host、静态检查、Debug 构建或 API 成功替代。
- `baseline.md`、`map.md` 为生成材料；`api-route-map.md` 为路由过程核对材料。本台账只引用其结果，不改写其生成口径。

## 6. 更新边界

迁移实现、当前债务和优化顺序先按 `ARCHITECTURE.md` 与 `current-optimization-review.md` 判断，再在本台账记录状态和证据指针。本文件不得新增目标职责、改变 API 契约或以过程材料取代目标架构。

## 7. 写面/读面迁移批次记录（G-item 与 T2c–T2h）

统一证据口径：Host `ctest` 终值 **69/69**（`item_facts_dispatch` 343、`character_facts_plugin` 378、`character_facts_dispatch` 214）、`check_http_routes` 143/143、`check_jni` 156/156、普通 + `-PqolKernelOnly=true` 双构建成功。**真机行为面一律按批次延后，结论 `NOT_ACCEPTED`，Host/静态检查不替代真机证据。**

| 批次 | 提交 | 内容 | 状态/证据 |
|---|---|---|---|
| G-item-1/2 | `9ed7b1ae`、`df7b6858` | `kSetMoney`/`kRejectDice` 隔离到 typed money/dice write port | 完成；Host + 真机证据见 VM-K5-08h 系列 |
| G-item-#10 | `d90918c2`、`d763e1e9` | inventory serializer 与 effective 计数移交 `core.item` owner；台账 #10 销账 | 完成；`item_count_api` 债务行已更新 |
| G-item-state | `3bbb2341` | inventory-state 写路由 4 kind 迁 `core.item` owner | 完成；VM-K5-08h |
| G-item-move | `55a7b294`、`8fc3a3fa` | `kMoveItem` 整路由迁 owner；VM-D2D-01 指向 owner 侧用例 | 完成；VM-D2D-01/02 |
| G-item-add | `e25984a1` | `kAddItem` 隔离到 typed inventory write port | 完成；VM-K5-08h |
| T2c-1/2/4 | `032f1146`、`154c69b6`、`9b1f486e` | `character.state` v1.0 capability；equip 族 4 case 迁 owner；typed character 写原语 20 + owner 分派骨架 | 完成；ADR 0002 |
| ADR 0002 | `3d207498` | owner 自组装 JSON 投影边界决策记录 | 已接受 |
| T2c-3（item） | `a242c256` | `kAcceptDice` owner 收口并删除 legacy item write port；builder item 族清零 | 完成；VM-K5-08h |
| T2c-4a..4e | `5f81c313`、`4310821d`、`88c86f06`、`aa5b9e61`、`d5f11eb4` | character 写 20 kind 族级 golden（无 state 数值、战斗 party、队伍、玩家、skills raw） | 完成；host 冻结批，真机延后 `NOT_ACCEPTED` |
| T2c-4f | `b80cb166` | 删除 legacy character 写 port/builder | 完成；layers 不变 |
| T2d-0..3 | `6cafd0ea`、`136acec6`、`bdf28c5c`、`0632acdc` | character 写 20 JNI wrapper 全切 typed + bridge 信封 helper；删 api/native 20 定义/声明 | 完成；bridge 直调 character 归零 |
| T2e-0..3 | `0e0b3274`、`a09d5393`、`59da79a8`、`56c0b8ee` | item-thin 12 JNI wrapper 切 typed + `item_write_result.h`；删 12 定义/声明/stub（equipment.inc 整删） | 完成；bridge 直调 item-thin 归零 |
| T2f-1..3 | `a0ce157d`、`6bba0409`、`586e6bd1` | B-item-legacy 4：add_money/minus_money/remove_item/move_item 切 typed（普通袋）；`basic.inc`/`use.inc` 整删 | 完成；bridge 直调 item-legacy 归零 |
| T2g-1 | `7ae94c76` | character 保留 2（add_experience/set_skill_usage）切 typed（无 kernel 门） | 完成；T2g-X（EB move 换 capability）并入 X 收尾批裁决 |
| T2h-0 | `fa8b3cd5` | 读面 golden 基线下沉 tests（`frozen_party/player/skills/member_json`）；character 372→378 | 完成；纯 tests，产品零改动 |
| T2h-B0 | `4562f929` | inventory fake 环境 + legacy `frozen_inventory_json` 4 fixture 等值；登记 owner-raw 未暴露、B2 门槛否决 | 完成（门槛判断）；见 §3「B2 门槛未通过」行 |
| T2h-B' | `9cfbe382`、`d5d2f0ba` | game_cache builder 注入化（A1，K1 legacy 不变）；删 3 条 `game_cache→api/native` include；layers 30→27 并棘轮基线 45→31 | 完成；本批真机读面字节延后复测 `NOT_ACCEPTED` |

- 序列后存量事实：bridge 直调 api/native 由 61 收缩至 **14 符号**（清单见 §3「bridge 直调」行）；api/native 剩余读 TU 与 3 kernel stub 见 §3「api/native 残余」行；T2h 的 B1/B2/B3 与批⑥（world/save/ui/quest/dialog 读面 + X 类）未执行。

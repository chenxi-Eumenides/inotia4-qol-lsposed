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
| `extension_bag` | D2a–D2c 已完成主要契约、资源声明、消费者切换和物理迁移。D2d 代码已接入 `contracts/extension_bag.h` v2.0 固定宽度 move capability：`game_system.cpp` 经 registry 取用，能力缺失时回退 `data_op_move_item`；`contracts/extension_bag_port.h` 的 legacy 全局声明（`use_item`/`capability()` 两面，见 §3 豁免行）维持登记；原 api/native 直调消费者已随 T2l 域批迁 game 层 | D2d 代码已收口；legacy 面清退并入阶段 6——阶段 6 静态清零达成后（T2n-4 `fd8cbadc`：layers/offsets/resources 0），该 2 面按 §3 豁免行继续登记，再评估触发不变。完成余下 hook/ptr/UI/生命周期收口、独立 target 与边界检查 | VM-D2D-01/02；VM-K5-08h；受影响 VM-43、VM-P4-07 |
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
| legacy 目录和过渡映射 | `api/native/` 目录已删除（T2l-dir `c4da2a6e`），脚本死映射已清理（T2m `2911afb5`）；余 `core/native`、根目录聚合文件 | 各文件职责吸收到 `game/`、`infra/`、`plugins/` 或 `contracts/`；删除映射；边界负例确实失败；基线收缩 |
| 插件私有越层与单一目标 | 仍有 legacy 私有 include；独立 target 不能覆盖全部宿主私有依赖 | 插件只 include `contracts/` 与自身目录；每个 target 独立编译并通过负例检查 |
| `contracts` 业务入口 | `contracts/autosell.h` 仍有全局业务函数，部分消费者尚未转 capability / owner command | 改为 provider 函数表、Host SDK 或 owner capability 后删除全局入口；host 与架构检查通过 |
| 引擎未闭合项 | 生产 `in_flight_enter/leave` wrapper 已接线：Host SDK 导出、registry 实现及 inventory_hooks/extension_bag 生产调用均已具备。VM-K2-01 仍观察到自动恢复路径的票据终态未追认、`degraded` 未同步清除 | 后续真机回归恢复路径，验证票据终态与 degraded 同步；证据沿用 VM-K2-01 |
| 生命周期票据与状态不同步 | VM-K2-01 实测：消费者 activate 时依赖方尚未 enable 则票据 `failed`，随后 `sync_gates` 重评估自动拉起 enabled，票据不追认；autosell `degraded:true` 残留（error 已 None） | 票据终态追认自动恢复路径；`sync_gates` 恢复时同步清除 `degraded` 残留；以 VM-K2-01 后续回归卡验证 |
| `item_count_api` legacy 访问器（#10） | 已收口（d90918c2）：`contracts::item_count_api()` 旧全局访问器删除；state builder、add-item 编码写入、`inventory_trade` 售价数量、`game/read/item_class.cpp` 编码判定四组消费者改走 typed API 表/owner serializer；`QolItemCountApi` typed 表与 `QolGameHost.item_count` 注入面保留（inventory_hooks/autosell 消费者合法使用） | `build_inventory_json` 已随 T2l-inv 迁 `game/access/inventory_legacy_read.cpp`（cache 槽 6/owner fallback/item_port 共用），api/native 薄转发面已删除（T2l-dir）；扩展袋 >99 真机数量视图卡待插件独立测试批补 |
| bridge 直调 api/native（**已清零**，T2l 序列闭合） | 14 符号（world 写 5/save 写 3/ui 写 5/quest 写 1）经 T2l-w-write/s-write/u/q/d 各域 typed 化；`api/native/` 目录已删除（T2l-dir `c4da2a6e`）；bridge 不再 include/call api/native | 已完成；仅 EB 豁免 2 面（见下行）维持登记，其退役条件不变 |
| EB 全局豁免面（`use_item` / `capability()`） | 仅剩两个 EB 全局/访问器面：① `extension_bag_use_item`（声明 `contracts/extension_bag_port.h:63`、定义 `plugins/extension_bag/extension_bag_port.cpp:232`），消费者 2 处——`bridge/native/gamebridge_domain_operations.cpp:217`（EB use 分支）、`host/item_inventory_state_write_port.cpp:70`；capability 无对应成员（`handle_confirm_use_item` 为原版确认键接管，语义不同）。② `extension_bag_capability()`（声明 `contracts/extension_bag_port.h:69`、定义 `host/extension_bag_port.inc:23`），消费者 game 层 `game/ops/item_inventory_state_write.cpp`（8 处）与 `game/state/game_state.cpp`（2 处）、`game/access/save.cpp`/`ui_state.cpp`（原 api/native 7 处随 T2l 序列迁入）、bridge（5 处）；`plugins/core.item/plugin.cpp:282` 为 TU-local 同名 SDK-require 包装，非本访问器消费者。`extension_bag_move_item`/`extension_bag_put_jewel` 已随 T2j-1/T2j-2 删除 | use 归宿 = 域化（逻辑迁 owner/域）或 v2.3 独立立项（含 string→POD 等值门禁）；accessor 退役 = contracts 注入面或 D2d 域化。再评估触发任一达成即重评：① use/accessor 出现新消费者；② 整目录批要求 bridge 零 api/插件字符串面；③ EB 域化批立项。阶段 6 静态清零不含该 2 过渡访问器（T2n-4 `fd8cbadc` 基线 0 后仍在登记中），再评估触发不变 |
| B2 门槛（已闭环，T2h-B2 `d091afa3`） | `core.item` 新增 `item.state` capability（raw inventory 投影，复用 `serialize_inventory_json`），host `frame_cache` inventory(6) 槽切 owner 适配器；W7 在 B0 fake 环境以 4 fixture 断言 `owner raw == frozen == legacy` 逐字节，`frozen_inventory_json` 保留为独立 golden | 清除条件已满足：owner-raw golden 建立且 inventory 槽切换完成；未改 JNI/HTTP 字节。后续仅余真机字节复测（随读面批次） |
| api/native 面（**终态：目录已删除**） | `api/native/` 目录与全部 TU/inc 已随 T2i-2/T2j/T2l 序列删除（`c4da2a6e`）；原面职责落点：读 builder → `host/legacy_json_builders.h`/`game/access/*`（`world_read`/`inventory_legacy_read`/`save`/`shop`/`ui_state`/`quest`/`save_preflight`），写原语 → `game/access|ops/*`；kernel 占位分支迁入 `game/access/inventory_legacy_read.cpp`（`{"bags":[]}` 逐字） | 已完成；cache 机制 4 边与非序列 13 条存量边已随 T2n-1（`88f3e96a`）/T2n-2（`0dc3eee5`）/T2n-3（`118d1044`）全部消除，静态基线经 T2n-4（`fd8cbadc`）终局收缩至 0；剩余未决 = world/nav/tiles 等读面与 save/日志/cache 槽真机回归（行为面另批）；机制批证据见 VM-T2N-01 |

## 4. 临时豁免与保留项

### 4.1 资源豁免

`plugins/core.ui/panels/settings.cpp` 的主菜单「更多游戏」运行时控件实例槽属于 `resource_unknown_symbol`。当前由主线程帧任务管理，不能按稳定 GOT 槽静态归属。

清除条件：P5/U11 动态 UI 绑定管理器建立后，登记 `own:ui.main_menu.more_games_entry` 与运行期 lease，并通过资源检查。

### 4.2 legacy 实现保留

- `core/native/game_cache.{h,cpp}`：保留在 `core/native`，目标映射为 `infra`；当前 `game_cache.cpp` 仅依赖 `infra/runtime.h`（原 4 条 infra→game 边与 `game_system.h`/`game_access.h`/`game_state.h`/`game_symbols.h` 死 include 已清零，T2n-1 `88f3e96a`），帧号取 `infra::frame_count()`、world 判定经注入缝 `frame_cache_set_world_gate`。player/party/inventory/skills/mercenaries 五槽 legacy builder 经 host 装配期注入（`frame_cache_set_json_builder`，A1 host↔infra 内部面）；T2i-2 删除 `api/native/game_character.*`/`game_party.*` 后，player/party/skills/mercenaries 四槽与其 owner fallback 由 `host/legacy_json_builders.h` 承担，inventory(6) 槽现指 `game/access/inventory_legacy_read.cpp`（T2l-inv）；world 槽 2/3/9/10/11 亦由该头 host 注入（T2l-w-read/w-read2）；system 槽先随 T2n-1 收口、后经 T2n-4 基线终局归零。
- `core/native/module_save.cpp` 与 `module_save_port.h`：实现仍依赖旧游戏访问面；稳定声明暂留 `contracts`，实现映射为 legacy/save。
- `core/native/inventory_trade.{h,cpp}`：仍依赖游戏访问面与状态面，映射为 legacy/inventory；T2n-3（`118d1044`）删除死符号 include（复核零符号使用、无裸地址）。
- `game_system.{h,cpp}`：直接迁入目标层会引入 legacy 依赖，须先拆出低层原语与上层聚合。（原同列的 `game_dialog*` 4 文件已随 T2l-d `1976ba4b` 删除。）

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

统一证据口径：Host `ctest` 终值 **72/72**（`item_facts_dispatch` 363、`item_facts_plugin` 34、`character_facts_plugin` 413、`character_facts_dispatch` 214、`world_legacy_readers` 62）、`check_http_routes` 143/143、`check_jni` 156/156、普通 + `-PqolKernelOnly=true` 双构建成功 + kernel `.so` 抽查。**真机行为面一律按批次延后，结论 `NOT_ACCEPTED`，Host/静态检查不替代真机证据。**

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
| T2h-B0 | `4562f929` | inventory fake 环境 + legacy `frozen_inventory_json` 4 fixture 等值；建立 owner-raw 门槛 golden | 完成；门槛已由 `d091afa3` 闭环（见 §3「B2 门槛」行） |
| T2h-B' | `9cfbe382`、`d5d2f0ba` | game_cache builder 注入化（A1，K1 legacy 不变）；删 3 条 `game_cache→api/native` include；layers 30→27 并棘轮基线 45→31 | 完成；本批真机读面字节延后复测 `NOT_ACCEPTED` |
| T2h-B1 v3 | `d07e2732` | player/party/skills 三槽改注入 owner 适配器（`character.state` 现取不缓存；插件停用逐字 fallback legacy） | 完成；layers 27 持平、文件 545→546；`character_facts_plugin` 388；真机读面字节延后 `NOT_ACCEPTED` |
| T2h-B2 | `d091afa3` | 新增 `contracts/item_state.h` capability；`core.item` 暴露 raw inventory 投影（复用 `serialize_inventory_json`）；inventory(6) 槽切 owner；W7 4-fixture `owner raw == frozen` 逐字节 | 完成；`item_facts_dispatch` 355、`item_facts_plugin` 34；contracts +1（文件 546→547）；真机延后 `NOT_ACCEPTED` |
| T2h-B3 v4 | `8f94b6c4` | `game_system` 4 处即时 state 改读 `data_player_json()`（写路径 `op_ok()` 已同步力刷 player 槽 → 写后即时同源）；`data_debug_item_raw_json` 整函数迁 `game/read/item_class.cpp` + bridge kernel 门 | 完成；`item_facts_dispatch` 355→363；layers 27 持平、文件 547；真机写响应/调试端点字节延后 `NOT_ACCEPTED` |
| W-e 续（api-route-map） | `c831f773` | `docs/refactor/api-route-map.md` 漂移销账：33 写路由行改指 owner 结果处理器 + `game/ops/*` typed 原语 + `contracts/game_ports/*` typed port + `bridge/native` helper；4 处 owner 名 `system_facts`→`core.system`；实测校正 10 处邻近引用（`world_write_port` 5 + `game_system` 读面 5）仍有效保留 | 完成；`check_http_routes` 143/143 |
| T2i-1 | `b9ccfa82` | 新增 `host/legacy_json_builders.h`（`legacy_member_json` + 4 个 builder 逐字内联）；assembly 槽 0/1/7/8 与 3 处 fallback 改源；零删除 | 完成；`character_facts_plugin` 413；layers 27 持平、文件 547→548；真机读面字节延后 `NOT_ACCEPTED` |
| T2i-2 | `af6e9abe` | 删除 `api/native/game_character.{cpp,h}`、`game_party.{cpp,h}` 四文件 + 主/测试 CMake 源清单、bridge/game_system include 清理；grep 词界归零 | 完成；**layers 27→23（-4）**、文件 548→544，`legacy/character` 与 `legacy/party` 层组消失；`character_facts_plugin` 413；真机延后 `NOT_ACCEPTED` |
| 棘轮收缩 | `f73d4ef9` | 基线 31→27，逐边 = T2i-2 TU 物理删除证据；`gen_baseline_doc.py` 同步 | 完成；`check_arch` 违规 23/基线 27、offsets 4 |
| T2j-1 | `624c142a` | bridge EB move 分支改 capability v2 `move_item`（同 impl 逐字）；删全局 `extension_bag_move_item` 声明+定义 | 完成；grep 词界 0、kernel `.so` 0、undefined 表 0；真机 EB 字节延后 `NOT_ACCEPTED` |
| T2j-2 | `b543c2d2` | 直删 0 消费者 `extension_bag_put_jewel`（声明+定义）；capability `put_jewel`/`put_jewel_native` 保留 | 完成；`.so`/undefined 0、保留面在位 |
| T2j-3 | `025542c2` | EB 全局豁免登记（`use_item` / `capability()` 边界、再评估触发）写入 ledger §3 | 完成；文档面（W20/W21） |
| T2l-w-read | `a257ad71` | world 读 map/units/enemies/interactives/drops host 内联 + cache world 5 槽切注入；`game_cache.cpp→game_world.h` include 删除 | 完成；layers 23→22（-1）、文件 544（tests 不计入 layer 图）；cache world 5 槽改 host 注入 |
| T2l-rh | `4c1d9100` | 新增 `contracts/runtime_hooks.h` POD 注入表 + `host/frame_source.cpp` 定义（零消费者零行为） | 完成；`runtime_hooks_contract` 10/0；kernel 同供给 |
| T2l-w-write | `fac32102` | world 写 5（teleport/×2/move/walk/walk_stop/interact）typed 迁 `game/access/world.cpp` 经表；`qol_log_seam.h`（motion 7 调用点宏重定义）；删两 inc | 完成；28 条字面量逐字、双构建；layers 22 持平、文件 543→542 |
| T2l-q | `e7e7d1d7` | `quest_quit` typed 迁 `game/access/quest.cpp` + 3 读 host/game 内联；删 `api/native/game_quest.*` | 完成；refresh 1/0 保持；`item_facts_dispatch` 363 |
| T2l-s-write | `44416ca7` | save 写 3 + `_direct` 迁 typed；preflight 迁 `game/access/save_preflight.{h,cpp}`（host 源行删除）；删 `api/native/game_save.*` | 完成；`host_tests` 2480/0；layers 持平 |
| T2l-u | `41526b9f` | ui 写 3 + 读 4 + X5（popup/top_panel）迁 game（`ui_state.cpp`）；删 `api/native/game_ui.*` 5 文件；`game_state.cpp→game_ui.h` include 删 | 完成；layers 22→**19（-3）**、文件 542→539 |
| T2l-d | `1976ba4b` | dialog 写 2+内部分发 + 读 2（story 原语迁 game）；删 `game_dialog.*` 4 文件；`data_op_save` 薄转发收口 | 完成；layers 19→**17（-2）**、文件 539→535 |
| T2l-integ | `9a173be9` | 棘轮收缩 27→21（T2l 序列真实降边）；`gen_baseline_doc.py` 同步 | 完成；check_arch 17/基线 21 |
| T2l-s-read | `5cc07691` | save 槽读（current/slots）迁 `game/access/save`；删 `api/native/game_save.*` 残件 | 完成；0 边、文件 533 |
| T2l-sho | `8047873c` | shop 读迁 `game/access/shop.{h,cpp}`；删 `api/native/game_shop.*` | 完成；0 边、`legacy/shop` 层组消失 |
| T2l-inv | `c6140166` | inventory legacy 读迁 `game/access/inventory_legacy_read.{h,cpp}`（kernel 分支逐字 stub）；删 `api/native/game_inventory.*` | 完成；B0 4-fixture frozen 363/363；0 边 |
| T2l-w-del | （停线，零提交） | world TU 整删前置核验不通过（nav/tiles/world_port/append_position 活消费者） | 停线登记；方案 A（T2l-w-read2）获准 |
| T2l-w-read2 | `7a8f9a7e` | world 活读面迁 `game/access/world_read.cpp` 单一实现；host 薄转发 + `append_position` 内联；删 `api/native/game_world.*` 5 文件 | 完成；frozen 62/0、双构建；`legacy/world` 层组消失、文件 528 |
| T2l-dir | `c4da2a6e` | 删主/tests CMake `api/native` include 路径（1+4）与空目录 `api/`；`baseline.md` 重生成 | 完成；**`api/native` 目录删除**；ctest 72/72 |
| T2m | `2911afb5` | 维护脚本死映射清理：删 `arch_common.py` 的 `_LEGACY_API_BY_STEM` + `api/native/` 分类分支、`gen_baseline_doc.py` 映射 `_bullet_map`；`baseline.md` 重生成 | 完成；清理前后 `check_arch`/`check_jni`/`routes` 输出**逐字节一致**；脚本目录 `api/native` 0 命中；`gen_baseline_doc --check` 一致 |
| T2n-1 | `88f3e96a` | cache 机制 4 边：`game_cache.cpp` 删 `game_system.h`/`game_access.h`/`game_state.h`/`game_symbols.h` 死 include；`data_frame_count`×5 改 `infra::frame_count()`；`game_in_world`×4 改注入缝 `frame_cache_set_world_gate`（host 装配期注入）；`build_gamestate_json`/`build_snapshot_json` 改本地前向声明 | 完成；**layers 17→13（-4）、offsets 4→3**、文件 528；`game_cache.cpp` 仅依赖 `infra/runtime.h`；真机 cache 槽字节延后 `NOT_ACCEPTED` |
| T2n-2 | `0dc3eee5` | 非序列 6 边：`game_access` 日志 provider/`qol_log_init` 搬 `nativeInit`（W41 时序保持）；tiles/variant 改 `qol_log_seam`（domain 序数对齐，W42）；`game_state` 帧任务改 `runtime_hooks` 表；`frame_task.cpp` 改 `infra::frame_count()` | 完成；**layers 13→7（-6）**、offsets 3、文件 528；ctest 72/72（`qol_log_seam` 19/0）；真机日志域字节延后 `NOT_ACCEPTED` |
| T2n-3 | `118d1044` | D 案：`save_enter/exit` 物理迁 `patch/`（`infra/save` 目录清空）；新增 game 原语 `SaveSiteRef{vma, call_off}` 收口符号常量；安装点恢复 `g_base + fn_resolve("F_*_VMA", …) + call_off` 归属（W46）；`inventory_trade` 死符号 include 删除；`game_variant` 注释顺正 | 完成；**layers 7→0、offsets 3→0、resources 6 unknown→0**；W43/44/46/47/48；计划外修正 2 处（`tests/test_infra_data.cpp` include 同步、V2-2.1 代码片 `+call_off` 等价结构以保行为逐字）；真机 save 进出/派发延后 `NOT_ACCEPTED` |
| T2n-4 | `fd8cbadc` | 棘轮终局收缩：violations 快照 21→0（layers/offsets/resources 分区冻结）；`gen_baseline_doc.py` 重生成 `baseline.md` | 完成；**四项校验 0/0/0（layers/offsets/resources）+ JNI 156/156**、文件 528、routes 143/143、ctest 72/72；**阶段 6 静态清零达成** |

- 序列后存量事实：**bridge 直调 api/native 已清零**，`api/native/` 目录已删除（T2l-dir `c4da2a6e`），脚本死映射已清理（T2m `2911afb5`，`scripts/` 目录零 `api/native` 命中）；仅剩 T2j 豁免 2 面（`use_item` / `capability()`，见 §3）。机制批 cache 4 边与非序列 13 条经 T2n-1/2/3 全部消除，T2n-4（`fd8cbadc`）棘轮终局 **21→0**：文件 **528**、layers **0/基线 0**、offsets **0**、resources **0**、ctest **72/72**（`world_legacy_readers` 62/0）、routes 143/143、JNI 156/156（双构建 + kernel `.so` 抽查通过）；T2h/T2i/T2j/T2l/T2m/T2n 全序列闭合；**api/native 拆除主线（T2b→T2n）静态基线清零**，剩余未决 = world/nav/tiles 等读面与 save/日志/cache 槽真机回归（行为面另批，见 §3）。

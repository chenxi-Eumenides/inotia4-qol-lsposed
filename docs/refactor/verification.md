# 验证矩阵与验收卡

> 定位：定义「什么算通过」与「证据放哪里」。架构规则见根目录 `ARCHITECTURE.md`；执行状态见 `ledger.md`。

## 1. 交付定义

| 结论 | 条件 |
|---|---|
| **PASS** | 声明的验证项全部执行通过，且证据可复现（命令 + 输出/日志位置） |
| **PASS（host-only）** | 变更未触及行为面（纯逻辑/纯工具/骨架），host 测试通过即可，须在卡中标注范围 |
| **NOT_ACCEPTED** | 触及行为面但缺少真机证据；或以静态审查、Debug 构建、API 成功替代真机回归 |
| **BLOCKED** | 设备/环境不可用导致无法取证；必须写明待补项与复现命令 |

证据定义：**证据 = 命令 + 该命令的实际输出**；短输出内联在卡片内，长输出给出项目内可复现位置
（如 `docs/history/` 或 `archive/`）。仅有过程摘要、无命令或无输出者**不算证据**。

证据位置约定：真机证据写入 `.tmp/<task>/` 后迁 `docs/history/` 或 `archive/`；脚本证据以命令与输出为准（可复跑）。

## 2. 测试分层

| 层 | 手段 | 覆盖 |
|---|---|---|
| 纯逻辑 | host 单测（`cpp/tests`，CMake + ctest） | rules、编码、几何、协议解析、快照/队列/事件/能力/状态 |
| 装配与失败路径 | host + fake 后端 | 安装/停用、资源冲突、部分失败与回滚、重复 enable/disable、缺依赖降级、契约版本不匹配 |
| 并发与切换 | host + fake | 配置热切换、迁移中并发切换、在途回调期 deactivate、帧预算超时 |
| 真机 | 每插件一张验收卡 | ABI、时序、交互、变体差异、崩溃兜底 |

## 3. 验收卡模板（插件级）

```
卡号：VM-<域>-<序号>        阶段：P2/P3/P4
覆盖插件/资源：<plugin id> / <resource id>
前置：变体=<原版|大修|monster>；开关=<key=值>；存档槽=<slot>
步骤：1) … 2) … 3) …
观测：屏幕状态 / API 响应字段 / 日志锚（domain + 关键字）
失败判据：安装失败未上报 / 状态与开关不符 / 崩溃 / 静默无效
证据：<命令与输出文件路径>
结论：PASS / NOT_ACCEPTED / BLOCKED + 原因
```

## 4. 已建立的验收卡

| 卡号 | 对象 | 命令（可复跑） | 实际输出（内联） |
|---|---|---|---|
| VM-P0-01 | 校验脚本族 + 基线棘轮 | `uv run python scripts/maintenance/check_arch.py` | `全部通过`（基线随迁移收缩：125→124→121）；`--freeze` 仅按规则前缀收缩，扩容需 `--force-freeze` |
| VM-P1-01 | `infra` 骨架（快照/命令队列/事件/能力/状态） | `cd module/app/src/main/cpp/tests && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure` | `100% tests passed, 0 tests failed out of 20`（P1 时点；当前 30/30）。范围：host-only，未接线运行时 |
| VM-P1-02 | 最小诊断接口 `GET /api/system/features` | `adb -s 192.168.3.54:5555 install -r output/inotia4_qol_lsposed_debug_2609170036_a8711e058093.apk && adb -s 192.168.3.54:5555 shell am force-stop com.com2us.inotia4.normal.freefull.google.global.android.common && adb -s 192.168.3.54:5555 shell monkey -p com.com2us.inotia4.normal.freefull.google.global.android.common -c android.intent.category.LAUNCHER 1 && curl -s http://192.168.3.54:8088/api/system/features` | `{"ok":true,"features":[{"id":"core","version":"0.1.0","state":"enabled","enabled":true,"error":null,"resource_count":0}]}`（`enter_slot` 后复测一致） |
| VM-P1-03 | `/api/health` 加法扩展 `ready` + `variant` | `curl -s http://192.168.3.54:8088/api/health` | `{"ok":true,"ready":true,"variant":{"series":"monster","version":"v27","known":true,"capabilities":3}}`；修复前日志 `game_variant: ... md5=cc517897dcaae8021e35a2426284c224 known=0` |
| VM-P2-01 | P2 纵向样板 `simple_mode`（入口 hook ×2 + 配置 + 状态 + 命令） | `curl -s http://192.168.3.54:8088/api/system/features`；`curl -s -X POST http://192.168.3.54:8088/api/config/set -H 'Content-Type: application/json' -d '{"simpleModeEnabled":true}'`；`curl -s -X POST http://192.168.3.54:8088/api/op/movement/teleport -H 'Content-Type: application/json' -d '{"map_id":34,"x":2,"y":20}'`；`curl -s http://192.168.3.54:8088/api/world/map/enemies` | 启动 `installed_disabled` + 两资源 `disabled`；开 → `enabled` + `live`（日志 `simple_mode enabled=1`）；关 → 回 `installed_disabled`（`enabled=0`）；**半血 A/B**：门禁开 `hp=135`，关门禁后刷图 `hp=270`（恰 2×）。伤害倍率子项：**非本卡证据**（历史归档，本轮未复测） |
| VM-P3-01 | P3 生命周期：`deactivate` 物理归还 / `activate` 重挂载 + 资源状态端点 | `curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"simple_mode","action":"deactivate"}'`；`curl -s http://192.168.3.54:8088/api/system/resources`；`curl -s http://192.168.3.54:8088/api/world/map/enemies` | deactivate `ticket=3` → `inactive` + 两资源 `free`；刷图 **`hp=270`**；activate `ticket=4` → `enabled` + `live`；刷图 **`hp=135`**；`GET /api/system/resources` 全程一致；无 FATAL。**U6 闭合：同址二次挂载安全** |
| VM-P3-02 | 资源事务故障注入与并发（host-only） | `cd module/app/src/main/cpp/tests && ctest --test-dir build --output-on-failure` | `100% tests passed, 0 tests failed out of 25`；`hooks_transaction: pass=277`、`plugin_lifecycle: pass=115`。覆盖：逆序回滚、`tombstone`+隔离、`resumable=false`、在途等待与帧预算超时、隔离拒绝、同插件单事务、`failed` 重试、非法迁移拒绝、`on_resource_ready` 失败全量回滚 |
| VM-P3-03 | P3.5：10 态状态 API、`degraded` 正交字段、tickets 端点 | `curl -s http://192.168.3.54:8088/api/system/features`；`curl -s -X POST http://192.168.3.54:8088/api/config/set -H 'Content-Type: application/json' -d '{"opEnabled":true}'`；`curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"simple_mode","action":"deactivate"}'`；`curl -s http://192.168.3.54:8088/api/system/tickets/2` | `features`：10 态名 + `degraded:false`；`tickets/2`：`{"ok":true,"ticket":2,"state":"pending"}` → 3 秒后 `{"ok":true,"ticket":2,"state":"done"}`，`inactive` + 两资源 `free`；`tickets/0`：`{"ok":false,"error":"invalid ticket"}`；`tickets/3` 在协议页帧冻结期停留 `pending`（帧驱动合法等待），关协议页帧号 `119→150` 后 `done`；无 FATAL |
| VM-P4-01 | P4-A1：`patch/` 机制 + `push_all` 插件化（call patch ×2 + 配置 + 状态） | `curl -s -X POST http://192.168.3.54:8088/api/config/set -H 'Content-Type: application/json' -d '{"pushAllEnabled":true}'`；`curl -s -X POST http://192.168.3.54:8088/api/world/movement/move_to -H 'Content-Type: application/json' -d '{"x":19,"y":8}'`；`adb -s 192.168.3.54:5555 shell "grep -i 'party filter' /sdcard/Android/data/com.com2us.inotia4.normal.freefull.google.global.android.common/files/inotia4-export.log"` | gate 关：`installed_disabled` + 两资源 `free`（call patch 无空转，§4.2 规则 6）；开：`enabled` + 两条 `live`（fail-closed 原字校验通过 → 两处物理写入成功）；走向 NPC 阻挡物后日志 `2026-09-17 15:50:46.668 f=776 I platform feature.cpp:34 push_all party filter active`（BL 重定向确实路由到模块）；关：回 `free`（物理还原）；无 FATAL。**用户真机确认（2026-09-17）：开启开关后实际推怪功能完好、已生效**——补齐「游戏内行为无真机证据」的长期缺口 |
| VM-P4-02 | P4-A2a：`infra/scheduler` + 帧源声明式 core 资源 | `curl -s http://192.168.3.54:8088/api/system/resources`；`curl -s -X POST http://192.168.3.54:8088/api/config/set -H 'Content-Type: application/json' -d '{"pushAllEnabled":true}'`；`curl -s http://192.168.3.54:8088/api/system/game_frame`（间隔采样） | core 两条 `patch:GAMESTATE_DrawPlay+0x20` / `patch:MainProcess+0x40` = `live`（启动日志 `16:23:30.114 f=0 I core frame_source.cpp:153 frame_source installed (render + logic anchors)`）；进世界后帧号 `262→306`（快帧）；命令排空经新 scheduler 两态：开 → `enabled` + 两资源 `live`，关 → `installed_disabled` + `free`；无 FATAL。**执行中发现并修复缺陷**：`frame_source_install` 成功路径漏写登记表状态（resources 误报 `free`）→ 补 `set_state(kLive)` 后复验为 `live` |
| VM-P4-03 | P4-A2b：`quest` 迁移为插件（零资源、常驻、帧节拍走 scheduler） | `curl -s http://192.168.3.54:8088/api/system/features`；`curl -s -X POST http://192.168.3.54:8088/api/system/enter_slot -H 'Content-Type: application/json' -d '{"slot":0}'`；`adb -s 192.168.3.54:5555 shell "grep -i 'quest resync' /sdcard/Android/data/com.com2us.inotia4.normal.freefull.google.global.android.common/files/inotia4-export.log"` | 安装重启后 `quest: enabled`（零资源 + 常驻）与 `core enabled [live,live]`；进档后日志 `2026-09-17 16:36:55.185 f=141 I platform feature.cpp:47 quest resync: fixed=0`（save-enter → 一次性逻辑帧 tick 经新 scheduler 执行）；无 FATAL |
| VM-P4-04 | P4-A3：首个能力契约 `attr_range` + attribute_range 迁移（3 entry hook、提供/撤销/恢复） | `curl -s http://192.168.3.54:8088/api/system/features`；`curl -s -X POST http://192.168.3.54:8088/api/config/set -H 'Content-Type: application/json' -d '{"opEnabled":true}'`；`curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"attribute_range","action":"deactivate"}'`；`curl -s http://192.168.3.54:8088/api/system/tickets/3` | 常驻：`attribute_range enabled` + 3 条 hook `live`（`MATH_GetRandom`/`UIDesc_AddOption`/`UIDesc_MakeItem`，能力已注册）；deactivate `ticket=3` → `deactivating`（前两条已归还、第三条在途）→ `inactive` + 3 条 `free`，ticket `done`；activate `ticket=4` → `enabled` + 3 条 `live`（能力重注册）；无 FATAL。消费者已改按能力查找（撤销期走降级路径，不缓存指针） |
| VM-P4-05 | P4-A4：`hooks/dispatch` 三段式 + 文本层共享资源（`MEMORYTEXT_GetText`） | `curl -s http://192.168.3.54:8088/api/system/features`；`curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"text_layer","action":"deactivate"}'`；`curl -s http://192.168.3.54:8088/api/world/map/id`；`curl -s http://192.168.3.54:8088/api/system/tickets/3` | 启动 `text_layer enabled` + `hook:MEMORYTEXT_GetText` = `live`；deactivate `ticket=3` → `done` → `inactive` + `free`（共享资源物理归还），撤销期文本热路径正常（`{"map_id":20,"id_name":"黑暗教团礼堂"}`）；重复 deactivate `ticket=5` = `failed`（非法迁移被前置校验拒绝）；activate `ticket=6` = `done` → `enabled` + `live`；无 FATAL。附注：ticket=3 在帧冻结窗口排队、进世界后落地（帧驱动语义与 VM-P3-03 一致） |
| VM-P4-06 | P4-B1：`special_equip` 迁移为插件（2 entry hook、常开）+ 核心件 `game/access/item_class` | `curl -s http://192.168.3.54:8088/api/system/features` | `special_equip enabled` + `hook:CHAR_CanEquipItem` / `hook:CHAR_CanUnequipItem` = `live`（声明式 fail-closed 安装）；无 FATAL。**用户真机确认（2026-09-17）：功能正常**。行为语义（用户校正后）：① **该功能逻辑**——普通队友、主角可穿脱 26 条「NPC 专属位」装备（cat 485-506 / 785-787 / 948），且其身上的专属位装备**可出售**；② **原版逻辑不变**——特殊 NPC 队友（任务入队）原生装备无法脱下、不存在穿上；其身上的专属位装备**不可出售**（出售门禁拦截）；普通装备穿脱售正常、不可穿到特殊 NPC 队友身上 |
| VM-P4-07 | P4-B2a-1：inventory hook 引擎声明式替换（24 条 `kEntryHook`、`gate=nullptr`） | `curl -s http://192.168.3.54:8088/api/system/features` | `inventory_hooks enabled` + **24/24 资源 `live`**（声明式 fail-closed 安装；自研引擎与 `g_install_blocked` 已删除）；其余插件不受符号表扩充与 `'x'` 校验前移影响（attribute_range 3/3、text_layer 1/1、special_equip 2/2、core 2/2、simple_mode/push_all 维持门禁关态）；无 FATAL。**用户真机冒烟确认（2026-09-17）：扩展背包移动/合并/装备/镶嵌、S2 堆叠、R-55 出售、R-63 徽章、入库收编均正常** |
| VM-P4-08 | P4-B2a-2：`game_patch` 机制收敛（`core_patches` 13 条 + `stack_limit` 6 条声明式；S2 符号型 BL 重定向入表） | `curl -s http://192.168.3.54:8088/api/system/features`；`curl -s -X POST http://192.168.3.54:8088/api/config/set -H 'Content-Type: application/json' -d '{"stackLimitIncrease":true\|false}'`；`adb -s 192.168.3.54:5555 shell "grep -i core_patches /sdcard/Android/data/com.com2us.inotia4.normal.freefull.google.global.android.common/files/inotia4-export.log"` | 启动 `core enabled` + **15/15 `live`**（13 core_patches + 2 帧源）；日志 `core_patches installed 12 + hub`、`monster compat: 0x7279574818 cmp w8,#0x62 -> #0x7F`；进世界（帧 135→173）后 `stack_limit` 两态随配置：`true` → `enabled` + 6/6 `live`，`false` → `installed_disabled` + 6/6 `free`（物理还原）；无 FATAL。**执行中发现并修复启动缺陷**：`core_patches_install` 误用 `bridge_ready()` 守卫（本入口在 `bridge_init()` 内部执行、该标志结尾才置位 → 恒未就绪、静默 fail-closed、API 不就绪、每秒重试）；改为仅判 `g_base` 后 bridge `ready:true`。行为面（S2 大堆叠出售/拆堆、monster 读档、主菜单 hub 跳过、999 clamp 两态）待用户冒烟确认 |
| VM-43 | W1：扩展背包·商店出售支持数量选择（部分出售） | 商店页对扩展袋物品：200 可堆叠 → 选 150 卖 → 余 50、金额 = 150×单价；选上限（全部）→ 槽消失；选 1 → 余 199；取消面板/二次确认 → 零变化；关闭堆叠上限后面板上限随 b 段；原版物品流程与金额不变；装备页整堆不变 | **用户真机确认（2026-09-17）：功能测试完成、没问题**。实现：新增 `UIStore_SellItem` 入口 hook（结算点接管，原版面板/确认/取消保留）；删除按钮 execute 替换 + 全局 popup 劫持（R-13 商店/装备 popup 状态互斥风险消除）；部分出售 canonical 递减写回 + 仅刷新商店投影（R-12）+ 失败退款；host `store_sell_rules` 22 断言 |
| VM-44 | P4-B3：`autosell` 迁移为零资源插件（命令端口门禁 + `infra::scheduler` 60 帧节拍 + 进/退档回调） | 设置页全局开关两态；进档后按规则自动出售（60 帧节流）；重进档规则保留（sidecar）；关开关停售；商店/装备页出售回归 | **用户真机确认（2026-09-17）：自动出售实测正确**。host `autosell_rules_tests` 130 + `autosell_store_tests` 102 断言；基线纯收缩 90→85（UI→autosell 的 feature→feature ×3、符号头 ×2 消除）；过渡依赖 save_enter/exit、sidecar 桥 → B4 |
| VM-45 | P4-C1：`world_teleport` 迁移为零资源插件 + `game/access/world` 收口 | 踩地图边界传送点 → 原生 UICHOICE 四项（±1/±10）；确认框价格与扣费；越界回绕；余额不足横幅；普通商店/NPC choice 行为不变；`GET /api/system/features` 出现 `world_teleport enabled` | **用户真机确认（2026-09-17）：功能正常**。实现：规则逐字搬迁；入口 patch 暂留插件内（双版本指纹 + 运行时 near-trampoline，现有声明式 patch 表达不了，待 `patch/` 支持运行时目标后转声明式）；基线纯收缩 83→81（含 1 条路径改名） |
| VM-46 | P4-C2：`craft_ui` 12 条声明式资源 + `custom_recipe` 插件（provides `craft_result` + `own:RECIPEBASE_MIXTUREBASE` kOwnership） | 开配置：宝石强化页模块两条（合成/宝石升阶）、3 格放料→合成→扣料/产物、未命中 94、金档 105/非宝石 97、描述文案、填入格不显示堆叠；**关配置：宝石强化页为原版配方且不闪退**；反复 toggle 无重复条目；`deactivate→activate` 资源归还与能力撤销 | **用户真机确认（2026-09-18）**。过程中修复三处：① 停用 fail-closed（残留面板可合成）② 门禁路径 fail-soft（ownership 启动失败被 tombstone/failed → 合成器入口消失）③ **停用还原原版组位**（关配置点宝石强化 SIGSEGV——反汇编证实 `UIMix_ButtonMenuListExe+296` 取空列表 `list[0]`）。基线 81→72；host 33/33（custom_recipe 2568、plugin_registry 185 断言） |

> VM-P1-03 执行时发现并修复线上缺陷：变体表停在 monster v26，设备 v27 查表失败
> （`known=0`、能力位 0，靠 `WH4JRN01` 回退分类）；重新生成后补入 v27/v29，设备命中
> `known=1, version=v27, caps=0x3`。该修复需同步到主线（见 `ledger.md` §5）。

## 5. 待建立

| 阶段 | 卡片 | 说明 |
|---|---|---|
| P4 | 每插件一张迁移卡 | 行为等价回归 + 开关两态 + 变体矩阵 |
| P6 | 外部插件轨卡 | 数据 schema 拒绝；未签名 native 显式启用/加载/禁用；托管资源回收；启动故障恢复；更新/回滚 |

## 6. 失败判据（任一命中即 NOT_ACCEPTED）

1. 安装/停用结果与状态上报不一致（静默失败）；
2. 开关关闭后仍有业务副作用（非空转透传）；
3. 同一地址出现第二个安装者或二次挂载 rc=-1；
4. 非主线程调用写副作用游戏函数；
5. 崩溃 / `UnsatisfiedLinkError` / 未捕获异常冒泡到游戏进程；
6. 证据只有静态审查、构建成功或 API 200，缺少可复现的行为观测。

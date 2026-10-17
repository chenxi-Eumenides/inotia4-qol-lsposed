# `inventory_hooks` 插件审阅登记（2026-10-18）

- 审阅对象：功能插件 `inventory_hooks`（`module/app/src/main/cpp/plugins/inventory_hooks/`，12 文件 2761 行）。
- 基线参考说明（阶段 0.75）：`docs/reference/plugins/inventory_hooks.md`（本批新建）。
- 静态审计：阶段 1 @oracle 只读，A–G 七维，findings IH-01…IH-12 + 基础设施观察 G-2。
- 现场基线（阶段 0）：设备 `192.168.3.56:5555`，`/api/health` = ready、variant `monster v29`、`/api/ui/screen` = `world`；运行模块 sha256 `a044632a…` = `output/Inotia4_v1.3.2_monster_v29_lspatched.apk` 内嵌模块；`inventory_hooks` enabled、`degraded=false`、24 资源 `live`。
- 基线证据：`.tmp/inventory-hooks-audit/baseline.md`。

## §1 本批登记问题

| 编号 | 严重度 | 文件:行 | 问题 | 处置 |
|---|---|---|---|---|
| IH-01 | P1 | `plugins/inventory_hooks/native_inventory_hook.cpp:1230-1232`、`inventory_hook_stage4.cpp:64-69` | UI 出售只按堆叠模式开关分流，不按物品实际编码；`canonical=200 (b=72)` 会被误判可走原生并少算金额 | 本批修复：`canonical==b && 1<=b<=127` 才走原生，实际使用 S2 编码时接管 |
| IH-02 | P1 | `contracts/sell_price.h:11-22`、`plugin.cpp:241-243`、`native_inventory_hook.cpp:1303-1308` | 折扣参数为 bool，只能表达 70/100%，无法表达 original 0%、c18 85% | 本批修复：倍率 `num/den` + `game/variants` 表 |
| IH-03 | P1 | `item_port.cpp:110-144`、`plugin.cpp:222-254`、`inventory_trade.cpp:130-148` | 三条出售路径数量口径不一致（view ≤999 / effective / 原始 count） | 本批修复：统一 `item.facts.quote` |
| IH-04 | P2 | `inventory_trade.cpp:134`、`verification-matrix.md:345-349`、`api-reference.md:1063-1069` | VM-30 写 API 100%，代码 70%，文档未写倍率 | 本批按用户裁决统一：API/autosell 跟随详情出售倍率（0/70/85%），商店 100%；同步文档 |
| IH-05 | P1 | `native_inventory_hook.cpp:1335-1348`、`inventory_trade.cpp:178-200`、`sell_transaction.h:21-27` | 退款失败被忽略，UI 无条件记 `refunded=1` | 本批修复：透传回滚失败状态并如实记录 |
| IH-06 | P2 | `native_inventory_hook.cpp:795-823,950-967` | `MoveItem`/`RemoveItemData`/`Divide` 原版失败或半写后仍可能回写 | 下批：先冻结原版返回值语义，再加失败注入 |
| IH-07 | P1 | `plugin.cpp:24-29,336-343`、`extension_bag/plugin.cpp:197-200` | 「禁止 deactivate」仅在注释；停用后 EB refresh 静默 no-op | 下批：宿主实际拒绝，直到合并进 core.item |
| IH-08 | P2 | `native_inventory_hook.cpp:1387-1437`、`host/plugin_registry.cpp:775-812` | 24 条全 required fail-closed，任一变体缺一个符号则整插件不可用 | 下批：保持 fail-closed，补逐变体符号矩阵 |
| IH-09 | P2 | `native_inventory_hook.cpp:1131,1174` | 两个 UI wrapper 直接调用 backup，缺空指针守卫 | 本批修复 |
| IH-10 | P2 | `native_inventory_hook.cpp:1256-1260`、`plugin.cpp:122-124` | UI 接受 bag 5，API/autosell 拒绝，语义不一致 | 本批按裁决：模块一律不接管；UI 透传原版 |
| IH-11 | P3 | `native_inventory_hook.cpp:988-1007`、`inventory_logging.cpp:21-30` | `caller` 恒 `libgame+0x0`；节流按调用次数冒充帧数 | 本批修复 |
| IH-12 | P3 | `plugin.cpp:354-358,371-375` | `inventory_support` 为 no-op toggle；status 恒 enabled | 本批改为真实 hidden 调试门禁（关闭时 wrapper 透传原版）；合并进 `core.item` 后删除；status 并入 ENG-04 |
| G-2 | — | `game/access/inventory.cpp:86-115` | 替换后的 FindItem handler 未登记 in-flight，release 等待可能漏计 | 基础设施观察，下批 host 边界补登记 |

审阅决议（阶段 2，用户裁决）：

- **D1 乘数模型**：表放 `game/variants`；向下取整；detail 路径 original 0%、overhaul 70%、monster 70%、c18 85%、unknown 0%；store 100%。API/autosell 跟随 detail 倍率；original 变体 autosell 为「自动销毁」。
- **D2 R2 接管边界**：`canonical == b` 且 `1<=b<=127` 走原生；实际使用 S2 编码（canonical>127 / a>0）才接管。b 段读取修正不归 `stack_limit`（R-51 重定向已证伪，`0x1261c4` 直读字段无法被 getter hook 拦截）。
- **D3 抽象面**：`inventory.slot` 仅 `resolve`/`view`/`remove`；`quote` 归 `item.facts`；`sell` 事务归 `inventory.action`。
- **D4 最终形态**：按合并进 `core.item` 后的形态组织，不新建 `core.inventory`。
- **D5 bag 5**：模块不接管，UI 透传原版，API/autosell 拒绝。
- **D6 验收范围**：真机仅 monster v29；c18/overhaul 用静态金标；unknown 变体 0%。
- **D7 分批**：本批 = D1/D2/D3 + IH-01/02/03/04/05/09/10/11/12；下批 = IH-06/07/08 + G-2。

## §1b 阶段 4 装配事故与修复

| 编号 | 现象 | 根因 | 处置 |
|---|---|---|---|
| R1 | 部署后 `assembly: feature metadata missing for inventory_hooks reason=missing/nonzero feature definitions required`，native bootstrap 不启动、API 不可用 | 宿主强制每个插件至少一条 `FeatureDefinition`（`host/assembly.cpp:480-489`）；IH-12 删除 no-op feature 后 `inventory_hooks` 为零 feature | 用户裁决：恢复为**真实调试门禁** `inventory_hooks.inventory_support`（`kHidden`，默认开启）；关闭时 23 条插件内 wrapper 全部透传原版，`FindItem` 由 Game Host handler 替换、不在开关范围；合并进 `core.item` 后删除该开关 |

补充：`host/assembly.cpp` 的注册失败日志已追加插件 id，便于定位同类装配问题。

## §2 无法验证项

- c18 `85%`、overhaul `70%` 价格倍率：仅 `sell_multiplier` 静态金标与 host 用例，未在对应变体真机验证（用户确认按静态金标验收，monster 真机只覆盖 70%）。
- unknown 变体 `0%`：仅 host 用例覆盖，无真机变体。
- UI 详情页接管/退款失败的运行时日志：host 覆盖分流与状态机，真机待阶段 4 取证。
- `inventory.slot` 1.0 暂无生产消费者，仅 host 用例覆盖 resolve/view/remove。
- 调试门禁关闭后的 23 条 wrapper 透传分支仅静态实现与 toggle 状态测试覆盖，未在真机逐条触发。
- UI 详情页出售（预览/确认两态）与退款失败日志未在本轮真机执行，留阶段 5 人工目视。

## §3 已完成验收登记

| 验收项 | 结果 | 证据 |
|---|---|---|
| Host 全量 CTest | **PASS（145/145）** | `cmake --build .tmp/inventory-hooks-audit/host -j 8`；`ctest --test-dir .tmp/inventory-hooks-audit/host --output-on-failure` |
| 基础层契约 | **PASS**：`item.facts` 3.4 `quote`、Item Port 3.4、`inventory.slot` 1.0、`game/variants::sell_multiplier` | `.tmp/inventory-hooks-audit/host` |
| 架构校验 | **PASS**：layers/offsets/jni/resources 全部通过 | `uv run python scripts/maintenance/check_arch.py` |
| 路由漂移 | **PASS**：native 147 / baseline 147 / duplicates 0 / missing 0 / extra 0 | `uv run python scripts/maintenance/check_http_routes.py` |
| JNI | **PASS**：Kotlin 163 / native 163，违规 0 | `uv run python scripts/maintenance/check_jni.py` |
| `git diff --check` | **PASS** | `git diff --check` |
| Android Debug 构建 | **PASS**：`BUILD SUCCESSFUL`，APK `output/inotia4_qol_lsposed_debug_2610180235_23d8baaf1065.apk`，SHA-256 `23d8baaf106518b513d921946741310c0a5b31d27af8ebe2bc6359bc552f265f` | `scripts/build-debug.sh` |
| 集成包 | **PASS**：`output/Inotia4_v1.3.2_monster_v29_lspatched.apk`，内嵌模块 sha256 = `23d8baaf…` | `scripts/patch-apk.sh`；解包比对 |
| 部署身份（阶段 4） | **PASS**：运行 `module identity` sha256 = `23d8baaf1065…`，与本批 Debug 构建一致 | `logcat --pid=22045`；`.tmp/inventory-hooks-audit/logcat-after.txt` |
| 装配（阶段 4） | **PASS**：`assembly: initialized=true registered=28 install_failed=0 kernel_only=false` | 同上 |
| 能力面（阶段 4） | **PASS**：`inventory_hooks` enabled、`degraded=false`、provides `inventory_refresh 1.0` / `inventory.action 2.0` / `inventory.slot 1.0`、24/24 资源 `live` | `/api/system/features` → `.tmp/inventory-hooks-audit/features-after.json` |
| 任务袋守卫（阶段 4） | **PASS**：`POST /sell_item {"bag":5,"slot":0}` → 票据 `failed` / `{"ok":false,"error":"task bag excluded"}` | 票据 2 |
| API 出售报价（阶段 4） | **PASS**：monster v29，category=10 count=3 unit=4，`multiplier=7/10 final=8`；money 28550→28558，槽位清空 | 票据 3；`inventory_trade.cpp:148` 日志 |
| 日志 | **PASS**：当前进程 FATAL/SIGSEGV = 0；无 `assembly: plugin descriptor registration failed` | `logcat --pid=22045` |


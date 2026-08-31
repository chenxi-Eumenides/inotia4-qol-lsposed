# 扩展背包主控文档

> 本文是扩展背包任务的长期控制面，唯一控制范围是本功能的目标架构、阶段状态、验收顺序、设计决策和证据登记。
> `README.md` 负责项目总览，`architecture.md` 负责代码结构与规范，`docs/backlog.md` 负责全局待办，`docs/environment.md` 负责环境与设备，`docs/module-save-store.md` 负责 sidecar 契约；本文引用这些文档，不复制其职责。
>
> **文档版本**：v1.22 ｜ **状态**：CURRENT ｜ **最后修改**：2026-08-31 ｜ **最近审核**：2026-08-31
> **当前阶段**：P0/P1/P2/P3 已通过（对应证据见 §5.2、§9 与 §13）；当前闸门为 P4 原版物品对象与逻辑背包事务桥接（进行中：P4.1 事务域校验与 P4.2 payload 桥接已实现并登记真机最小闭环证据 `E-2026-08-31-04`/`E-2026-08-31-05`；P4.3–P4.5 待实施）｜ P5 范围已包含扩展拖动建立协议（0x81），按 P5 串行规则逐路径真机验收

## 0. 控制面恢复块

- **唯一入口**：会话压缩、换代理或重新打开任务时，先读本节，再读“当前状态”和“当前闸门”，不得以聊天记录或旧日志替代事实。
- **当前结论**：`NOT_ACCEPTED`。P0/P1/P2/P3 的历史通过不替代 P4–P8 最终真机验收；源码存在、host 测试或单次 API 调用均不能视为后续阶段功能完成。
- **下一允许动作**：P4.1/P4.2 已实现、构建、host 通过并经只读补丁审查（Oracle，批准）与真机最小闭环验证（`E-2026-08-31-04`/`E-2026-08-31-05`）；下一步为 P4.3 ownership ledger 真实接线，随后 P4.4 统一事务入口、P4.5 隔离 schema；故障注入类真机项（目标满/取消/插入失败）按「验收路径未就绪」转 P5 路径矩阵，不得标记已通过。
- **P0 纯度约束**：冷启动配置 false 已验证扩展状态 `injected=false` 且 `extension_tab_button=false`（重启后复核）；当前构建满足本轮严格基线的扩展 hook 纯度要求。设置 UI/堆叠合并等其他模块能力不纳入扩展 hook 判定。
- **状态更新规则**：状态只能前进或明确回退；每次状态变化必须同时更新本文顶部状态、对应阶段、证据 ID 和变更日志。
- **冲突裁决**：代码结构以 `architecture.md` 为准，待办来源以 `docs/backlog.md` 为准，设备与工具链以 `docs/environment.md` 为准，API 端点以 `docs/api-reference.md` 为准，sidecar 契约以 `docs/module-save-store.md` 为准；本文只裁决扩展背包范围、阶段、顺序和验收状态。若 `backlog.md` 的交互验收方式与本文冲突，以本文的扩展背包验收闸门为准，并将差异回写 backlog。

## 1. 当前状态与阶段指针

- P0/P1/P2/P3 已有通过记录；当前阶段 P4 原版物品对象与逻辑背包事务桥接进行中：P4.1/P4.2 已实现并登记真机最小闭环证据，P4.3–P4.5 未实施。
- host 测试与 debug APK 构建只证明代码可构建或纯逻辑成立，不能替代真机验收。
- 不依据旧 handoff 的 depth1 试验描述继续改动：当前工作树为袋容器同父挂载加动态位置换算，具体事实见 §9。
- 任何 P3 代码或设备操作前必须重新记录 `git status --short` 与构建身份；不得以旧的“工作树干净”结论代表当前输入。

**结构化控制状态**

| 字段 | 当前值 |
|---|---|
| Overall | `NOT_ACCEPTED` |
| Current phase | `P4` 原版物品对象与逻辑背包事务桥接（进行中：P4.1/P4.2 完成，P4.3–P4.5 未实施） |
| Current gate | 原版物品进入扩展袋调用 `SAVE_SaveItem`、扩展物品进入原版袋调用 `SAVE_LoadItem`，统一事务状态须与 §8.5 prepare journal 顺序一致；每个物品在任意时刻只有一个逻辑所有者 |
| Blocking issue | P4.3 ownership ledger 真实接线、P4.4 统一事务入口、P4.5 隔离 schema 未实施；故障注入类真机项（目标满/取消/插入失败）无 API 表达路径，登记「验收路径未就绪」转 P5；P4 整体通过前不得进入 P5 三方向移动与扩展拖动建立协议 |
| Next allowed action | 实施 P4.3 ownership ledger 接线（复用既有四态 generation handle）；随后 P4.4/P4.5；已登记证据 `E-2026-08-31-04`/`E-2026-08-31-05` 覆盖空槽往返保真、合并身份规则与任务袋拒绝 |
| Owner | 当前执行代理；同一验收项一次只允许一个代码、构建或设备变量 |
| Evidence | P0 `E-2026-08-28-01`；P1 `E-2026-08-28-03`（引用块待补）；P2 `E-2026-08-29-01`（引用块待补）；P3 标签稳定性 `E-2026-08-30-01`；P3 装备/解除 `E-2026-08-31-01`；P3 选中反馈 `E-2026-08-31-02`；P3 弹窗 6 `E-2026-08-31-03`；P4.2 最小闭环 `E-2026-08-31-04`；P4 任务袋拒绝 `E-2026-08-31-05` |

## 2. 不变的实施基线

以下是后续排查必须保持一致的模型摘要，详细实现直接查看对应源码：

### 状态模型

文件：`module/app/src/main/cpp/virtual_bag_state.h`

- `kBagCount = 5`，`kSlotCount = 16`。
- **当前实现事实**：`kFixedCapacities` 已删除；运行时容量统一由 `derive_capacity(BagType)` 派生，见 §8.4 与 ADR-004。固定值 `16/8/4/0/0` 仅是历史原型记录，不得再描述为当前实现。
- **未验收边界**：容量派生和 payload 模型已有 P1 历史证据，但替换、解除、出售和超容规则仍须按其所属阶段逐项真机验证；不得因状态模型存在而宣称最终交付完成。
- **最终要求**：每个扩展逻辑袋必须先装备实际的背包物品，再由该物品的属性派生容量；未装备时入口不可用或容量为零。必须定义唯一容量派生函数及 `BagType` 到容量的规则，并让入口、绘制、命中、拖动、移动校验、自动入库和满包提示全部使用同一结果。解除、替换、出售已装备背包且袋内物品超容时的溢出/阻断规则必须明确。不能视为已完成，直到装备物品身份、payload、容量链和上述边界均有实现与证据。
- UI 模式：`kOriginal`、`kModule`、`kExitingModule`。
- 状态包含原版/扩展物品、选中对象、投影信息和 `PendingTransfer`。
- `Item` 保存类别、数量以及序列化 payload；不保存 native 物品指针。

### 持久化模型

文件：`module/app/src/main/java/com/inotia4/export/ExtensionBagUiBridge.kt`

- section：`extensionbags.items`，v4 是最终 sidecar 唯一允许的版本。
- 物品通过序列化 payload 保存；`pending` 用于跨包移动中断后的恢复。
- **当前原型事实**：文件仍含 `virtualbags.items` 回退、v2/v3 分支、`migrateLegacyState` 和自动回写；这是开发测试遗留，最终交付前必须删除。旧版本格式转换不是最终功能，也不进入最终验收范围。
- **当前原型事实**：普通移动产生的 `pending` 与跨包 journal 目前仅在内存中；非显式保存期间的持久化调用不构成落盘。因此当前恢复仅能覆盖进程内路径，不能宣称支持跨进程事务恢复。

### 投影和移动模型

文件：`module/app/src/main/cpp/game_ui_virtbag.cpp`

- **当前实现事实**：P2 已登记正式窗口投影 install/restore 与退出快照恢复；P3 在此基础上接入原版控件树和输入。任何 P3 控件行为仍须独立真机验收，不能把 P2 投影通过外推为 P3 通过。
- **P3 当前边界**：扩展标签使用原版 `ControlItem` 控件并挂在原版袋容器；投影、快照和输入路径以实际控制流为准，未验收行为不得作为最终能力宣传。
- **最终目标**：投影期间同时保存原版物品指针、当前袋 `direct/GOT` 值和原版容量字段，退出时恢复这些快照；对象所有权必须按 §8.5 管理，不能仅靠模块释放。
- **未验收边界**：容量派生已由 `derive_capacity(BagType)` 统一实现，但已装备袋替换、解除、出售和超容的最终规则仍未完成真机验收；不得把派生实现等同于完整容量能力通过。
- **最终要求**：扩展容量由已装备的扩展背包物品属性派生，并在投影、绘制、点击、拖动和移动校验中使用同一个派生结果；不得以固定绘制 16 格代替实际容量。
- 原版第 6 袋（索引 `5`）是任务物品专用袋。它保留给原版处理，必须完全排除在扩展入口、逻辑映射、原版→扩展、扩展→原版、自动入库和自动装备之外。当前源码仅拒绝原版→扩展的源索引 `5`，其余路径尚未全量隔离。
- 原版第 6 袋（索引 `5`）的“任务物品专用”语义属于 P0 必须登记的逆向/运行证据，不得只以源码注释或单个拦截作为依据。在证据补齐前，仍按最严格策略完全排除它；当前已知的扩展→原版目标、拖拽目标命中、退出/恢复和 pending 解析存在放行风险，不能宣称全路径隔离。
- 三条移动路径：
  - 原版→扩展：`move_original_to_extension_locked()`（实现见 `game_ui_virtbag.cpp`）
  - 扩展→原版：`move_extension_to_original_locked()`（实现见 `game_ui_virtbag.cpp`）
  - 扩展→扩展：`move_extension_to_extension_locked()`（实现见 `game_ui_virtbag.cpp`）
- 移动以 `PendingTransfer` 记录事务，保存成功后清理；中断时由恢复逻辑处理。
- **当前原型事实**：`PendingTransfer` 的内存记录并不等于可恢复 journal；保存成功后清理的描述仅适用于最终协议，不能用于当前实现结论。

## 3. 未验收问题

按优先级记录，不在本节复制源码实现：

### P0：发布身份前提

- 后续每次功能验收都必须能关联源码快照、构建产物、设备安装包和运行日志。
- 每项功能验收都必须使用身份已核对的 APK；身份核对本身不替代功能测试。

### P1：交互与跨包移动

- 扩展物品点击后是否唯一选中扩展对象，而不是命中原版投影槽。
- 扩展→扩展、原版→扩展、扩展→原版是否进入正确路径并保持源/目标物品一致。
- 扩展物品拖动、放置失败和取消时是否不会重复删除或丢失物品。
- 原版袋切换、扩展袋切换、投影安装/清理和唯一选中状态是否一致。

### P2：存档与 UI

- 原版保存成功后 sidecar 写入；未保存内存改动丢失。
- 切换存档、退出页面、回主菜单、重启进程后的投影和 pending 清理。
- 扩展背包入口图标、信息、解除按钮、选中效果和切换音效。
- 固定容量原型不得作为最终验收结论；必须验证装备背包物品后的派生容量及未装备限制。

## 4. 验收顺序

严格一次只验证一项；通过后再进入下一项：

1. 核对当前 APK、设备安装包和启动身份。
2. 原型阶段记录临时容量 `16/8/4/0/0`；最终验收验证装备背包物品后的派生容量、未装备限制和无效槽行为。
3. 原版袋/扩展袋切换及唯一选中。
4. 扩展物品点击、选中和信息显示。
5. 扩展→扩展移动。
6. 原版→扩展移动。
7. 扩展→原版移动。
8. 堆叠合并开启/关闭及满包边界。
9. 自动装备：原版背包优先，原版满后进入扩展背包，全部满时复用原版满包提示。
10. 保存、退出、切档、回主菜单、重启和 pending 恢复。
11. UI 完整性与崩溃回归。

每项只部署该项所需版本。API-first 只有在 `docs/api-reference.md` 已登记并可实际驱动该项操作时才成立；扩展背包的进入、切袋、点击、拖动、移动和保存验收不得把现有 debug 注入端点冒充正式操作 API。正式 API 操作面（能力、请求、响应、错误和幂等语义）必须先设计、实现并登记；在此之前，相关项只能标记为“验收路径未就绪”，不能判通过。API 无法覆盖的真实触摸才由用户执行并反馈，且必须明确操作目标和结论。通过前不得开始下一项。每项至少记录：操作前状态、操作、操作后状态、对应日志结果和用户结论。失败时不跨项修复，也不同时改变代码、构建和安装条件。详细字段使用 §5 的证据模板。

验收唯一使用真机 `192.168.3.54`（TCP ADB/API）；仅启动弹窗允许执行已登记脚本点击 `(420,280)`，其余验收不使用坐标触摸；不使用 `192.168.3.11` 或双机覆盖要求。

### 4.1 API 验收边界

- `docs/api-reference.md` 是 API 端点的唯一权威；本文只规定扩展背包验收所需能力和闸门，不直接替代 API 规格。
- **操作面形态（用户 2026-08-28 决策，修订 ADR-008）**：扩展背包不设独立 API 域；数据与移动操作面 = 原版背包 API（`/api/item/inventory*`，bag 6..10 并入）。读取当前原版/扩展袋状态、三方向移动、配置切换（`/api/config/set`）、保存/切档/重启准备均由原版 API 承载。进入/退出扩展视图、切换逻辑袋、点击/选中/信息属于开发期视图控制能力，由 `/api/debug/extension_bag/*` 提供；该组端点在 P3（正式控件接入）前作为视图验收的操作手段，其结论须与控件树/绘制证据互相印证，不得单独作为 UI 正确性依据。拖动/放置/取消由 `move_item` 的原子事务语义（失败自动回滚）等效承载，真实拖动路径在 P3 控件接入后补充。
- 现有 debug 状态注入端点（equip/item）只能用于开发测试；其写入能力必须在最终发布构建中删除或严格隔离并登记。
- API 面未覆盖的能力（真实拖动/放置/取消）在 P3 前按“验收路径未就绪”处理，不得通过。只有 API 明确无法覆盖的真实触摸才由用户执行；该例外不改变唯一真机、串行验收和证据要求。
- 设备连接恢复时，按 `docs/environment.md §3.1` 已登记的 adb connect、health 轮询和 enter_slot 顺序重跑 P0 smoke，不重新设计连接流程，也不把连接恢复前的超时记录改写为通过。

## 5. 日志与证据登记

- **文件日志**：`/sdcard/Android/data/<游戏包>/files/inotia4-export.log`；Java 启动时会截断旧内容，因此每轮验收结束后立即拉取留档。
- **实时日志**：`adb logcat -s Inotia4Export:V Inotia4VirtBag:V`；前者是 Java/模块身份日志，后者是 native 扩展背包日志。配合 `scripts/analyze/live_session.py` 使用。
- **sidecar**：`getExternalFilesDir(null)/module-saves/`，格式和容量以 `docs/module-save-store.md` 为准。

### 5.1 证据模板

每次验收使用唯一证据 ID（格式 `E-YYYY-MM-DD-NN`），至少填写：

| 字段 | 内容 |
|---|---|
| Gate / Scope | 对应验收项或阶段，以及本次只验证的变量 |
| Source | `git log -1 --oneline`、`git status --short` |
| Build identity | APK 路径、SHA-256、签名、包名、版本 |
| Device | 唯一测试真机 `192.168.3.54:5555`、型号、连接方式和 API 可达性 |
| Config | `extensionBagEnabled`、`moveMergeEnabled`、`stackLimitIncrease` 等影响本项判定的配置快照 |
| Bag / Projection | 当前原版袋、扩展对外逻辑袋/内部索引、overlay 或正式投影状态、原版窗口快照摘要；必须确认原版索引 `5` sentinel 未变化 |
| Ownership / Persistence | 对象所有权状态、分配/释放计数、pending/journal 是否落盘、sidecar generation/CRC、已提交状态与 prepare 状态 |
| Before / Action / After | 操作前状态、单一操作、操作后状态 |
| Logs | 文件日志/logcat 起始位置与关键结果 |
| Result / User verdict | `通过/失败/阻断`；API 结论由执行代理记录，只有物理触摸结论必须由用户确认 |

### 5.2 已登记证据

#### E-2026-08-26-01：P0 本地构建与唯一真机身份链

| 字段 | 内容 |
|---|---|
| Gate / Scope | P0 身份关联；仅核对本地 build → 唯一真机已安装模块 → 启动/API/文件日志，不验证扩展功能 |
| Source | `aa542c2 docs(backlog): 归档已验收的切换闪烁修复`；工作树非干净。本轮构建输入差异摘要 `4c7f98cd776a4bce5842262774842f3eec7dafb7419d1eca935d2edc91e05a97`（命令：`git diff --binary -- module/app \| sha256sum`）；不以 `HEAD` 单独代表构建源码 |
| Build identity | `module/app/build/outputs/apk/debug/app-debug.apk`；历史身份链 APK SHA-256 `8dbfe54ebc79650a7d22783262bfb4320e561381b71b8731d450ea83fece003d`；`com.inotia4.export` `versionCode=170` / `versionName=0.6.11`；v2 签名有效；证书 SHA-256 `00ee2c43b0b66361f405fa4f97ec0876c6dcc113b3e0f004a29192e42cb5cbd6` |
| Device | 唯一真机 `192.168.3.54:5555`，`Phh-Treble with GApps`，Android 12，TCP ADB；已安装模块包版本为 `170/0.6.11`，APK SHA-256 与本地 debug APK 相同 |
| Target game / startup | `com.com2us.inotia4.normal.freefull.google.global.android.common` `1.3.2 (32)`；游戏 APK SHA-256 `29c75274d0866b94dfc447db571de2b9e432a17870b055f848c9fcb8e53e8276`；进程存在，`GET /api/health` 返回 `{"ok":true}`，`GET /api/system/game` 报告模块版本 `0.6.11` 与目标游戏包名 |
| Before / Action / After | 仅只读身份核对；当时配置为 `extensionBagEnabled=true`，因此不作为原版 smoke 结果 |
| Logs | 文件日志 `/sdcard/Android/data/com.com2us.inotia4.normal.freefull.google.global.android.common/files/inotia4-export.log` 可读；本轮末尾锚点为 `2026-08-26 14:18:16.697`、`seq=2687` |
| Result / User verdict | `阻断`：身份链完成；等待关闭扩展功能后，在唯一真机通过 API 完成原版背包 smoke |

#### E-2026-08-27-02：P0 原版移动保存与重启重读失败

| 字段 | 内容 |
|---|---|
| Gate / Scope | P0 单机 API 原版背包 smoke；扩展关闭，仅验证原版物品移动、原版保存、重启后重读 |
| Source | `fd7e0b6c255b4bd4e99e57554896b4de0a984c98`；工作树干净；当前 debug APK SHA-256 `babad530dcf52ade66472ef3b569a717e7c5ba33e954736ee668434db5b88d2f` |
| Build identity | `module/app/build/outputs/apk/debug/app-debug.apk`；`com.inotia4.export` `versionCode=170` / `versionName=0.6.12`；V2 签名证书 SHA-256 `00ee2c43b0b66361f405fa4f97ec0876c6dcc113b3e0f004a29192e42cb5cbd6` |
| Device | 唯一真机 `192.168.3.54:5555`；`Phh-Treble with GApps`；Android 12；重启前 API health 正常 |
| Config | `extensionBagEnabled=false`、`moveMergeEnabled=true`、`stackLimitIncrease=true`、`opEnabled=true` |
| Bag / Projection | 操作前 `bag0/slot4=16`、`bag1/slot6=1`；将 1 个大恢复药水合并至 `bag0/slot4` 后为 `17`，`bag1/slot6` 为空；重启后 API 重读仍为 `bag0/slot4=17`；扩展状态 `injected=false`、`extension_tab_button=false` |
| Ownership / Persistence | 原版 API 移动和 `POST /api/system/save` 均返回 `ok=true`；sidecar 分项未取得；原版索引 5 未在本轮写入 |
| Before / Action / After | 已完成原版移动→保存→强制停止/启动→执行登记触摸脚本；重启后进入 world 并成功读到 `17`，随后游戏 native 崩溃，无法完成稳定性复测 |
| Logs | `adb logcat` 时间锚点 `2026-08-27 18:18:10.936`：`Fatal signal 11 (SIGSEGV)`；栈顶 `libgame.so!Scene_Draw_POPUP_SC_CHARACTER_INFO+608`；进程退出后 `192.168.3.54:8088` 不可达 |
| Result / User verdict | `阻断`：移动保存持久化结果已验证，但重启后 native 崩溃，P0 不通过；不得进入 P1+ |

#### E-2026-08-28-01：P0 原版背包 smoke 通过（v0.6.14）

| 字段 | 内容 |
|---|---|
| Gate / Scope | P0 单机 API 原版背包 smoke；扩展关闭，验证 原版移动→保存→重启→重读 完整闭环；全程 API 驱动，无用户物理触摸 |
| Source | `ba516dd feat(v0.6.14): block agreement launch outside main menu`；工作树仅本文档改动 |
| Build identity | `module/app/build/outputs/apk/debug/app-debug.apk`；SHA-256 `233b52c2bf34bbda74af22bec414f8c3771408f8079b876e11f16ed97773a859`；`com.inotia4.export` `versionCode=172` / `versionName=0.6.14`；V2 证书 SHA-256 `00ee2c43b0b66361f405fa4f97ec0876c6dcc113b3e0f004a29192e42cb5cbd6`；设备安装 APK SHA-256 与本地构建一致 |
| Device | 唯一真机 `192.168.3.54:5555`；`Phh-Treble with GApps`；Android 12；TCP ADB；`GET /api/health` 全程正常 |
| Config | `extensionBagEnabled=false`、`moveMergeEnabled=true`、`stackLimitIncrease=true`、`opEnabled=true`；文件日志确认 `extensionBagEnabled=false applied=true` |
| Bag / Projection | 进入前 `bag0/slot4=17`（昨日保存结果仍持久）、`bag1/slot6` 空；索引 `5` sentinel 全程 `capacity=16, slot_count=0` 未变；移动 1 个大恢复药水 `bag0/4→bag1/6` 后 `16/1`；重启重读一致 `bag0/4=16`、`bag1/6=1`；扩展状态重启后复核 `injected=false`、`extension_tab_button=false` |
| Ownership / Persistence | `POST /api/system/save` 返回 `ok=true`（money=133640、map=30）；原版移动 `move_item ok=true`；重启→重读闭环通过；本轮原版 smoke 不依赖 sidecar，sidecar generation/CRC 未单独读取 |
| Before / Action / After | 冷启动→`GET /api/ui/screen` 检测 `agreement`→`POST /api/ui/dialog/select {action:ok}` 返回 `tap_dispatched`→轮询至 `main_menu`→`enter_slot(0)`→轮询世界就绪（money/leader/map 有效）→移动→保存→force-stop 重启→重复弹窗 API→`enter_slot(0)`→重读验证 `16/1` |
| Logs | 文件日志锚点 `2026-08-28 13:48:27.476`：`module identity: versionCode=172 versionName=0.6.14 sha256=233b52c2...`；操作记录含 `POST /api/ui/dialog/select {action=ok}` 与 `POST /api/system/enter_slot {slot=0} result={ok:true}`；模块自动拦截 Hive 支付弹窗并恢复 |
| Result / User verdict | `通过`：API 结论由执行代理记录；启动弹窗检测/关闭、进档、背包读取、移动、保存、重启重读全部经 API 完成；P0 闸门关闭，P1 待 ADR-008 API 登记 |

#### E-2026-08-28-02：扩展交互正式 API 登记并真机验证（v0.6.15）

| 字段 | 内容 |
|---|---|
| Gate / Scope | ADR-008 正式操作面就绪验证；`/api/extension_bag/*` 六端点全链路真机验证（状态/视图/切袋/点击/三方向移动/错误语义/sidecar 持久化）；非扩展功能验收 |
| Source | `ba516dd` 基础上新增扩展背包 API 实现（未提交时验证，提交即 v0.6.15）；构建含 `move_original_to_extension_slot_locked` 重构与 `INVEN_RemoveItemDirect` 返回值修复 |
| Build identity | `com.inotia4.export` `versionCode=173` / `versionName=0.6.15`；`GET /api/system/game` 报告 `0.6.15`；经 `adb install -r` 部署 |
| Device | 唯一真机 `192.168.3.54:5555`；全程 API 驱动（协议页经 `dialog/select {action:ok}` 关闭，无用户触摸） |
| Config | 验证期 `extensionBagEnabled=true`（API 测试必需）；验证结束恢复 `false`（运行时 `injected=true` 为既有已知行为，冷启动恢复严格基线） |
| Bag / Projection | 原版→扩展：`bag0/4` 魔法衣料 cat41×1 → `bag6`（源槽清空、扩展 slot0 入位）；扩展→原版：返回至 `bag0/4` 原槽；ext→ext：`bag6/1` cat2×1 → `bag7/1`；重启后 sidecar 精确恢复 `bag7/{0,1}`；索引 5 双向拒绝（`task bag excluded`） |
| Ownership / Persistence | `POST /api/system/save` ok=true（扩展开启态）；重启重读 sidecar 物品一致；无 pending 残留（`recovery_action: none`） |
| Before / Action / After | 状态→enter_view(6)→幂等拒绝→click 空/实物→move 三方向→task bag 拒绝→orig→orig 重定向→select/exit 语义→save→force-stop 重启→enter_slot→sidecar 比对→物品归位→保存→关闭扩展 |
| Logs | 文件日志 `[OP] extension_bag/*` 全记录；`cross move: original->extension source removal failed`（修复前）→ 修复后移动成功；`game_symbols.h` 已登记 `INVEN_RemoveItemDirect` 返回值不可信（删除成功仍返回 0） |
| Result / User verdict | `通过`：六端点全部按规格工作；发现并修复返回值误判缺陷（真机验证其回滚曾致运行时源物品移除——磁盘存档未受影响，经重载恢复）；preflight 初始化未就绪误判已登记 backlog |

#### E-2026-08-31-04：P4.2 空槽往返 payload 字节保真与合并身份规则（API 驱动）

| 字段 | 内容 |
|---|---|
| Gate / Scope | P4.2 最小闭环 §7.2「空槽」+「可合并/身份规则」；仅验证 payload 往返一致性与合并判定，不验证故障注入 |
| Source | `2b9c003 fix(P3): 修复面板替换与背包识别`；工作树含未提交 P4.1/P4.2 改动（`virtual_bag_state.h`、`game_ui_virtbag.cpp`、`tests/test_host.cpp`、本文档与 `docs/p4-native-item-transaction-bridge.md`） |
| Build identity | 本地 `module/app/build/outputs/apk/debug/app-debug.apk` SHA-256 `5660927285cb36d60636b0daf0df18a7edb53fba27f3289809665e8725256ad8`；设备 `base.apk` SHA-256 前 20 位一致（`5660927285cb36d60636`）；`com.inotia4.export` `versionCode=175` / `versionName=0.6.17` |
| Device | 唯一真机 `192.168.3.54:5555`，TCP ADB；`GET /api/health` 全程 `{"ok":true}` |
| Config | 扩展开启态：status `enabled=true, injected=true`；全程经 `/api/item/inventory/move_item`（bag 6..10 并入）驱动 |
| Bag / Projection | 试前 `bag7/0`=恢复药水（大）×18（payload `Ejj6yQ…==`）、`bag8/0`=背包（小）×1（`EofqSQ0…==`）、原版主袋 slot6=恢复药水（大）×2；原版索引 `5` 全程无成功写入，sentinel 未受影响 |
| Ownership / Persistence | 每步 `recovery_action=none`、`pending` 空；扩展→原版落位为首空槽（`INVEN_SaveItemOnEmpty` 语义）；orig→ext 成功后按 P2 设计安装扩展视图并锁移动，属预期行为 |
| Before / Action / After | ① `bag7/0`×18→原版（落 slot2）→返回 `bag7/0`：payload 逐字节一致；② `bag8/0`→原版 slot2→返回：payload 一致；③ 原版 slot6×2→`bag7`：payload 身份不同（`ErsISg…`≠`Ejj6yQ…`）不合并、独立落 `slot1`，符合合并身份规则 |
| Logs | logcat 关键行 `.tmp/p42-evidence.log`（22 行：`cross move` ×4、`task bag excluded` ×2、P2 拦截 ×5）；文件日志留档 `.tmp/inotia4-export-p42.log`，尾部 `write_restore` mode 1→0（关面板复位视图） |
| Result / User verdict | `通过`（API 结论，执行代理记录）；两次用户触摸仅为进入/退出扩展视图（P2 设计的触摸边界），物品操作全部 API 完成 |

#### E-2026-08-31-05：P4 事务域任务袋 5 双向拒绝（API 驱动）

| 字段 | 内容 |
|---|---|
| Gate / Scope | P4 §7.2「非法目标」；仅验证 bag5 作为源/目标的事务拒绝 |
| Source / Build identity / Device / Config | 同 `E-2026-08-31-04` |
| Bag / Projection | 拒绝前后扩展状态逐字段一致（bag7/0 ×18、bag7/1 ×2、bag8/0 ×1），`recovery_action=none`；任务袋 sentinel 无写入 |
| Before / Action / After | `move_item bag=7→to_bag=5` → `{"ok":false,"error":"task bag excluded"}`；`bag=5→to_bag=7` → 同样拒绝；视图未退出时段被 P2 守卫先行拦截（`extension view open; movement disabled`），守卫顺序正确 |
| Logs | logcat `2026-08-31 20:32:45.084/.102` 两条 `[OP] … result={ok:false,error=task bag excluded}`（见 `.tmp/p42-evidence.log`） |
| Result / User verdict | `通过`（API 结论，执行代理记录） |

#### E-2026-08-31-06：P4.3 审计循环（API 驱动）与 orig→ext 保持当前视图变更

| 字段 | 内容 |
|---|---|
| Gate / Scope | P4.3 真机审计循环（不含视图进出借用配对轮，需触摸另登记）+「orig→ext 后保持当前视图」行为变更回归；用户 2026-08-31 决策：跨域移动后不进入扩展视图，对齐原版"移动后停留" |
| Source | `2b9c003`；工作树含未提交 P4.1–P4.3 改动（`virtual_bag_state.h`、`game_ui_virtbag.cpp`、`tests/test_host.cpp`、文档）；P4.3 实现经 Oracle 只读复核（1 项 High 缺陷修复后 Fix approved，session `ses_fa7f5c104ffeFxT8t2O0Dxz43k`） |
| Build identity | `app-debug.apk` SHA-256 `72cad3ede778c460dc15…`；`com.inotia4.export` 175/0.6.17；`adb install -r` 成功后 force-stop 重启加载 |
| Device | 唯一真机 `192.168.3.54:5555`；全程 API 驱动（协议页经 `dialog/select ok`，无用户触摸） |
| Config | 扩展开启态（`enabled=true, injected=true`） |
| Bag / Projection | 初始 `bag7/0`×18（`Ejj6yQ…`）、`bag8/0`×1（`EofqSQ0…`）；3 轮循环后逐字节一致；原版网格无残影（RefreshItemArea 清理）；索引 `5` 全程仅拒绝、无写入 |
| Ownership / Persistence | 3 轮 ×（ext→orig + orig→ext + ext→ext×2 + 拒绝×2）；审计 6 条全 `balanced=1`、`borrows=0`、`defer free=0`、静止 `objects=0`；`allocated 2→6 / released 1→3 / handed_over 1→3` 与操作一一对应；`/api/system/save` → 强杀 → 重启 → 读档：ext 物品字节级恢复、`mode=original`、`recovery_action=none` |
| Before / Action / After | 12 笔成功移动 + 3 笔任务袋拒绝 + 1 笔空源拒绝，全程无崩溃；orig→ext 后 `mode/selected` 不再被改写（P2 锁不再触发） |
| Logs | `.tmp/p43-evidence.log`（25 行：ownership audit ×6、cross move ×13、拒绝若干） |
| Result / User verdict | `通过`（API 结论，执行代理记录）；限制：视图借用配对的真机轮未含（进/出扩展视图需触摸，待补） |

#### E-2026-08-31-07：P4.3 视图借用配对真机轮（用户触摸，35 次开关含快速切换）

| 字段 | 内容 |
|---|---|
| Gate / Scope | P4.3 退出条件「重复进入、切换、退出后视图外借用为零」；仅验证借用配对与计数稳定，不含移动/入库 |
| Source / Build identity | 同 `E-2026-08-31-06` 工作树；APK `f2158ee554066a3a212a…`（含 mode/selected 生命周期约束 v1.21 修复） |
| Device / Config | 唯一真机 `192.168.3.54:5555`；扩展开启态；用户触摸：原版 ↔ 扩展三个逻辑袋（6/7/8）慢速随机切换约 1 分钟 + 快速切换多次 |
| Bag / Projection | 35 次投影安装（internal 0/1/2 全覆盖，快速阶段单次切换间隔 0.3–0.7s）；结束时用户停在原版视图 `mode=original selected=-1 recovery=none` |
| Ownership / Persistence | 35 条 `restore_module_view` 审计与 35 次安装一一配对；每条 `balanced=1`、`borrows=0`（归还后归零）；快速阶段 `allocated=4/released=1/handed_over=1` 恒定（缓存复用，循环计数无增长）；`defer free=0`、`balanced=0=0` |
| Before / Action / After | 缓冲清空 → 用户慢速+快速开关切换 → 拉取分析；全程无崩溃、API 持续响应 |
| Logs | `.tmp/p43-view-borrow-evidence.log`（70 行：安装 ×35 + 审计 ×35）；限制：install 侧无审计行，视图打开期间的 `borrows>0` 中间态未直接观测，配对由 restore 侧归零 + 全程平衡推断 |
| Result / User verdict | `通过`（API 结论，执行代理记录；切换操作为用户物理触摸）。P4.3 真机退出条件至此全部覆盖（失败移动/入库见 `E-2026-08-31-06`） |

#### 已引用、证据块待补的历史记录

- `E-2026-08-28-03`（P1）与 `E-2026-08-29-01`（P2）已在 §9 与 §13 引用，但完整字段尚未回填至本节；本次不从旧日志推断或补造缺失字段。
- 在补齐原始 APK 身份、设备、配置、操作前后状态与日志锚点前，它们只能作为历史阶段引用，不能支撑任何 P3 通过结论。

#### E-2026-08-30-01：P3 扩展标签挂载动画期位置稳定

| 字段 | 内容 |
|---|---|
| Gate / Scope | P3 原版控件正式接入；仅验证扩展标签在面板开合动画期间的位置稳定性，不验证信息页、选中、拖动或移动语义 |
| Source | `a3bef0b feat(P3): 扩展标签显示与交互收尾——挂载修正+点击修复+底框+选中互斥`；工作树非干净：`game_access.cpp/.h`、`game_symbols.h`、`game_ui_virtbag.cpp` 与本文档均有未提交改动 |
| Build identity | `module/app/build/outputs/apk/debug/app-debug.apk`；SHA-256 `6bf68b5ca51c120b162865ac9a1808aff4456829f936b88477f2fde13dc99f70`；`com.inotia4.export` `versionCode=175` / `versionName=0.6.17` |
| Device | 唯一真机 `192.168.3.54:5555`，`Phh-Treble with GApps`，Android 12，TCP ADB；`GET /api/health` 返回 `{"ok":true}` |
| Config | `enabled=true`、`injected=true`、`extension_tab_button=true`、`inventory_frame_active=true`、`recovery_action=none`；当前扩展状态 `mode=original`、`originalSelected=4`、`infoBag=2` |
| Bag / Projection | 当前逻辑袋容量 `[16,4,12,0,0]`；原版索引 `5` sentinel 为 `capacity=16, slot_count=0`；标签已在原版袋容器控件树中。未采集原始 rect 数值，位置结论仅基于用户在面板开合动画中的视觉观察 |
| Ownership / Persistence | 本轮不改变物品、所有权或 sidecar；`recovery_action=none` |
| Before / Action / After | 扩展标签已挂载并显示；用户手动开合面板观察标签动画；用户结论为“没有移动、偏移的现象，可以视作位置稳定” |
| Logs | 验收后文件日志存在（1466 行）；本项为用户物理触摸视觉验收，未采集 rect/Frida 诊断日志，不能据此外推其他 P3 语义 |
| Result / User verdict | `通过`：仅 P3 标签挂载动画期位置稳定性通过；P3 整体仍为 `NOT_ACCEPTED` |

#### E-2026-08-31-01：P3 扩展袋装备与解除正式路径真机通过

| 字段 | 内容 |
|---|---|
| Gate / Scope | P3 扩展袋装备（拖放至扩展标签 + 装备按钮原版满溢出）与解除（信息页按钮 + HTTP），含正确类型背包物品回流与保存往返一致性；不含扩展拖动建立协议（0x81，转后续） |
| Source | `e28f498`/`d4e0916`/`1f97d32`（解除/装备符号、equip_bag 状态函数、主集成——装备/解除正式路径、标签悬空根治、thunk 页共享）；工作树随后含 drawdiag 移除与本文档 v1.17 |
| Build identity | `module/app/build/outputs/apk/debug/app-debug.apk`；SHA-256 `412c4777d23a406b95f6bfbecdf93ba3b30276ecd4c4ab4089b75bcbfa6ac461`；`com.inotia4.export` `versionCode=175` / `versionName=0.6.17` |
| Device | 唯一真机 `192.168.3.54:5555`，`Phh-Treble with GApps`，Android 12，TCP ADB；`GET /api/health` 返回 `{"ok":true}` |
| Config | `enabled=true`、`injected=true`、`extension_tab_button=true`；扩展类型 `[4,1,3,0,0]`（大/小/中/空/空）→ 全满验证 `[4,1,3,1,1]` |
| Before / Action / After | 用户真机操作：拖放背包（小）到空闲扩展标签 → 装备成功、物品消耗、标签更新；点击背包物品→装备按钮（原版袋位 1..4 全满）→ 溢出装备到扩展空位、不再弹“背包已满”；解除扩展袋后重新装备不同类型 → 详情页显示新类型；标签不再可拖出物品。API 往返：显式保存→强杀→重进读档，`types` 与背包物品数量两次往返一致（5×背包小+1×手提包） |
| Logs | 本次收尾验证日志含 `desc open gate hook patched`、`injected=true`、装备/解除/回滚 VIRTBAG_LOG；drawdiag 诊断日志已随收尾移除 |
| Result / User verdict | `通过`（用户确认“看起来没问题了”）：拖放/按钮两入口装备、解除、物品回流、详情类型刷新、标签不可拖动、保存往返一致 |

#### E-2026-08-31-02：P3 扩展袋选中反馈无问题

| 字段 | 内容 |
|---|---|
| Gate / Scope | P3 按下与唯一选中反馈视觉；用户在本阶段测试装备/解除时顺带观察 |
| Device | 唯一真机 `192.168.3.54:5555`（同 E-2026-08-31-01） |
| Before / Action / After | 用户在装备/解除验收中多次点击扩展标签（选中/详情/切换），观察选中高亮与唯一选中互斥 |
| Result / User verdict | `通过`（用户确认“选中反馈，目前看没问题”） |

#### E-2026-08-31-03：P3 装备全满弹窗 6 用户确认通过

| 字段 | 内容 |
|---|---|
| Gate / Scope | P3 收尾最后一项；装备时原版袋 1..4 + 扩展袋 6..10 全部满，点击装备按钮触发弹窗 6（扩展无空位时透传原版 `UIEquip_ButtonEquipExe`）；仅验证弹窗 6 触发与扩展状态不变，不改变移动/选中/信息等其他 P3 变量 |
| Source | `a259830 docs(P3): 控制面 v1.17——装备/解除与选中反馈证据登记、全满边界与拖动转后续`；工作树仅含与控制面无关的未跟踪目录 `docs/改版作者的一些挖坟文档/` |
| Build identity | `module/app/build/outputs/apk/debug/app-debug.apk`；SHA-256 `412c4777d23a406b95f6bfbecdf93ba3b30276ecd4c4ab4089b75bcbfa6ac461`；`com.inotia4.export` `versionCode=175` / `versionName=0.6.17`（同 E-2026-08-31-01/02） |
| Device | 唯一真机 `192.168.3.54:5555`，`Phh-Treble with GApps`，Android 12，TCP ADB |
| Config | `extensionBagEnabled=true`、`injected=true`、`extension_tab_button=true`；原版袋 1..4 全满 + 扩展袋 6..10 全满 |
| Before / Action / After | 原版袋 1..4 全满 + 扩展袋 6..10 全满状态下，用户点击装备按钮触发弹窗 6；弹窗出现，扩展状态不变 |
| Logs | 同 E-2026-08-31-01 会话延续 |
| Result / User verdict | `通过`（用户确认“我已检查，没问题”）：P3 收尾最后一项通过；P3 整体由 `收尾中` 推进为 `已通过`，进入 P4；扩展拖动（0x81）按用户决策并入 P5 范围 |

## 6. 关键代码索引

| 文件 | 作用 |
|---|---|
| `module/app/src/main/cpp/game_ui_virtbag.cpp` | 扩展背包投影、绘制、事件、拖动、移动事务和恢复 |
| `module/app/src/main/cpp/virtual_bag_state.h` | 容量、状态、物品、pending 和纯状态判定 |
| `module/app/src/main/cpp/game_ui_virtbag.h` | 扩展背包 native 公共接口 |
| `module/app/src/main/cpp/game_patch.cpp` | 原版控件事件接入与输入分流 |
| `module/app/src/main/cpp/game_inventory.cpp` | 原版库存操作与投影同步 |
| `module/app/src/main/cpp/game_save.cpp` | 保存生命周期接线 |
| `module/app/src/main/cpp/game_ui.cpp` | 场景绘制与 UI 接入点 |
| `module/app/src/main/cpp/game_symbols.h` | 原版 UI/库存符号与函数签名 |
| `module/app/src/main/cpp/symbol_registry.h` | native 符号注册与解析入口 |
| `module/app/src/main/cpp/gamebridge.cpp` | JNI 导出与 native 分发 |
| `module/app/src/main/java/com/inotia4/export/ExtensionBagUiBridge.kt` | section v4 读写和 pending 持久化；当前 v2/v3 兼容代码是待删除的原型遗留 |
| `module/app/src/main/java/com/inotia4/export/NativeBridge.kt` | Kotlin/native API 声明 |
| `module/app/src/main/cpp/tests/test_host.cpp` | 状态、payload、JSON、合并和恢复测试 |

## 7. 继续工作的规则与代理协作

- 未真机验证 = 未完成；源码存在对应路径也不等于功能完成。
- 先建立可复现的 APK→设备→运行链，再验证单项功能。
- 本文只控制扩展背包的范围、阶段、决策、证据和验收；代码规范、API、环境、sidecar 和全局待办仍由对应权威文档维护。
- 代理只能修改自己负责的代码或证据；修改本文状态前必须提供证据 ID，冲突时保留失败记录并按 §0 裁决，不得静默覆盖。
- 并行代理不得同时改变同一验收项的代码、构建版本或设备条件；每项验收只保留一个活动变量。
- 交接必须填写：`Completed`、`Not completed`、`Current gate`、`Confirmed facts`、`Open hypotheses`、`Do not repeat`、`Next exact action`、`Required evidence`。

## 8. 最终目标架构（替代当前临时实现）

### 8.1 目标定义

当前工作树中的基线代码只作为原型和问题定位材料，不作为最终架构约束。正式目标为：

> **不扩大原版背包数量和原版存档结构；复用原版背包 UI、物品对象和库存操作逻辑；通过运行时逻辑背包映射将扩展背包接入原版背包窗口；原版保存成功后，将扩展数据写入模块 sidecar。**

“只在保存时接入模块存档”仅描述持久化时机，不表示运行时可以不接入扩展数据。运行时必须完成逻辑背包与原版背包窗口之间的映射、同步和所有权管理。

### 8.2 原版能力边界

设计必须以原版 `libgame.so` 已确认的能力为基础：

- 原版背包数据：`INVEN_pItem`，6袋×16槽，每槽为物品指针；袋容量由袋对象字段决定。索引 `5` 是原版任务物品专用袋，不参与扩展功能。
- 原版 UI：`UIEquip_CreateInvenControl` 创建控件，`UIEquip_DrawInvenBag`、`UIEquip_DrawInvenItem` 绘制，`UIEquip_InvenBagControlEventProc`、`UIEquip_InvenItemControlEventProc` 处理输入和物品上下文。
- 原版库存操作：`INVEN_MoveItem`、`INVEN_SaveItemOnEmpty`、`INVEN_RemoveItemDirect` 以及原版装备、使用、出售、堆叠和掉落入库路径。
- 原版物品生命周期：`SAVE_SaveItem` 序列化，`SAVE_LoadItem` 重建；重建失败或结束使用时必须正确归还 `ITEMPOOL` 对象。
- 原版存档生命周期：`SAVE_Save`/`SAVE_SaveInventory` 保存原版数据，`SAVE_LoadInventory` 读取原版数据。

以上函数的具体签名和地址以 `game_symbols.h`、`symbol_registry.h` 和反汇编证据为准，不在本节重复登记地址。

### 8.3 逻辑背包与原版窗口映射

正式实现不增加原版袋数量。扩展袋仅作为逻辑编号存在，并在打开扩展视图时复用一个原版背包显示窗口：

```text
原版逻辑背包：0..4（扩展可映射范围）
原版任务物品专用袋：5（仅原版处理，扩展完全排除）
扩展逻辑背包（对外编号）：6..10
扩展逻辑背包（内部索引）：0..4
原版物理 UI 窗口：固定存在，数量和控件结构不变
```

映射层必须明确维护：

1. 当前显示的逻辑背包编号和原版窗口编号。
2. 逻辑槽位与原版 UI 槽位的对应关系。
3. 原版窗口进入前的物品指针、当前袋、容量和控件状态快照；映射窗口只能是原版索引 `0..4`。
4. 扩展物品临时 native 对象的所有权、缓存和释放时机。
5. 原版 UI 操作完成后，扩展逻辑状态的同步方向和结果。
6. 任意失败、退出、切档和进程中断时的恢复状态。
7. 原版索引 `5` 的隔离：不得作为扩展映射源/目标，且扩展操作不得向其中读取、写入或自动投递物品。

映射期间不得长期保存 native 物品指针，不得把扩展物品永久写入原版库存，不得改变原版背包数量，不得修改原版 `save*.dat` 格式。

**编号约定**：文档、API、日志和证据使用对外逻辑编号 `6..10`；native 状态结构、`PendingTransfer` 和 sidecar 槽位使用扩展内部索引 `0..4`。唯一换算为 `logicalBagId = internalBagIndex + 6`、`internalBagIndex = logicalBagId - 6`；原版袋编号仍为 `0..5`，不得把扩展内部索引直接当作原版袋编号。所有边界输入必须拒绝负数、原版索引 `5` 和超出各自编号域的值。

### 8.4 Native 对象所有权与容量派生

**对象所有权状态机**必须至少包含：

1. `module-owned`：对象只由模块持有，扩展逻辑状态负责其 payload，模块负责释放。
2. `borrowed-for-view`：仅在原版窗口绘制/事件调用窗口内借用；原版控件不得在窗口外保留该指针，退出、切换和异常必须归还到模块所有权。
3. `inventory-owned`：对象已由原版库存接管；模块不得再次释放或继续使用原指针。
4. `released`：释放完成且不可再访问。

每次状态转移必须有唯一转移方、转移时机和失败回滚；使用分配/释放计数或等价诊断证明覆盖切换、取消、失败、切档和重启。`SAVE_LoadItem` 失败时不得按 category/count 创建近似物品；该记录必须进入隔离区并提供只读诊断，不能被 `normalize()` 或下一次保存静默覆盖。

**容量派生契约**：唯一派生函数以“该扩展袋已装备的背包物品完整身份与 payload”为输入，输出有效槽数和入口状态。它必须明确 `BagType`、物品属性、未装备、替换、解除、出售和超容溢出的规则；所有 UI、移动和自动入库路径只能调用该契约，不得读取固定数组或各自复制容量判断。

**容量派生契约 v1（2026-08-28 落地，证据链见 `docs/system/bag.md` §5）**：
- 派生函数 = `virtual_bag::derive_capacity(BagType)`（virtual_bag_state.h），静态真源 = ITEMSTATICBASE 表（category 1/2/3/4 → 容量 4/8/12/16；kNone/未装备 → 0）
- 未装备：容量 0，`click()` 拒绝，视图/移动/自动入库均不可用
- 超容溢出（P1 规则）：normalize 不丢弃袋内数据（禁止静默覆盖），容量裁剪仅在视图与交互层生效；替换/解除/出售的正式规则待逆向补齐（bag.md §7 未决）
- 已替换 `kFixedCapacities` 的全部运行时读取（rendering/drag/normalize/click）；host 测试 284 项通过

### 8.5 持久化边界

- 扩展数据保存在模块 sidecar 的 `extensionbags.items` section。
- 原版保存成功后，才允许提交扩展背包 sidecar；但保护原版与扩展一致性的 prepare journal 必须在调用原版保存之前独立落盘。
- 持久化状态必须区分“已提交扩展状态”和“未提交 prepare journal”，不能仅按 payload 相等判断事务完成。journal 至少包含 `slotId`、`generation`、`transactionId`、源/目标身份、数量、payload 标识和提交阶段。
- 正式顺序为：写入含 prepare journal 的 sidecar → 调用原版保存 → 原版成功后写入新扩展状态并提交 generation → sidecar 提交成功后清理 journal。任一步失败都保留 journal，重启时按 transactionId/generation 幂等重放或回滚，并记录最终选择。

**prepare journal 格式 v1（2026-08-28 落地）**：
- 落盘位置：sidecar 独立 section `extensionbags.journal`（v1），与 committed section `extensionbags.items` 物理分区；复用 ModuleSaveStore 的原子写/CRC/last-good/generation
- 记录字段：`transactionId`（Kotlin `j-<millis>-<counter>` 生成）、`stage`（0=prepared / 1=original_saved / 2=sidecar_committed）、`generation`、方向、源/目标袋槽、payload（b64，与 PendingTransfer 同语义）、sourcePayload（ext→orig）
- 恢复裁决 = `journal_recovery_action(state, journal, worldProbe)`（virtual_bag_state.h，host 测试覆盖）：**原版世界实态探针优先于 stage 标记**（stage 可能在"原版保存成功后、stage 落盘前"崩溃时落后于实态）——orig→ext 源槽已无源物品 ⇒ 重放扩展侧；仍有 ⇒ 回滚。ext→orig 反向对称。committed 已等于目标态或 stage=2 ⇒ 仅清理（幂等）。journal 非法 ⇒ kDiscard 隔离并告警，禁止按其重放
- 执行顺序（P7 收口）：pending(内存) → journal(stage=0) → 原版变更+原版保存 → journal(stage=1) → sidecar 提交 committed → 清 journal
- 原版保存失败时，扩展改动不得被标记为已保存；sidecar 写入失败或两者之间进程中断时，不得静默清除未完成事务。
- 原版槽 `0..2` 与模块 sidecar 槽一一对应。
- 扩展物品保存使用完整 `SAVE_SaveItem` payload，不保存 native 指针。
- 最终实现不得读取旧 section，不得包含 v2/v3 格式转换、`migrateLegacyState` 或自动迁移/回写逻辑。
- sidecar 必须绑定目标游戏 APK/libgame 身份（至少包名、版本和对应身份摘要）；身份不兼容时拒绝反序列化并保留只读诊断数据。
- 所有自动保存和显式保存调用点必须统一经过上述协调器；不得存在直接调用原版保存而绕过 prepare/journal/sidecar 提交的路径。存档面板进入是否触发保存必须作为明确产品决策：若保留“进入即保存”，必须记录其与原版“打开面板只读旧档”语义的差异及影响；若不保留，进入面板不得隐式写入当前进度。

## 9. 详细子项目与阶段闸门

以下阶段是开发阶段，不等同于第4节的最终真机验收项。每个阶段完成后必须满足其完成要求，失败时回到本阶段，不跨阶段修改事务语义。

**阶段状态总表**：当前仅允许按照表中 `Current phase` 推进；未登记证据的阶段不得标记为完成。

| 阶段 | 状态 | 闸门 |
|---|---|---|
| P0 | ✅ 通过（2026-08-28，E-2026-08-28-01） | 身份、唯一真机 API 原版基线和证据 |
| P1 | ✅ 通过（2026-08-29，E-2026-08-28-03：v0.6.17/175 真机验证） | 逻辑背包模型与 sidecar 原型：容量派生逆向、所有权状态机、prepare journal、host 测试 |
| P2 | ✅ 通过（2026-08-29，E-2026-08-29-01：10 轮压测 + 回主菜单恢复） | 原版 UI 只读窗口原型：正式投影 install/restore、移动门禁、索引5 sentinel |
| P3 | ✅ 通过（2026-08-31，标签稳定 `E-2026-08-30-01`、装备/解除 `E-2026-08-31-01`、选中反馈 `E-2026-08-31-02`、全满弹窗 6 `E-2026-08-31-03`；扩展拖动 0x81 并入 P5） | 原版控件与扩展窗口的正式接入 |
| P4 | 🔄 进行中（P4.1/P4.2 已实现并登记 `E-2026-08-31-04`/`E-2026-08-31-05`；P4.3–P4.5 未实施） | 原版物品对象与逻辑背包事务桥接：唯一 Save/Load 桥、所有权唯一、失败隔离 |

### 阶段 P0：冻结原版基线与可复现身份

**内容**

- 固定当前目标游戏 APK、`libgame.so`、模块源码快照和构建环境。
- 记录原版6袋、16槽、容量变化、切袋、点击、拖动、堆叠、装备、使用、出售、掉落入库和保存行为。
- 建立 APK→设备安装包→运行日志的身份关联。
- 唯一使用真机 `192.168.3.54` 的 API；不使用坐标触摸或第二台真机。
- 证明扩展关闭时不注入扩展入口、控件树或扩展 hook；若只能关闭功能标志而不能满足该条件，必须单独登记为“带注入的近原版基线”，不得替代严格 smoke。
- 登记原版索引 `5` 为任务物品专用袋的逆向来源或可重复运行证据，并建立 sentinel 快照：物品指针、容量、切袋可见性和保存前后内容均不得被扩展路径改变。

**完成要求**

- 原版游戏在不启用扩展功能时可以正常进入背包、移动物品、保存和重新读取。
- 保存前后原版背包状态一致。
- 记录 APK SHA-256、签名、包名、版本、设备标识和日志起始位置。
- 严格 smoke 的配置、hook 和控件树均证明未受扩展改动；索引 `5` sentinel 比对通过。

**测试目标**

- 唯一真机 `192.168.3.54` 完成一次 API 原版背包 smoke 测试。
- 失败时不进入扩展实现，先修复基线或记录阻断原因。

### 阶段 P1：逻辑背包模型与 sidecar 原型

**内容**

- 定义5个扩展袋、逻辑槽位、物品 payload 和 pending/journal 格式；`16/8/4/0/0` 仅保留为原型测试配置，最终容量必须由已装备背包物品派生。
- 定义扩展物品的唯一所有权规则、对象所有权状态机、分配/释放计数和失败转移规则。
- 完成“装备背包物品→容量派生”的逆向和模型设计：装备物品完整身份、`BagType` 映射、替换/解除/出售及超容溢出规则。
- 验证 sidecar 槽隔离、CRC、last-good 和损坏恢复；旧格式转换不属于最终范围。
- 定义与已提交状态分离的 prepare journal、generation/transactionId 规则、幂等重放/回滚规则和游戏身份绑定。

**完成要求**

- 纯逻辑移动、合并、满包、取消、失败回滚和恢复状态可确定重放。
- 扩展状态不含 native 指针。
- sidecar 写入失败不会丢失可恢复事务信息。

**测试目标**

- host 测试覆盖正常 payload、非法 payload、不同物品身份、99/999 上限、空槽/满槽和所有 pending 阶段。
- 使用固定样本验证 v4 `SAVE_SaveItem` payload 在 normalize、JSON 往返、合并和恢复路径中不丢失。
- 覆盖语义损坏 payload 的隔离、只读诊断和禁止静默覆盖；验证不兼容 APK/libgame 身份拒绝反序列化。

**P1 完成登记（2026-08-29，证据链 `404f3c3`→`06b42b7`→`f3e25f1`→`2f6340c`→`362c220`）**

- 容量派生：逆向全链闭环（`docs/system/bag.md` §5，ITEMSTATICBASE 4/8/12/16）+ `derive_capacity` 实现 + 真机端到端（equip 手包→4/中包→12、保存→重启→capacities 由 types 派生重建）
- 所有权状态机：`ownership_ledger.h` 四态转移表 + generation 句柄 + 计数审计（host 33 用例）；运行时接线属 P3
- prepare journal v1：独立 section `extensionbags.journal` + 三方对照恢复裁决（世界探针优先于 stage）+ Kotlin 存储辅助
- 游戏身份绑定：`gameIdentity`（签名摘要前缀）写入 committed state section；不匹配身份拒绝反序列化并隔离为默认态
- 完成要求对照：纯逻辑移动/合并/满包/取消/回滚/恢复可确定重放 ✓（host 356 用例）；扩展状态不含 native 指针 ✓（payload=字节序列化+category/count 缓存）；sidecar 写失败不丢可恢复事务 ✓（journal 独立 section 复用容器原子写/CRC/last-good）
- 真机 v0.6.17 附带修复两缺陷并登记 backlog：跨袋同槽移动误拒、锁内 op_ok 自死锁（模式约束进 P7 审计）

### 阶段 P2：原版 UI 只读窗口原型

**内容**

- 不开放物品移动，只验证原版 UI 窗口复用。
- 当前 overlay 只能作为原型观察对象；P2 完成必须证明正式窗口投影路径已安装并可恢复，不能以 overlay 显示或坐标命中替代。
- 验证逻辑扩展袋切换到原版窗口后，原版按钮、格子、容量、物品绘制和退出恢复。
- 验证原版窗口的 direct/GOT 当前袋字段、容量字段、物品指针和控件树快照。

**完成要求**

- 扩展视图显示期间原版物品不被永久改变。
- 进入、切换、退出、F3、回主菜单和切档后，原版背包指针和容量完全恢复。
- 扩展窗口只显示有效容量范围，容量不足的槽不可选中。
- 原版索引 `5` 不得被选作投影窗口或任何扩展操作的源/目标。

**测试目标**

- 唯一真机 `192.168.3.54` 通过 API 重复打开/切换/退出扩展视图至少10轮，无崩溃、无原版物品变化、无错误选中。
- 通过控件树、描述菜单和快照比对确认进入、切换、退出、切档及异常恢复；每轮记录 projection 状态、对象所有权和索引 `5` sentinel。

**阻断条件**

- 出现原版物品丢失、重复、悬空指针、容量错误、选中错位或崩溃，立即停止后续阶段。

### 阶段 P3：原版控件与扩展窗口的正式接入

**内容**

- 使用原版 `ControlObject`/`ControlButton`/`ControlItem` 控件树，不采用长期独立 overlay 作为最终输入模型。
- 复用原版背包按钮、格子绘制、点击动画、选中效果和触摸分发。
- 建立逻辑袋编号到原版 UI 当前窗口的统一映射。
- 处理扩展袋二次点击、信息菜单、解除按钮和切换音效。

**完成要求**

- 原版按钮样式、按下态、选中态和点击反馈与原版一致。
- 二次点击扩展袋进入该袋信息，不错误返回原版袋。
- 扩展物品点击只命中对应扩展逻辑槽，不能命中原版投影槽。
- 不同容量的无效槽不可点击、不可拖动、不可进入信息状态。
- 原版第 6 袋（索引 `5`）不得成为扩展入口、映射窗口或扩展移动目标。

**测试目标**

- 逐项测试按钮正常/按下/选中/禁用状态。
- 测试已装备背包物品所派生容量的入口、信息和无效槽行为；临时值 `16/8/4/0/0` 不构成最终验收。
- 对照原版截图、事件日志和控件状态进行比对。

**P3 当前实现与未验收边界（v1.17 对账）**

- **Confirmed facts**：扩展标签为 `ControlItem`、真实袋物品对象 `data[0]`、袋容器同父挂载、父链绝对位置换算；容器子控件不足 6 时延迟重试。`E-2026-08-30-01` 标签挂载动画期稳定；`E-2026-08-31-01` 装备（拖放至扩展标签 + 装备按钮原版袋位 1..4 全满溢出到扩展空位）与解除（信息页按钮 + HTTP，按类型重建背包物品回流原版库存、接收袋切换、persist 失败回插）经用户真机确认，保存→强杀→读档 `types`/物品两次往返一致；`E-2026-08-31-02` 选中反馈确认无问题；`drawdiag` 诊断日志已移除。
- **Not completed**（v1.18 关闭）：弹窗 6 边界已由用户真机确认通过（`E-2026-08-31-03`）；扩展拖动（0x81）由"转后续"改为并入 P5；G-9 `GetBagSlotIndex` 语义仍待定。原版与扩展物品数据不一致属预期——物品真实所有权流动（P4）尚未开始。
- **Do not repeat**：不得以代码注释“天然不漂移”、P2 压测或 host 测试宣称 P3 已通过；不得把“透传原版函数”当作用户确认过的弹窗 6。
- **Next exact action**：用户真机点击装备按钮（原版+扩展全满场景）确认弹窗 6；登记结果后 P3 本阶段收尾，扩展拖动进入后续阶段。
- **Required evidence**：全满弹窗 6 的用户结论 + 弹窗出现时扩展状态不变；不改变其他 P3 变量。

### 阶段 P4：原版物品对象与逻辑背包事务桥接

**内容**

- 原版物品进入扩展袋时调用 `SAVE_SaveItem` 保存完整 payload。
- 扩展物品进入原版袋时调用 `SAVE_LoadItem` 重建对象。
- 只在操作窗口内使用 native 对象，完成后明确释放或转移所有权。
- 扩展→扩展只修改逻辑背包，不调用原版 `INVEN_MoveItem`。
- 建立统一事务状态：准备、pending 已写、逻辑状态已写、原版状态已写、提交完成。
- 事务状态必须与 §8.5 的 prepare journal 顺序一致，不能把仅存在内存的 pending 记为跨进程恢复能力。
- 原版索引 `5` 不得出现在任一事务的源、目标、恢复或回滚路径。

**完成要求**

- 每个物品在任意时刻只有一个逻辑所有者。
- 任意失败路径都能恢复源物品、目标物品、sidecar 状态和 UI 状态。
- payload 能保留装备属性、附魔、宝石孔、稀有度和堆叠数量等完整信息。

**测试目标**

- 覆盖空槽、目标满、可合并、不可合并、数量达到上限、非法目标、取消和插入失败。
- 使用 native 对象生命周期日志或调试计数检查重复释放和泄漏。
- host 测试只覆盖纯事务判定；native 对象、控件和真实库存必须在真机验证。

**P4.1/P4.2 验收进度（v1.19 登记）**

- 已通过（真机 API，`E-2026-08-31-04`/`E-2026-08-31-05`）：空槽双向字节保真（×18 药水与背包（小）各一完整往返，payload 逐字节一致，×18 三次序列化比对一致）、合并身份规则（不同 payload 不合并、独立落位）、非法目标（任务袋 5 双向 `task bag excluded`）。
- 行为登记：跨域移动为整堆语义（ext→orig 不接受部分数量，`count` 参数不参与）——**暂时实现**（用户 2026-08-31 决策：引擎路径可实现按数量移动，暂不实施，后续补齐部分数量语义）；orig→ext 成功后安装扩展视图并按 P2 锁移动，视图退出为真实触摸边界；`/api/debug/extension_bag/*` 为开发期视图/诊断端点，不构成退出页面的正式操作面。
- 验收路径未就绪（转 P5 路径矩阵，host 测试已覆盖纯逻辑）：目标满、数量上限、取消/提交前失败注入、插入失败（`INVEN_SaveItemOnEmpty` 故障）——当前 API 无法表达故障注入；P4.2 host 测试（`test_host.cpp`）已覆盖全部失败路径断言。

### 阶段 P5：三方向移动与原版库存逻辑接入

严格按以下顺序独立完成，不合并验收：

1. 扩展→扩展：逻辑状态移动、堆叠和回滚。
2. 原版→扩展：序列化、扩展写入、原版源物品删除。
3. 扩展→原版：payload 重建、原版入库、扩展源物品清理。

**扩展拖动建立协议（0x81）**（用户 2026-08-31 决策，并入 P5）：作为独立路径并行入轨，与上述三方向路径分别独立真机验收；扩展拖动协议涉及扩展→扩展、扩展→原版两条路径（建立后复用原版落点判定），原版→扩展走原版拖动可用、不在此范围。建立协议须覆盖：扩展物品拖出（按下命中扩展槽、拖动期 hit-test、放置到扩展/原版目标槽）、放置失败回滚、取消回滚、跨视图拖动；其 payload/事务/所有权/准备日志须复用 P4 事务契约与 §8.5 prepare journal，不得独立于 §8.5 单独落盘。

**完成要求**

- 原版 UI 的点击、拖动、放置和取消行为保持一致。
- 原版容量、堆叠和物品所有权规则保持一致。
- 原版第 6 袋完全排除在三方向移动与扩展拖动之外。
- 失败不会出现重复物品、丢失物品、源槽残留或目标槽错误。
- 每条路径都有明确日志：源、目标、payload 标识、数量、事务阶段和结果；扩展拖动还需记录建立协议版本、拖动期 hit-test 命中点、放置结果（成功/失败/取消）和回滚阶段。
- 扩展拖动协议版本号、命中扩展/原版目标槽的 hit-test 行为、放置失败回滚与取消回滚必须与原版拖动保持一致。

**测试目标**

- 每条路径分别测试有效移动、目标满、不可合并、取消、非法槽和中断恢复。
- 增加 `SAVE_LoadItem` 失败、原版插入失败、原版保存失败、sidecar 写入失败和进程中断的故障注入；扩展→扩展、原版→扩展、扩展→原版分别记录重复 payload 和所有权转移结果。
- 扩展拖动路径独立覆盖：拖出成功/失败、放置到扩展槽、放置到原版槽、放置到非法槽、放置到任务袋（索引 5 必须拒绝）、拖动期取消、`SAVE_LoadItem` 失败回滚、原版保存失败回滚、sidecar 写入失败回滚、进程中断回滚（journal 阶段 0/1/2 分别覆盖）。
- 通过后才允许进入下一条路径；任一路径失败不得同时修改其他路径。

### 阶段 P6：全局库存来源与自动装备接入

**内容**

- 审计所有生成物品进入背包的路径：掉落、奖励、开箱、合成、商店购买、脱装备和物品使用结果。
- 统一空槽查找顺序：原版背包优先，原版满后进入扩展背包。
- 全部背包满时复用原版“背包已满”提示。
- 自动入库和自动装备只搜索原版索引 `0..4` 与已装备的扩展逻辑袋，绝不向原版索引 `5` 投递。
- 明确扩展物品能否被使用、装备、出售、强化和镶嵌；不能支持的路径必须阻断并返回明确错误。

**完成要求**

- 任何新增物品都不会绕过逻辑背包映射而丢失。
- 自动装备和普通入库遵循相同的容量、堆叠和所有权规则。

**测试目标**

- 原版有空槽、原版满/扩展有空槽、全部满三种场景。
- 覆盖自动装备、掉落拾取、奖励入库和脱装备入库。

### 阶段 P7：保存、切档、重启与异常恢复

**内容**

- 固化保存顺序：写入 prepare journal → 恢复临时 UI 窗口 → 调用原版保存 → 原版成功后提交 sidecar 新状态 → sidecar 成功后清理 pending/journal；原版保存失败或中途崩溃均按 transactionId/generation 重放或回滚。
- 覆盖手动保存、自动保存、进入存档、切换存档、回主菜单、退出背包和进程重启。
- 审计并收口全部原版保存调用点和自动保存调用点，不得存在绕过协调器的直接保存路径；明确存档面板进入是否会写入当前进度。
- 注入原版保存失败、sidecar 写失败和两者之间进程中断。

**完成要求**

- 原版保存失败时扩展状态不被标记为已提交。
- sidecar 写失败时保留可恢复 pending，不静默清除。
- 切档不会串用其他槽的扩展物品。
- 未保存的内存改动在回主菜单、切档或重启后丢弃。

**测试目标**

- 唯一真机 `192.168.3.54` 通过 API 验证保存成功、保存失败、sidecar 损坏、last-good 恢复和 pending 恢复。
- 每次异常测试都保存原版 inventory、sidecar generation/CRC、pending 状态和日志。
- 每次异常测试还必须保存 prepare journal、transactionId、原版索引 `5` sentinel、控件树/描述菜单快照和对象分配/释放计数。
- 最终持久化实现只接受 `extensionbags.items` v4；旧 section、旧版本转换和自动迁移代码必须在 P8 前移除。

### 阶段 P8：最终回归与发布验收

**内容**

- 完成第4节 1–11 项严格串行验收。
- 对照原版背包完成 UI、输入、物品、存档和崩溃回归。
- 冻结最终源码快照和发布 APK，不再使用临时原型版本作为验收对象。

**完成要求**

- 唯一真机 `192.168.3.54` 通过全部适用 API 验收项；只有 API 无法覆盖的触摸项才由用户确认。
- 每项都有完整证据：操作前状态、操作、操作后状态、日志、APK/设备身份和用户结论。
- host 测试、native 构建、符号检查、APK 身份检查和崩溃日志均通过。
- 最终源码不含旧 section、v2/v3 格式转换或自动迁移代码。
- 最终发布构建不得保留未登记、可绕过事务协议的 `DebugController` 扩展背包写入入口。
- 必须完成索引 `5` sentinel 比对、重复 payload 恢复、`SAVE_LoadItem`/原版插入/原版保存/sidecar 写入故障注入、控件树与描述菜单快照恢复，以及全部自动保存调用点统一协调的证明。
- 任一项失败，整体保持 `NOT_ACCEPTED` 状态。

## 10. 最终成果验收矩阵

最终成果必须同时满足以下五类结果：

| 类别 | 必须达到的结果 |
|---|---|
| 原版兼容 | 原版6袋、原版物品、原版存档格式和原版功能不被破坏；第 6 袋（索引 `5`）始终仅由原版处理 |
| 扩展功能 | 5个扩展袋、容量由已装备背包物品派生、切换、信息、选中、拖动和三向移动可用；临时容量 `16/8/4/0/0` 不作为最终证据 |
| 逻辑一致性 | 容量、堆叠、装备、使用、入库、满包提示和失败回滚与原版一致 |
| 持久化 | 原版保存成功后 sidecar 正确提交；切档、重启和 pending 恢复正确 |
| 发布质量 | 唯一真机 `192.168.3.54` 通过全部适用验收，未发生崩溃、丢物、复制物品、悬空指针或跨存档污染 |

## 11. 决策记录（ADR）

### ADR-001：复用原版窗口而非扩大原版背包

- **日期**：2026-08-26
- **决策**：不扩大原版背包数量，不修改原版 `save*.dat` 结构；扩展袋通过逻辑映射接入原版窗口。
- **理由**：保持原版 UI、物品生命周期和存档兼容边界。
- **状态**：生效；除非用户明确批准架构变更，不得改写。

### ADR-002：扩展物品只持久化序列化 payload

- **日期**：2026-08-26
- **决策**：不把 native 物品指针作为持久化数据；原版保存成功后提交 sidecar。
- **理由**：native 指针跨进程无效，且必须区分原版保存与 sidecar 提交结果。
- **状态**：生效。

### ADR-003：真机结论优先于静态或 host 结果

- **日期**：2026-08-26
- **决策**：host 测试、静态代码路径或单次 API 操作不能替代唯一真机的完整最终验收。
- **理由**：控件、native 对象生命周期和存档交互只能在唯一目标运行环境完成闭环验证。
- **状态**：生效。

### ADR-004：容量测试固定值不进入最终架构

- **日期**：2026-08-26
- **决策**：`kFixedCapacities = 16/8/4/0/0` 仅用于原型测试环境；最终实现必须先装备扩展背包物品，再从其属性派生容量并驱动全部 UI 与移动路径。
- **理由**：当前固定数组绕过真实装备状态，且不能代表最终容量、入口可用性或无效槽语义。
- **状态**：已满足（2026-08-28）——`kFixedCapacities` 已删除，运行时容量统一由 `derive_capacity(BagType)` 派生（ITEMSTATICBASE 镜像 4/8/12/16），见 §8.4 容量派生契约 v1。

### ADR-005：最终 sidecar 不兼容旧格式

- **日期**：2026-08-26
- **决策**：最终交付物只读取和写入 `extensionbags.items` v4；不包含旧 section、v2/v3 转换、`migrateLegacyState` 或自动回写逻辑。
- **理由**：旧格式转换仅服务开发测试；正式数据模型以完整 `SAVE_SaveItem` payload 为唯一物品表示。
- **状态**：生效；当前原型遗留代码待删除。

### ADR-006：原版第 6 袋由扩展功能完全隔离

- **日期**：2026-08-26
- **决策**：原版索引 `5`（第 6 袋、任务物品专用袋）只由原版处理。扩展逻辑只能使用原版索引 `0..4`，并在入口、映射、三方向移动、自动入库、自动装备、事务恢复和 UI 命中中主动排除索引 `5`。
- **理由**：避免扩展物品写入任务物品专用袋，或从该袋取走原版任务物品，产生不符合原版的行为。
- **状态**：生效；当前源码仅覆盖原版→扩展源袋的部分拦截，P8 前必须全路径完成。

### ADR-007：保存采用 prepare journal 协调

- **日期**：2026-08-27
- **决策**：所有显式保存和自动保存均先独立落盘 prepare journal，再调用原版保存；原版成功后提交 sidecar 新状态，最后清理 journal。已提交状态与未提交事务必须分开表达。
- **理由**：单纯“原版保存成功后写 sidecar”会留下进程中断窗口，内存 pending 无法支持跨进程恢复；统一协调器还可阻止直接保存调用造成状态分叉。
- **状态**：生效；P7 前必须收口全部保存调用点并完成故障注入。

### ADR-008：扩展交互操作面 = 原版背包 API 并入（v2，用户 2026-08-28 决策）

- **日期**：2026-08-27（初版）；2026-08-28 修订（v2）
- **决策**：扩展背包不设独立 API 域。数据与移动操作面 = 原版背包 API（`/api/item/inventory*` 原生支持扩展逻辑袋 6..10：读取并入、`move_item` 按域分派三方向事务）。进入/退出视图、切换逻辑袋、点击/选中/信息为开发期视图控制能力，归入 `/api/debug/extension_bag/*`。开发期 debug 注入端点（equip/item）不构成发布操作面，最终构建必须删除或隔离。
- **理由**：用户明确要求“扩展背包不要有 API，直接在原版背包操作、获取中添加支持”；独立域会把同一背包数据模型割裂成两套端点，增加消费方与验收成本。v1（独立 `/api/extension_bag/*` 六端点，v0.6.15）已真机验证后按本决策迁移。
- **状态**：生效（v2）；P1+ 数据/移动验收一律使用原版背包 API，视图类验收按 §4.1 修订条款执行。

### ADR-009：存档面板保存语义必须显式决定

- **日期**：2026-08-27
- **决策**：存档面板进入是否写入当前进度不得由 hook 隐式决定；必须明确选择“进入即保存”或保持原版“打开面板只读旧档”，并将选择、影响和验收证据登记在控制面。
- **理由**：隐式改变原版存档面板语义会导致用户打开旧档面板时当前进度被意外写入。
- **状态**：待产品/实现决策；决策前 P7 不得通过。

## 12. 风险登记

| 风险 | 触发条件 | 处理规则 | 状态 |
|---|---|---|---|
| 原版物品丢失/重复/悬空指针 | 映射退出、失败回滚或重建对象异常 | 立即停止当前及后续阶段，保留日志和证据 | 开放 |
| 原版保存成功但 sidecar 未提交 | sidecar 写入失败或进程中断 | 保留 pending/journal，不得标记已保存 | 开放 |
| 跨存档污染 | 切档、重启后读取错误槽位 | 记录槽位、generation、CRC，回退 P7 | 开放 |
| 固定容量误作正式能力 | 固定数组未替换、装备状态未驱动容量 | 不得进入 P8；验证装备派生容量、未装备限制和全部 UI/移动路径 | 开放 |
| 任务袋污染 | 扩展路径读取或写入原版索引 `5` | 立即停止相关验收，记录源/目标和日志，回退对应阶段 | 开放 |
| 保存调用未收口 | 任一显式或自动保存绕过协调器 | 立即将 P7 回退，登记调用点、原版与 sidecar generation 差异并补齐统一入口 | 开放 |
| WAL 崩溃窗口 | prepare 未落盘、提交间中断或 journal 被误清理 | 注入故障并验证幂等重放/回滚；失败时不得进入 P8 | 开放 |
| 装备背包移除后超容 | 解除、替换或出售装备物品导致有效容量下降 | 按已登记规则阻断或处理溢出，保存前后不得丢物；规则未定前禁止容量验收 | 开放 |
| 扩展 API 验收缺口 | API 未登记或只能调用 debug 注入端点 | 相关阶段标记“验收路径未就绪”，不得用伪 API 结论通过 | 开放 |
| payload/身份不兼容 | payload 语义损坏或目标 APK/libgame 不匹配 | 隔离记录、只读诊断并拒绝反序列化；不得 normalize 或静默回写 | 开放 |
| 多代理状态漂移 | 未读当前控制状态或覆盖失败记录 | 先读 §0，使用唯一证据 ID，禁止静默覆盖 | 持续控制 |

## 13. 变更日志

| 版本 | 日期 | 变更摘要 | 责任方 |
|---|---|---|---|
| v1.22 | 2026-08-31 | 登记 `E-2026-08-31-07`（P4.3 视图借用配对真机轮：用户触摸 35 次安装/归还全配对，restore 审计 borrows=0、balanced=1、快速切换计数零增长）。P4.3 真机退出条件全部覆盖，P4.3 关闭；P4 剩余 P4.4 唯一事务入口、P4.5 失败隔离、P4.6 验证移交 | 当前执行代理 |
| v1.21 | 2026-08-31 | mode/selected 生命周期约束（用户确认实施）：① orig→ext 移动补删第二处 mode 写点（1366 行区，首处已随保持视图决策移除）；② ext→orig 移动仅扩展视图安装（拖放上下文）时写 mode/selected，API 路径不改写；③ 读档边界归零无 pending 的 kModule 残留（`kExitingModule`/pending 交恢复流程）。真机回归：读档 original/-1、双方向移动后保持 original/-1、任务袋拒绝正常、payload 一致（APK `f2158ee5…`）；归零分支因 sidecar 已净未触发（防御性保留） | 当前执行代理 |
| v1.20 | 2026-08-31 | 登记 `E-2026-08-31-06`（P4.3 审计循环：3 轮 API 驱动跨域/ext→ext/失败移动，审计 balanced 全 1、borrows=0、保存重启读档字节级一致；视图借用配对真机轮待补）；用户决策落地：orig→ext 后保持当前视图（不切 mode、不装投影，RefreshItemArea 清残影，P2 锁不再因 API 移动触发）；P4.3 实现完成并经 Oracle 复核修复 1 项 High（equip remove 验证失败分支不得释放 INVEN 持有对象）后 Fix approved；发现登记：sidecar `mode/selected` 残留可跨读档恢复（本次经完整保存周期清除；加载边界归零待处理）、未保存移动跨重启按防复制回退原位（P7 边界内预期行为） | 当前执行代理 |
| v1.19 | 2026-08-31 | P4 推进：登记 `E-2026-08-31-04`（P4.2 空槽往返 payload 字节保真 + 合并身份规则，双物品完整往返 API 驱动）与 `E-2026-08-31-05`（任务袋 5 双向拒绝）；阶段指针更新为 P4 进行中（P4.1/P4.2 完成、P4.3–P4.5 未实施）；行为登记：跨域整堆语义、orig→ext 后 P2 视图锁为设计行为、debug 视图端点不构成正式操作面；目标满/数量上限/取消/插入失败真机注入登记「验收路径未就绪」转 P5，host 已覆盖 | 当前执行代理 |
| v1.18 | 2026-08-31 | 登记 `E-2026-08-31-03`（P3 收尾最后一项通过：装备时原版+扩展全满弹窗 6 用户点击确认）；P3 整体推进为 `已通过`，当前阶段指针切换至 P4 原版物品对象与逻辑背包事务桥接（未开始）；扩展拖动建立协议（0x81）由"转后续"改为并入 P5 范围，作为独立维度与三方向路径并行验收，须复用 P4 事务契约与 §8.5 prepare journal | 当前执行代理 |
| v1.17 | 2026-08-31 | 登记 `E-2026-08-31-01`（扩展袋装备/解除正式路径真机通过：拖放+按钮双入口、物品回流、保存往返一致）与 `E-2026-08-31-02`（选中反馈无问题）；移除 `drawdiag` 诊断日志；扩展拖动建立协议（0x81）由用户明确转后续；装备全满弹窗 6 边界标记为透传代码保证、待用户点击确认；P3 阶段指针推进至收尾 | 当前执行代理 |
| v1.16 | 2026-08-30 | 登记 `E-2026-08-30-01`：用户真机手动确认扩展标签在面板开合动画期无移动/偏移；P3 仍未整体通过，下一单项改为原版信息页与解除语义验收 | 当前执行代理 |
| v1.15 | 2026-08-30 | P3 控制面对账：顶部和阶段指针从过期 P1 修正为 P3 进行中；登记当前 `ControlItem` 袋容器挂载、二次点击原版 `MakeDesc`、G-8/G-9 与 drawdiag 的实现/未验收边界；修正已删除的固定容量和“所有权账本待接线”过期表述；标记 P1/P2 引用证据块待补，未新增或伪造 P3 通过证据 | 当前执行代理 |
| v1.14 | 2026-08-29 | P2 通过（E-2026-08-29-01）：正式窗口投影（install 写原版窗口数据源/restore 写回快照，35:35 配对、overlay 绘制 0 次）；10 轮开/切/退压测全 PASS（26 原版物品零变化、bag5 sentinel=0）；回主菜单恢复验证通过；移动门禁（视图打开期间拒绝移动，P2 不开放移动语义） | 当前执行代理 |
| v1.13 | 2026-08-29 | P1 收尾通过：游戏身份绑定（gameIdentity 签名摘要）+ C++ 前向兼容用例；host 测试 356 项；P1 阶段标记 ✅（E-2026-08-28-03） | 当前执行代理 |
| v1.12 | 2026-08-28 | v0.6.17/175 真机验证通过（E-2026-08-28-03）：派生容量端到端（equip 手包→4/中包→12/未装备→0；保存→重启→capacities 由 types 派生重建）；修复两缺陷——跨袋同槽移动误拒（预存在 `requested_dst_slot == src_slot` 条件）、world 下 debug equip/item 自死锁（op_ok 锁内触发 frame 刷新抢同锁，模式登记 backlog） | 当前执行代理 |
| v1.11 | 2026-08-28 | P1 prepare journal 格式 v1 落地：sidecar 独立 section `extensionbags.journal` + 三方对照恢复裁决（世界探针优先于 stage）；C++ JournalRecord/序列化/裁决纯函数 + Kotlin ExtensionBagJournal 存储辅助；host 测试 319 项通过 | 当前执行代理 |
| v1.10 | 2026-08-28 | P1 容量派生契约 v1 落地：ITEMSTATICBASE 镜像 `derive_capacity(BagType)`（4/8/12/16）替换 kFixedCapacities 全部运行时读取；host 测试 284 项通过；ADR-004 标记已满足 | 当前执行代理 |
| v1.9 | 2026-08-28 | P1 启动：容量派生逆向第一阶段完成（`docs/system/bag.md`）——袋对象=已装备背包物品对象、容量=物品+0x10 bit0..24、存档按容量循环编码袋内物品、真机实测 16/8/8/8/4；ADR-008 v2 已生效（原版背包 API 并入） | 当前执行代理 |
| v1.8 | 2026-08-28 | 用户决策：取消独立扩展背包域，原版背包 API（bag 6..10 并入）为正式操作面，视图类端点迁入 `/api/debug/extension_bag/*`；ADR-008 修订为 v2；§4.1 操作面条款重写；v0.6.15 的 `/api/extension_bag/*` 移除 | 当前执行代理 |
| v1.7 | 2026-08-28 | 登记 E-2026-08-28-02：`/api/extension_bag/*` 六端点登记至 api-reference §十并真机验证（v0.6.15）；ADR-008 正式操作面就绪，P1 验收路径解除阻断；登记 `INVEN_RemoveItemDirect` 返回值不可信发现 | 当前执行代理 |
| v1.6 | 2026-08-28 | 登记 E-2026-08-28-01：P0 原版背包 smoke 在唯一真机通过（v0.6.14，身份链完整，全程 API 无用户触摸）；P0 闸门关闭，P1–P8 按 ADR-008 保持“验收路径未就绪” | 当前执行代理 |
| v1.5 | 2026-08-27 | 登记真机2启动弹窗前置：固定 `ANDROID_SERIAL=192.168.3.54:5555` 执行 `(420,280)` 脚本点击；除该触摸例外外，启动后续与验收继续由代理使用 API 完成 | 当前执行代理 |
| v1.4 | 2026-08-27 | 登记 P0 冷启动纯度、原版移动保存和 API 进档阻断；保持 `NOT_ACCEPTED` | 当前执行代理 |
| v1.3 | 2026-08-27 | 根据三份审查补充分层事实、overlay/投影边界、扩展编号换算、对象所有权、装备派生容量、prepare journal、API 验收前置、P0 纯度、索引 5 sentinel、故障注入、ADR-007 至 ADR-009 和新增风险 | 当前执行代理 |
| v1.2 | 2026-08-26 | 收束扩展背包验收至唯一真机 `192.168.3.54` 的 API 链路；将固定容量、旧格式转换和原版第 6 袋全路径隔离登记为未完成的最终交付约束；新增 ADR-004 至 ADR-006 | 当前执行代理 |
| v1.1 | 2026-08-26 | 登记 E-2026-08-26-01：本地 build 与当时标记为真机 2 的安装包哈希一致，启动/API/文件日志已关联；P0 保持阻断。双机 smoke 的旧策略已由 v1.2 的唯一真机 API 策略取代 | 当前执行代理 |
| v1.0 | 2026-08-26 | 从一次性交接文档升级为长期控制面；补充权威边界、状态、证据、代理恢复、ADR 和风险登记 | 当前执行代理 |

## 14. 必须保留的设计决策摘要

- 不扩大原版背包数量，不修改原版 `save*.dat` 的结构。
- 不把扩展物品 native 指针作为持久化数据。
- 不把“自绘 overlay 能显示”视为原版 UI/逻辑复用完成。
- 不把“原版保存成功”视为 sidecar 已成功提交；两者必须分别记录结果。
- 不在未验证所有权和生命周期前开放三方向移动。
- 固定容量 `16/8/4/0/0` 仅用于原型测试；最终容量必须由已装备背包物品派生。
- 最终交付物不保留旧 section、v2/v3 格式转换或自动迁移代码。
- 原版第 6 袋（索引 `5`）不参与任何扩展映射、移动、自动入库或自动装备。
- 不以 host 测试、静态代码路径或单次 API 结果替代唯一真机的完整最终验收。

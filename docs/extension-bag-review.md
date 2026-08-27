# gpt-5.6-terra

审查对象：`docs/extension-bag-control-plane.md`。本结论基于静态阅读控制面、sidecar 契约与当前实现源码；未修改实现、未访问真机、未调用设备 API。

## 结论

正式目标架构可行：不扩充原版背包数量，以逻辑背包映射复用原版窗口，并在原版保存成功后提交模块 sidecar，能够保持原版 `save*.dat` 兼容。但当前实现仍是独立 overlay 与临时 native 对象缓存，尚未达到文档所定义的正式投影、控件复用、跨存储恢复和任务袋隔离要求，因此整体必须维持 `NOT_ACCEPTED`，不得进入功能验收。

## 阻断问题

1. **运行时投影尚未实现。** `game_ui_virtbag.cpp:1717-1720` 明确当前不将扩展物品投影到 `g_inven`；`virtual_bag_sync_projected_*()` 在 2268-2281 均返回 `false`。绘制和命中仍依赖 overlay 与绝对坐标。控制面 §2 对当前实现的“投影”描述不准确，必须明确区分当前原型和 P2/P3 的正式目标。
2. **保存协议存在不可恢复窗口。** `persist_state_locked()` 仅在 `g_explicit_save_in_progress` 时写入 sidecar；普通移动中的 `pending` 与 `g_unsaved_cross_moves` 仅在内存中。`virtual_bag_save_game()` 先执行原版 `fn_save()`，再写 sidecar；原版成功而 sidecar 失败或进程中断时，原版与扩展状态会分叉。
3. **原版任务袋索引 5 未全路径隔离。** 原版→扩展已拒绝源袋 5，但扩展→原版、拖拽目标命中、退出显示与恢复路径仍接受 5。不能依赖原版的六袋通用遍历自动规避任务袋。
4. **容量尚未由真实装备物品派生。** `normalize()`、绘制和触摸仍直接读取 `kFixedCapacities`；`types` 仅为元数据，未保存装备物品的完整身份与 payload，也未定义替换、解除或满包状态下的规则。
5. **native 对象所有权不完整。** 当前缓存对象由模块释放；正式投影后，若对象被原版库存或控件引用，按当前释放规则会产生悬空指针或重复释放。`SAVE_LoadItem` 失败后按 category/count 创建近似物品的回退路径也会丢失物品属性，最终实现应拒绝并隔离该记录。

## 必须补足的设计

- 定义唯一的容量派生函数，以已装备背包物品为输入，并统一驱动入口、绘制、命中、拖动、移动校验、自动入库和满包提示。
- 定义对象所有权状态机：`module-owned`、`borrowed-for-view`、`inventory-owned` 与释放/转移条件；用分配和释放计数验证切换、取消、失败、切档和重启路径。
- 设计持久化 prepare journal：至少包含 `slotId`、`generation`、`transactionId`、源/目标身份及提交阶段。journal 与已提交扩展状态必须分开表达，并提供幂等重放或回滚规则；不能只按 payload 相等判断事务完成。
- 建立统一的原版袋许可判断，所有映射、移动、恢复、自动入库、自动装备和 API 路径均复用，且永久拒绝索引 5。
- 对 payload 的语义损坏建立隔离和只读诊断策略，避免 `normalize()` 或下一次保存无声覆盖有效 sidecar；sidecar 同时应绑定目标游戏 APK/libgame 身份，拒绝在不兼容版本上反序列化。

## 文档修订建议

1. 将“当前原型事实”和“最终目标”分成独立小节，并将当前 overlay/坐标命中明确标为未通过 P2/P3。
2. 将 §8.4/P7 的“原版保存成功后写 sidecar”扩展为完整故障模型，明确 prepare、原版保存、sidecar 提交和恢复的状态转换。
3. 规定 P0 的“关闭扩展功能”是否还要求不注入 hook；当前配置默认启用扩展背包，单纯关闭功能标志不等于原版未受注入影响。
4. 消除控制面“API 优先验收”与 `docs/backlog.md`“用户触摸验收”的冲突，并说明权威优先级。
5. 将最终验收补充为：索引 5 sentinel 比对、重复 payload 事务恢复、`SAVE_LoadItem`/原版插入/原版保存/sidecar 写入故障注入、控件树和描述菜单快照恢复，以及全部自动保存调用点的统一协调证明。

## 当前实现还需移除的原型遗留

最终交付前仍需删除 `virtualbags.items` 回退、v2/v3 分支与自动迁移；同时应删除或严格隔离 `DebugController` 的扩展背包测试写入接口，避免其成为发布构建的未受控状态注入入口。

---

# deepseek-v4-pro

审查对象：`docs/extension-bag-control-plane.md`。本结论基于静态阅读控制面、sidecar 契约与当前实现源码；未修改任何文件、未访问真机、未调用设备 API。

## 结论

方案可行，且是这条路线里风险最低的唯一解；但作为控制面文档，它把若干"应证未证"的假设写成了既定事实，并与当前源码存在多处可证伪的不一致。整体必须维持 `NOT_ACCEPTED`。

## 一、方案可行性：可行

1. **"不扩原版背包 + 复用原版窗口 + 逻辑映射 + sidecar" 是这类固定 UI 单机游戏 mod 的唯一低风险路线。** `game_symbols.h:79` 确认 `INVEN_pItem = 6袋×0x80`（16 槽×8B 指针），`INVEN_GetBagSize`（`G_BAG_TABLE_VMA`）确认"袋容量由袋对象字段决定"。扩袋数或改 `save*.dat` 都会破坏原版存档兼容，ADR-001 的判断成立。
2. **关键逆向已就位。** `SAVE_SaveItem/SAVE_LoadItem/ITEMPOOL_Free/INVEN_SaveItemOnEmpty/INVEN_MoveItem` 等符号均已登记（`game_symbols.h:241-243,353-419`）；三条移动路径（`game_ui_virtbag.cpp:794/960/1109`）已实现且带事务/回滚逻辑；sidecar 容器（`ModuleSaveStore`，CRC + last-good + AtomicFile）已落地。
3. **文档的治理设计（证据 ID、串行闸门、ADR、风险登记、真机优先）本身质量很高**，能有效防止"host 测试冒充真机验收"和"多代理状态漂移"。

## 二、文档与源码不一致（建议修正）

1. **§2.3 把"投影到原版背包槽区"写成当前模型，但当前源码是独立 overlay。** 源码 draw 注释明确写着 "never projects extension objects into g_inven……independent overlay"（`game_ui_virtbag.cpp:1717-1719`、`1462-1464`），且投影同步接口 `virtual_bag_sync_projected_slot/item_control/bag`（`:2268-2281`）**全是空桩返回 `false`**。`restore_module_view_locked` 那套快照/恢复代码被注释标为"stale view……restore once"的遗留。§2.3 应把"投影"降级为**最终目标**、把"独立 overlay"标为**当前原型**，与 §8.3/P3/§14 对齐，否则排查时会按投影模型去读代码而误判。

2. **`persist_state_locked` 的落盘语义与文档承诺不符——存在"循环保证"缺口。** `persist_state_locked(bool force)` 把 `force` 用 `(void)force` 丢弃（`:1280`），且 `!g_explicit_save_in_progress` 时直接 `return true`（`:1281`）。`g_explicit_save_in_progress` 只在 `virtual_bag_save_game()` 内被置位（`:2250-2261`）。因此 move 函数里那十几次 `persist_state_locked(true)`（`:901/906/946/1012/1021/1052` 等）在非存档期间**全是空操作**，sidecar 只在存档那一刻落盘。由此产生一个文档未点破的矛盾：§8.4/P7 承诺"sidecar 写失败时保留可恢复 pending/journal"，但 pending 只有作为 save 的一部分才会写入 sidecar——而它要保护的恰恰是"原版 save 成功、sidecar 写失败/进程中断"的场景，此时 pending **尚未落盘**，重启后无从恢复。需要二选一：(a) 把 pending 独立于 save 单独 journal；(b) 明确该场景只能靠"重启后比对原版库存 vs sidecar"来探测失配，并给出探测算法。

## 三、文档缺口（需补齐）

3. **"索引 5 = 任务物品专用袋"是全篇隔离策略（ADR-006）的基石，却无任何逆向证据。** 全文找不到支撑该断言的反汇编地址/符号/实测记录，源码唯一体现是 `move_original_to_extension_locked` 里 `if (src_bag == 5)` 的注释（`:814-816`），属于同语反复。这是**可证伪的关键假设**——如果 5 号袋其实不是任务袋，整条隔离策略就指向了错误目标。应补一条 P0 级证据项：登记 `INVEN_pItem` 索引 5 的专用语义来源（如 `INVEN_DrawInvenBag`/切袋逻辑对 5 的特殊分支）。

4. **§8.3 的"扩展逻辑背包 6..10"与内部表示 0..4 冲突，未定义映射。** `virtual_bag_state.h` 里 `kBagCount=5`、`valid_index` 用 0..4，`PendingTransfer.src_bag/dst_bag` 对扩展袋也是 0..4（`recovery_action` 以 `dst_bag >= kBagCount` 判非法）；"6..10"只是文档层的逻辑编号。两者并存且无换算规则，实现/排查时极易产生 off-by-one 混淆，需明确一条编号约定。

5. **扩展背包的 UI 交互没有 API 面，§4 的"API 优先"在 P1–P8 无法成立。** 现有 HTTP 只有 debug 端点 `/api/debug/extension_bag/{status,equip,item}`（`DebugController.kt:66-83`，且 backlog P2 已登记"DebugController 未登记/未隔离"），`nativeExtensionBagTestEquip/TestItem` 只是测试注入（设 type/设 item），**不是"进扩展视图/点扩展袋 tab/拖动扩展物品/切袋"的 UI 操作**。而唯一验收真机 2（`192.168.3.54`）只能 API 操控、禁用触摸（`environment.md:129`）。所以 §4"只有 API 无法覆盖的物理触摸才由用户执行"在扩展袋交互上要么变成"全部走用户触摸"，要么必须补一套扩展袋 UI 操作 API。文档需明确选哪条，否则 P1–P8 验收无路可走。

6. **ADR-004 的"容量由已装备背包物品派生"缺逆向任务拆解。** 当前 `set_test_equipped` 只写 `types[index]`，随即被 `normalize()` 用固定数组覆盖（`virtual_bag_state.h:234-240、188-193`）；状态模型里根本没有"装备"概念，`BagType` 是裸枚举不绑定任何实际物品。文档把这个逆向目标当成"最终要求"挂 ADR-004 下，但没登记需要逆向哪个符号/结构、也没单列阶段闸门。建议在 P1 内明确"装备背包物品→容量派生链"的逆向目标与证据要求。

## 四、次要建议

7. **§5.1 证据模板偏原版背包**，缺扩展背包专用字段（当前逻辑袋、overlay/投影状态、pending 是否落盘、sidecar generation/CRC）。P1–P8 验收需要这些字段才能判定"投影安装/清理、pending 恢复"是否真的发生。

8. **§0 的阻断态缺"连接恢复→重跑 smoke"的可操作步骤。** 文档只说"连接恢复后由代理执行"，建议直接引用 `environment.md §3.1` 的 adb connect / health 轮询 / enter_slot 命令序列，避免恢复时重复探索。

## 一句话总结

架构可落地、逆向底子够、治理纪律好；但需要先修掉两个"文档≠源码"（§2.3 的投影 vs overlay、`persist_state_locked` 的空操作落盘），补上三处硬缺口（索引 5 的证据、6..10 编号映射、扩展袋 UI 的 API 面），再谈 P1–P8 的验收，否则验收顺序会卡在"无 API 可驱动、无证据可判定"上。

---

# kimi-k3

审查对象：`docs/extension-bag-control-plane.md`。本结论基于对控制面文档、`architecture.md`、`docs/module-save-store.md`、`docs/environment.md`、`docs/backlog.md`、`docs/api-reference.md` 与全部相关源码（`virtual_bag_state.h`、`game_ui_virtbag.cpp` 全量 2320 行、`game_save.cpp`、`game_patch.cpp`、`ExtensionBagUiBridge.kt`、`DebugController.kt` 等）的交叉核对；未修改任何文件、未访问真机、未调用设备 API。

## 结论

方案总体可行：序列化无损持久化、sidecar 容器、回调包装 + BL 绘制补丁三条技术支柱均已在原型中实证，ADR-001 的架构方向（不扩原版袋、复用原版窗口、sidecar 持久化）成立。但文档与代码存在 3 处事实性脱节、4 处设计未闭环、2 处验收路径缺口；建议 P0 smoke 通过后先修订控制面再进 P2，否则按现文档验收会得出错误结论。

## 一、文档与代码的事实性脱节（最高优先级）

1. **§2「投影和移动模型」描述的是不存在的实现。** 当前代码是 Scheme C 独立 overlay：扩展物品不进 `g_inven`，由模块自持对象经 `fn_item_draw_porting` 自绘（`game_ui_virtbag.cpp:1717-1720` 注释明言 "never projects extension objects into g_inven"）；`g_module_view_installed` 全文件从未置 `true`（仅 `:123/451/1603` 三处置 false），投影安装路径是死代码。§2 把投影快照模型描述为「不变的实施基线」，应改为「当前原型 = overlay + 死代码投影机构，最终目标 = §8.3 投影映射」。
2. **代码注释与实际接线矛盾未登记。** `:1922` 注释称 "Phase one is deliberately read-only. Do not route release events to the legacy projection/move transaction handlers"，但 `handle_bag_drop_release_locked` 已实际接线三向移动（`:1904、:1913`）。§2 列三条移动路径为现状是对的，但应同时登记该注释已失效，避免后续代理被注释误导而误判移动未实现。
3. **pending 恢复能力被高估。** §2 称「pending 用于跨包移动中断后的恢复」，但 `persist_state_locked` 丢弃 `force` 参数（`:1280`）且在非显式保存期间直接 `return true`（`:1281`）——pending 从不落盘，进程中断后无 pending 可恢复；`UnsavedCrossMove` journal 同为纯内存态且上限 128 条。当前恢复机制仅覆盖进程内中断，文档应如实登记「pending 当前不落盘，跨进程事务恢复未实现」。

## 二、设计未闭环（P7 前必须补齐，否则验收必失败）

1. **保存 WAL 顺序存在崩溃窗口。** P7 固化顺序「原版保存 → sidecar 提交」：若进程在 `fn_save()` 成功与 sidecar 写入之间崩溃，原→扩移动丢物品、扩→原移动复制物品。pending/journal 必须在原版保存**之前**落盘（写含 pending 的 sidecar → fn_save → 提交 sidecar → 清 pending），文档顺序应颠倒。
2. **`fn_save()` 调用点未收口。** `game_patch.cpp:403` 批量宝石合成直接调 `fn_save()` 绕过 `virtual_bag_save_game()`——任何模块内直接保存都会造成「原版已存、sidecar 未存」失步。文档应要求审计全部 `fn_save` 调用点，并逆向确认游戏自身是否存在自动保存路径（当前仅 hook 存档面板 enter，未 hook `SAVE_Save` 本体）。
3. **存档面板 enter 即自动保存是未登记的行为变更**（`game_ui_virtbag.cpp:1685-1690`）。原版打开存档面板不写盘；现实现打开即保存当前槽。用户「打开面板为读旧档」的场景会被强制写入当前进度，此决策应入 ADR 并说明影响面。
4. **装备派生容量只有要求没有设计。** ADR-004 定义「必须由装备物品派生」，但背包物品在哪装备（占槽/是否消耗）、`BagType`→容量映射表、解除/出售已装备背包且袋内物品超容时的溢出规则全部空白，P1/P3 无法据此写验收标准。
5. **第 6 袋隔离缺口坐标登记不全。** 文档只说「仅拒绝原版→扩展源索引 5」，实际另有 3 处放行袋 5 未登记：`move_extension_to_original_locked` 允许 `target_bag=5`（`:961-963`）；`original_bag_button_index` 允许命中袋 5（`:1549`）；`recovery_action`/`parse_pending_transfer` 以 `>=6` 拒绝即放行 5（`virtual_bag_state.h:172-176、418-423`）。

## 三、验收路径缺口（§4 API-first 策略当前不可执行）

1. **扩展背包没有任何操作 API。** 现有仅 3 个 debug 端点（`/api/debug/extension_bag/status|equip|item`），切袋、进入扩展视图、三向移动只能由触摸事件 0x17/0x18 驱动——§4 第 3–9 项将全部退化为用户触摸，与「可由 API 完成必须 API 执行」直接冲突。需规划扩展背包操作端点（或 native 触摸事件注入端点）并在文档登记端点清单。
2. **api-reference.md 未登记这 3 个端点**（该文件自称 debug 域仅 2 端点，`docs/api-reference.md:34`），违反 architecture §9.1-3「调试端点必须登记」；控制面以 api-reference 为 API 权威，需同步。
3. **P0 基线纯度不足。** `extensionBagEnabled=false` 时 `virtual_bag_inventory_enter_wrapper` 仍无条件安装 5 个扩展 tab 按钮进原版控件树（`:1681` 无 enabled 检查），禁用态下不可见不响应但树已改。「原版基线 smoke」并非严格原版，P0 证据应注明此偏差或先补 enabled 门。

## 四、小项（后续修订时并入）

1. 证据模板 §5.1 建议补「config 快照」（`extensionBagEnabled`/`moveMergeEnabled`/`stackLimitIncrease` 直接影响行为判定）。
2. 风险登记表缺 4 条：fn_save 未收口失步、WAL 崩溃窗口、装备背包移除溢出、API 验收缺口。
3. §9 P1 的 host 测试目标，`test_host.cpp` 已覆盖 normalize/click/base64/payload/merge/JSON 往返/recovery 判定——建议登记已覆盖项，避免 P1 重复建设。
4. `backlog.md` 扩展背包表（编号从 2 开始、写「用户真机触摸验收」）与控制面 §4 的 11 项和 API-first 规则双重冲突；控制面 §0 称待办以 backlog 为准，需先裁决以哪份为准。

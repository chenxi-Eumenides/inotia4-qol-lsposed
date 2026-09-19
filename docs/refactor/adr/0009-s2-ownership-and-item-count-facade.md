# ADR-0009：S2 归属与 `item_count` facade（P1 冻结）

- **状态**：已采纳（P1 实施中）
- **关联**：`docs/refactor/architecture-review-2026-09-19.md` §7/§8/§9.5/§9.6；ADR-0007（扩展背包契约与机制）；`ledger.md` §7「P1 裁决冻结」
- **性质**：本文冻结 P1 的目标边界与实施规格；**不代表已实施**。

## 背景（事实，2026-09-19 实测）

1. `contracts/stack_codec.h`（26 个 `constexpr`/`inline` 符号）同时承载：S2 位段编解码、99/999 策略、native bag marker、effective 视图、写回门控——被判混合职责（审阅 §8.5）。
2. 该头有 **30 个消费者文件**：`plugins/extension_bag`（15）、`plugins/inventory_hooks`（4）、`core/native/inventory_trade.cpp`、`api/native/game_inventory*`、`data/native/item_class.*`、`tests/*`。
3. S2 布局补丁（2 条位段缩窄 + 10 条读重定向）由 `host/core_patches.cpp:40-62` 以 `owner="core"` 持有，在 `bridge_init` 内安装、失败即整体不就绪（boot-fatal）；6 条 clamp 已在 `plugins/stack_limit`（gate=`plugin.stack_limit.enabled`）。
4. `host/plugin_registry.cpp` 的 `sync_plugin_resources` / `all_want`（:285–:425、:852–:898）对 `kCallPatch` fail-soft 资源**允许部分安装**，且在 `all_want` 为真时即调用 `on_enabled`（:817）——**无法表达"整组原子就绪才允许启用"**。

## 决策 1：`item_count` facade 采读+写（用户裁决 B）

`contracts/game_host.h` 新增 `QolItemCountApi`（POD，仅函数指针）：

| 成员 | 签名 | 语义 |
|---|---|---|
| `canonical` | `QolResult (*)(const void*, uint32_t*)` | 读取持久化 S2 canonical；非编码类别走原版数量 |
| `effective` | `QolResult (*)(const void*, uint32_t*)` | 开启态 `0..999`；关闭态原始低 7 位视图 |
| `is_encoded` | `QolResult (*)(const void*, QolItemCountEncoding*)` | 三态类别判定 |
| `write_effective` | `QolResult (*)(void*, uint32_t)` | 按当前视图 clamp 并写回；非编码类别拒绝改数量位 |
| `writeback_needed` | `QolResult (*)(const void*, uint32_t, int32_t*)` | 判断是否需要 S2 回写 |

- 三态 POD：`QolItemCountEncoding { kUnknown=0, kNotEncoded=1, kEncoded=2 }`。
- 错误语义：`nullptr` → `kQolErrInvalidArgument`（不修改 out）；**provider 未驻留或 999 原子组未满足 → `kQolErrNotReady`（不得写入默认 0）**；`kUnknown` 的数量读写 → `kQolErrUnsupported`（不猜测位段）。
- gate 关闭 ≠ provider 不可用：仍可读 `canonical`；`effective`/`write` 回落原始 99 视图。
- 读写在**同一调用窗内同步完成**（R-47/R-49，禁止退化为跨帧命令）。
- **不进入公共 contracts**：`stack_codec` 全部 `constexpr`、S2 位移/掩码、`max_count`、`effective_*`、`writeback_needed`、`stack_limit_enabled()`/`stack_limit_set_effective()`、native bag marker。

## 决策 2：S2 归 `stack_limit` 的 999 功能组，不做 boot-fatal（用户裁决）

- **目的**：开关**不改变数据的存读方式**；关闭态不使用额外数据位。
- **三条不变量**：
  1. **开启时写必须走 S2 布局**；
  2. **关闭 / provider 失败时，读写均回落原始语义**（低 7 位/native 视图）；
  3. **写侧与布局补丁同生共死**：布局补丁未就绪 ⇒ 999 功能视为不可用 ⇒ 读写回落原始。**禁止**出现"启用态用 S2 写、布局补丁缺失"。
- **状态推导**：`provider_resident`（codec + facade dispatch 驻留）× `atomic_group_satisfied`（全部 atomic claims `live`）× `gate_want` ⇒ `feature_999_usable`。
- gate 开但组未满足：facade 返回 `kQolErrNotReady`，调用方走原版 fallback；插件 `degraded=true`、`error` 指向失败资源 id。
- **不变量 ③ 的判定点**：置于 facade 的 `write_effective` / `writeback_needed` 内，依据 provider availability 与原子组状态；**不得**只看配置开关或旧全局 bool。
- 取代审阅 §8.2 的字面约束：布局补丁仍由宿主机制（`patch/` 资源通道）安装，但**归属与生命周期**归 `stack_limit` 的 999 功能组。

## 决策 3：引擎增补——原子资源组

- `contracts/plugin.h` 的 `ResourceClaim` 增字段 `bool atomic`（默认 `false`，**追加在结构末尾**以免破坏既有聚合初始化）。
- **同一插件内所有 `atomic == true` 的 claims 构成一个原子组**：
  - 任一成员安装失败 ⇒ **逆序释放该组本轮已装成员**（含已 `live`），整组保持 `free`；插件 `degraded`（`error` 指名失败资源 id）；每逻辑帧重试（复用 `retry_pending`/门禁重试路径）。
  - **只有整组 `live` 才允许** `on_enabled` / 能力发布 / 功能可用——`host/plugin_registry.cpp` 的 `all_want` 判定改为 `all_want && atomic_group_satisfied`。
  - 组内**不得出现"部分 live 对外可见"**：安装中间态对 facade、能力、`on_enabled`、功能 wrapper 一律不可用。
  - 回滚物理失败仍守既有安全规则：`tombstone + isolated + failed`（不得虚报 `free`）。
- 不引入数组/group-id 型契约面；将来需要多组时在 P4（DAG 阶段）演进。

## `contracts/stack_codec.h` 符号拆分

| 符号 | 去向 | 理由 |
|---|---|---|
| `kContractMajor/Minor` | 删除（由 facade ABI 版本承载） | codec 不再是公共契约 |
| `CountEncoding` | 公共 POD `QolItemCountEncoding` | 三态是调用方需要的稳定事实 |
| `kLegacyMax` / `kExtendedMax` / `max_count` | `stack_limit` 实现头 | 99/999 是**策略**，不是 ABI |
| `kNativeBagObjectMarkerShift/Mask`、`write_native_bag_object_marker` | `extension_bag` 私有头/实现 | marker 描述扩展背包对象（审阅 §8.4） |
| `kS2ShiftA/BitsA/MaskA`、`kS2ShiftB/BitsB/MaskB`、`kS2Max` | `stack_limit` 实现头 | S2 布局知识 |
| `s2_split_a/split_b/combine/read_count/write_count/clamp` | `stack_limit` 实现头 | 位段实现 |
| `effective_read_count` / `effective_write_count` / `effective_clamp` / `effective_view_count` | `stack_limit` 实现头（外部改用 facade） | 运行时视图策略 |
| `writeback_needed` | `stack_limit` 实现头（外部改用 facade） | 混入扩展开关与 S2 策略 |

## 999 功能组资源清单

- **迁入 12 条 S2 claims**（来自 `host/core_patches.cpp:40-62`）：2 条位段缩窄 + 10 条读重定向。
- **6 条 clamp**：已在 `plugins/stack_limit`（`P_INVEN_MOVE_ITEM_CLAMP_150/_158`、`P_INVEN_SAVE_ITEM_DIRECT_CLAMP_DC/_F0`、`P_INVEN_SAVE_ITEM_DATA_CLAMP_7C`、`P_INVEN_FIND_SAVE_SLOT_CLAMP_238`）。
- **getter/写路径**：`hook:ITEM_GetCumulateCount`、`INVEN_ConsumeItem`、`INVEN_SaveItem`、`INVEN_MoveItem`、`INVEN_SaveItemDirect`、`ITEMSYSTEM_Divide`、`INVEN_RemoveItemData`；**`ITEMSYSTEM_MakeItem` 不纳入**（当前无可信数量回写）。
- 全组：`kRequired + kFailClosed + resumable=true + atomic=true`，gate=`plugin.stack_limit.enabled`。
- **`host/core_patches` 迁移后**仅保留：`P_HUB_LOGIN_SKIP`（owner=`host`，reason 只述 Hive 登录跳过）+ monster 特征扫描（不建 claim、不入组、不伪装 S2 资源）；删除 S2 owner/reason 与 12 条 claim。不创建 `core.compat`。

## 消费者迁移批次（30 文件）

| 批次 | 文件 | 替换方式 | 关键约束 |
|---|---|---|---|
| 1 读侧（8） | `api/native/game_inventory.{h,cpp}`、`game_inventory_{basic,read}.inc`、`data/native/item_class.{h,cpp}`、`inventory_hooks/inventory_hook_stage4.{h,cpp}` | codec → facade；三态经公共 POD；错误向上传递 | **批次 1+2 必须同一提交/构建**（防"读 S2、写原始"混合态） |
| 2 写侧（16） | `core/native/inventory_trade.cpp`、`plugins/core.ui/panels/settings.cpp`、`extension_bag` 非 model 的 12 文件、`inventory_hooks/native_inventory_hook.{h,cpp}` | `effective_*`/全局 bool → `write_effective`/`writeback_needed`；marker 留 `extension_bag` 私有 | 同上 |
| 3 模型层（3） | `model/virtual_bag_state.h`、`virtual_bag_state_json.inc`、`virtual_bag_transaction_rules.inc` | sidecar 永远 canonical S2；运行时视图经 facade；marker 私有化 | 与 1/2 同批验收 |
| 4 测试（3） | `tests/test_host.cpp`、`test_infra_data.cpp`、`test_inventory_hook_stage4.cpp` | 公共面直测 → 私有实现/facade/错误码测试 | 仅 host 可先行，行为面仍需真机 |

## 风险与验收

**风险排序**：① 读写门控不一致 → S2 高位丢失或 canonical 破坏；② 原子组回滚不完整 → 部分 live / 重复 `on_enabled` / 能力提前发布；③ provider residency 与 gate 混淆 → 关闭功能时误卸载 canonical 解释器；④ 传递 include 遗漏消费者 → 编译通过但运行时仍读裸位段；⑤ hub/monster 误入资源组；⑥ 物理回滚失败却报 `free`。

**host 断言**：契约（5 成员 + `kQolErrNotReady` + out 不被写 0 + 三态完整）；资源（12+6+getter/写路径 owner 正确、S2 claim 不在 `core`、hub/monster 不混组）；原子组（6 条用例：首/中/含上轮 live 失败、重试成功、中间态不可见、gate 关闭）；消费者（99/100/127/128/199/999、非编码/宝石/装备/marker 不改位段）；回归（canonical 跨开关往返不变、关闭态读原始 b、开启态写 S2）。

**真机卡**：`VM-P1-S2-01` 启动与资源态；`VM-P1-S2-02` 开启后堆叠写/拆堆并核对 S2；`VM-P1-S2-03` 关闭后读回原始 b 视图、关闭期不得写 bits22–24；`VM-P1-S2-04` canonical=199 的"开→写→关读→开读"前后相等；`VM-P1-S2-05` 注入某 atomic claim 失败 → `degraded` + error=该 id + 组成员无对外功能，恢复后下一逻辑帧全 `live` 且 `on_enabled` 仅一次。

## 后果

- **正向**：公共 `contracts/` 零 codec 语义；S2 归属与失效语义明确；关闭"部分安装即启用"漏洞；原子组可复用（`craft_ui` 5 核心等）。
- **代价**：引擎新增原子组语义与测试；facade 引入一次间接调用（热路径）；`core_patches`→`stack_limit` 的资源迁移需真机验证 boot→assembly 时序与失败回落。

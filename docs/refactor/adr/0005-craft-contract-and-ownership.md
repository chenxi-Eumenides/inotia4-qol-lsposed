# ADR-0005：合成结果层能力契约与行为所有权

- 状态：已采纳（C2 实施中）
- 关联：`ARCHITECTURE.md` §3.4（能力契约）/§4.1（kOwnership）/§8（资源失败→状态映射）、
  `contracts/craft_result.h`、`contracts/ownership.h`、`docs/refactor/ledger.md` C2

## 背景

- `feature/craft_ui` 与 `feature/custom_recipe` 当前是**源码级双向依赖**：craft_ui 的 hook
  wrapper 直接 include custom_recipe 的头调结果层；custom_recipe 又反调 craft_ui 的只读状态
  原语（读格、读 mixType、读选中格）。依赖矩阵禁止 features 间水平互调，双向依赖必须在 C2 收口。
- custom_recipe 的合成结果层（3 格隐式配方、宝石升阶）逻辑复杂且已有完整 host 单测，适合作为
  **能力提供者**；craft_ui 是其唯一消费者。
- 结果层的历史接口从控件状态读入参（provider 读 UIMix），与「界面归界面、结果归结果」的边界
  相悖，且无法在无控件的 host 环境复用。
- 停用路径存在行为缺陷：`custom_recipe_table_deactivate` 只收回 RECIPEBASE 记录数，不解除
  `bind_base_record_count` 绑定；`three_slot_mode_active`/`three_slot_execute` 未并入 `enabled()`，
  残留面板上关总开关后仍可完成一次合成（消耗材料、产出物品）。
- 宿主机制分派仅实现 `kCallPatch`：`contracts::ResourceMechanism::kOwnership` 已存在但
  `host/assembly` 与 `host/plugin_registry` 的非入口路径会把它当 patch 站点解析；共享表注入
  这类「非 hook/patch 的互斥槽占有」无法声明。
- 引擎安装循环只有「任一资源失败即整体回滚 + failed」一条策略，`ARCHITECTURE.md` §8 的
  `kRequired×kFailSoft` / `kOptional×任一` → `degraded` 尚未实现。

## 决策

1. **结果层能力契约**：新增 `contracts/craft_result.h`（POD capability v1.0，id=`craft_result`），
   暴露 `enabled / three_slot_mode_active / is_jewel_tier_up_recipe / form_ui_type_for_mix_type /
   craft_gate / is_jewel_item / place / prepare / execute / material_needs / make_item /
   desc_should_replace`。**所有界面状态入参携带**（选中物品、三格类别/物品、持有总数由调用方
   读好后放入 Input），provider 不读 UIMix；返回枚举沿用现有 `PlaceOutcome/PrepareOutcome/
   CraftOutcome/CraftGate` 同名同值，并新增 `CraftExecuteResult::kAborted` 区分「停用/无待确认」
   与「格子已变」。C2 Step 3/4 由 custom_recipe 注册 provider、craft_ui 作消费者经能力注册表获取。
2. **行为所有权机制**：新增 `contracts/ownership.h`（`OwnershipOps{acquire,release}`）；
   资源 `mechanism=kOwnership` 时 `claim.handler` 指向该结构，宿主 install/release 非入口路径
   直接调 acquire/release，不假设调用点 patch 站点。custom_recipe 的 `.bss` 表注入以该资源声明
   （acquire=`custom_recipe_table_ensure`、release=`custom_recipe_table_deactivate`）。
3. **§8 逐资源失败策略**：安装循环按 `ResourceClaim.requirement × failure` 处置——
   `kRequired×kFailClosed` 失败 → 逆序回滚 + `failed`；其余 → 记录 error + `degraded`、
   跳过该资源继续安装其余（不改生命周期态）。
4. **停用 fail-closed 修复**：`three_slot_mode_active` / `form_ui_type_for_mix_type` /
   `desc_should_replace` 并入 `enabled()`；`three_slot_prepare` 生成代际 token，
   `three_slot_execute` 以纯函数 `execute_token_valid(enabled, pending_token, current_token)`
   前置校验，停用时作废在途确认。只改停用态，启用态行为不变。

## 后果

- 正向：craft_ui ↔ custom_recipe 的双向 include 可由能力契约取代（Step 3/4）；结果层可在
  host 以纯入参测试；kOwnership 使「表注入」等行为所有权进入资源登记/归还语义；§8 能力为
  后续可选资源（如 UI 组件、平台能力）提供 degraded 降级路径；停用后残留面板不再能合成。
- 代价：能力表与入参结构一旦冻结（v1.0）须保持 ABI 稳定；craft_ui 需按契约重构为消费者
  （Step 3/4）；`kOwnership` 的 acquire/release 语义由提供者保证幂等，宿主只做分派。

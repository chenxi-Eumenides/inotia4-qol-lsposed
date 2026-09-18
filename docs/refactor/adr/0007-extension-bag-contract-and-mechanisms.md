# ADR-0007：扩展背包契约与机制声明式化

- 状态：已采纳（D2 实施中）
- 关联：`ARCHITECTURE.md` §2.2（依赖矩阵与自我约束）/§3.4（能力契约）/§4（资源所有权与机制）/§7（宿主装配）、
  ADR-0004（`plugins/` 单层收口）、ADR-0005（能力契约与行为所有权）、ADR-0006（UI 组件分层）、
  `docs/refactor/ledger.md` D2、`archive/docs/development/features/extension-bag/rulebook.md`（R-01..R-66）

## 背景（事实）

- `feature/extension_bag` 共 32 文件 / 12,742 行：5 个编译 `.cpp`、14 个顶层 `.inc`、
  `model/` 9 个 header-only（4 `.h` + 5 `.inc`）。当前为 `feature/` 旧路径，未拍平到 `plugins/`。
- 机制**零声明式**：2 条 entry hook + 21 条 call patch + 4 处 popup-state 裸写 +
  11 个动态 desc 实例槽的 `PtrHook` + `move_merge` GOT + 自制 RX thunk；安装、回滚、占用声明均手写，
  不可由宿主机制枚举、观测与归还。
- 与 `feature/patch` 存在**编译期双向依赖**。
- 反向消费者 15+：多数经 `core/native/extension_bag_port.h` 调用；该头以 `std::string` 作为接口类型，
  违反 §3.4（能力契约须 POD/固定宽度）且把 EB 实现细节暴露给消费者。
- 两条轮询线程：300ms 注入线程、150ms 生命周期线程；与 `infra/scheduler` 帧源模型冲突。
- 出售链四方参与：EB store / `store_sell::plan` / `inventory_hooks`（H-23/H-24）/ `autosell`；
  R-13「商店/装备 popup 状态互斥」须保持。
- 归档规则册 R-01..R-66 全部有代码落点；册中行号已失效，定位一律**按符号**而非行号。
- 归档 VM 编号（VM-43..46/49/50 等）与 `docs/refactor/verification.md` 现有编号存在冲突，
  引用以 `archive/docs/development/features/extension-bag/verification-matrix.md` 为准。

## 决策

1. **能力契约 `contracts/extension_bag.h` v1.0**：POD + 固定宽度 + 函数指针的**读写混合 capability**，
   provider = `plugins/extension_bag`。「同调用窗内联事务」登记为 §3.4 只读原则的**登记例外**
   （先例 `craft_result`、`ui_choice.open`），退出条件 = native_service / 命令设施落地。
   `std::string` JSON 函数**不进契约**（留插件 `api.h`，仅 bridge/host 调用）；
   `bags_json` 若需跨插件则改缓冲式 `int32_t (*write_json)(char*, uint32_t)`。
2. **`model/` 9 文件留在 `plugins/extension_bag/model/`，不进 `domains/`**：插件私有状态机，无第二消费者。
3. **两个头归位**：`stack_codec.h` → `game/read/`（纯位编解码）；`stack_limit_port.h` →
   `contracts/stack_limit.h`（声明与实现分离，实现暂留 `core/native/stack_limit_state.cpp`，P5 收口）。
4. **机制全量声明式（29 条；口径校正：M1/M2 + C1–C21 + P1–P4 + S1 + X1）**：
   原「25 条」为侦察期粗算；逐点核对 raw 安装面后修正为 29，D2a-3a 已按 21 条 call
   patch 站点登记（`game/access/patches.cpp` 的 `P_EXT_*`）。
    - M1/M2 entry hook：`required` × `failSoft`（D2a-3b-1 按晚就绪语义校正：原记录为
      `failClosed`，与旧 300ms 线程重试语义不符；装配期失败保持 `free` + 插件 `degraded`，
      gate 开且 `free` 时宿主每逻辑帧重试到成功），gate = `extensionBagEnabled`。
   - C1–C21 call patch（8 save callsite + 6 draw/gate + 5 store + 2 mix）：功能型 `required` × `failSoft`，
     装饰型 `optional` × `failSoft`；**save callsite 机制 owner = `extension_bag`，
     行为编排 owner = `core/native/module_save.cpp`**；P5 若出现第二参与者再迁 `save_flow`。
   - P1–P4 popup-state：`kOwnership`，`required` × `failSoft`。
    - S1：11 个动态 desc 实例槽申报 `own:desc_button_execute_slots`（kOwnership，
      gate = `extensionBagEnabled`）；`acquire` 只登记命名空间所有权、`release` 调全量
      `reset_extension_desc_item_hooks_locked`，**per-item 安装仍保留业务路径 raw `PtrHook`
      写入**（过渡形态，清除条件 = U11 动态绑定器）。
   - X1：`ptrslot:MOVE_MERGE_GOT`（gate = nullptr，wrapper 内自门控
     `move_merge_enabled() || extension_source_protection_enabled()`，R-28）。
   - X2：删自制 `allocate_draw_thunk`，改用宿主 thunk 池；新增 `PatchKind::kBranchRedirect`（B 尾调用）。
5. **两条轮询线程删除**：300ms 注入线程 → gated claim（gate 开 + free 时每逻辑帧重试）；
   150ms 生命周期线程 → `infra/scheduler` 帧任务。
6. **出售链边界**：`store_sell::plan` 留 `domains/inventory`；`hook:UIStore_SellItem` owner = EB；
   `UIEquip_OKDestroyItem`（H-23）owner = `inventory_hooks`；R-13 隔离保持。
7. **B2c**：`inventory_hooks` 业务迁入 `plugins/inventory_hooks`，改 `optional_requires = [extension_bag]`、
   可 deactivate（「禁止 deactivate」文档保留至 VM-D2-03 通过）。
8. **U11（ui-kit adapter）未冻结前，EB 直依 `game/access/ui_kit`**（过渡依赖登记）。
9. **`feature/patch` 目录在 D2c 完成后删除**。

## 后果

- 正向：基线大幅收缩（目标 ≤ 30）；编译期双向依赖消除；资源可枚举、可观测、可归还；
  旧路径 `feature/extension_bag`/`feature/patch` 收口到 `plugins/`。
- 代价与约束：
  - 热路径多一次 capability 查找；**禁止跨帧缓存裸指针**。
  - 动态槽的 raw `PtrHook` 写入仍是 tombstone 盲区，须真机占用验证。
  - `core/native/extension_bag_port.h` 反向依赖需 D2b 全部消费者切换完成后才能删。

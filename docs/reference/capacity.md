# 容量与预算

模块的固定容量统一由 `module/app/src/main/cpp/contracts/capacity.h` 定义。不要在实现文件里再写字面量；新增容量时先判断它属于哪个维度，再决定派生关系。

## 维度

| 常量 | 计数对象 | 约束 |
|---|---|---|
| `kMaxPlugins` | 插件描述符 | 插件注册表、owner 表、HTTP owner 表、feature 顺序表共用 |
| `kMaxPluginResources` | 单个插件的资源声明 | 描述符校验；当前最大内置插件 31 条，预算 64 |
| `kMaxDeclaredResources` | 全局资源声明 | `kMaxPlugins × kResourceBudgetPerPlugin`，当前 1024 |
| `kHostClaimCount` / `kHostClaimReserve` | 宿主 claim | save 3 + 帧源 2 + core_patches 3；预留 16 |
| `kMaxPatchPoints` | 物理补丁地址占用 | 从声明预算派生 |
| `kMaxCallPatches` / `kMaxShortHooks` / `kMaxInsnPatches` | 机制记录 | 从声明预算派生 |
| `kMaxThunkSlots` | 共享 near/thunk 槽 | 单条资源最多 4 槽（多槽 insn patch），按最坏情况派生 |
| `kMaxActiveTransactions` | 并发事务 | 按插件数派生 |
| `kMaxFeatureStatuses` | feature 状态记录 | `(插件数 + 宿主 owner 数) × 每插件 feature 上限` |

## 编译期校验

- 每个有资源的插件在 `plugin.h` 导出 `kResourceCount`，并在资源数组所在 TU 用 `static_assert` 与数组长度对齐。
- `host/assembly.cpp` 的 `BuiltInPlugin` 表携带 `resource_count`，`constexpr` 汇总内置总量与最大单插件数量，并断言：
  - 内置总量 + 宿主 claim ≤ `kMaxDeclaredResources`；
  - 剩余预算 ≥ `kHostClaimReserve`；
  - 最大单插件资源 ≤ `kMaxPluginResources`。
- 宿主 claim 数由 `kSaveEnterClaimCount`、`kSaveExitClaimCount`、`kCorePatchClaimCount`、`kFrameSourceClaimCount` 相加，并断言等于 `kHostClaimCount`。
- 共享池关系在 `capacity.h` 内用 `static_assert` 固定：thunk 槽覆盖 call patch（1 槽）、短跳 hook（2 槽）和多槽 insn patch（4 槽）。

## 扩容规则

1. 先确认是哪张表触顶：声明表看 `hooks_claims`，物理地址表看 `patch_points`，机制表看对应安装日志。
2. 同一维度的容量只改 `capacity.h` 一处；派生值不要重复写字面量。
3. 新增插件资源时，同时更新该插件的 `kResourceCount`；编译期断言会拦截不一致。
4. 需要扩大总预算时，优先调整 `kResourceBudgetPerPlugin` 或 `kMaxPlugins`，让派生表一起变化；不要单独放大某个机制表。
5. 外部插件轨尚未冻结（`ARCHITECTURE.md` §9）。未来接入时，应在外层增加配额和准入控制，而不是取消全局预算：宿主预留、内置池、外部池分层，超配额只拒绝该插件。

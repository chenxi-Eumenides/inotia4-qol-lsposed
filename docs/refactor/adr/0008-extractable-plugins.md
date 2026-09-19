# ADR-0008：可外置插件与 Game Host API 不插件化

- 状态：已采纳（2026-09-18；设计 v0.3.14）
- 关联：`ARCHITECTURE.md` §1.1（目标）/§2.1–§2.4（分层、依赖矩阵、职责、布局）/§3.1（内置轨与外部形态）/§3.8（Game Host API 与 Host SDK）/§6.1（读模型归属）/§7.1（出口面插件）/§7.4（属性范围示例）/§11（P1/P5/P6）/§12.2（明确不做）、
  ADR-0004（拍平 `plugins/` 单层）、ADR-0006（UI 组件分层）

## 背景（事实）

- 目标结构此前保留顶层 `domains/` 作为共享语义层，且依赖矩阵（v0.3.13）为 `plugins → game/domains` 开边：插件与宿主内部实现同编译进一个 `.so`，可直接 include `game/` 强类型原语。
- 当前 `domains/` 实际只剩 `attribute_range` 一个域（纯规则），其余语义已拆解或归各插件；`domains/attribute_range` 与 `plugins/attribute_range` 并存，是过渡形态。
- P6 计划支持外部 native 插件（未签名、全权）。若不先把插件边界写成目标约束，届时每条 `plugins → game/host` include 边都会变成不可外置的隐式耦合，且期间新增的内置插件会继续按「同一 `.so` 可随意引用宿主内部」的惯例生长。
- Host API 此前整体归入 P6 未冻结草案（§3.8 旧文），导致内置插件与宿主私有实现之间没有稳定 seam；内核设施（日志/配置自举、加载器、资源机制）与业务插件的边界也未在文档中明确，容易出现「万能不可停用插件」的替代方案。

## 决策

1. **可外置插件**：所有内置插件从目标结构上必须可独立外置。一个 `plugins/<id>/` 目录拥有全部私有规则、UI、生命周期实现与测试；只依赖①公共 `contracts/`（含 Host SDK / Game Host API 函数表类型）②经注册表获得的其他插件 capability/native_service ③自身目录。禁止 include 其他插件源码，禁止依赖宿主私有 `game/`、`host/` 实现。内置静态与外部 native 仅**加载/分发形态**不同，插件边界相同。
2. **取消目标层 `domains/`**：共享语义不再以可链接的宿主内部 `domains` 实现供插件直接消费。有多个消费者的语义成为独立官方 provider 插件并发布 capability（只读）或命令（副作用）；只有一个消费者的规则留在该插件目录。`domains/attribute_range` 属迁移债，P5 删除。
3. **Game Host API 不插件化**：基础版本化 Game Host API 是插件加载/执行前置的宿主内核 ABI 子表，由 `host/` 实现并注入，负责 target/变体、主线程调用、低层游戏读写/调用适配与受控 raw 便利面。它**没有普通插件生命周期、不能停用**。宿主内部 `game/` 是其 adapter/实现，插件不能直接 include。稳定角色/物品/世界语义由官方 provider 插件提供，业务写操作经 owner 插件命令端口，高阶功能是独立插件。
4. **「一切功能皆插件」不包含内核设施**：日志与配置自举（R8）、插件加载器、资源机制、Game Host API 与 Host SDK 属宿主内核，不参与插件生命周期、不可停用、不属于插件边界模型。
5. **冻结分级**：v0.3.14 冻结 Host 核心 / Game 子表的**归属与职责边界**；具体 C ABI 函数表在 P5 为内置插件落地并允许继续演进，外部容器与对外 SDK 兼容边界在 P6 冻结。P6 前不向第三方发布兼容承诺。
6. **拒绝万能 game provider**：不设一个把全部游戏读写/语义集中暴露的单一 provider 插件。低层原语归不可停用的 Game Host API 内核；稳定语义按消费者拆为多个官方 provider 插件；这样单个 provider 停用只级联其真实消费者，而非全模块。

## 拒绝的备选

- **保留顶层 `domains/` 作为插件可链接共享层**：与「插件零编译期依赖、可独立裁剪」冲突；共享域会成为所有插件的隐式公共依赖，无法独立停用或外置，且随消费者增长持续膨胀。
- **将 Game Host API 做成一个高特权内核插件**：内核设施不参与插件生命周期；做成插件会引入「不能停用但要走插件状态机」的分类特例，并让加载/执行前置依赖反向依赖被加载对象。
- **单一万能 game provider 插件**：把全部游戏语义集中在一个 provider，任一部分失效即级联全模块，且其接口无法按消费者边界收敛。

## 后果

- 正向：P6 外置不再需要拆解隐式 include 边；插件可独立测试与裁剪；跨插件共享的只读语义通过 capability 显式化，可被第三方复用；内核设施与业务插件边界清晰。
- 代价：插件不能直接使用 `game/` 强类型原语，须经 Game Host API 或 owner provider，现有 include 边需在迁移中改造；`domains/attribute_range` 需在 P5 拆为 `attribute_quality` provider 与 `attribute_range_display` 消费者（§7.4）；依赖矩阵由八目录收敛为七目录，`check_layers`/`arch_common` 的层映射需在 P5 同步（不在本 ADR 修改脚本）。
- 边界：本 ADR 只冻结目标结构与分类，不声称已实施；当前实现仍含 `domains/attribute_range` 与 `plugins → game` 过渡 include 边（见 `docs/refactor/ledger.md`）。

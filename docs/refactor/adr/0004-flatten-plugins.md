# ADR-0004：拍平 features/products 为 plugins 单层

- 状态：已采纳（2026-09-18；v0.3.11 补充 API owner 与动态 capability）
- 关联：`ARCHITECTURE.md` §1.2（R8）、§2、§3.4.5、§7、§11、ADR-0001

## 背景

原设计为双层：`features/<id>/`（行为单元，L3）与 `products/*`（呈现面：`api_export`、`qol`，L4）。实践中确认：

1. **交流协议上两层没有区别**——都经 capability/命令/事件消费彼此。区分只剩三个机制属性（生命周期、数量与竞争、装配地位），且当前出口面全部"用不上"：HTTP 已有配置开关、无同类竞争者、装配顺序可由 `requires/provides` 拓扑排序表达。为两个单例容器维持一套用不上的状态机是纯成本。
2. **分类是持续的认知税**：每个贡献者要回答"这是 feature 还是 product"，边界案例（未来功能自带 UI/HTTP 需求）反复产生争论；P6 开放后第三方作者也要理解这套内部分类。
3. **翻转窗口正在关闭**：D1 已裁决 `products/qol` 不提前落地（没有"非游戏耦合"的 UI 消费者），`products/` 目录至今未创建——现在拍平的迁移成本为零，P5 建目录后再翻转会多一次全量搬迁。

## 决策

1. **拍平为 `plugins/` 单层**：删除 `products/` 层，`features/` 更名 `plugins/`；一切功能皆插件。HTTP 出口与游戏内 UI 容器成为首批**出口面内置插件** `core.api` / `core.ui`，与普通插件走同一 `PluginDescriptor`、十态生命周期与资源事务。
2. **core.api 只做服务器与注册内核**：拥有 `http.registry` 能力；负责 HTTP 解析、路由分派、JSON 信封/错误映射、`/api/op` 门控、query/command 线程纪律及在途 route lease。自身只保留 `/api/health`、`/api/system/routes` 等传输级基础端点，不承载 character/world/item 等业务 API，不做游戏语义计算、不直接读游戏内存。
3. **路由 owner 与 URL 解耦**：内置 API 由 API provider 或功能插件注册，默认按领域拆分，也允许以 `api.info`/`api.action` 形成独立启停边界；插件 id 不决定 URL。`docs/reference/api-reference.md` 中既有路径、方法和成功响应冻结，P5 只迁移 owner；外部插件才强制 `/api/ext/<插件id>/...`。P5 必须产出冻结路由到唯一 owner 的表并校验重复与遗漏。
4. **core.ui 只做容器**：设置页容器、共享控件（`ui_kit`）、面板注册与导航；面板实现归各插件 `ui/`；游戏内存与绘制原语经 `game/access/ui*`（host 注入 adapter），`core.ui` 不直接依赖 `game/`。
5. **能力身份静态、可用性动态**：capability id、版本、接口和发布模式必须静态列入 `PluginDescriptor.provides`；`kDynamic` 能力可在运行期低频 publish/revoke，并发布 `capability_registered/revoked` 事件。`optional_requires` 据此延迟接入/反注册；required 依赖仍走 blocked/级联。进档、面板、商店等高频上下文不反复注册能力，由稳定接口的 `availability()/probe()`、快照或领域事件表达。
6. **自举兜底红线（R8）**：最后观测手段是 `infra/log`（内核设施），最后恢复手段是外部 `config.json` + 重启；二者不得依赖任何插件。据此**不需要**"不可禁用插件"分类——`core.api`/`core.ui` 均为普通可禁用插件，自举悖论由内核设施而非特权插件化解。
7. **不引入 before/after 装配顺序**：`requires/provides` 拓扑排序已覆盖出口依赖排序，不需要第二套排序机制。
8. **执行节点 = P5**：`products/` 从未创建，无迁移成本；P5 收口时按新结构落地 `core.api`/`core.ui`、内置 API provider 和动态 capability 协议（U10：`http.registry` v1 剩余 ABI 与 owner 表）。**更名已于 P4-D0 提前执行（用户裁决，避免 D1/D2 二次搬迁）**：`features/` → `plugins/` 机械搬迁、工具链与基线同步；P5 只承担 `core.api`/`core.ui`、动态能力、JNI 与脚本收紧。

## 后果

- 正向：单一心智模型，贡献者与第三方作者零分类决策；出口面获得与其他插件一致的禁用/观测/仲裁语义；分层九目录收敛为八目录，依赖矩阵减少一层。
- 代价：需维护由脚本校验的“冻结路由 → owner 插件”表；`http.registry` 与 capability generation/lease 成为新的冻结面（U10）；`core.api` 故障时 HTTP 诊断整体不可用，最终观测仍依赖 log 与外部 config（R8 兜底）。

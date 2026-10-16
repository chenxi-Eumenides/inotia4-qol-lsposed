# `core.api` 插件

## 1. 作用与 owner 定位

`core.api` 是普通核心插件，注册 ID 为 `core.api`、版本为 `0.1.0`。它声明自身的 HTTP 路由元数据，通过 Host SDK 暴露共享 HTTP registry/dispatch 能力，并拥有 `api.enabled` feature 与 listener 配置。它不是 Kotlin HTTP server，不聚合领域数据；声明路由元数据也不意味着插件实现了对应 native handler。

完整 HTTP 契约见 [API 参考](../api-reference.md)，模块边界见 [ARCHITECTURE.md](../../../ARCHITECTURE.md)。以下实现说明以当前源码为准，不复制 HTTP schema。

## 2. 功能面

插件 descriptor 从 `owned_routes.inc` 引入以下五条 GET 路由元数据，owner 均为 `core.api`：

| 方法 | 路径 | 用途 | 需要 route lease |
|---|---|---|---|
| GET | `/api/health` | 健康入口 | 否 |
| GET | `/api/system/features` | 插件/feature 状态出口 | 是 |
| GET | `/api/system/resources` | 资源状态出口 | 是 |
| GET | `/api/system/tickets/{id}` | 命令票据结果回取 | 是 |
| GET | `/api/system/routes` | 路由元数据查询 | 否 |

这五条路由均未声明 OP 门控。具体数据读取与响应不由 `core.api/plugin.cpp` 聚合实现。

`routes.inc` 是生成的全量路由元数据表，当前基线为 147 条；它不是插件安装时逐条注册的输入。该全表中 owner 为 `core.api` 的第六条是 `POST /api/op/features/lifecycle`，实际由 Host 常驻控制设施处理，不在上述五条插件 descriptor 声明中。owner 标签与执行归属的区别见 §4。

## 3. 使用的模块功能与消费者

插件 descriptor 没有 capability requires，也没有 Game Host port；提供的两个 capability 见 §5。

`core.data`、`core.character`、`core.item` 的 HTTP capability 依赖为可选，Debug 构建的 `core.custom_ui` 也将调试 HTTP capability 声明为可选。provider 缺失或撤销只影响相应 HTTP 接线，不级联停用这些消费者的非 HTTP 功能；其他消费者按各自 descriptor 的 `requires` / `optional_requires` 规则处理。

`settings`、`extension_bag` 通过静态 `HttpEndpointDescriptor` 声明设置 UI / 扩展背包调试端点，并声明可选的 `http.registry`、`http.dispatch` 依赖，由 Host binder 接线，不再由插件安装回调直接调用 `register_command`。

消费声明可在 `module/app/src/main/cpp/plugins/` 下对应插件的 `plugin.cpp` 中核对；各端点的业务 handler 仍归对应 owner。

## 4. 职责范围与边界

- **Transport**：Kotlin `ApiServer` 构造、启动和关闭 AndServer，并注入 socket factory；`core.api` 不负责 socket、HTTP 请求解析或网络响应传输。
- **接线与分发**：Host `http_endpoint_binder` 根据各插件 descriptor 绑定路由与 handler，管理失败回滚及停用 drain；native registry/dispatch 提供查找、门控、lease 与 handler 分发机制。
- **常驻管理**：`/api/config/list`、`/api/config/set`、`/api/config/receipt/{id}` 的路由 owner 标签是 `settings`，与配置 namespace `core.settings` 不同。`/api/op/features/lifecycle` 的标签是 `core.api`。这四个端点的生产处理都属于 Host 常驻控制设施，不属于普通插件 handler 的生命周期，也不要求设置页插件处于 active。
- **数据与写入**：领域 HTTP 数据和操作由对应领域/功能 owner 实现；`core.api` 不读取 live 游戏语义，也不替代 Host 常驻配置 Store。

Host 常驻控制请求在普通 registry 接受态与 OP 门控之前分发，不依赖 `core.api` 插件生命周期。但 listener 停止后，外部 HTTP 请求无法到达这些端点；常驻分发不代表另有独立网络入口。

## 5. 对外提供与依赖影响

| Capability | 版本 | 插件暴露的接口地址 |
|---|---|---|
| `http.registry` | 1.1 | `&ctx.sdk->http` |
| `http.dispatch` | 1.1 | `&ctx.sdk->http_dispatch` |

接口实现由 Host SDK 提供：registry 包含注册/注销、查找、列举及 lease；dispatch 包含 query/command handler 注册、owner 注销及请求分发。内部 ABI 以 `module/app/src/main/cpp/contracts/http.h` 为准，面向调用者的 HTTP 契约仍以 [API 参考](../api-reference.md) 为准。

provider 不可用时，Host binder 阻止新的非 resident 请求并摘除相应接线，等待在途 handler 与 route lease 排空；provider 恢复后，为仍活跃的 owner 重新接线。这不是 `core.api` 停用回调自行关闭共享 registry 的行为。

## 6. 读写线程与生命周期机制

### 插件回调与 Host 接线

`plugin_install` 只检查 `ctx.sdk` 非空，将 SDK 的 `http` / `http_dispatch` 接口地址写入 capability provision，并保存 `g_sdk`；不验证函数表，不注册全量路由，也不绑定 lifecycle handler。`plugin_deactivate` 只清除 `g_sdk`；插件状态中的 enabled 据此判断。

生产路由的注册、回滚和 drain 由 Host binder/registry/dispatch 管理。普通 handler 分发独立检查 OP 授权，获取 route lease，执行 handler 后释放 lease；owner 注销先阻止新请求，再等待在途请求结束。

生产 lifecycle 端点调用 Host controller 提交 desired activate/deactivate 请求，返回异步 receipt；实际 lifecycle 由主线程帧步进 reconciler 推进，HTTP 线程不直接执行游戏内存写入。`plugin.cpp` 中保留的 `lifecycle_command` 仅为兼容测试辅助，执行 body 解析、action 映射和命令投递，不是生产端点的接线入口。

### Feature 与 OP 授权

`api.enabled` 是默认开启的 General toggle：Debug/Release 设置页均显示（Release 只过滤 Hidden 分类）。分类与空描述只影响显示元数据。feature callback 为 no-op，但 feature 值仍参与 Host 生命周期判定与 listener runtime effects，不代表开关无效。

API listener 与 `module.security.features.op.enabled` 授权独立：关闭 API 不改变 OP 值，开启 API 也不自动授权 OP。listener 状态不是 native dispatch 的权限开关；普通请求仍按 endpoint 元数据独立执行 OP 门控。游戏内设置 UI 的配置写入/恢复路径不因 API listener 停止而关闭。

### Listener 与配置

标准模式下，Kotlin listener 由 Host runtime effects 驱动：没有 effect 时不启动，清除 effect 时停止；`actualEnabled`、`desiredEnabled` 任一关闭或 listener config 无效时停止。只有两项状态均启用、配置有效且已取得启动 context 时才启动。

server 已存在时，listener 配置变化安排 500 ms 后 reconcile；reconcile 发现配置不同则停止旧 server 并按新配置启动。启动失败记录错误，其中 bind failure 通知 `PluginRuntimeEffects`。listener 的控制调度位于 Kotlin 的单线程 `api-server-control` executor，不在 native feature callback 内直接操作 socket。

| `core.api` 配置项 | JSON 类型与原生校验 | 默认监听值 |
|---|---|---|
| `listen_address` | 字符串，解码后非空白且为有效 UTF-8 | `0.0.0.0` |
| `listen_port` | 整数，范围 1–65535 | `8088` |

`validate_api_config` 对不符合上述要求的值返回参数错误；其他 key 不被该插件校验器拒绝。该校验不保证地址可解析或端口可绑定；实际启动错误由 listener 报告。runtime effect 中配置无效会停止 listener，不应将被拒绝、未提交的配置写入等同于 listener 已停用。

`KERNEL_ONLY` bootstrap 是独立例外：启动默认 `0.0.0.0:8088` 的 loader health server，不走普通插件 runtime effects。

## 7. 文件地图

以下路径相对于仓库根目录：

| 子职责 | 文件 | 关键事实 |
|---|---|---|
| 插件描述符与回调 | `module/app/src/main/cpp/plugins/core.api/plugin.cpp` | capability 接口、五条 metadata、feature、配置校验、安装/停用及兼容测试辅助 |
| 插件声明 | `module/app/src/main/cpp/plugins/core.api/plugin.h` | descriptor 与兼容测试辅助入口声明 |
| 插件自有路由元数据 | `module/app/src/main/cpp/plugins/core.api/owned_routes.inc` | 五条 GET 路由与门控/lease 标记 |
| 生成的全量路由表 | `module/app/src/main/cpp/plugins/core.api/routes.inc` | 147 条路由的 method/path/owner/门控/lease 元数据 |
| HTTP 内部 ABI | `module/app/src/main/cpp/contracts/http.h` | registry/dispatch 函数表及 1.1 版本 |
| Host endpoint 接线 | `module/app/src/main/cpp/host/http_endpoint_binder.cpp` | descriptor 绑定、回滚、provider 可用性与 owner drain |
| Host registry | `module/app/src/main/cpp/host/http_registry.cpp` | 路由、owner 接受态与 lease 生命周期 |
| Host dispatch | `module/app/src/main/cpp/host/http_dispatch.cpp` | resident 前置分发、OP、handler 与在途请求 |
| Host 常驻路由声明 | `module/app/src/main/cpp/host/http_resident_routes.inc` | 常驻控制端点元数据 |
| Host 常驻控制实现 | `module/app/src/main/cpp/host/features/http_control.cpp` | 配置与 desired lifecycle 请求、receipt 回取 |
| Kotlin HTTP transport | `module/app/src/main/java/com/inotia4/qol/ApiServer.kt` | runtime effect、监听配置、AndServer 启停与启动失败处理 |
| 设置与扩展背包调试端点 | `module/app/src/main/cpp/plugins/settings/plugin.cpp`、`module/app/src/main/cpp/plugins/extension_bag/plugin.cpp` | 静态 endpoint descriptor 与可选 HTTP 依赖 |

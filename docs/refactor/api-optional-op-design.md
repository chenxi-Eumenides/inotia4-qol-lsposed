# API 可选接线与通用 OP 权限设计

## 1. 范围与阶段

本设计记录 API 服务归属、可选 HTTP 接线、runtime effects 与通用 OP 权限的冻结合同及 Gate 1/Phase 2 接线口径。Phase 1 冻结 `contracts/host_sdk.h`、`contracts/http.h`、`contracts/plugin.h` 和 HTTP capability ABI；Phase 2 混合 transport metadata 合同见 §8；Phase 3 对照当前 source 与文档的 owner/migration 事实见 §9。合同冻结输入为 `.slim/deepwork/api-optional-op-architecture.md`。

通用配置 revision/CAS handoff 已按 Gate 0 attempt 3 冻结，精确接口与冲突语义见 §6。既有未提交 settings 目录改名、route owner 和插件参考说明，以及四个 retired tests 均保持原状；不改变 API/OP 所有权，不恢复 API 到 platform owner。

## 2. 归属模型

- `core.api` 插件拥有 General、默认开启、priority -5 的 `api.enabled` service feature，以及 `listen_address`/`listen_port` 配置。该 General 项在 Debug/Release 设置页均显示；分类只影响 catalog 展示，不改变 API listener 或服务行为。Kotlin 提供 server transport adapter，并根据 Host runtime effect 中的 actual、desired 与有效配置接线；Host 不拥有 API 产品 feature，也不根据 API 服务状态特判插件业务。
- 普通业务插件可将 HTTP provider 声明为 optional dependency。provider 缺失、撤销只影响该插件的 HTTP endpoint 接线；不级联停用插件的非 HTTP 业务。provider 恢复后按生命周期重新接线。
- 插件在 endpoint descriptor 上独立声明是否需要 OP。Host 只提供通用 `module.security/op.enabled` 权限查询与执行前门禁（General、默认关闭、priority -4）；OP 与 API service 完全独立，不因 API 状态触发 OP 的写入或级联。
- route metadata label、endpoint 的 lifecycle owner 和配置 namespace 是不同概念：`route_owner` 是路由表标签；插件 lifecycle owner 取 `PluginDescriptor::id`；三条配置 resident route 的 owner label 仍为 `settings`，配置 namespace 仍为 `core.settings`，Host 按路径处理并且不要求设置页插件 active。

## 3. Host SDK v1.26 runtime effects、security 与 module-health ABI；v1.27 desc hook V2

Host SDK major 保持 1，当前 minor 为 27。v1.24 在 `QolPluginRuntimeEffect` 末尾追加 `desired_enabled`；v1.25 在 `QolHostSdk` 尾部追加 module-health facts 指针；v1.26 为 feature catalog 增加 display priority 与 `read_categories`；v1.27 在 `QolHostSdk` 尾部追加 `desc_hook_v2` 指针。`QolHostSdk` 的 runtime/security/module-health/desc_hook_v2 指针分别位于 offset 824/832/840/848，SDK 大小为 856 bytes。Phase 2 route metadata 本身不再扩展 Host SDK。

```cpp
struct QolPluginRuntimeEffect {
    uint32_t struct_size;
    uint64_t event_revision;
    uint64_t config_revision;
    const char* plugin_id;
    uint8_t actual_enabled;
    const char* config_json;
    uint32_t config_json_size;
    uint8_t desired_enabled;
};

using QolPluginRuntimeObserver =
    void (*)(void* context, const QolPluginRuntimeEffect* effect);

struct QolRuntimeEffectsApi {
    uint32_t struct_size;
    void* context;
    QolResult (*subscribe_self)(void*, QolOwnerToken,
        QolPluginRuntimeObserver, void*, uint64_t* out_id);
    QolResult (*unsubscribe)(void*, QolOwnerToken, uint64_t id);
};

struct QolSecurityApi {
    uint32_t struct_size;
    void* context;
    QolResult (*op_enabled)(void*, uint8_t* out_enabled);
};
```

`subscribe_self` 由 Host 验证 `QolOwnerToken` 并只订阅该 token 对应插件；成功注册后同步 replay 当前 actual、desired 与 raw config，后续事件携带单调递增的 `event_revision` 和对应 `config_revision`。`desired_enabled` 表示 Host `PluginDesiredSnapshot::want_run()`（`desired_enable && any_feature`）；`actual_enabled` 独立反映运行 actual，不得互相替代。Kotlin API listener 仅在 `actual_enabled && desired_enabled && config` 有效时启动；desired 关闭立即停止 listener，不等待 actual reconcile。Kotlin listener 的绑定结果不决定 native actual。observer 在 hub 锁外调用。`plugin_id`、`config_json` 与 effect 对象只在 observer 回调期间有效；异步处理必须先复制所需内容。

Kotlin/JNI 接线沿用冻结订阅 seam：`nativeRuntimeEffectSubscribe(pluginId: String, callbacks: Class<*>): Long`、`nativeRuntimeEffectUnsubscribe(subscriptionId: Long): Boolean`；Host 调用静态 `onRuntimeEffect(pluginId: String, eventRevision: Long, configRevision: Long, actualEnabled: Boolean, desiredEnabled: Boolean, configJson: String)`。

runtime effect 是 Host revisioned observer event，不是 Kotlin 对 native 状态的定时查询；订阅时同步回放当前状态，后续只派发新 revision。`module_health` 也由 Host 维护并经 revisioned event 推送，native 不运行 1 秒健康轮询。

`QolSecurityApi` 是只读通用 OP 权限查询，与 `api.enabled` service feature 分离。尾字段和函数表均通过现有 Host SDK `abi.size`/`abi.minor` 协商；此阶段只定义合同，不增加生产实现或调用方。

## 4. HTTP capability 与 descriptor endpoint ABI

`http.registry` 与 `http.dispatch` 当前均为 v1.1。消费者按 descriptor 中声明的 major/minor 协商；声明为 optional 的消费者在 provider 缺失或撤销时只省略 HTTP 接线，不级联停用其非 HTTP 功能。是否为 optional 以具体 descriptor 为准；不把所有 HTTP consumer 一概描述为 optional。

endpoint POD：

```cpp
enum class HttpHandlerKind : uint8_t { kQuery = 0, kCommand = 1 };

struct HttpEndpointDescriptor {
    HttpMethod method;
    const char* path;
    const char* route_owner;
    HttpHandlerKind kind;
    uint8_t op_required;
    uint8_t lease_required;
    HttpQueryHandlerFn query_handler;
    HttpCommandHandlerFn command_handler;
    void* ctx;
};

using PluginConfigValidateFn =
    QolResult (*)(const char* key, const char* json, uint32_t json_size);
```

`HttpQueryHandlerFn`、`HttpCommandHandlerFn` 复用 `contracts/http.h` 中现有 typedef，不改变函数 ABI。GET 必须声明 `kQuery` 且只填 `query_handler`；POST 必须声明 `kCommand` 且只填 `command_handler`。endpoint 数组、路径/label、handler 与 `ctx` 在描述符注册期间保持有效。`route_owner` 是 HTTP route metadata label，不能创建或隐式激活同名插件；handler lifecycle owner 由包含该声明的 descriptor ID 决定。

`PluginDescriptor` 在现有 `feature_count` 完整尾部后追加 Phase 1 handler-bearing endpoint 字段：

```cpp
const HttpEndpointDescriptor* http_endpoints = nullptr;
uint32_t http_endpoint_count = 0;
PluginConfigValidateFn validate_config = nullptr;
```

Phase 2 仅在该既有尾部后追加无 handler/context 的 route metadata 描述符与表指针：

```cpp
struct HttpRouteMetadataDescriptor {
    HttpMethod method;
    const char* path;
    const char* route_owner;
    uint8_t op_required;
    uint8_t lease_required;
};

const HttpRouteMetadataDescriptor* http_route_metadata = nullptr;
uint32_t http_route_metadata_count = 0;
```

`validate_config` 接受单个 config key 与未解析的 raw JSON value byte span；配置语义/校验仍由 descriptor owner 提供。Host catalog 包含所有注册 descriptor 的 feature 定义，唯一按身份排除注册 ID 为 `settings` 的设置页插件；不按插件 namespace 或额外可见性标记筛选。设置页模型在 Debug 显示 Hidden，在 Release 过滤 Hidden。全部 core feature（包括 `core.api.api.enabled`）为 Hidden 且无描述；`core.custom_ui.open_debug_components` 仅由 Debug descriptor 条件注册。

现有 `http_queries` 与 `http_query_count` 标记为 deprecated compatibility tail 并保留其字段、偏移及运行兼容；Phase 2 不删除或迁移旧 descriptor。旧 descriptor 的 `method` 仍决定分流：GET 按 legacy query handler 分发，POST 按 command 路径分发；保留旧函数签名及响应/票据语义。它与 handler-bearing `http_endpoints`、无 handler 的 `http_route_metadata` 是三种不同表示。

## 5. 64-bit ABI assertions

`settings_catalog_abi_tests` 冻结以下追加布局，旧前缀同时由 existing prefix assertions 保护：

| Type/member | Offset / size (bytes) |
|---|---:|
| `QolHostSdk` v1.22 prefix | size 824 |
| `QolHostSdk::runtime_effects` | 824 |
| `QolHostSdk::security` | 832 |
| `QolHostSdk::module_health` | 840 |
| `QolHostSdk` v1.26 | size 848 |
| `QolHostSdk::desc_hook_v2` | 848 |
| `QolHostSdk` v1.27 | size 856 |
| `QolFeatureCatalogItem::display_priority` | offset 40; item size 48 |
| `QolFeatureControlApi::read_categories` | offset 96; API size 104 |
| `QolPluginRuntimeEffect` fields | `struct_size=0`, `event_revision=8`, `config_revision=16`, `plugin_id=24`, `actual_enabled=32`, `config_json=40`, `config_json_size=48`, `desired_enabled=52` |
| `QolPluginRuntimeEffect` | size 56 |
| `QolRuntimeEffectsApi` | `struct_size=0`, `context=8`, `subscribe_self=16`, `unsubscribe=24`; size 32 |
| `QolSecurityApi` | `struct_size=0`, `context=8`, `op_enabled=16`; size 24 |
| `HttpEndpointDescriptor` | `method=0`, `path=8`, `route_owner=16`, `kind=24`, `op_required=25`, `lease_required=26`, `query_handler=32`, `command_handler=40`, `ctx=48`; size 56 |
| `PluginDescriptor` existing prefix | `http_queries=160`, `features=176`, `http_endpoints=192`, `http_endpoint_count=200`, `validate_config=208` |
| `PluginDescriptor` metadata tail | `http_route_metadata=216`, `http_route_metadata_count=224`; total size 232 |
| `HttpRouteMetadataDescriptor` | `method=0`, `path=8`, `route_owner=16`, `op_required=24`, `lease_required=25`; size 32 |

## 6. 配置与权限迁移目标

以下为 Host/配置迁移合同：

通用配置 revision/CAS handoff 已冻结，不增加 API 专属 Host 配置 writer。JNI seam 为 `nativePluginConfigRequestIfRevision(pluginId: String, key: String, valueJson: String, expectedConfigRevision: Long): String` 与 `nativePluginConfigReceipt(requestId: Long): String`；返回既有 admission/receipt JSON，不新增 HTTP schema 或插件 SDK 写权限。内部 controller seam 为 `request_set_config_if_revision(plugin_id, key, json, size, expected_revision, out_receipt)`，异步入队后由 worker 在 service mutex 保护的事务内比较全局 revision 并 durable commit。Revision 冲突返回 `kQolErrConflict`（5）；冲突或失败不提交 revision/runtime effect，普通 config 写行为不变。Kotlin listener fallback 按 `(eventRevision, failedPort)` 最多提交一次 CAS；仅当当前快照端口与失败端口一致且不是 8088 时请求 8088，之后查询 receipt 并等待新的 runtime effect，不盲目重试。

Kotlin 内部 listener fallback 通过 `nativePluginConfigSubscribeReceipt` 等待一次 Host receipt event，不使用 200ms/50ms 定时轮询。Host 最多保留 256 个活动 receipt observers；待处理 receipt 在转终态时通知一次，订阅时若 receipt 已终态则在 controller mutex 外回放该终态且不返回活动 token。JNI callback 以 `(requestId, feature_receipt_json(receipt))` 传递 receipt JSON。公开 `GET /api/config/receipt/{id}` 仍按 API 契约供调用方轮询；内部 observer 不改变 HTTP schema、Host SDK v1.25 或 HTTP capability ABI。

| owner | feature/config | 默认与迁移规则 |
|---|---|---|
| `core.api` | feature `api.enabled`；config `listen_address`、`listen_port` | General、默认 true、priority -5；Debug/Release 设置页均显示；listener 默认 `0.0.0.0:8088`。当前 Store 支持从 `module.platform.features.api.enabled`、`core.api.features.core.api.http_registry` 与 root `apiEnabled` 迁移；旧 owner 值仅在新目标缺失时使用，API 旧 feature 按保守 AND，缺失按 true。listener config 从受支持的旧 namespace/camelCase 键及 root `listenAddress`/`listenPort` 搬到 `core.api`。 |
| `module.security` | feature `op.enabled` | General、默认 false、priority -4；Debug/Release 设置页均显示；只迁移明确保存的旧 `module.platform.features.op.enabled`、root `opEnabled` 或 `core.settings.config.opEnabled` 布尔值；已有目标（包括 false）优先，旧 false 必须保留；不可从 API 状态推导或提升。 |

迁移按单一原子持久化事务执行：新目标值已存在时优先保留，否则按表中规则写入；迁移失败不得部分提交或报告 durable 成功。迁移后移除已处理的旧 API/OP feature/config 投影，避免重启时旧值覆盖新值；`module.platform` 的 `debug.logging` 等非 API 机制按其既有职责保留。上述 root/key 支持范围以 `host/core_settings_config_store.cpp::migrate_host_owner_values` 为准；历史名称映射不等于任意同名旧配置都会导入。

## 7. 接线阶段与验证门

1. **Phase 1 / 合同基线**：冻结 HTTP registry/dispatch capability v1.1、endpoint descriptor tail 与 Host SDK v1.23/runtime-security 接口。
2. **Gate 1 / Host 与 consumers 接线**：Host SDK v1.24 只在 `QolPluginRuntimeEffect` 尾部增加 `desired_enabled`；runtime event hub/JNI 同时提供 actual 与 `want_run()` desired，Kotlin API listener 采用 `actual_enabled && desired_enabled && valid config`。继续执行 owner-self subscribe/unsubscribe、安全权限 query、descriptor endpoint validate/bind/revoke、optional dependency availability/rebind、`core.api` service 配置与 `module.security` OP migration。通用 config revision/CAS handoff 使用 §6 的既有冻结接口。验证 provider 缺失与恢复、在途 handler/lease drain、重复启停无重复注册、API/OP 四种组合和 OP 执行前拒绝。
3. **Gate 2 / 后续交付**：受影响定向 Host 测试与架构/路由检查通过后，由授权 owner 串行执行 Debug build、部署和设备证据。本合同阶段不运行 Android build、真机/API 写入、full CTest、EB 或 Release。

不得增加 API 专属 CAS、listener-bind-failure native bridge、API 状态 Host 特判或 OP cascade；配置比较并提交统一走 §6 的通用 revision/CAS handoff。

## 8. Phase 2 混合 transport 与 route metadata 合同

Phase 2 route metadata 本身不提升 Host SDK minor，也不提升 `http.registry` / `http.dispatch` capability（保持 v1.1），不改变公开 HTTP method/path/body/status 或 REST schema。当前完整 Host SDK 为 v1.27（settings feature catalog 尾部扩展见 §3、§5；desc hook V2 见 §3）；这不属于 Phase 2 route metadata 变更。现有 147 条路由基数冻结为 68 GET、79 POST，其中 21 条 OP-gated；route metadata 只表达现有路由事实与门禁，不改写 handler 所有权或响应行为。当前运行时 dispatch 盘点为 119 条 native-handled routes、24 条 direct-Kotlin routes 与 4 条 Host-resident control routes，总计仍为 147；这是执行路径统计，与 route source inventory 的 plugin/Host resident owner facts 不同，不替换 method/path/owner/OP/lease 事实。

`HttpRouteMetadataDescriptor` 是单独的轻量 metadata：只有 method、path、兼容 `route_owner` label、OP 与 lease 标志；没有 handler/context，不能伪造空 handler、占位 handler或生成 native dispatch。既有 `HttpEndpointDescriptor` 的 handler 校验保持不变：GET 仍要求 query handler，POST 仍要求 command handler。需要 native handler 的路由继续用 typed endpoint 或保留的 legacy handler 声明；由 Kotlin 通过既有 direct-JNI owner 处理的路由才可只声明 metadata。metadata-only 路由没有 native handler 时继续得到既有 501；OP 拒绝为 403，provider 缺失或 drain 为 503，未声明为 404。

四条 Host resident control route（`/api/config/list`、`/api/config/set`、`/api/config/receipt/{id}`、`/api/op/features/lifecycle`）继续由常驻控制设施处理，runtime owner 为空；三个 config route 的 `settings` label 与 lifecycle route 的 `core.api` label 均保持不变。三条 `/api/debug/{ui,path,item/raw}` 诊断 route 继续由 Host legacy-debug facts/diagnostics facility 提供，`legacy.debug` 只是兼容 label，不创建同名插件。

`/api/debug/settings-ui/inject`、`/api/debug/settings-ui/restore`、`/api/debug/settings-ui/open-option`、`/api/debug/settings-ui/open-panel` 四条现有 settings POST 仍是 native handler 路由，不属于 metadata-only；不得为满足 metadata coverage 更换其 handler、响应或票据行为。所有 route label 保持既有值；实际 lifecycle owner 始终是声明该 route 的插件 descriptor ID，Host resident route 则没有插件 runtime owner。

## 9. Phase 3 当前实现对照

- `core.api` 的 General `api.enabled` 默认 true、priority -5；Debug/Release settings UI 均显示。Kotlin listener 仅在 Host runtime effect 的 actual、desired 都启用且 listener config 有效时启动，desired 关闭时先停止 transport。category/description 不影响运行状态，listener state 不代表 OP authorization state。
- Native/runtime 初始化由 `MainActivity` 的 `onCreate` 与 focus 事件触发；并发真实 Activity 事件 coalesce 到同一个启动 worker，不安排 timer retry。runtime effect 的 initial replay 到达后再 reconcile listener；API listener config 更新且现有 server 在运行时，`ApiServer` 使用可取消的 500ms 单次 reconcile debounce，不是周期轮询。外部 `/api/system/events` 与公开 config receipt API 的客户端轮询仍按其各自 API 契约执行。
- `module.security/op.enabled` 是 General、默认 false、priority -4 的独立 OP authorization。Debug/Release settings UI 均显示。dispatcher 只对 endpoint metadata 标记需要 OP 的 handler 查询该 feature；未授权返回 403。API listener 开启/关闭都不写入或推导该值。实现证据：`host/http_dispatch.cpp` 的 OP check 与 `host/core_settings_config_store.cpp::migrate_host_owner_values` 的 owner-specific migration。
- 当前 optional HTTP 例子包括 `core.data` 和 Debug-only `core.custom_ui`；provider 缺失仅影响其可选 HTTP endpoints。其他消费者依照各自 descriptor 中 `requires`/`optional_requires` 声明。
- Host SDK v1.25 的 `module_health` facts 是只读状态能力；`module_health.enabled` 为 Hidden/default-on，Debug 设置页显示、Release 过滤。它不替代 API availability 或 OP permission。
- 本阶段只同步架构、API、plugin reference、migration 与静态 metadata 断言；没有生产源码或 API schema 改动。近址 thunk mmap 的独立 inspection/evidence 状态记录在 [`audits/infrastructure-20261013/inspection.md`](audits/infrastructure-20261013/inspection.md)。

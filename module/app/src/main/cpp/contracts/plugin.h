#pragma once

// 插件契约（ARCHITECTURE.md §3.3 插件清单、§4.3 资源描述符）。
//
// 契约类型限制：只用 POD 结构体、固定宽度 C 类型与函数指针；禁止 STL 容器、
// 虚函数、异常、std::string。字符串与数组均为静态存储，生命周期 ≥ 进程，
// 宿主注册时直接引用。
//
// 依赖方向：contracts 不依赖任何层。PluginStatusOut.state 只存 §3.2 状态机
// 数值，由宿主映射为 infra 状态类型；运行期能力注册仍走 infra/capabilities.h。

#include <cstdint>

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/version.h"

namespace qol::contracts {

// 资源机制（§4.1）：入口 hook / 调用点 patch / 指针槽 / 行为所有权。
enum class ResourceMechanism : uint8_t {
    kEntryHook = 0,
    kCallPatch = 1,
    kPtrSlot = 2,
    kOwnership = 3,
};

// 资源策略（§4.2）：私有 / 共享。
enum class ResourcePolicy : uint8_t {
    kPrivate = 0,
    kShared = 1,
};

// 资源必要性（§8 资源失败 → 插件状态映射）。
enum class ResourceRequirement : uint8_t {
    kRequired = 0,
    kOptional = 1,
};

// 消费者可恢复错误时的资源行为（§8）：fail-soft 记录并继续 / fail-closed 降级透传。
enum class ResourceFailure : uint8_t {
    kFailSoft = 0,
    kFailClosed = 1,
};

// 资源占用状态（§4.2）：free 可分配 / disabled 空转透传 / live 已服务 / tombstone 保留占用。
enum class ResourceState : uint8_t {
    kFree = 0,
    kDisabled = 1,
    kLive = 2,
    kTombstone = 3,
};

// 稳定英文名，供诊断接口输出；越界返回 "unknown"。
inline const char* resource_state_name(ResourceState state) {
    switch (state) {
        case ResourceState::kFree:
            return "free";
        case ResourceState::kDisabled:
            return "disabled";
        case ResourceState::kLive:
            return "live";
        case ResourceState::kTombstone:
            return "tombstone";
    }
    return "unknown";
}

// 能力需求：id + 版本范围（§3.4）。
struct CapabilityRequirement {
    const char* id;
    QolVersionRange range;
};

// 能力提供声明：id + 版本 + 接口指针（§3.3）。宿主在插件进入 enabled 时按本声明
// 调 infra/capabilities 注册；消费者经 require_capability 取得同一 iface。
struct CapabilityProvisionRef {
    const char* id;
    QolVersion version;
    const void* iface;
};

// 资源声明（= §4.3 ResourceDescriptor）：插件只声明，机制由宿主通用执行。
// handler 是声明式安装的执行体：宿主按 mechanism 选择机制引擎并把 handler
// 交给它，不解释其语义；共享资源为 nullptr。
struct ResourceClaim {
    const char* id;
    const char* symbol;
    ResourceMechanism mechanism;
    ResourcePolicy policy;
    ResourceRequirement requirement;
    ResourceFailure failure;
    bool resumable;
    const char* owner;
    const char* reason;
    const char* gate;
    uint32_t step_budget;
    void* handler;
};

// JNI 绑定元数据（§7.2）：三向校验（Kotlin external ↔ native 导出 ↔ 本表）
// 在 P5 落地；signature_hash 在此之前为 nullptr。
struct JniBinding {
    const char* kotlin_method;
    const char* native_symbol;
    const char* signature_hash;
};

// 插件状态输出（§3.2）：state 为状态机数值，由宿主映射；
// contracts 不依赖 infra，故此处只存 int32_t。
struct PluginStatusOut {
    int32_t state;
    bool enabled;
    const char* error;
};

// 插件回调上下文：迁移期允许 sdk/game == nullptr，供尚未切换到 Host SDK / Game Host ABI 的插件继续运行。
// P5 收口后，sdk/game 为空应视为安装失败；plugin_id 由宿主持有，生命周期 ≥ 进程。
struct PluginContext {
    uint32_t struct_size;
    uint16_t sdk_major;
    uint16_t sdk_minor;
    const QolHostSdk* sdk;
    const QolGameHost* game;
    const char* plugin_id;
};

// 插件清单（§3.3 逐字段）。数量字段显式，不使用哨兵终止。
struct PluginDescriptor {
    const char* id;
    const char* version;
    uint32_t requires_count;
    const CapabilityRequirement* requires;
    uint32_t optional_count;
    const CapabilityRequirement* optional_requires;
    uint32_t provides_count;
    const CapabilityProvisionRef* provides;
    uint32_t resources_count;
    const ResourceClaim* resources;
    uint32_t jni_count;
    const JniBinding* jni;
    int (*install)(PluginContext&);
    void (*deactivate)(PluginContext&);
    void (*on_enabled)(PluginContext&);
    void (*on_disabled)(PluginContext&);
    void (*fill_status)(PluginStatusOut&);
    // 可选；受限生命周期回调（§3.3）：资源安装成功后宿主交还 backup（原函数入口）。
    // 只允许缓存 backup/更新门禁等常数时间操作，禁止资源操作、游戏内存读写与跨线程等待。
    // 返回非 0 → 宿主把该资源按还原释放撤回，并把插件置 failed（reason=resource_ready_failed）。
    int32_t (*on_resource_ready)(PluginContext&, const char* resource_id, void* backup);
};

}  // namespace qol::contracts

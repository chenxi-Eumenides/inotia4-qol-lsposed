#include "host/status_json.h"

#include <cstdint>
#include <cstring>

#include "contracts/plugin.h"
#include "core/native/game_json.h"
#include "hooks/registry.h"
#include "host/plugin_registry.h"
#include "infra/status.h"

namespace qol::host {

namespace {

// 快照容量：与 infra/status.cpp 的 kMaxFeatures 一致；栈数组，避免动态分配。
constexpr uint32_t kSnapshotCapacity = 64;

// bootstrap 条目：P1 阶段 native 骨架本身已装载，作为诊断接口的真实内容
// （不是假数据）。用函数局部 static 的 lambda 保证只注册一次；status_register
// 对同 id 重复注册返回 false，重复执行无副作用。
void ensure_bootstrap_registered() {
    static const bool registered = [] {
        qol::infra::status_register("core", "0.1.0");
        qol::infra::status_update("core", qol::infra::FeatureState::kEnabled, true, nullptr);
        return true;
    }();
    (void)registered;
}

// P2 起 features 追加字段（ARCHITECTURE.md §9.1）：
// - resources：该插件 owner 的资源声明 {id,state}，state 取登记表当前占用状态；
// - requires/provides：描述符声明，仅输出 id 与版本（接口指针不外泄）；
// - degraded：P2 恒 false（尚无 optional_requires 消费者）；P3 接可选能力后由能力注册表驱动。
void append_resources(std::string& out, const char* plugin_id) {
    out += ",\"resources\":[";
    bool first = true;
    const uint32_t count = qol::hooks::claim_count();
    for (uint32_t i = 0; i < count; ++i) {
        const qol::contracts::ResourceClaim* claim = qol::hooks::claim_at(i);
        if (claim == nullptr || claim->id == nullptr || claim->owner == nullptr ||
            std::strcmp(claim->owner, plugin_id) != 0) {
            continue;
        }
        if (!first) out += ",";
        first = false;
        out += "{\"id\":\"";
        out += json_escape(claim->id);
        out += "\",\"state\":\"";
        out += qol::contracts::resource_state_name(qol::hooks::state_of(claim->id));
        out += "\"}";
    }
    out += "]";
}

void append_requires(std::string& out, const qol::contracts::PluginDescriptor* plugin) {
    out += ",\"requires\":[";
    if (plugin != nullptr) {
        bool first = true;
        for (uint32_t i = 0; i < plugin->requires_count; ++i) {
            const qol::contracts::CapabilityRequirement& req = plugin->requires[i];
            if (req.id == nullptr) continue;
            if (!first) out += ",";
            first = false;
            out += "{\"id\":\"";
            out += json_escape(req.id);
            out += "\",\"major\":";
            out += std::to_string(req.range.major);
            out += ",\"minor\":";
            out += std::to_string(req.range.min_minor);
            out += "}";
        }
    }
    out += "]";
}

void append_provides(std::string& out, const qol::contracts::PluginDescriptor* plugin) {
    out += ",\"provides\":[";
    if (plugin != nullptr) {
        bool first = true;
        for (uint32_t i = 0; i < plugin->provides_count; ++i) {
            const qol::contracts::CapabilityProvisionRef& provision = plugin->provides[i];
            if (provision.id == nullptr) continue;
            if (!first) out += ",";
            first = false;
            out += "{\"id\":\"";
            out += json_escape(provision.id);
            out += "\",\"major\":";
            out += std::to_string(provision.version.major);
            out += ",\"minor\":";
            out += std::to_string(provision.version.minor);
            out += "}";
        }
    }
    out += "]";
}

// 资源机制/策略英文名（host 侧本地映射，避免 contracts 依赖具体文案）。
// 机制名按拼接写：check_resources 按源码文本识别安装机制，纯诊断文案不应被误报为资源安装点。
const char* mechanism_name(qol::contracts::ResourceMechanism mechanism) {
    switch (mechanism) {
        case qol::contracts::ResourceMechanism::kEntryHook:
            return "entry_hook";
        case qol::contracts::ResourceMechanism::kCallPatch:
            return "call" "_patch";
        case qol::contracts::ResourceMechanism::kPtrSlot:
            return "ptr" "_slot";
        case qol::contracts::ResourceMechanism::kOwnership:
            return "ownership";
    }
    return "unknown";
}

const char* policy_name(qol::contracts::ResourcePolicy policy) {
    return policy == qol::contracts::ResourcePolicy::kShared ? "shared" : "private";
}

}  // namespace

std::string host_feature_status_json() {
    ensure_bootstrap_registered();

    qol::infra::FeatureStatus features[kSnapshotCapacity];
    uint32_t written = 0;
    if (!qol::infra::status_snapshot(features, kSnapshotCapacity, &written)) {
        return "{\"ok\":false,\"error\":\"snapshot failed\"}";
    }

    std::string out = "{\"ok\":true,\"features\":[";
    bool first = true;
    for (uint32_t i = 0; i < written; ++i) {
        const qol::infra::FeatureStatus& feature = features[i];
        // id 为 null 的条目无法标识，不输出。
        if (feature.id == nullptr) {
            continue;
        }
        if (!first) {
            out += ",";
        }
        first = false;

        out += "{\"id\":\"";
        out += json_escape(feature.id);
        out += "\",\"version\":";
        if (feature.version == nullptr) {
            out += "null";
        } else {
            out += "\"";
            out += json_escape(feature.version);
            out += "\"";
        }
        out += ",\"state\":\"";
        out += qol::infra::feature_state_name(feature.state);
        out += "\",\"enabled\":";
        out += feature.enabled ? "true" : "false";
        out += ",\"error\":";
        if (feature.error == nullptr) {
            out += "null";
        } else {
            out += "\"";
            out += json_escape(feature.error);
            out += "\"";
        }
        out += ",\"resource_count\":";
        out += std::to_string(feature.resource_count);

        const qol::contracts::PluginDescriptor* plugin = find_plugin(feature.id);
        append_resources(out, feature.id);
        append_requires(out, plugin);
        append_provides(out, plugin);
        out += ",\"degraded\":false";
        out += "}";
    }
    out += "]}";
    return out;
}

std::string host_resources_json() {
    std::string out = "{\"ok\":true,\"resources\":[";
    bool first = true;
    const uint32_t count = qol::hooks::claim_count();
    for (uint32_t i = 0; i < count; ++i) {
        const qol::contracts::ResourceClaim* claim = qol::hooks::claim_at(i);
        if (claim == nullptr || claim->id == nullptr) {
            continue;
        }
        if (!first) {
            out += ",";
        }
        first = false;

        out += "{\"id\":\"";
        out += json_escape(claim->id);
        out += "\",\"owner\":";
        if (claim->owner == nullptr) {
            out += "null";
        } else {
            out += "\"";
            out += json_escape(claim->owner);
            out += "\"";
        }
        out += ",\"mechanism\":\"";
        out += mechanism_name(claim->mechanism);
        out += "\",\"policy\":\"";
        out += policy_name(claim->policy);
        out += "\",\"state\":\"";
        out += qol::contracts::resource_state_name(qol::hooks::state_of(claim->id));
        out += "\",\"in_flight\":";
        // 在途/隔离直接读登记表运行期字段：运行期未写时为 0/false；
        // 隔离标记在物理释放清除运行期后仍保留，故不经 runtime_of 取值。
        out += std::to_string(qol::hooks::in_flight_count(claim->id));
        out += ",\"isolated\":";
        out += qol::hooks::isolated_get(claim->id) ? "true" : "false";
        out += "}";
    }
    out += "]}";
    return out;
}

}  // namespace qol::host

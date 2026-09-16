#include "host/status_json.h"

#include <cstdint>

#include "core/native/game_json.h"
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
        out += "}";
    }
    out += "]}";
    return out;
}

}  // namespace qol::host

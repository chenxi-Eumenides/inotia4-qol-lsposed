#include "infra/status.h"

#include <cstring>

namespace qol::infra {

namespace {

// 静态容量上限，避免动态分配。超限注册返回 false，调用方可感知。
constexpr uint32_t kMaxFeatures = 64;

FeatureStatus g_features[kMaxFeatures];

// 按 id 查槽位；未注册返回 kMaxFeatures。
uint32_t find_feature(const char* id) {
    for (uint32_t i = 0; i < kMaxFeatures; ++i) {
        // id 是静态字符串：不同翻译单元的同内容字面量地址可不同，逐字节比较。
        if (g_features[i].id != nullptr && std::strcmp(g_features[i].id, id) == 0) {
            return i;
        }
    }
    return kMaxFeatures;
}

}  // namespace

const char* feature_state_name(FeatureState state) {
    switch (state) {
        case FeatureState::kRegistered: return "registered";
        case FeatureState::kResolving: return "resolving";
        case FeatureState::kBlocked: return "blocked";
        case FeatureState::kUnsupported: return "unsupported";
        case FeatureState::kInvalid: return "invalid";
        case FeatureState::kInstalledDisabled: return "installed_disabled";
        case FeatureState::kEnabled: return "enabled";
        case FeatureState::kDeactivating: return "deactivating";
        case FeatureState::kInactive: return "inactive";
        case FeatureState::kFailed: return "failed";
        case FeatureState::kDegraded: return "degraded";
    }
    // 诊断接口对越界/未知值也不得崩溃或返回空指针。
    return "unknown";
}

bool status_register(const char* id, const char* version) {
    if (id == nullptr) {
        return false;
    }
    if (find_feature(id) != kMaxFeatures) {
        return false;
    }
    for (uint32_t i = 0; i < kMaxFeatures; ++i) {
        if (g_features[i].id == nullptr) {
            FeatureStatus& slot = g_features[i];
            slot.id = id;
            slot.version = version;
            // 状态机起点：已登记、未启用、无错误。
            slot.enabled = false;
            slot.state = FeatureState::kRegistered;
            slot.error = nullptr;
            slot.resource_count = 0;
            return true;
        }
    }
    return false;
}

bool status_update(const char* id, FeatureState state, bool enabled, const char* error) {
    if (id == nullptr) {
        return false;
    }
    const uint32_t slot = find_feature(id);
    if (slot == kMaxFeatures) {
        return false;
    }
    g_features[slot].state = state;
    g_features[slot].enabled = enabled;
    g_features[slot].error = error;  // nullptr 表示清除错误
    return true;
}

uint32_t status_count() {
    uint32_t count = 0;
    for (uint32_t i = 0; i < kMaxFeatures; ++i) {
        if (g_features[i].id != nullptr) {
            ++count;
        }
    }
    return count;
}

bool status_get(const char* id, FeatureStatus* out) {
    if (id == nullptr || out == nullptr) {
        return false;
    }
    const uint32_t slot = find_feature(id);
    if (slot == kMaxFeatures) {
        return false;
    }
    *out = g_features[slot];
    return true;
}

bool status_snapshot(FeatureStatus* out, uint32_t capacity, uint32_t* written) {
    if (out == nullptr || written == nullptr) {
        return false;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < kMaxFeatures && n < capacity; ++i) {
        if (g_features[i].id != nullptr) {
            out[n++] = g_features[i];
        }
    }
    *written = n;
    return true;
}

// 模块级重置：只清本登记表，供隔离断言使用；聚合重置见 infra/test_reset.cpp。
void status_reset_for_test() {
    for (uint32_t i = 0; i < kMaxFeatures; ++i) {
        g_features[i] = FeatureStatus{};
    }
}

}  // namespace qol::infra

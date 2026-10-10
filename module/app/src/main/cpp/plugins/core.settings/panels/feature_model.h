#pragma once

#include "../../../contracts/host_sdk.h"
#include <vector>

namespace qol::features::core_ui::feature_model {
using contracts::FeatureCategory;
using contracts::FeatureType;

struct Item {
    const char* plugin_id;
    const char* feature_id;
    const char* label;
    const char* description;
    FeatureCategory category;
    FeatureType type;
    bool desired;
};

inline bool visible(FeatureCategory category, bool debug) {
    return category != FeatureCategory::kHidden || debug;
}

inline const char* category_title(FeatureCategory category) {
    switch (category) {
        case FeatureCategory::kGeneral: return "基础设置";
        case FeatureCategory::kGameplay: return "优化功能";
        case FeatureCategory::kTools: return "工具入口";
        case FeatureCategory::kHidden: return "隐藏功能（调试）";
    }
    return "其他功能";
}

// Catalog contains static Host-owned metadata only. No business pointer or default is copied.
inline bool build(const contracts::QolFeatureCatalogPlugin* plugins, uint32_t count,
                  bool debug, std::vector<Item>& out) {
    out.clear();
    if (count != 0 && plugins == nullptr) return false;
    for (uint32_t p = 0; p < count; ++p) {
        const auto& plugin = plugins[p];
        if (plugin.plugin_id == nullptr ||
            (plugin.feature_count != 0 && plugin.features == nullptr)) {
            out.clear();
            return false;
        }
        out.push_back({plugin.plugin_id, nullptr, plugin.plugin_id,
                       "控制插件整体启用状态；实际运行状态由宿主决定。",
                       FeatureCategory::kGeneral, FeatureType::kToggle, true});
        for (uint32_t f = 0; f < plugin.feature_count; ++f) {
            const auto& feature = plugin.features[f];
            if (feature.id == nullptr || feature.name == nullptr) {
                out.clear();
                return false;
            }
            if (!visible(feature.category, debug)) continue;
            out.push_back({plugin.plugin_id, feature.id, feature.name,
                           feature.description == nullptr ? "" : feature.description,
                           feature.category, feature.type, false});
        }
    }
    return true;
}

inline bool toggle_enabled(bool valid, uint64_t request, bool gesture) {
    // Actual inactivity never blocks management writes that can restore a plugin.
    return valid && request == 0 && !gesture;
}
inline bool entry_enabled(const Item& item, bool valid, bool enabled,
                          uint64_t request, bool gesture) {
    return !item.desired && item.type == FeatureType::kEntry && valid && enabled &&
           request == 0 && !gesture;
}
}  // namespace qol::features::core_ui::feature_model

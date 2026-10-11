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
    const char* action_label;
    FeatureCategory category;
    FeatureType type;
};

inline bool visible(FeatureCategory category, bool debug) {
    return category != FeatureCategory::kHidden || debug;
}

inline const char* category_title(FeatureCategory category) {
    switch (category) {
        case FeatureCategory::kGeneral: return "基础设置";
        case FeatureCategory::kBasicOptimization: return "基础优化设置";
        case FeatureCategory::kGameplay: return "更多优化功能";
        case FeatureCategory::kTools: return "更多工具";
        case FeatureCategory::kHidden: return "调试开关";
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
        for (uint32_t f = 0; f < plugin.feature_count; ++f) {
            const auto& feature = plugin.features[f];
            if (feature.id == nullptr || feature.name == nullptr) {
                out.clear();
                return false;
            }
            if (!visible(feature.category, debug)) continue;
            out.push_back({plugin.plugin_id, feature.id, feature.name,
                           feature.description == nullptr ? "" : feature.description,
                           feature.action_label == nullptr ? "" : feature.action_label,
                           feature.category, feature.type});
        }
    }
    return true;
}

inline bool toggle_enabled(bool valid, uint64_t request, bool gesture) {
    // Actual inactivity never blocks management writes that can restore a plugin.
    return valid && request == 0 && !gesture;
}
inline bool entry_enabled(const Item& item, bool valid, bool request, bool gesture) {
    return item.type == FeatureType::kEntry && valid && !request && !gesture;
}
}  // namespace qol::features::core_ui::feature_model

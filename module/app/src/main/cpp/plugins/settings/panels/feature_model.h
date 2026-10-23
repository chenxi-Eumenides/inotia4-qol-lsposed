#pragma once

#include "../../../contracts/host_sdk.h"
#include <algorithm>
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
    int8_t display_priority = 0;
};

inline bool visible(FeatureCategory category, bool debug) {
    return category != FeatureCategory::kHidden || debug;
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
                            feature.category, feature.type, feature.display_priority});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Item& a, const Item& b) {
        return a.display_priority < b.display_priority;
    });
    return true;
}

inline bool build_categories(const contracts::FeatureCategoryDefinition* categories,
                             uint32_t count, bool debug, const std::vector<Item>& items,
                             std::vector<contracts::FeatureCategoryDefinition>& out) {
    out.clear();
    if (count != 0 && categories == nullptr) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& category = categories[i];
        if (category.name == nullptr) { out.clear(); return false; }
        if (!visible(category.category, debug)) continue;
        if (category.category == FeatureCategory::kGameInfo) {
            out.push_back(category);
            continue;
        }
        if (std::any_of(items.begin(), items.end(), [&](const Item& item) {
                return item.category == category.category;
            })) out.push_back(category);
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.display_priority < b.display_priority;
    });
    return true;
}

inline bool toggle_enabled(bool valid, uint64_t request, bool gesture) {
    // Actual inactivity never blocks management writes that can restore a plugin.
    return valid && request == 0 && !gesture;
}
inline const char* owner_failure_text(contracts::FeatureOwnerAvailability availability) {
    switch (availability) {
        case contracts::FeatureOwnerAvailability::kAvailable: return "";
        case contracts::FeatureOwnerAvailability::kInstallFailed: return "安装失败";
        case contracts::FeatureOwnerAvailability::kActivationFailed: return "激活失败";
        case contracts::FeatureOwnerAvailability::kBlocked: return "依赖不可用";
        case contracts::FeatureOwnerAvailability::kUnsupported: return "当前版本不支持";
        case contracts::FeatureOwnerAvailability::kInvalid: return "插件无效";
        case contracts::FeatureOwnerAvailability::kFailed: return "插件不可用";
    }
    return "插件不可用";
}
inline bool owner_available(contracts::FeatureOwnerAvailability availability) {
    return availability == contracts::FeatureOwnerAvailability::kAvailable;
}
inline bool toggle_enabled(bool valid, uint64_t request, bool gesture, bool owner_available) {
    return owner_available && toggle_enabled(valid, request, gesture);
}
inline bool entry_enabled(const Item& item, bool valid, bool request, bool gesture,
                          bool owner_available = true) {
    return owner_available && item.type == FeatureType::kEntry && valid && !request && !gesture;
}
}  // namespace qol::features::core_ui::feature_model

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "catalog.h"

// 「宝石强化」呈现文案表（ADR 0001 D3：合成域的配方呈现文案归 custom_recipe）。
// Scope 由 custom_recipe UI 子层持有并作为参数传入；本表不读取 infra 的活动 scope。

namespace qol::features::custom_recipe {

inline constexpr int32_t kTextScopeRecipeButton = 2;
inline constexpr int32_t kTextScopePanelTitle = 3;
inline constexpr int32_t kTextScopePageTab = 4;
inline constexpr int32_t kTextScopeQualityUnavailable = 5;
inline constexpr int32_t kTextScopeMaterialMayHaveBeenConsumed = 6;
inline constexpr int32_t kTextScopeProductPrepareFailed = 7;
// 本插件私有 scope（不进入 Host scope；仅供本插件提示使用）：
inline constexpr int32_t kTextScopeRecipeMismatch = 8;    // 配方未命中
inline constexpr int32_t kTextScopeCraftUnavailable = 9;  // 已匹配但上下文不可用 / 弹窗创建失败
inline constexpr int32_t kTextScopeStateChanged = 10;      // 确认期间格子/代际已变化
inline constexpr int32_t kTextScopePlaceWriteFailed = 11;  // 放料写入失败

// Popup uses known text-database ids; the private local scopes limit replacement to this call.
inline constexpr uint32_t kQualityUnavailablePopupId = 97;
inline constexpr uint32_t kMaterialMayHaveBeenConsumedPopupId = 94;
inline constexpr uint32_t kProductPrepareFailedPopupId = 94;
inline constexpr uint32_t kRecipeMismatchPopupId = 94;
inline constexpr uint32_t kCraftUnavailablePopupId = 94;
inline constexpr uint32_t kStateChangedPopupId = 94;
inline constexpr uint32_t kPlaceWriteFailedPopupId = 99;

struct TextEntry {
    const char* key;              // 语义键（唯一）
    uint32_t text_id;             // 被替换的游戏 text id
    int32_t scope;                // custom_recipe UI 本地 scope
    const char* text;             // 替换文本（nullptr = 保持原文）
};

inline constexpr TextEntry kTextEntries[] = {
    {"recipe.jewel_tier_up", ::custom_recipe::kJewelTierUpLabelWordId,
     kTextScopeRecipeButton, "宝石升阶"},  // UIMix 配方按钮
    {"recipe.jewel_tier_up.title", ::custom_recipe::kJewelTierUpLabelWordId,
     kTextScopePanelTitle, "宝石升阶"},  // 面板标题框
    {"quality.unavailable", kQualityUnavailablePopupId, kTextScopeQualityUnavailable,
     "功能不可用"},
    {"craft.material_may_have_been_consumed", kMaterialMayHaveBeenConsumedPopupId,
     kTextScopeMaterialMayHaveBeenConsumed, "合成失败，材料可能已扣，请检查背包。"},
    {"craft.product_prepare_failed", kProductPrepareFailedPopupId,
     kTextScopeProductPrepareFailed, "产物准备失败，材料未扣。"},
    {"craft.recipe_mismatch", kRecipeMismatchPopupId, kTextScopeRecipeMismatch,
     "配方不匹配，请检查材料组合。"},
    {"craft.unavailable", kCraftUnavailablePopupId, kTextScopeCraftUnavailable,
     "合成暂不可用，材料未扣。"},
    {"craft.state_changed", kStateChangedPopupId, kTextScopeStateChanged,
     "合成状态已变化，请重新填入材料。"},
    {"craft.place_write_failed", kPlaceWriteFailedPopupId, kTextScopePlaceWriteFailed,
     "放入失败，请重试。"},
};

inline constexpr size_t kTextEntryCount = sizeof(kTextEntries) / sizeof(kTextEntries[0]);

inline const TextEntry* text_entries(size_t* out_count) {
    if (out_count != nullptr) *out_count = kTextEntryCount;
    return kTextEntries;
}

// 查表：scope 与 id 同时命中且文案非空时返回字面量，否则 nullptr。
inline const char* text_lookup(int32_t scope, uint32_t text_id) {
    for (size_t i = 0; i < kTextEntryCount; ++i) {
        if (kTextEntries[i].text_id != text_id) continue;
        if (kTextEntries[i].scope != scope) continue;
        return kTextEntries[i].text;
    }
    return nullptr;
}

inline const TextEntry* text_entry_by_key(const char* key) {
    if (key == nullptr) return nullptr;
    for (size_t i = 0; i < kTextEntryCount; ++i) {
        if (kTextEntries[i].key != nullptr && std::strcmp(kTextEntries[i].key, key) == 0) {
            return &kTextEntries[i];
        }
    }
    return nullptr;
}

}  // namespace qol::features::custom_recipe

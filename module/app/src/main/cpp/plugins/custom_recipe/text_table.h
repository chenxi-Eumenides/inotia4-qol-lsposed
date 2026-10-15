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

// ---- 两个不同的 id 域（不可混用）----
//
// ① 弹窗索引域：传给 UIPopupMsg_CreateOKFromTextData 的参数（与 ui/feature.h 的 kText* 同值），
//    决定游戏弹哪个既有弹窗；它**不**是消费者的匹配键。
inline constexpr uint32_t kPopupIndexOnlyJewel = 97;             // 原版「只有宝石道具才可以。」
inline constexpr uint32_t kPopupIndexMaterialInsufficient = 94;  // 原版「材料不足。无法进行合成。」
inline constexpr uint32_t kPopupIndexAlreadyPlaced = 99;         // 原版「该宝石已被添加。」
//
// ② MEMORYTEXT 域：游戏侧 UIPopupMsg_CreateOKFromTextData(index) 先读 TEXTDATABASE 第 index 行
//    的 u16，再以该值调 MEMORYTEXT_GetText(id) —— 消费者看到的 text_id 是这个 u16。
inline constexpr uint32_t kMemoryTextOnlyJewel = 35413;             // TEXTDATABASE row 97
inline constexpr uint32_t kMemoryTextMaterialInsufficient = 35410;  // TEXTDATABASE row 94
inline constexpr uint32_t kMemoryTextAlreadyPlaced = 35415;         // TEXTDATABASE row 99

// 弹窗索引 → MEMORYTEXT id 的显式映射（用于诊断日志与测试交叉核对）。
// 证据：TEXTDATABASE（表 index 73，record_size 2）row 94/97/99 的 u16 = 35410/35413/35415；
// 工程 raw 导出与三份 supported 输入 APK 的 game_res/game.dat.jpg 在这些行上一致。
struct PopupTextId {
    uint32_t popup_index;
    uint32_t memory_text_id;
};

inline constexpr PopupTextId kPopupTextIds[] = {
    {kPopupIndexMaterialInsufficient, kMemoryTextMaterialInsufficient},
    {kPopupIndexOnlyJewel, kMemoryTextOnlyJewel},
    {kPopupIndexAlreadyPlaced, kMemoryTextAlreadyPlaced},
};

inline constexpr size_t kPopupTextIdCount = sizeof(kPopupTextIds) / sizeof(kPopupTextIds[0]);

// 未知索引返回 0（调用方据此不做替换；不猜测其它弹窗的 id）。
inline constexpr uint32_t popup_memory_text_id(uint32_t popup_index) {
    for (size_t i = 0; i < kPopupTextIdCount; ++i) {
        if (kPopupTextIds[i].popup_index == popup_index) return kPopupTextIds[i].memory_text_id;
    }
    return 0;
}

struct TextEntry {
    const char* key;         // 语义键（唯一）
    uint32_t text_id;        // 消费者匹配键：MEMORYTEXT id（② 域）
    int32_t scope;           // custom_recipe UI 本地 scope
    const char* text;        // 替换文本（nullptr = 保持原文）
};

inline constexpr TextEntry kTextEntries[] = {
    {"recipe.jewel_tier_up", ::custom_recipe::kJewelTierUpLabelWordId,
     kTextScopeRecipeButton, "宝石升阶"},  // UIMix 配方按钮（该路径直接以 wordId 调 GetText）
    {"recipe.jewel_tier_up.title", ::custom_recipe::kJewelTierUpLabelWordId,
     kTextScopePanelTitle, "宝石升阶"},  // 面板标题框
    {"quality.unavailable", kMemoryTextOnlyJewel, kTextScopeQualityUnavailable,
     "功能不可用"},  // 弹窗索引 97
    {"craft.material_may_have_been_consumed", kMemoryTextMaterialInsufficient,
     kTextScopeMaterialMayHaveBeenConsumed, "合成失败，材料可能已扣，请检查背包。"},  // 索引 94
    {"craft.product_prepare_failed", kMemoryTextMaterialInsufficient,
     kTextScopeProductPrepareFailed, "产物准备失败，材料未扣。"},  // 索引 94
    {"craft.recipe_mismatch", kMemoryTextMaterialInsufficient, kTextScopeRecipeMismatch,
     "配方不匹配，请检查材料组合。"},  // 索引 94
    {"craft.unavailable", kMemoryTextMaterialInsufficient, kTextScopeCraftUnavailable,
     "合成暂不可用，材料未扣。"},  // 索引 94
    {"craft.state_changed", kMemoryTextMaterialInsufficient, kTextScopeStateChanged,
     "合成状态已变化，请重新填入材料。"},  // 索引 94
    {"craft.place_write_failed", kMemoryTextAlreadyPlaced, kTextScopePlaceWriteFailed,
     "放入失败，请重试。"},  // 索引 99
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

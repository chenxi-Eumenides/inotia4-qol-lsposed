#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "catalog.h"

// 「宝石强化」呈现文案表（ADR 0001 D3：合成域的配方呈现文案归 custom_recipe）。
//
// 该 id（35291）同时是宝石强化页的页签名，作用域 2/3 由 craft_ui 在 UIMix 界面推入；
// 文案表里不存在这一串，因此只能由模块提供。页签作用域**不登记**，保证页签名保持原文。
//
// 表与查表是纯逻辑（不触游戏内存），编入 host 单测。插件源码不 include infra/（依赖矩阵），
// 故此处对 TextScope 只做不透明声明；真正的枚举值定义在 infra/text_scope.h，由宿主/测试提供。

namespace qol::infra {
enum class TextScope : uint8_t;
TextScope active_text_scope();
}  // namespace qol::infra

namespace qol::features::custom_recipe {

struct TextEntry {
    const char* key;              // 语义键（唯一）
    uint16_t text_id;             // 被替换的游戏 text id
    qol::infra::TextScope scope;  // 生效窗口（与 Host SDK scope 数值一致）
    const char* text;             // 替换文本（nullptr = 保持原文）
};

inline constexpr TextEntry kTextEntries[] = {
    {"recipe.jewel_tier_up", ::custom_recipe::kJewelTierUpLabelWordId,
     static_cast<qol::infra::TextScope>(2), "宝石升阶"},  // UIMix 配方按钮
    {"recipe.jewel_tier_up.title", ::custom_recipe::kJewelTierUpLabelWordId,
     static_cast<qol::infra::TextScope>(3), "宝石升阶"},  // 面板标题框
};

inline constexpr size_t kTextEntryCount = sizeof(kTextEntries) / sizeof(kTextEntries[0]);

inline const TextEntry* text_entries(size_t* out_count) {
    if (out_count != nullptr) *out_count = kTextEntryCount;
    return kTextEntries;
}

// 查表：text_id 命中、scope == active、且 text != nullptr 时返回字面量；否则 nullptr。
inline const char* text_lookup(uint16_t text_id, qol::infra::TextScope active) {
    if (static_cast<uint8_t>(active) == 0) return nullptr;
    for (size_t i = 0; i < kTextEntryCount; ++i) {
        if (kTextEntries[i].text_id != text_id) continue;
        if (kTextEntries[i].scope != active) continue;
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

// 按当前线程作用域查表（after-Modify 消费者回调体的纯逻辑部分，可在 host 单测直接断言）。
inline const char* recipe_owner_lookup(uint32_t text_id) {
    return text_lookup(static_cast<uint16_t>(text_id), qol::infra::active_text_scope());
}

}  // namespace qol::features::custom_recipe

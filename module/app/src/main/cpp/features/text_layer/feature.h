#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "contracts/plugin.h"
#include "contracts/text_layer.h"
#include "infra/text_scope.h"

// 模块自定义文本层（text_layer 插件，ARCHITECTURE.md §3.3/§4.5）。
//
// 提供共享资源 `hook:MEMORYTEXT_GetText`（全游戏唯一文本读取热点，232 调用点）与
// text_layer v1 能力。**行为等价于原 feature/ui/module_text**：
//   wrapper：先调原函数 → 查本层内置表（按当前线程作用域）→ 命中则用模块字面量，否则用原值；
//   在此基础上把最终基准交给 hooks/dispatch 三段式分派（无消费者时逐字等价）。
//
// 消费者不再各自挂 GetText（LSPosed 同址二次挂载会失败），改经能力 subscribe；
// 本文件的表与查表逻辑是纯逻辑（不触游戏内存），编入 host 单测。

namespace qol::features::text_layer {

// 资源 id：plugin.cpp 的 ResourceClaim 与 wrapper 的分派键共用。
inline constexpr const char* kResourceIdGetText = "hook:MEMORYTEXT_GetText";

struct Entry {
    const char* key;              // 语义键（唯一；下方 host test 断言唯一性）
    uint16_t text_id;             // 被替换的游戏 text id
    qol::infra::TextScope scope;  // 生效窗口
    // nullptr = 明确保持游戏原文（占位用，维持连续 id 区间不被打破），不做替换。
    const char* text;
};

// ---- 内置文本表（唯一真源）----
//
// ① 角色面板战斗属性标签（kCharacterPanel）：游戏文本表漏翻了 11 个属性缩写。
//    35181 LV / 35182 EXP / 35183 HP / 35184 MP **保持原文**；
//    35188 M. DEF 面板未显示，显式登记 nullptr 占位以维持 35185..35196 连续区间。
//    文案不超过四个汉字：物攻 / 法攻 / 防御力 / 暴击率 / 命中率 / 爆伤 /
//    物抗 / 法抗 / 闪避率 / 武器格挡 / 盾牌格挡。35192 的含义为 P.RES = 物理抗性。
// ② UIMix 配方按钮（kUimixRecipeButton）：模块「宝石强化」条目改为显示「宝石升阶」。
//    该 id（35291）同时是宝石强化页的**页签名**，而页签由 UIMix_ButtonMenuListDraw 绘制、
//    不在本作用域内，故只有配方按钮改名（见 lookup 矩阵用例）。文案表里不存在这一串，
//    因此只能由模块提供。
inline constexpr Entry kEntries[] = {
    {"charinfo.dmg", 35185, qol::infra::TextScope::kCharacterPanel, "物攻"},        // DMG
    {"charinfo.magic_dmg", 35186, qol::infra::TextScope::kCharacterPanel, "法攻"},  // M. DMG
    {"charinfo.def", 35187, qol::infra::TextScope::kCharacterPanel, "防御力"},      // DEF
    {"charinfo.magic_def", 35188, qol::infra::TextScope::kCharacterPanel, nullptr},  // 面板未显示
    {"charinfo.crit_rate", 35189, qol::infra::TextScope::kCharacterPanel, "暴击率"},
    {"charinfo.hit_rate", 35190, qol::infra::TextScope::kCharacterPanel, "命中率"},
    {"charinfo.crit_dmg", 35191, qol::infra::TextScope::kCharacterPanel, "爆伤"},
    {"charinfo.phys_res", 35192, qol::infra::TextScope::kCharacterPanel, "物抗"},
    {"charinfo.magic_res", 35193, qol::infra::TextScope::kCharacterPanel, "法抗"},
    {"charinfo.evade", 35194, qol::infra::TextScope::kCharacterPanel, "闪避率"},
    {"charinfo.weapon_block", 35195, qol::infra::TextScope::kCharacterPanel, "武器格挡"},
    {"charinfo.shield_block", 35196, qol::infra::TextScope::kCharacterPanel, "盾牌格挡"},
    {"recipe.jewel_tier_up", 35291, qol::infra::TextScope::kUimixRecipeButton, "宝石升阶"},
    {"recipe.jewel_tier_up.title", 35291, qol::infra::TextScope::kUimixPanelTitle, "宝石升阶"},
};

inline constexpr size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

// 编译期保证角色面板段位于表首、连续升序且恰好覆盖 35185..35196（无空洞）。
inline constexpr uint16_t kCharinfoIdMin = 35185;
inline constexpr uint16_t kCharinfoIdMax = 35196;
inline constexpr size_t kCharinfoIdCount = static_cast<size_t>(kCharinfoIdMax - kCharinfoIdMin) + 1;
constexpr bool charinfo_block_ok() {
    for (size_t i = 0; i < kCharinfoIdCount; ++i) {
        if (kEntries[i].scope != qol::infra::TextScope::kCharacterPanel) return false;
        if (kEntries[i].text_id != static_cast<uint16_t>(kCharinfoIdMin + i)) return false;
    }
    return kEntryCount > kCharinfoIdCount &&
           kEntries[kCharinfoIdCount].scope != qol::infra::TextScope::kCharacterPanel;
}
static_assert(charinfo_block_ok(),
              "角色面板文本段必须位于表首并覆盖 35185..35196 连续升序、无空洞");

inline const Entry* entries(size_t* out_count) {
    if (out_count != nullptr) *out_count = kEntryCount;
    return kEntries;
}

// 查表：text_id 命中、scope == active、且 text != nullptr 时返回字面量；否则 nullptr。
inline const char* lookup(uint16_t text_id, qol::infra::TextScope active) {
    if (active == qol::infra::TextScope::kNone) return nullptr;
    for (size_t i = 0; i < kEntryCount; ++i) {
        if (kEntries[i].text_id != text_id) continue;
        if (kEntries[i].scope != active) continue;
        return kEntries[i].text;  // 可能是 nullptr（显式保持原文）
    }
    return nullptr;
}

inline const Entry* entry_by_key(const char* key) {
    if (key == nullptr) return nullptr;
    for (size_t i = 0; i < kEntryCount; ++i) {
        if (kEntries[i].key != nullptr && std::strcmp(kEntries[i].key, key) == 0) {
            return &kEntries[i];
        }
    }
    return nullptr;
}

// 业务门禁：true = 表替换与能力查询生效；false = 撤销期，wrapper 透传原函数。
void set_active(bool active);
bool active();

// 宿主在资源安装成功后交还 backup（原函数入口）；未知 id 忽略，恒返回 0。
int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup);

// 能力接口实现：转发 hooks/dispatch（phase/kind 分别取 DispatchPhase/ConsumerKind 数值）。
uint64_t api_subscribe(int32_t phase, int32_t kind, const char* (*fn)(uint32_t, const char*, void*),
                       void* ctx);
void api_unsubscribe(uint64_t id);

// 共享资源 wrapper：由 plugin_descriptor 的 ResourceClaim.handler 声明，宿主安装。
const char* memorytext_get_text_wrapper(uint16_t text_id);

}  // namespace qol::features::text_layer

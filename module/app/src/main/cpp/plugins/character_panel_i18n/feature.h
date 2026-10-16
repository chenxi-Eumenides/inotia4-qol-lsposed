#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../../contracts/plugin.h"
#include "../../contracts/text_layer.h"

// 角色面板汉化（character_panel_i18n，ADR 0001 D3/D4）：本插件是**内容贡献者**，
// 文案表由本插件持有；`core.custom_ui` 通过保留 ID 的 `text_layer` 能力提供文本机制。
//
// 本插件同时持有角色面板绘制窗口 hook（`hook:Scene_Draw_Popup_SC_CharacterInfo`，见
// charinfo_scope.*），把该绘制窗口标记为 kCharacterPanel 作用域；文本机制只在当前线程
// 处于该作用域时替换 35185..35196 的返回值，其它界面一律原样。
//
// `MEMORYTEXT_GetText` 由 core.custom_ui 持有唯一 hook，避免同一地址重复挂载。

namespace qol::features::character_panel_i18n {

struct Entry {
    const char* key;              // 语义键（唯一）
    uint16_t text_id;             // 被替换的游戏 text id
    int32_t scope;                // 生效窗口（与 Host SDK scope 数值一致）
    // nullptr = 明确保持游戏原文（占位用，维持连续 id 区间不被打破），不做替换。
    const char* text;
};

// ---- 角色面板战斗属性标签表（本插件的唯一真源）----
//
// 游戏文本表漏翻了 11 个属性缩写：35181 LV / 35182 EXP / 35183 HP / 35184 MP **保持原文**；
// 35188 M. DEF 面板未显示，显式登记 nullptr 占位以维持 35185..35196 连续区间。
// 文案不超过四个汉字：物攻 / 法攻 / 防御力 / 暴击率 / 命中率 / 爆伤 /
// 物抗 / 法抗 / 闪避率 / 武器格挡 / 盾牌格挡。35192 的含义为 P.RES = 物理抗性。
inline constexpr Entry kEntries[] = {
    {"charinfo.dmg", 35185, contracts::kTextScopeCharacterPanel, "物攻"},        // DMG
    {"charinfo.magic_dmg", 35186, contracts::kTextScopeCharacterPanel, "法攻"},  // M. DMG
    {"charinfo.def", 35187, contracts::kTextScopeCharacterPanel, "防御力"},      // DEF
    {"charinfo.magic_def", 35188, contracts::kTextScopeCharacterPanel, nullptr},  // 面板未显示
    {"charinfo.crit_rate", 35189, contracts::kTextScopeCharacterPanel, "暴击率"},
    {"charinfo.hit_rate", 35190, contracts::kTextScopeCharacterPanel, "命中率"},
    {"charinfo.crit_dmg", 35191, contracts::kTextScopeCharacterPanel, "爆伤"},
    {"charinfo.phys_res", 35192, contracts::kTextScopeCharacterPanel, "物抗"},
    {"charinfo.magic_res", 35193, contracts::kTextScopeCharacterPanel, "法抗"},
    {"charinfo.evade", 35194, contracts::kTextScopeCharacterPanel, "闪避率"},
    {"charinfo.weapon_block", 35195, contracts::kTextScopeCharacterPanel, "武器格挡"},
    {"charinfo.shield_block", 35196, contracts::kTextScopeCharacterPanel, "盾牌格挡"},
};

inline constexpr size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

// 编译期保证连续升序覆盖 35185..35196（无空洞、无越界）。
inline constexpr uint16_t kCharinfoIdMin = 35185;
inline constexpr uint16_t kCharinfoIdMax = 35196;
inline constexpr size_t kCharinfoIdCount =
    static_cast<size_t>(kCharinfoIdMax - kCharinfoIdMin) + 1;
constexpr bool charinfo_block_ok() {
    if (kEntryCount != kCharinfoIdCount) return false;
    for (size_t i = 0; i < kCharinfoIdCount; ++i) {
        if (kEntries[i].scope != contracts::kTextScopeCharacterPanel) return false;
        if (kEntries[i].text_id != static_cast<uint16_t>(kCharinfoIdMin + i)) return false;
    }
    return true;
}
static_assert(charinfo_block_ok(),
              "角色面板文本段必须覆盖 35185..35196 连续升序、无空洞");

inline const Entry* entries(size_t* out_count) {
    if (out_count != nullptr) *out_count = kEntryCount;
    return kEntries;
}

// 查表：text_id 命中、scope == active、且 text != nullptr 时返回字面量；否则 nullptr。
inline const char* lookup(uint16_t text_id, int32_t active) {
    if (active == contracts::kTextScopeNone) return nullptr;
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

// 宿主 SDK（install 注入）。
void bind_sdk(const contracts::QolHostSdk* sdk);
const contracts::QolHostSdk* host_sdk();

// 当前线程作用域镜像（由 charinfo_scope 的绘制窗口 wrapper 维护）。
int32_t active_scope_value();
void set_active_scope_value(int32_t value);

// 表查找消费者（after 阶段 Modify）：按当前线程作用域查表；命中返回字面量，未命中 nullptr。
const char* owner_table_modify(uint32_t text_id, const char* current, void* ctx);

// 经 `text_layer` 能力注册/注销本插件的表消费者（install/deactivate；幂等）。
bool register_consumers();
void unregister_consumers();

inline constexpr int32_t kOwnerTablePhase = contracts::kTextPhaseAfter;
inline constexpr int32_t kOwnerTableKind = contracts::kTextConsumerModify;

}  // namespace qol::features::character_panel_i18n

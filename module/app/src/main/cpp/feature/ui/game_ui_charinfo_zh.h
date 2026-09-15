#pragma once

#include <cstddef>
#include <cstdint>

// 角色属性面板（POPUP_SC_CHARACTER_INFO）战斗属性行标签中文化。
//
// 游戏文本表漏翻了 11 个属性缩写（DMG / M. DMG / …）。本功能只在角色面板每帧绘制
// （Scene_Draw_POPUP_SC_CHARACTER_INFO）执行期间，把 MEMORYTEXT_GetText 命中的白名单
// text id 的返回值替换为中文；其它界面、其它线程一律返回原文本。
//
// 纯查表逻辑（charinfo_zh::）不触任何游戏内存，编入 host 单测；hook wrapper 见 .cpp。
// 设计依据：docs/reference/game/ui.md §5（文本表）、architecture.md §2.2.1（Native Hook 选型）。

namespace charinfo_zh {

struct Entry {
    uint16_t text_id;
    // nullptr：该 id 明确保持游戏原文（面板上显示英文缩写），不做替换。
    const char* zh;
};

// 替换表覆盖的连续 id 区间（表内不得有空洞，故用区间端点做 O(1) 下标）。
// inline constexpr（C++17）：header-inline 的 lookup 在多个 TU 中 odr-use 同一实体，避免 ODR 歧义。
inline constexpr uint16_t kTextIdMin = 35185;
inline constexpr uint16_t kTextIdMax = 35196;

// 唯一事实来源：35181 LV / 35182 EXP / 35183 HP / 35184 MP 按用户裁决保持原文；
// 35188 M. DEF 面板未显示，显式登记为不替换（占位维持区间连续）。
inline constexpr Entry kEntries[] = {
    {35185, "伤害"},  // DMG
    {35186, "魔攻"},  // M. DMG
    {35187, "防御"},  // DEF
    {35188, nullptr}, // M. DEF（面板未显示，不替换）
    {35189, "暴击"},  // CRT
    {35190, "命中"},  // H.RATE
    {35191, "爆伤"},  // C.DMG
    {35192, "毒抗"},  // P.RES
    {35193, "魔抗"},  // M. RES
    {35194, "回避"},  // EVD
    {35195, "武防"},  // W.D.R
    {35196, "盾防"},  // S.D.R
};

inline constexpr size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

namespace detail {
// 编译期保证「下标 = text_id - kTextIdMin」成立：表必须按 id 升序且无空洞。
constexpr bool table_is_contiguous() {
    for (size_t i = 0; i < kEntryCount; ++i) {
        if (kEntries[i].text_id != static_cast<uint16_t>(kTextIdMin + i)) return false;
    }
    return true;
}
}  // namespace detail

static_assert(detail::table_is_contiguous(),
              "charinfo_zh 替换表必须覆盖 kTextIdMin..kTextIdMax 的连续升序 id 区间");

// MEMORYTEXT_GetText 是全游戏热点函数（232 调用点/每帧多次）：一次范围比较 + 一次数组下标，
// 零分配、零循环、无字符串比较。命中且登记了中文才返回非空。
inline const char* lookup(uint16_t text_id) {
    if (text_id < kTextIdMin || text_id > kTextIdMax) return nullptr;
    return kEntries[text_id - kTextIdMin].zh;
}

}  // namespace charinfo_zh

// 安装两处 LSPosed Native Hook（幂等；bridge 未就绪时静默返回，可后续重试）。
// 调用时机：nativeInit 在 bridge_init 成功后。
void charinfo_zh_install_if_ready();

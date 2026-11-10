#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

// 属性总览的纯数据模型：分区行表、分页映射与数值格式化。
//
// 本头不依赖 Host SDK / Game Host / 游戏内存，可在 host 单测中直接断言
// 行数、页边界与格式化结果；overview.cpp 只负责读取与绘制。
//
// 分页规则：每页最多 10 行；分区超过 10 行时按顺序拆到后续页（"后续页顺延"），
// 页数由分区行数推导。当前 61 行 → 8 页（2 + 3 + 2 + 1）。

namespace qol::features::character_panel {

// 读取来源：属性 id / 主属性下标 / 面板派生 getter / 纯说明行。
enum class RowKind : uint8_t {
    kAttrInt,          // character_read.attr，原值整数
    kAttrMilli,        // character_read.attr，千分比 → raw/10 保留 1 位小数 + %
    kMainStat,         // character_read.main_stat(index)
    kDefense,          // attr17*(attr18+1000)/1000
    kNormalDamage,     // character_read.normal_damage
    kNormalMagicDamage,
    kHitRate,          // hit_rate_1000（千分比）
    kEvasion,          // evasion_rate_1000（千分比）
    kDamageReduce,     // display_damage_reduce_rate（千分比）
    kText,             // 纯说明行，无数值
};

struct RowSpec {
    const char* label;
    RowKind kind;
    int32_t arg;  // kAttrInt/kAttrMilli → attr id；kMainStat → index；其它不用
};

inline constexpr RowSpec kPanelRows[] = {
    {"力量", RowKind::kMainStat, 0},
    {"敏捷", RowKind::kMainStat, 1},
    {"体力", RowKind::kMainStat, 2},
    {"智力", RowKind::kMainStat, 3},
    {"精力", RowKind::kMainStat, 4},
    {"物攻", RowKind::kNormalDamage, 0},
    {"法攻", RowKind::kNormalMagicDamage, 0},
    {"防御力", RowKind::kDefense, 0},
    {"暴击率", RowKind::kAttrMilli, 0},  // attr 0
    {"命中率", RowKind::kHitRate, 0},
    {"爆伤", RowKind::kAttrMilli, 3},  // attr 3
    {"物抗", RowKind::kDamageReduce, 0},
    {"法抗", RowKind::kAttrMilli, 11},  // attr 11
    {"闪避率", RowKind::kEvasion, 0},
    {"武器格挡", RowKind::kAttrMilli, 19},  // attr 19
    {"盾牌格挡", RowKind::kAttrMilli, 20},  // attr 20
};

// 隐藏属性：数值按 attr 原值显示（33 的标签已注明千分比口径）。
inline constexpr RowSpec kHiddenRows[] = {
    {"HP吸收(千分比)", RowKind::kAttrInt, 33},
    {"固定吸血", RowKind::kAttrInt, 36},
    {"治疗增幅", RowKind::kAttrInt, 42},
    {"双倍攻击概率", RowKind::kAttrInt, 52},
    {"重击概率", RowKind::kAttrInt, 66},
    {"增伤阶段", RowKind::kAttrInt, 71},
    {"命中嘲讽", RowKind::kAttrInt, 72},
    {"反伤", RowKind::kAttrInt, 76},
    {"移动速度", RowKind::kAttrInt, 91},
    {"武器攻击帧", RowKind::kAttrInt, 93},
    {"仇恨系数", RowKind::kAttrInt, 94},
    {"嘲讽概率", RowKind::kAttrInt, 98},
    {"蓝耗增减", RowKind::kAttrInt, 32},
    {"命中回蓝", RowKind::kAttrInt, 63},
    {"命中固定回蓝", RowKind::kAttrInt, 112},
    {"额外普攻开关", RowKind::kAttrInt, 106},
    {"额外普攻增幅", RowKind::kAttrInt, 109},
    {"命中沉默", RowKind::kAttrInt, 111},
    {"反击减益", RowKind::kAttrInt, 110},
    {"HP上限", RowKind::kAttrInt, 30},
    {"MP上限", RowKind::kAttrInt, 31},
    {"MP恢复", RowKind::kAttrInt, 29},
};

inline constexpr RowSpec kElementRows[] = {
    {"火", RowKind::kAttrInt, 22},
    {"风", RowKind::kAttrInt, 23},
    {"寒气", RowKind::kAttrInt, 24},
    {"神圣", RowKind::kAttrInt, 25},
    {"黑暗", RowKind::kAttrInt, 26},
    {"毒", RowKind::kAttrInt, 27},
    {"重力摆", RowKind::kAttrInt, 78},
    {"冰霜", RowKind::kAttrInt, 79},
    {"治愈气息", RowKind::kAttrInt, 80},
    {"狂战士", RowKind::kAttrInt, 81},
    {"瞬间恢复", RowKind::kAttrInt, 82},
    {"魔力专家", RowKind::kAttrInt, 83},
    {"减少敌意值", RowKind::kAttrInt, 84},
    {"失明抗性", RowKind::kAttrInt, 85},
    {"睡眠抗性", RowKind::kAttrInt, 86},
    {"眩晕抗性", RowKind::kAttrInt, 87},
    {"恐惧抗性", RowKind::kAttrInt, 88},
    {"沉默抗性", RowKind::kAttrInt, 89},
    {"减速抗性", RowKind::kAttrInt, 90},
};

inline constexpr RowSpec kGlobalRows[] = {
    {"掉落率", RowKind::kText, 0},
    {"经验加成", RowKind::kText, 0},
    {"金币获得", RowKind::kText, 0},
    {"幸运", RowKind::kText, 0},
};

struct SectionSpec {
    const char* title;
    const RowSpec* rows;
    uint32_t count;
};

inline constexpr SectionSpec kSections[] = {
    {"主属性与面板属性", kPanelRows,
     static_cast<uint32_t>(sizeof(kPanelRows) / sizeof(kPanelRows[0]))},
    {"隐藏属性", kHiddenRows,
     static_cast<uint32_t>(sizeof(kHiddenRows) / sizeof(kHiddenRows[0]))},
    {"元素与抗性", kElementRows,
     static_cast<uint32_t>(sizeof(kElementRows) / sizeof(kElementRows[0]))},
    {"全局加成", kGlobalRows,
     static_cast<uint32_t>(sizeof(kGlobalRows) / sizeof(kGlobalRows[0]))},
};

inline constexpr uint32_t kSectionCount = static_cast<uint32_t>(sizeof(kSections) /
                                                                sizeof(kSections[0]));
inline constexpr uint32_t kMaxRowsPerPage = 10;
// 快照容量上限；分区行数增大时需要同步调整。
inline constexpr uint32_t kMaxSectionRows = 32;
inline constexpr uint32_t kPanelRowCount =
    static_cast<uint32_t>(sizeof(kPanelRows) / sizeof(kPanelRows[0]));
inline constexpr uint32_t kHiddenRowCount =
    static_cast<uint32_t>(sizeof(kHiddenRows) / sizeof(kHiddenRows[0]));
inline constexpr uint32_t kElementRowCount =
    static_cast<uint32_t>(sizeof(kElementRows) / sizeof(kElementRows[0]));
inline constexpr uint32_t kGlobalRowCount =
    static_cast<uint32_t>(sizeof(kGlobalRows) / sizeof(kGlobalRows[0]));
inline constexpr uint32_t kTotalRowCount =
    kPanelRowCount + kHiddenRowCount + kElementRowCount + kGlobalRowCount;

static_assert(kPanelRowCount <= kMaxSectionRows, "主属性分区行数超过快照容量");
static_assert(kHiddenRowCount <= kMaxSectionRows, "隐藏属性行数超过快照容量");
static_assert(kElementRowCount <= kMaxSectionRows, "元素行数超过快照容量");
static_assert(kGlobalRowCount <= kMaxSectionRows, "全局加成行数超过快照容量");

// 全局加成行不是角色属性：每行都带同一句注记。
inline constexpr const char* kGlobalNote = "不是角色属性（佣兵团技能 / 掉落表 / 怪物表）";
// 单行读取失败时显示的占位。
inline constexpr const char* kUnknownValue = "--";

inline constexpr uint32_t pages_in_section(uint32_t count) {
    return count == 0 ? 0 : (count + kMaxRowsPerPage - 1) / kMaxRowsPerPage;
}

inline constexpr uint32_t total_page_count() {
    uint32_t pages = 0;
    for (uint32_t i = 0; i < kSectionCount; ++i) pages += pages_in_section(kSections[i].count);
    return pages == 0 ? 1 : pages;
}

// 页码 → 分区 + 分区内页序号 + 该页起始行/行数（行数受 10 行与分区末页限制）。
struct PageRef {
    uint32_t section;
    uint32_t local_page;
    uint32_t first_row;
    uint32_t row_count;
};

// 页码越界返回 false（不写 out）。
inline bool locate_page(uint32_t page, PageRef* out) {
    uint32_t remaining = page;
    for (uint32_t i = 0; i < kSectionCount; ++i) {
        const uint32_t pages = pages_in_section(kSections[i].count);
        if (remaining < pages) {
            if (out != nullptr) {
                const uint32_t first_row = remaining * kMaxRowsPerPage;
                const uint32_t left = kSections[i].count - first_row;
                out->section = i;
                out->local_page = remaining;
                out->first_row = first_row;
                out->row_count = left > kMaxRowsPerPage ? kMaxRowsPerPage : left;
            }
            return true;
        }
        remaining -= pages;
    }
    return false;
}

// 单行数值格式化（固定缓冲，无动态分配）：
//   - 千分比类（kAttrMilli/kHitRate/kEvasion/kDamageReduce）：raw/10，保留 1 位小数 + %
//   - 其它数值：整数
//   - kText：全局加成注记
//   - 读取失败：`--`（kText 行不受失败标志影响）
inline bool row_is_milli(RowKind kind) {
    return kind == RowKind::kAttrMilli || kind == RowKind::kHitRate ||
           kind == RowKind::kEvasion || kind == RowKind::kDamageReduce;
}

inline void format_value(const RowSpec& row, bool ok, int32_t value, char* out,
                         size_t capacity) {
    if (out == nullptr || capacity == 0) return;
    if (row.kind == RowKind::kText) {
        (void)std::snprintf(out, capacity, "%s", kGlobalNote);
        return;
    }
    if (!ok) {
        (void)std::snprintf(out, capacity, "%s", kUnknownValue);
        return;
    }
    if (!row_is_milli(row.kind)) {
        (void)std::snprintf(out, capacity, "%d", value);
        return;
    }
    // raw=1234 → "123.4%"；raw=-5 → "-0.5%"（int64 规避 INT32_MIN 取负溢出）。
    const int64_t tenths = value;
    const bool negative = tenths < 0;
    const int64_t magnitude = negative ? -tenths : tenths;
    (void)std::snprintf(out, capacity, "%s%lld.%lld%%", negative ? "-" : "",
                        static_cast<long long>(magnitude / 10),
                        static_cast<long long>(magnitude % 10));
}

}  // namespace qol::features::character_panel

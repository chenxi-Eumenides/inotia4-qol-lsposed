#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

// 属性总览的纯数据模型：分区行表、分页映射与数值格式化。
//
// 本头不依赖 Host SDK / Game Host / 游戏内存，可在 host 单测中直接断言
// 行数、页边界、名称回退与格式化结果；overview.cpp 只负责读取与绘制。
//
// 结构：主属性 5 + 面板派生 11 + attr 0..114 全量 115 + 全队共享 12 = 143 行；
// 12 页双列（P1 主属性/面板，P2..P11 attr 每页 12 行，P12 全队共享 6/6）。

namespace qol::features::character_panel {

// 读取来源：属性 id / 主属性下标 / 面板派生 getter / 全队数据。
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
    kGroupSkill,       // 佣兵团技能是否已学：learned_text / missing_text
    kPartyDropMultiplier,  // 全队掉宝倍率：(1000+Σattr92)/1000
};

struct RowSpec {
    const char* label;  // nullptr = 用 attr_display_name(arg)，未知显示 attr<id>
    RowKind kind;
    int32_t arg;  // kAttrInt/kAttrMilli → attr id；kMainStat → index；kGroupSkill → act id
    // kGroupSkill：已学/未学文案。
    const char* learned_text = nullptr;
    const char* missing_text = nullptr;
};

inline constexpr RowSpec kMainStatRows[] = {
    {"力量", RowKind::kMainStat, 0},
    {"敏捷", RowKind::kMainStat, 1},
    {"体力", RowKind::kMainStat, 2},
    {"智力", RowKind::kMainStat, 3},
    {"精力", RowKind::kMainStat, 4},
};

// 面板可见派生值（与原版人物面板同源 getter）。
inline constexpr RowSpec kDerivedRows[] = {
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

// 已确认名称的 attr id（72 项）；其余 id 一律显示 attr<id>，不得编造名称。
struct AttrName {
    int32_t id;
    const char* name;
};

inline constexpr AttrName kAttrNames[] = {
    {0, "暴击率"}, {1, "暴击抵抗率"}, {2, "暴击伤害抵抗率"}, {3, "暴击伤害"},
    {4, "物理攻击"}, {5, "普攻伤害增幅"}, {7, "物理防御基础"}, {8, "魔法攻击"},
    {9, "魔法增伤"}, {10, "专属增伤开关"}, {11, "魔法抵抗"}, {12, "闪避"},
    {13, "总敏捷"}, {14, "命中率基数"}, {15, "命中率百分比"}, {17, "防御"},
    {18, "防御增幅系数"}, {19, "武器格挡"}, {20, "盾牌格挡"}, {22, "火"},
    {23, "风"}, {24, "寒气"}, {25, "神圣"}, {26, "黑暗"}, {27, "毒"},
    {28, "等级驱动属性"}, {29, "MP恢复"}, {30, "HP上限"}, {31, "MP上限"},
    {32, "蓝耗增减"}, {33, "HP吸收"}, {36, "固定吸血"}, {40, "友军受击回血"},
    {42, "治疗增幅"}, {44, "魔法伤害削减"}, {52, "双倍攻击概率"}, {58, "命中生成护盾"},
    {60, "高额吸血触发概率"}, {61, "高额吸血数值"}, {63, "命中回蓝"}, {66, "重击概率"},
    {69, "命中持续状态层数"}, {70, "重置召唤物CD概率"}, {71, "增伤阶段"},
    {72, "命中嘲讽"}, {75, "行为距离加成"}, {76, "反伤"}, {78, "重力摆"},
    {79, "冰霜"}, {80, "治愈气息"}, {81, "狂战士"}, {82, "瞬间恢复"},
    {83, "魔力专家"}, {84, "减少敌意值"}, {85, "失明抗性"}, {86, "睡眠抗性"},
    {87, "眩晕抗性"}, {88, "恐惧抗性"}, {89, "沉默抗性"}, {90, "减速抗性"},
    {91, "移动速度"}, {92, "掉宝率"}, {93, "武器攻击帧"}, {94, "仇恨系数"},
    {98, "嘲讽概率"}, {106, "额外普攻开关"}, {107, "受击获得持续增益概率"},
    {109, "额外普攻增幅"}, {110, "反击减益"}, {111, "命中沉默"},
    {112, "命中固定回蓝"}, {113, "总攻击"},
};
inline constexpr uint32_t kAttrNameCount =
    static_cast<uint32_t>(sizeof(kAttrNames) / sizeof(kAttrNames[0]));

// 未知 id 返回 nullptr；调用方显示 attr<id>。
inline const char* attr_display_name(int32_t id) {
    for (uint32_t i = 0; i < kAttrNameCount; ++i) {
        if (kAttrNames[i].id == id) return kAttrNames[i].name;
    }
    return nullptr;
}

// 行标签：显式 label 优先；否则 attr 名称；未知 id 显示 attr<id>。
inline void format_label(const RowSpec& row, char* out, size_t capacity) {
    if (out == nullptr || capacity == 0) return;
    if (row.label != nullptr) {
        (void)std::snprintf(out, capacity, "%s", row.label);
        return;
    }
    const char* name = attr_display_name(row.arg);
    if (name != nullptr) {
        (void)std::snprintf(out, capacity, "%s", name);
        return;
    }
    (void)std::snprintf(out, capacity, "attr%d", row.arg);
}

// attr 0..114 全量行表（label 为空，按 id 取名/回退）。
inline constexpr uint32_t kAttrRowCount = 115;
struct AttrRowTable {
    RowSpec rows[kAttrRowCount];
};
constexpr AttrRowTable make_attr_rows() {
    AttrRowTable table{};
    for (uint32_t i = 0; i < kAttrRowCount; ++i) {
        table.rows[i].label = nullptr;
        table.rows[i].kind = RowKind::kAttrInt;
        table.rows[i].arg = static_cast<int32_t>(i);
        table.rows[i].learned_text = nullptr;
        table.rows[i].missing_text = nullptr;
    }
    return table;
}
inline constexpr AttrRowTable kAttrRowTable = make_attr_rows();

// 全队共享：首行全队掉宝倍率，随后 11 行是佣兵团技能（act 119..142）。
inline constexpr RowSpec kTeamRows[] = {
    {"全队掉宝倍率", RowKind::kPartyDropMultiplier, 0},
    {"卷轴掉率", RowKind::kGroupSkill, 130, "+0.2pp", "未获得"},
    {"材料掉率", RowKind::kGroupSkill, 138, "+0.2pp", "未获得"},
    {"装备掉率", RowKind::kGroupSkill, 142, "+0.1pp", "未获得"},
    {"传说装备率", RowKind::kGroupSkill, 122, "+3%", "未获得"},
    {"开箱好道具", RowKind::kGroupSkill, 126, "已获得", "未获得"},
    {"稀有佣兵", RowKind::kGroupSkill, 134, "已获得", "未获得"},
    {"经验获得", RowKind::kGroupSkill, 133, "+2%", "未获得"},
    {"出售价格", RowKind::kGroupSkill, 123, "+3%", "未获得"},
    {"购买价格", RowKind::kGroupSkill, 131, "-2%", "未获得"},
    {"药水效果", RowKind::kGroupSkill, 135, "+10%", "未获得"},
    {"药水冷却", RowKind::kGroupSkill, 141, "-3秒", "未获得"},
};

struct SectionSpec {
    const char* title;
    const RowSpec* rows;
    uint32_t count;
};

inline constexpr SectionSpec kSections[] = {
    {"主属性", kMainStatRows,
     static_cast<uint32_t>(sizeof(kMainStatRows) / sizeof(kMainStatRows[0]))},
    {"面板属性", kDerivedRows,
     static_cast<uint32_t>(sizeof(kDerivedRows) / sizeof(kDerivedRows[0]))},
    {"角色属性", kAttrRowTable.rows, kAttrRowCount},
    {"全队共享", kTeamRows,
     static_cast<uint32_t>(sizeof(kTeamRows) / sizeof(kTeamRows[0]))},
};

inline constexpr uint32_t kSectionCount = static_cast<uint32_t>(sizeof(kSections) /
                                                                sizeof(kSections[0]));
inline constexpr uint32_t kMaxRowsPerColumn = 11;
// 快照容量上限；attr 区最大 115 行。
inline constexpr uint32_t kMaxSectionRows = 128;
inline constexpr uint32_t kMainStatRowCount =
    static_cast<uint32_t>(sizeof(kMainStatRows) / sizeof(kMainStatRows[0]));
inline constexpr uint32_t kDerivedRowCount =
    static_cast<uint32_t>(sizeof(kDerivedRows) / sizeof(kDerivedRows[0]));
inline constexpr uint32_t kTeamRowCount =
    static_cast<uint32_t>(sizeof(kTeamRows) / sizeof(kTeamRows[0]));
inline constexpr uint32_t kTotalRowCount =
    kMainStatRowCount + kDerivedRowCount + kAttrRowCount + kTeamRowCount;

static_assert(kMainStatRowCount <= kMaxSectionRows, "主属性分区行数超过快照容量");
static_assert(kDerivedRowCount <= kMaxSectionRows, "面板属性行数超过快照容量");
static_assert(kAttrRowCount <= kMaxSectionRows, "attr 分区行数超过快照容量");
static_assert(kTeamRowCount <= kMaxSectionRows, "全队共享行数超过快照容量");
static_assert(kAttrNameCount == 72, "已确认 attr 名称必须为 72 项");

// 单行读取失败时显示的占位。
inline constexpr const char* kUnknownValue = "--";

// 页的一列：引用某个分区内的连续行（section 为 kSections 下标）。
struct ColumnSpec {
    uint32_t section;
    uint32_t first_row;
    uint32_t row_count;
};

struct PageSpec {
    const char* title;
    ColumnSpec left;
    ColumnSpec right;  // row_count == 0 表示本页只有左列
};

// 12 页固定布局：P1 主属性/面板；P2..P11 attr 每页 12 行（左 6 / 右 6，末页 7）；
// P12 全队共享 6/6。
inline constexpr PageSpec kPages[] = {
    {"主属性与面板属性", {0, 0, 5}, {1, 0, 11}},
    {"角色属性", {2, 0, 6}, {2, 6, 6}},
    {"角色属性", {2, 12, 6}, {2, 18, 6}},
    {"角色属性", {2, 24, 6}, {2, 30, 6}},
    {"角色属性", {2, 36, 6}, {2, 42, 6}},
    {"角色属性", {2, 48, 6}, {2, 54, 6}},
    {"角色属性", {2, 60, 6}, {2, 66, 6}},
    {"角色属性", {2, 72, 6}, {2, 78, 6}},
    {"角色属性", {2, 84, 6}, {2, 90, 6}},
    {"角色属性", {2, 96, 6}, {2, 102, 6}},
    {"角色属性", {2, 108, 6}, {2, 114, 1}},
    {"全队共享", {3, 0, 6}, {3, 6, 6}},
};
inline constexpr uint32_t kPageCount =
    static_cast<uint32_t>(sizeof(kPages) / sizeof(kPages[0]));

static_assert(kPageCount == 12, "属性总览固定 12 页");
static_assert(kPages[0].left.row_count <= kMaxRowsPerColumn &&
                  kPages[0].right.row_count <= kMaxRowsPerColumn &&
                  kPages[1].left.row_count <= kMaxRowsPerColumn &&
                  kPages[1].right.row_count <= kMaxRowsPerColumn &&
                  kPages[2].left.row_count <= kMaxRowsPerColumn &&
                  kPages[2].right.row_count <= kMaxRowsPerColumn &&
                  kPages[3].left.row_count <= kMaxRowsPerColumn &&
                  kPages[3].right.row_count <= kMaxRowsPerColumn &&
                  kPages[4].left.row_count <= kMaxRowsPerColumn &&
                  kPages[4].right.row_count <= kMaxRowsPerColumn &&
                  kPages[5].left.row_count <= kMaxRowsPerColumn &&
                  kPages[5].right.row_count <= kMaxRowsPerColumn &&
                  kPages[6].left.row_count <= kMaxRowsPerColumn &&
                  kPages[6].right.row_count <= kMaxRowsPerColumn &&
                  kPages[7].left.row_count <= kMaxRowsPerColumn &&
                  kPages[7].right.row_count <= kMaxRowsPerColumn &&
                  kPages[8].left.row_count <= kMaxRowsPerColumn &&
                  kPages[8].right.row_count <= kMaxRowsPerColumn &&
                  kPages[9].left.row_count <= kMaxRowsPerColumn &&
                  kPages[9].right.row_count <= kMaxRowsPerColumn &&
                  kPages[10].left.row_count <= kMaxRowsPerColumn &&
                  kPages[10].right.row_count <= kMaxRowsPerColumn &&
                  kPages[11].left.row_count <= kMaxRowsPerColumn &&
                  kPages[11].right.row_count <= kMaxRowsPerColumn,
              "每列最多 11 行");
static_assert(kPages[11].left.row_count + kPages[11].right.row_count == kTeamRowCount,
              "全队共享页必须覆盖全部分区行");

// 页码越界返回 false（不写 out）。
inline bool page_at(uint32_t index, const PageSpec** out) {
    if (index >= kPageCount) return false;
    if (out != nullptr) *out = &kPages[index];
    return true;
}

// 单行数值格式化（固定缓冲，无动态分配）：
//   - 千分比类（kAttrMilli/kHitRate/kEvasion/kDamageReduce）：raw/10，保留 1 位小数 + %
//   - 其它数值：整数
//   - kGroupSkill：!ok → `--`；已学 → learned_text；未学 → missing_text
//   - kPartyDropMultiplier：倍率 = (1000+sum)/1000，保留 2 位
inline bool row_is_milli(RowKind kind) {
    return kind == RowKind::kAttrMilli || kind == RowKind::kHitRate ||
           kind == RowKind::kEvasion || kind == RowKind::kDamageReduce;
}

inline void format_value(const RowSpec& row, bool ok, bool learned, int32_t value, char* out,
                         size_t capacity) {
    if (out == nullptr || capacity == 0) return;
    if (!ok) {
        (void)std::snprintf(out, capacity, "%s", kUnknownValue);
        return;
    }
    if (row.kind == RowKind::kGroupSkill) {
        const char* text = learned ? row.learned_text : row.missing_text;
        (void)std::snprintf(out, capacity, "%s", text != nullptr ? text : kUnknownValue);
        return;
    }
    if (row.kind == RowKind::kPartyDropMultiplier) {
        // 倍率 = (1000+sum)/1000，保留 2 位小数（四舍五入）；value 为 sum（千分比）。
        const int64_t total = 1000 + static_cast<int64_t>(value);
        const int64_t rounded = (total * 100 + 500) / 1000;
        const bool negative = rounded < 0;
        const int64_t magnitude = negative ? -rounded : rounded;
        (void)std::snprintf(out, capacity, "×%s%lld.%02lld（+%d）", negative ? "-" : "",
                            static_cast<long long>(magnitude / 100),
                            static_cast<long long>(magnitude % 100), value);
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

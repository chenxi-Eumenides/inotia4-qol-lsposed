#pragma once

#include <cstdint>
#include <cstdio>

namespace qol::contracts {

// Prefixes are selected from ITEMGRADEBASE; class bits originate in ITEMCLASSBASE.
inline const char* item_rarity_prefix(int variant_series, int class_bits, int raw_rarity) {
    static constexpr const char* base_weapon[] = {
        "生锈的 ", "陈旧的 ", "", "太古的 ", "锐利的 ", "打磨的 ", "工匠的 ",
        "钢铁 ", "钛金 ", "秘银 ", "", "", "", "", "", ""};
    static constexpr const char* base_armor[] = {
        "生锈的 ", "陈旧的 ", "", "太古的 ", "优质的 ", "坚固的 ", "工匠的 ",
        "高级 ", "耀眼的 ", "神秘的 ", "", "", "", "", "", ""};
    static constexpr const char* c18_weapon[] = {
        "", "", "", "", "", "", "", "", "", "", "精金 ", "神铸 ", "弑神 ", "天命 ", "星陨 ", ""};
    static constexpr const char* c18_armor[] = {
        "生锈的 ", "陈旧的 ", "", "太古的 ", "优质的 ", "坚固的 ", "工匠的 ",
        "高级 ", "耀眼的 ", "神秘的 ", "精金 ", "神铸 ", "弑神 ", "天命 ", "星陨 ", ""};
    if (raw_rarity < 0 || raw_rarity >= 16 || (class_bits & 1) == 0) {
        return "";
    }
    const bool c18 = variant_series == 4;
    if ((class_bits & 2) != 0) return c18 ? c18_weapon[raw_rarity] : base_weapon[raw_rarity];
    if ((class_bits & 4) != 0) return c18 ? c18_armor[raw_rarity] : base_armor[raw_rarity];
    return "";
}

// 选项值显示文本：percent=true 时按游戏 "%d.%d%%" 规则（value 为 0.1% 单位）；
// 否则按 "%d" 输出。对应 UIDesc_AddOption 根据 ITEMOPTINFOBASE +7 bit0 选择的格式。
inline bool item_option_display_text(bool percent, int32_t value, char* out, uint32_t capacity) {
    if (out == nullptr || capacity == 0) return false;
    int written = 0;
    if (!percent) {
        written = std::snprintf(out, capacity, "%d", value);
    } else {
        const int32_t magnitude = value < 0 ? -value : value;
        written = std::snprintf(out, capacity, "%s%d.%d%%", value < 0 ? "-" : "",
                                magnitude / 10, magnitude % 10);
    }
    return written > 0 && static_cast<uint32_t>(written) < capacity;
}

}  // namespace qol::contracts

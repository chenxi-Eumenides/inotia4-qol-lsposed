#pragma once

#include <cstdint>

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

}  // namespace qol::contracts

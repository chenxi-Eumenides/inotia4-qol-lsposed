#pragma once

namespace qol::contracts {

// Prefixes are selected from ITEMGRADEBASE; class bits originate in ITEMCLASSBASE.
inline const char* item_rarity_prefix(int class_bits, int raw_rarity) {
    static constexpr const char* weapon[] = {
        "生锈的 ", "陈旧的 ", "", "太古的 ", "锐利的 ", "打磨的 ", "工匠的 ",
        "钢铁 ", "钛金 ", "秘银 ", "", "", "", "", ""};
    static constexpr const char* armor[] = {
        "生锈的 ", "陈旧的 ", "", "太古的 ", "优质的 ", "坚固的 ", "工匠的 ",
        "高级 ", "耀眼的 ", "神秘的 ", "", "", "", "", ""};
    if (raw_rarity < 0 || raw_rarity >= 15 || raw_rarity == 2 || (class_bits & 1) == 0) {
        return "";
    }
    if ((class_bits & 2) != 0) return weapon[raw_rarity];
    if ((class_bits & 4) != 0) return armor[raw_rarity];
    return "";
}

}  // namespace qol::contracts

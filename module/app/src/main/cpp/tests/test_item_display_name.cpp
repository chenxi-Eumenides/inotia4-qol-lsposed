#include <cassert>
#include <cstring>

#include "contracts/item_display_name.h"

int main() {
    constexpr const char* weapon[] = {"生锈的 ", "陈旧的 ", "", "太古的 ", "锐利的 ",
                                      "打磨的 ", "工匠的 ", "钢铁 ", "钛金 ", "秘银 ",
                                      "", "", "", "", ""};
    constexpr const char* armor[] = {"生锈的 ", "陈旧的 ", "", "太古的 ", "优质的 ",
                                     "坚固的 ", "工匠的 ", "高级 ", "耀眼的 ", "神秘的 ",
                                     "", "", "", "", ""};
    for (int rarity = 0; rarity < 16; ++rarity) {
        assert(std::strcmp(qol::contracts::item_rarity_prefix(0, rarity), "") == 0);
        assert(std::strcmp(qol::contracts::item_rarity_prefix(1, rarity), "") == 0);
        assert(std::strcmp(qol::contracts::item_rarity_prefix(3, rarity),
                           rarity < 15 ? weapon[rarity] : "") == 0);
        assert(std::strcmp(qol::contracts::item_rarity_prefix(5, rarity),
                           rarity < 15 ? armor[rarity] : "") == 0);
    }
    assert(std::strcmp(qol::contracts::item_rarity_prefix(5, 15), "") == 0);
}

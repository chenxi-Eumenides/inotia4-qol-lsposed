#include <cassert>
#include <cstring>

#include "contracts/item_display_name.h"

int main() {
    constexpr const char* base_weapon[] = {"生锈的 ", "陈旧的 ", "", "太古的 ", "锐利的 ",
                                           "打磨的 ", "工匠的 ", "钢铁 ", "钛金 ", "秘银 ",
                                           "", "", "", "", "", ""};
    constexpr const char* base_armor[] = {"生锈的 ", "陈旧的 ", "", "太古的 ", "优质的 ",
                                          "坚固的 ", "工匠的 ", "高级 ", "耀眼的 ", "神秘的 ",
                                          "", "", "", "", "", ""};
    constexpr const char* c18_weapon[] = {"", "", "", "", "", "", "", "", "", "",
                                          "精金 ", "神铸 ", "弑神 ", "天命 ", "星陨 ", ""};
    constexpr const char* c18_armor[] = {"生锈的 ", "陈旧的 ", "", "太古的 ", "优质的 ",
                                         "坚固的 ", "工匠的 ", "高级 ", "耀眼的 ", "神秘的 ",
                                         "精金 ", "神铸 ", "弑神 ", "天命 ", "星陨 ", ""};
    auto check = [](int variant, int bits, int raw, const char* expected) {
        assert(std::strcmp(qol::contracts::item_rarity_prefix(variant, bits, raw), expected) == 0);
    };
    for (int raw = 0; raw < 16; ++raw) {
        check(0, 0, raw, "");
        check(0, 1, raw, "");
        check(0, 3, raw, base_weapon[raw]);
        check(0, 5, raw, base_armor[raw]);
        check(4, 3, raw, c18_weapon[raw]);
        check(4, 5, raw, c18_armor[raw]);
    }
    check(0, 3, -1, "");
    check(0, 5, 16, "");
}

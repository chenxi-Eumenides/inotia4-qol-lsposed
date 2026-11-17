// damage_widen 物防 wrapper 的 attr 0x51 方向回归。
//
// 原版 CHAR_CalcPhysicalDefense（c18 v603 0xe08a4-0xe08c0）：
//   rate = rate - trunc(rate*attr51/1000)，即减伤率乘 (1000-attr51)/1000。
// attr51 是防御减益项：>0 时降低减伤率、放大承受伤害。
// 若 wrapper 写成 rate += trunc(rate*attr51/1000)，attr51=1000 时减伤率翻倍，
// 会把不减免的伤害压到 4%（例如 67000 -> 2680）。

#include "contracts/game_host.h"
#include "plugins/damage_widen/plugin.cpp"

#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

struct FakeChar {
    int32_t attrs[128]{};
};

QolResult fake_attr(void* character, int32_t attr_id, int32_t* out_value) {
    if (character == nullptr || out_value == nullptr) return qol::contracts::error(1, "bad", 3);
    auto* ch = static_cast<FakeChar*>(character);
    *out_value = ch->attrs[attr_id & 0x7f];
    return qol::contracts::ok();
}

int32_t g_cal_result = 0;

QolResult fake_physical_defense(void*, int32_t, int32_t* out_value) {
    if (out_value != nullptr) *out_value = g_cal_result;
    return qol::contracts::ok();
}

int32_t fake_physical_backup(void*, void*, int32_t damage) { return damage; }

}  // namespace

int main() {
    using namespace qol::features::damage_widen;

    qol::contracts::QolCharacterReadApi read_api{};
    read_api.attr = &fake_attr;
    read_api.physical_defense_calculate = &fake_physical_defense;
    g_character_read.store(&read_api);
    g_enabled.store(true);

    FakeChar defender{};
    g_cal_result = 480;  // 自然减伤率 480‰（attr7 取 0）

    // attr51=0：减伤率保持 480‰，行为与原版一致。
    check(calc_physical_defense_wrapper(nullptr, &defender, 67000) == 34840,
          "attr51=0 keeps native reduction: 67000*(1000-480)/1000");

    // attr51=500：减伤率 480 - 240 = 240‰。
    defender.attrs[81] = 500;
    check(calc_physical_defense_wrapper(nullptr, &defender, 67000) == 50920,
          "attr51=500 subtracts correction: 67000*(1000-240)/1000");

    // attr51=1000：减伤率归零，伤害全额保留（原版 67000，玩家反馈的开启后 2680 即反向实现）。
    defender.attrs[81] = 1000;
    check(calc_physical_defense_wrapper(nullptr, &defender, 67000) == 67000,
          "attr51=1000 zeroes reduction: full 67000 damage");

    // attr51 使减伤率超过 1000‰：原版得到负减伤，伤害被放大，wrapper 保持宽中间值。
    defender.attrs[81] = 2000;
    g_cal_result = 800;
    check(calc_physical_defense_wrapper(nullptr, &defender, 67000) == 120600,
          "attr51=2000 drives rate negative and amplifies damage");

    // 属性读面不可用时透传 backup。
    g_physical_backup.store(&fake_physical_backup);
    g_character_read.store(nullptr);
    check(calc_physical_defense_wrapper(nullptr, &defender, 67000) == 67000,
          "missing read api falls back to backup");

    return failures == 0 ? 0 : 1;
}

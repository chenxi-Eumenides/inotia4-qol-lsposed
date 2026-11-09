// damage_widen 暴击 wrapper 的角色参数回归。
//
// 原版 CHAR_GetCriticalDamage(attacker, defender, damage)：
//   attr2（暴击抵抗）来自 defender，attr3（暴击增伤）来自 attacker。
// 该测试刻意让 defender.attr3 远大于 attacker.attr3；若 wrapper 再次把两者都读到
// defender 上，结果会立刻偏离 vanilla 公式。

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

QolResult fake_physical_defense(void*, int32_t, int32_t* out_value) {
    if (out_value != nullptr) *out_value = 0;
    return qol::contracts::ok();
}

int32_t backup_calls = 0;
int32_t fake_critical_backup(void*, void*, int32_t damage) {
    ++backup_calls;
    return damage;
}

}  // namespace

int main() {
    using namespace qol::features::damage_widen;

    qol::contracts::QolCharacterReadApi read_api{};
    read_api.attr = &fake_attr;
    read_api.physical_defense_calculate = &fake_physical_defense;
    g_character_read.store(&read_api);
    g_critical_backup.store(&fake_critical_backup);
    g_enabled.store(true);

    FakeChar attacker{};
    FakeChar defender{};
    attacker.attrs[3] = 1000;    // 攻方暴击增伤：+100%
    attacker.attrs[2] = 999;     // 若错读攻方 attr2，暴抗差会从 1000 变成 1
    defender.attrs[2] = 0;       // 守方暴击抵抗
    defender.attrs[3] = 999999;  // 若错读守方 attr3，结果会爆炸

    backup_calls = 0;
    check(get_critical_damage_wrapper(&attacker, &defender, 100) == 300,
          "critical uses attacker attr3=1000 and defender attr2=0: 100+100*2000/1000");
    check(backup_calls == 0, "critical wrapper does not fall back with available attrs");

    // attr3 必须跟随 attacker：改为 0 后应为 2 倍伤害。
    attacker.attrs[3] = 0;
    check(get_critical_damage_wrapper(&attacker, &defender, 100) == 200,
          "critical follows attacker attr3=0: 100+100*1000/1000");
    attacker.attrs[3] = 1000;

    // defender attr3 不得影响结果。
    defender.attrs[3] = 1;
    check(get_critical_damage_wrapper(&attacker, &defender, 100) == 300,
          "defender attr3 does not affect critical result");
    defender.attrs[3] = 999999;

    // attr2 必须跟随 defender：守方暴抗 >=1000 时加成段钳 0，结果 2 倍。
    defender.attrs[2] = 1000;
    check(get_critical_damage_wrapper(&attacker, &defender, 100) == 200,
          "defender attr2=1000 clamps resistance bonus to zero");
    defender.attrs[2] = 0;

    // 属性读取不可用时透传 backup（原生数学）。
    g_character_read.store(nullptr);
    backup_calls = 0;
    check(get_critical_damage_wrapper(&attacker, &defender, 100) == 100 && backup_calls == 1,
          "critical wrapper falls back to backup when attr API is unavailable");

    g_character_read.store(&read_api);
    g_enabled.store(false);
    backup_calls = 0;
    check(get_critical_damage_wrapper(&attacker, &defender, 100) == 100 && backup_calls == 1,
          "critical wrapper falls back when feature disabled");

    return failures == 0 ? 0 : 1;
}

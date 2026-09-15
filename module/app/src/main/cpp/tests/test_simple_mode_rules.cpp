#include "feature/simple_mode/simple_mode_rules.h"

#include <cstdint>
#include <cstdio>
#include <limits>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using simple_mode::damage_percent;
using simple_mode::halved_max_hp;
using simple_mode::max_hp_percent;
using simple_mode::scale_by_percent;
using simple_mode::Side;

// 阵营矩阵穷举：3×3 全部组合，逐条钉死倍率语义。
static void test_damage_matrix() {
    // 受害者属玩家侧 → 一律 50，与攻击者身份无关。
    CHECK(damage_percent(Side::kMonster, Side::kPlayer) == 50);   // 敌人打玩家
    CHECK(damage_percent(Side::kPlayer, Side::kPlayer) == 50);    // 玩家侧互击（不应发生）
    CHECK(damage_percent(Side::kNeutral, Side::kPlayer) == 50);   // 中立打玩家
    // 玩家侧打怪物 → 150（+50%，×1.5）。
    CHECK(damage_percent(Side::kPlayer, Side::kMonster) == 150);
    // 怪物互殴 / 怪物打 NPC → 100。
    CHECK(damage_percent(Side::kMonster, Side::kMonster) == 100);
    CHECK(damage_percent(Side::kMonster, Side::kNeutral) == 100);
    // 玩家侧打 NPC / 装饰物 → 100（它们不是敌人，不加伤）。
    CHECK(damage_percent(Side::kPlayer, Side::kNeutral) == 100);
    // 中立来源 → 100。
    CHECK(damage_percent(Side::kNeutral, Side::kMonster) == 100);
    CHECK(damage_percent(Side::kNeutral, Side::kNeutral) == 100);
}

static void test_max_hp_matrix() {
    CHECK(max_hp_percent(Side::kMonster) == 50);
    CHECK(max_hp_percent(Side::kPlayer) == 100);
    CHECK(max_hp_percent(Side::kNeutral) == 100);
}

static void test_scale_by_percent() {
    CHECK(scale_by_percent(100, 150) == 150);
    CHECK(scale_by_percent(100, 50) == 50);
    CHECK(scale_by_percent(100, 100) == 100);
    CHECK(scale_by_percent(7, 150) == 10);   // 7×1.5 = 10.5 → 向下取整 10
    // ×1.5 是分数倍率：1 点伤害取整后仍是 1（不增益），2 → 3 才是精确 +50%，3 → 4（4.5 取整）。
    CHECK(scale_by_percent(1, 150) == 1);
    CHECK(scale_by_percent(2, 150) == 3);
    CHECK(scale_by_percent(3, 150) == 4);
    // 取整方向：向下（整数除法）。
    CHECK(scale_by_percent(7, 50) == 3);
    CHECK(scale_by_percent(3, 50) == 1);
    // 缩放后至少保留 1：否则 CHAR_AddDamage 会因 damage<=0 直接丢弃整次伤害。
    CHECK(scale_by_percent(1, 50) == 1);
    CHECK(scale_by_percent(2, 50) == 1);
    // 非法/零值原样返回，不被放大。
    CHECK(scale_by_percent(0, 150) == 0);
    CHECK(scale_by_percent(-5, 150) == -5);
    // 溢出保护。
    const std::int32_t big = std::numeric_limits<std::int32_t>::max();
    CHECK(scale_by_percent(big, 150) == big);          // ×1.5 后超界 → 钳 INT32_MAX
    CHECK(scale_by_percent(1000000, 150) == 1500000);  // 未超界时按 ×1.5 缩放
}

static void test_halved_max_hp() {
    CHECK(halved_max_hp(100) == 50);
    CHECK(halved_max_hp(101) == 50);
    CHECK(halved_max_hp(2) == 1);
    // 下限 1：不允许出现 0（0 上限会让角色立即判定死亡/异常）。
    CHECK(halved_max_hp(1) == 1);
    CHECK(halved_max_hp(0) == 1);
    CHECK(halved_max_hp(-100) == 1);
}

int main() {
    test_damage_matrix();
    test_max_hp_matrix();
    test_scale_by_percent();
    test_halved_max_hp();
    std::printf("simple_mode_rules: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

// special_equip 纯逻辑 host 测试：原版判定链「跳过 bit4」后的重放判定与出售门禁决策。
// 只链接 features/special_equip/rules.cpp，不依赖游戏进程。

#include "features/special_equip/rules.h"

#include <cstdio>

using qol::features::special_equip::equip_replay_allows;
using qol::features::special_equip::sell_gate_decides_block;
using qol::features::special_equip::unequip_replay_allows;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

// 原版 CHAR_CanEquipItem ①③④ 重放。
void test_equip_replay() {
    // ① 非真装备：即使角色权限与等级都满足也拒绝。
    CHECK(!equip_replay_allows(false, true, 99, 1));
    CHECK(!equip_replay_allows(false, false, 99, 1));

    // ③ 角色无装备权限（特殊 NPC 闸门）：拒绝。
    CHECK(!equip_replay_allows(true, false, 99, 1));

    // ④ 等级：低于需求拒绝、相等/高于放行。
    CHECK(!equip_replay_allows(true, true, 9, 10));
    CHECK(equip_replay_allows(true, true, 10, 10));
    CHECK(equip_replay_allows(true, true, 11, 10));

    // 等级 0 需求：0 级角色放行（u8 零扩展 + 32 位有符号比较）。
    CHECK(equip_replay_allows(true, true, 0, 0));

    // 依赖缺失时调用方传占位 false，必须 fail-closed。
    CHECK(!equip_replay_allows(true, false, 0, 0));
}

// 原版 CHAR_CanUnequipItem ② 重放。
void test_unequip_replay() {
    CHECK(unequip_replay_allows(true));
    CHECK(!unequip_replay_allows(false));
}

// 出售门禁：仅装备详情 + 菜单角色失去装备权限时拦截。
void test_sell_gate() {
    // 非装备详情（背包=2 / 扩展袋=1 / 未就绪=-1）一律放行。
    CHECK(!sell_gate_decides_block(2, true, true, false));
    CHECK(!sell_gate_decides_block(1, true, true, false));
    CHECK(!sell_gate_decides_block(-1, true, true, false));

    // 装备详情但角色缺失或原语未解析：放行（不误拦）。
    CHECK(!sell_gate_decides_block(0, false, true, false));
    CHECK(!sell_gate_decides_block(0, true, false, false));

    // 装备详情 + 角色存在 + 原语已解析：按 CHAR_CanChangeEquip 决定。
    CHECK(!sell_gate_decides_block(0, true, true, true));    // 可动装备 → 放行
    CHECK(sell_gate_decides_block(0, true, true, false));    // 特殊 NPC 佣兵 → 拦截
}

}  // namespace

int main() {
    test_equip_replay();
    test_unequip_replay();
    test_sell_gate();
    std::printf("special_equip_rules: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

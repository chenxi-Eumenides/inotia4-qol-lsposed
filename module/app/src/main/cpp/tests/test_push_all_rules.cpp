// plugins/push_all/rules.h host 单测：party_decision 矩阵（纯逻辑，不触游戏内存）。
// 判定顺序：type==1（怪物）→ 0；active group → 0；否则返回 party_index。

#include "plugins/push_all/rules.h"

#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using qol::features::push_all::party_decision;

namespace {

void test_monster_always_allowed() {
    // C_TYPE==1：怪物，无论是否 active group / 索引如何，一律视为队伍成员。
    CHECK(party_decision(1, false, -1) == 0);
    CHECK(party_decision(1, true, -1) == 0);
    CHECK(party_decision(1, false, 3) == 0);
}

void test_active_group_allowed() {
    // 主控本人 / 主控召唤物：即使 party_index==-1 也放开。
    CHECK(party_decision(0, true, -1) == 0);
    CHECK(party_decision(2, true, -1) == 0);
    CHECK(party_decision(2, true, 5) == 0);
}

void test_teammate_uses_index() {
    // 队友：active group 为假，保留原版索引（>=0 可推）。
    CHECK(party_decision(2, false, 0) == 0);
    CHECK(party_decision(2, false, 1) == 1);
    CHECK(party_decision(2, false, 3) == 3);
}

void test_facility_rejected() {
    // 场景设施（商人等 NPC）：active group 假且索引 -1 → 不可推。
    CHECK(party_decision(2, false, -1) == -1);
    CHECK(party_decision(0, false, -1) == -1);
}

}  // namespace

int main() {
    test_monster_always_allowed();
    test_active_group_allowed();
    test_teammate_uses_index();
    test_facility_rejected();
    std::printf("push_all_rules: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

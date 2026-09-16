// host 单测：模块 wrapper enter → 原版面板 VMA 的反查注册表。
#include "core/native/popup_enter_alias.h"

#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void test_register_resolve() {
    popup_enter_alias_clear();
    constexpr uintptr_t kShopWrapper = 0x7a489dba00ULL;
    constexpr uintptr_t kShopEnter = 0x14f4b8;

    // 未登记：不产生映射（调用方保持「enter - g_base」原路径）。
    CHECK(popup_enter_alias_resolve(kShopWrapper) == kPopupEnterAliasNone);
    CHECK(popup_enter_alias_resolve(0) == kPopupEnterAliasNone);

    popup_enter_alias_register(kShopWrapper, kShopEnter);
    CHECK(popup_enter_alias_resolve(kShopWrapper) == kShopEnter);
    // 其它地址不受影响。
    CHECK(popup_enter_alias_resolve(kShopWrapper + 0x10) == kPopupEnterAliasNone);

    popup_enter_alias_clear();
}

static void test_ignore_invalid() {
    popup_enter_alias_clear();
    popup_enter_alias_register(0, 0x14f4b8);
    popup_enter_alias_register(0x1000, 0);
    CHECK(popup_enter_alias_resolve(0x1000) == kPopupEnterAliasNone);
    CHECK(popup_enter_alias_resolve(0) == kPopupEnterAliasNone);
    popup_enter_alias_clear();
}

static void test_reregister_and_unregister() {
    popup_enter_alias_clear();
    popup_enter_alias_register(0x2000, 0x14a8b0);
    popup_enter_alias_register(0x2000, 0x14f4b8);
    // 同一 wrapper 重复登记覆盖 VMA，不占用新槽位。
    CHECK(popup_enter_alias_resolve(0x2000) == 0x14f4b8);

    CHECK(popup_enter_alias_unregister(0x2000));
    CHECK(popup_enter_alias_resolve(0x2000) == kPopupEnterAliasNone);
    CHECK(!popup_enter_alias_unregister(0x2000));
    CHECK(!popup_enter_alias_unregister(0));
    popup_enter_alias_clear();
}

static void test_multi_slot() {
    popup_enter_alias_clear();
    // 模块目前替换 4 处真实面板 enter（背包/存档槽/合成/商店），槽位须支持并发共存。
    popup_enter_alias_register(0x3000, 0x14a8b0);
    popup_enter_alias_register(0x4000, 0x14c720);
    popup_enter_alias_register(0x5000, 0x14b330);
    popup_enter_alias_register(0x6000, 0x14f4b8);
    CHECK(popup_enter_alias_resolve(0x3000) == 0x14a8b0);
    CHECK(popup_enter_alias_resolve(0x4000) == 0x14c720);
    CHECK(popup_enter_alias_resolve(0x5000) == 0x14b330);
    CHECK(popup_enter_alias_resolve(0x6000) == 0x14f4b8);

    popup_enter_alias_unregister(0x4000);
    CHECK(popup_enter_alias_resolve(0x3000) == 0x14a8b0);
    CHECK(popup_enter_alias_resolve(0x4000) == kPopupEnterAliasNone);
    CHECK(popup_enter_alias_resolve(0x6000) == 0x14f4b8);
    popup_enter_alias_clear();
}

int main() {
    test_register_resolve();
    test_ignore_invalid();
    test_reregister_and_unregister();
    test_multi_slot();
    std::printf("popup_enter_alias_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

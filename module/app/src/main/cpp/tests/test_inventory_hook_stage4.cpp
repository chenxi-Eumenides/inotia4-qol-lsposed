#include "feature/patch/inventory_hook_stage4.h"

#include <cstdint>
#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static int g_original_value = 0;
static int g_extension_value = 0;
static int g_original_calls = 0;
static int g_extension_calls = 0;
static bool* g_recursive_guard = nullptr;

static int original_count(int32_t) { ++g_original_calls; return g_original_value; }
static int extension_count(int32_t) { ++g_extension_calls; return g_extension_value; }
static int recursive_original(int32_t category) {
    ++g_original_calls;
    if (g_original_calls == 1 && g_recursive_guard != nullptr) {
        return stage4_have_item(category, recursive_original, extension_count, *g_recursive_guard);
    }
    return 0;
}

static int g_empty_original = 0;
static bool g_empty_extension = false;
static int g_empty_original_calls = 0;
static int g_empty_extension_calls = 0;
static int empty_original(int32_t, int32_t) { ++g_empty_original_calls; return g_empty_original; }
static bool empty_extension(int32_t, int32_t) { ++g_empty_extension_calls; return g_empty_extension; }

static void* g_extension_item = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234));
static int g_identify_calls = 0;
static int g_extension_action_calls = 0;
static int g_backup_action_calls = 0;
static bool identify_item(void* item, int32_t* bag, int32_t* slot) {
    ++g_identify_calls;
    if (bag != nullptr) *bag = 6;
    if (slot != nullptr) *slot = 2;
    return item == g_extension_item;
}
static bool extension_consume(void*) { ++g_extension_action_calls; return true; }
static bool extension_remove(bool) { return true; }
static bool remove_item_extension(void*) { ++g_extension_action_calls; return true; }
static void backup_consume(void*) { ++g_backup_action_calls; }
static int backup_remove(void*) { ++g_backup_action_calls; return 7; }

static void* item_at(int32_t, int32_t) { return g_extension_item; }
static int g_equip_extension_calls = 0;
static int g_equip_backup_calls = 0;
static bool extension_equip(void*, int32_t bag, int32_t slot, int32_t equip_slot) {
    ++g_equip_extension_calls;
    return bag == 6 && slot == 2 && equip_slot == 3;
}
static int backup_equip(void*, int32_t, int32_t, int32_t) { ++g_equip_backup_calls; return 9; }

static int g_jewel_backup_result = 0;
static int g_jewel_backup_calls = 0;
static int jewel_backup(void*, void*) { ++g_jewel_backup_calls; return g_jewel_backup_result; }

static int g_install_calls = 0;
static int g_uninstall_calls = 0;
static int g_install_fail_at = -1;
static int fake_install(void*, void*, void** backup) {
    const int index = g_install_calls++;
    if (index == g_install_fail_at) return 1;
    if (backup != nullptr) *backup = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + index));
    return 0;
}
static int fake_uninstall(void*) { ++g_uninstall_calls; return 0; }

static void test_queries() {
    bool guard = false;
    g_original_value = 1; g_extension_value = 5; g_original_calls = 0; g_extension_calls = 0;
    CHECK(stage4_have_item(1, original_count, extension_count, guard) == 1);
    CHECK(g_original_calls == 1 && g_extension_calls == 0 && !guard);

    g_original_value = 0; g_extension_value = 5; g_original_calls = 0; g_extension_calls = 0;
    CHECK(stage4_have_item(1, original_count, extension_count, guard) == 1);
    CHECK(g_original_calls == 1 && g_extension_calls == 1 && !guard);

    g_original_value = 4; g_extension_value = 6; g_original_calls = 0; g_extension_calls = 0;
    CHECK(stage4_get_item_count(1, original_count, extension_count, guard) == 10);
    CHECK(g_original_calls == 1 && g_extension_calls == 1 && !guard);

    g_empty_original = 1; g_empty_extension = false; g_empty_original_calls = 0; g_empty_extension_calls = 0;
    CHECK(stage4_is_having_empty_slot(1, 0, empty_original, empty_extension, guard) == 1);
    CHECK(g_empty_extension_calls == 0 && !guard);

    g_empty_original = 0; g_empty_extension = true; g_empty_original_calls = 0; g_empty_extension_calls = 0;
    CHECK(stage4_is_having_empty_slot(2, 1, empty_original, empty_extension, guard) == 1);
    CHECK(g_empty_original_calls == 1 && g_empty_extension_calls == 1 && !guard);
    CHECK(stage4_is_having_empty_slot(0, 1, empty_original, empty_extension, guard) == 0);
    CHECK(g_empty_extension_calls == 1 && !guard);

    g_original_calls = 0; g_extension_calls = 0; g_recursive_guard = &guard;
    CHECK(stage4_have_item(1, recursive_original, extension_count, guard) == 1);
    CHECK(g_original_calls == 2 && g_extension_calls == 1 && !guard);
    g_recursive_guard = nullptr;
}

static void test_object_operations() {
    bool guard = false;
    g_identify_calls = 0; g_extension_action_calls = 0; g_backup_action_calls = 0;
    stage4_consume_item(g_extension_item, identify_item, extension_consume, backup_consume, guard);
    CHECK(g_extension_action_calls == 1 && g_backup_action_calls == 0 && !guard);
    stage4_consume_item(reinterpret_cast<void*>(static_cast<uintptr_t>(0x9999)), identify_item,
                        extension_consume, backup_consume, guard);
    CHECK(g_backup_action_calls == 1 && !guard);

    g_extension_action_calls = 0; g_backup_action_calls = 0;
    CHECK(stage4_remove_item(g_extension_item, identify_item, remove_item_extension,
                             backup_remove, guard) == 1);
    CHECK(g_extension_action_calls == 1 && g_backup_action_calls == 0 && !guard);
    CHECK(stage4_remove_item(reinterpret_cast<void*>(static_cast<uintptr_t>(0x9999)),
                             identify_item, remove_item_extension, backup_remove, guard) == 7);
    CHECK(g_backup_action_calls == 1 && !guard);

    g_equip_extension_calls = 0; g_equip_backup_calls = 0;
    CHECK(stage4_equip_item(nullptr, 0, 0, 3, item_at, identify_item,
                            extension_equip, backup_equip) == 1);
    CHECK(g_equip_extension_calls == 1 && g_equip_backup_calls == 0);
    CHECK(stage4_equip_item(nullptr, 0, 0, 4, nullptr, identify_item,
                            extension_equip, backup_equip) == 9);
    CHECK(g_equip_backup_calls == 1);

    g_jewel_backup_result = 0; g_jewel_backup_calls = 0; g_extension_action_calls = 0;
    CHECK(stage4_put_jewel(nullptr, g_extension_item, identify_item, jewel_backup,
                            remove_item_extension) == 0);
    CHECK(g_jewel_backup_calls == 1 && g_extension_action_calls == 1);
    g_jewel_backup_result = 3;
    CHECK(stage4_put_jewel(nullptr, g_extension_item, identify_item, jewel_backup,
                            remove_item_extension) == 3);
    CHECK(g_extension_action_calls == 1);
}

static void test_install_transaction() {
    void* backup_a = nullptr;
    void* backup_b = nullptr;
    const Stage4HookSpec hooks[] = {{reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), &backup_a},
                                    {reinterpret_cast<void*>(3), reinterpret_cast<void*>(4), &backup_b}};
    g_install_calls = 0; g_uninstall_calls = 0; g_install_fail_at = -1;
    CHECK(stage4_install_transaction(hooks, 2, fake_install, fake_uninstall));
    CHECK(g_install_calls == 2 && g_uninstall_calls == 0 && backup_a != nullptr && backup_b != nullptr);

    backup_a = nullptr; backup_b = nullptr; g_install_calls = 0; g_uninstall_calls = 0; g_install_fail_at = 1;
    CHECK(!stage4_install_transaction(hooks, 2, fake_install, fake_uninstall));
    CHECK(g_install_calls == 2 && g_uninstall_calls == 1 && backup_a == nullptr && backup_b == nullptr);
}

int main() {
    test_queries();
    test_object_operations();
    test_install_transaction();
    std::printf("stage4_hook_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

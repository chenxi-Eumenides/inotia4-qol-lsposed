#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"

#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (condition) {                                                        \
            ++g_pass;                                                           \
        } else {                                                                \
            ++g_fail;                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                       \
    } while (false)

void test_draw_end_can_read_inventory_state_while_holding_runtime_lock() {
    std::mutex runtime_mutex;
    bool snapshot_callback_reentered = false;
    bool draw_body_holds_lock = false;

    qol::features::extension_bag::model::with_snapshot_then_lock(
        runtime_mutex,
        [&]() {
            // The game-state provider is synchronous and may re-enter EB; it must run
            // before draw-end acquires the non-recursive runtime mutex.
            const std::lock_guard<std::mutex> callback_lock(runtime_mutex);
            snapshot_callback_reentered = true;
            return 5;
        },
        [&](int state) {
            CHECK(state == 5);
            draw_body_holds_lock = !runtime_mutex.try_lock();
            if (!draw_body_holds_lock) runtime_mutex.unlock();
        });

    CHECK(snapshot_callback_reentered);
    CHECK(draw_body_holds_lock);
}

void test_main_menu_cleanup_due_edges_and_retries() {
    using qol::features::extension_bag::model::main_menu_cleanup_due;
    CHECK(main_menu_cleanup_due(-1, 3, false));
    CHECK(!main_menu_cleanup_due(3, 3, false));
    CHECK(main_menu_cleanup_due(5, 3, false));
    CHECK(!main_menu_cleanup_due(3, 5, false));
    CHECK(main_menu_cleanup_due(3, 3, true));
    CHECK(!main_menu_cleanup_due(-1, -1, false));
}

void test_inventory_state_reader_uses_popup_state_entry() {
    std::ifstream source_file(QOL_EXTENSION_BAG_PUBLIC_RUNTIME_SOURCE);
    const std::string source((std::istreambuf_iterator<char>(source_file)),
                             std::istreambuf_iterator<char>());
    const size_t function_start = source.find("int virtual_bag_inventory_state_id() {");
    const size_t function_end = source.find("\n}", function_start);
    CHECK(source_file.good() || source_file.eof());
    CHECK(function_start != std::string::npos && function_end != std::string::npos);
    if (function_start == std::string::npos || function_end == std::string::npos) return;
    const std::string function = source.substr(function_start, function_end - function_start);
    CHECK(function.find("std::lock_guard<std::mutex> lock(g_virtual_bag_mtx)") !=
          std::string::npos);
    CHECK(function.find("g_state_entry == nullptr") != std::string::npos);
    CHECK(function.find("*reinterpret_cast<int32_t*>(g_state_entry)") != std::string::npos);
    CHECK(function.find("world_facts()") == std::string::npos);
    CHECK(function.find("facts->state") == std::string::npos);

    std::mutex runtime_mutex;
    const int popup_state_id = 5;
    const int global_world_state = 0;
    int cleanup_calls = 0;
    int generation = 9;
    qol::features::extension_bag::model::with_snapshot_then_lock(
        runtime_mutex, [&] { return popup_state_id; }, [&](int inventory_state) {
            if (inventory_state >= 0 && inventory_state != 5) {
                ++cleanup_calls;
                ++generation;
            }
        });
    CHECK(global_world_state != popup_state_id);
    CHECK(cleanup_calls == 0 && generation == 9);

    const int offscreen_popup_state_id = 2;
    qol::features::extension_bag::model::with_snapshot_then_lock(
        runtime_mutex, [&] { return offscreen_popup_state_id; }, [&](int inventory_state) {
            if (inventory_state >= 0 && inventory_state != 5) {
                ++cleanup_calls;
                ++generation;
            }
        });
    CHECK(cleanup_calls == 1 && generation == 10);
}

}  // namespace

int main() {
    test_draw_end_can_read_inventory_state_while_holding_runtime_lock();
    test_main_menu_cleanup_due_edges_and_retries();
    test_inventory_state_reader_uses_popup_state_entry();
    std::printf("extension_bag_draw_lock_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

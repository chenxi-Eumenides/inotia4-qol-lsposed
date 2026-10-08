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

std::string read_source(const char* path) {
    std::ifstream source_file(path);
    return std::string(std::istreambuf_iterator<char>(source_file),
                       std::istreambuf_iterator<char>());
}

std::string source_function(const std::string& source, const char* signature) {
    const size_t start = source.find(signature);
    if (start == std::string::npos) return {};
    const size_t end = source.find("\n}", start);
    if (end == std::string::npos) return {};
    return source.substr(start, end - start);
}

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
    CHECK(main_menu_cleanup_due(-1, 4, true, false));  // Known main-menu state at startup.
    CHECK(!main_menu_cleanup_due(4, 4, true, false));  // Stable menu state is edge-triggered.
    CHECK(main_menu_cleanup_due(5, 4, true, false));  // Left world into the menu.
    CHECK(main_menu_cleanup_due(5, 3, true, false));  // Left world into another non-world state.
    CHECK(!main_menu_cleanup_due(4, 5, true, false));  // Entering world is not cleanup.
    CHECK(!main_menu_cleanup_due(-1, 5, true, false)); // Inventory panel state 4 is not consulted.
    CHECK(main_menu_cleanup_due(4, 4, true, true));   // Failed cleanup retries only in menu.
    CHECK(!main_menu_cleanup_due(5, 5, false, true)); // Unknown world state cannot authorize reset.
    CHECK(!main_menu_cleanup_due(-1, -1, false, false));
}

void test_inventory_state_reader_uses_popup_state_entry() {
    const std::string source = read_source(QOL_EXTENSION_BAG_PUBLIC_RUNTIME_SOURCE);
    const size_t function_start = source.find("int virtual_bag_inventory_state_id() {");
    const size_t function_end = source.find("\n}", function_start);
    CHECK(function_start != std::string::npos && function_end != std::string::npos);
    if (function_start == std::string::npos || function_end == std::string::npos) return;
    const std::string function = source.substr(function_start, function_end - function_start);
    CHECK(function.find("std::lock_guard<std::mutex> lock(g_virtual_bag_mtx)") !=
          std::string::npos);
    CHECK(function.find("g_state_entry == nullptr") != std::string::npos);
    CHECK(function.find("*reinterpret_cast<int32_t*>(g_state_entry)") != std::string::npos);
    CHECK(function.find("world_facts()") == std::string::npos);
    CHECK(function.find("facts->state") == std::string::npos);
}

void test_panel_edges_cannot_reset_loaded_state() {
    const std::string render = read_source(QOL_EXTENSION_BAG_RENDER_SOURCE);
    const std::string draw_end = source_function(render, "void virtual_bag_draw_end_locked(int inventory_state) {");
    CHECK(!draw_end.empty());
    CHECK(draw_end.find("prepare_main_menu_locked") == std::string::npos);
    CHECK(draw_end.find("g_virtual_bag_state = {}") == std::string::npos);

    const std::string lifecycle = read_source(QOL_EXTENSION_BAG_LIFECYCLE_SOURCE);
    const std::string tick = source_function(lifecycle, "int32_t extension_bag_lifecycle_tick(int64_t frame, void* ctx) {");
    const std::string cleanup = source_function(lifecycle, "bool prepare_main_menu_locked() {");
    CHECK(tick.find("read_world_state_id") != std::string::npos);
    CHECK(tick.find("main_menu_cleanup_due") != std::string::npos);
    CHECK(tick.find("extension_bag_session_admitted_locked") != std::string::npos);
    CHECK(cleanup.find("extension_bag_session_admitted_locked") <
          cleanup.find("g_virtual_bag_state = {}"));
}

void test_unadmitted_panel_mutators_fail_before_touching_state() {
    const std::string runtime = read_source(QOL_EXTENSION_BAG_RUNTIME_SOURCE);
    const std::string tab_click =
        source_function(runtime, "void handle_extension_tab_click_locked(int extension_bag) {");
    CHECK(tab_click.find("extension_bag_session_admitted_locked") != std::string::npos);
    CHECK(tab_click.find("extension_bag_session_admitted_locked") <
          tab_click.find("g_virtual_bag_state"));

    const std::string public_runtime = read_source(QOL_EXTENSION_BAG_PUBLIC_RUNTIME_SOURCE);
    const std::string capacity = source_function(
        public_runtime, "bool virtual_bag_logical_capacity(int32_t logical_bag, int32_t* out_capacity) {");
    const std::string equip = source_function(public_runtime,
        "std::string data_virtual_bag_test_equip(int index, int bag_type) {");
    const std::string item = source_function(public_runtime,
        "std::string data_virtual_bag_test_item(int index, int slot, int category, int count) {");
    CHECK(capacity.find("extension_bag_session_admitted_locked") != std::string::npos);
    CHECK(equip.find("extension_bag_session_admitted_locked") < equip.find("set_test_equipped"));
    CHECK(item.find("extension_bag_session_admitted_locked") < item.find("set_item("));

    const std::string render = read_source(QOL_EXTENSION_BAG_RENDER_SOURCE);
    const std::string install_view =
        source_function(render, "bool install_module_view_locked(int bag) {");
    CHECK(install_view.find("extension_bag_session_admitted_locked") <
          install_view.find("set_bag_size_word_locked"));

    const std::string store = read_source(QOL_EXTENSION_BAG_STORE_SOURCE);
    const std::string store_click =
        source_function(store, "uint64_t store_tab_item_proc(void* ctrl, uint64_t event, void*, void*) {");
    const std::string store_install =
        source_function(store, "void store_install_module_view_locked(int bag) {");
    CHECK(store_click.find("extension_bag_session_admitted_locked") <
          store_click.find("store_pending_tab = index"));
    CHECK(store_install.find("extension_bag_session_admitted_locked") <
          store_install.find("set_bag_size_word_locked"));

    const std::string mix = read_source(QOL_EXTENSION_BAG_MIX_SOURCE);
    const std::string mix_click =
        source_function(mix, "uint64_t mix_tab_item_proc(void* ctrl, uint64_t event, void*, void*) {");
    const std::string mix_install =
        source_function(mix, "void mix_install_module_view_locked(int bag) {");
    CHECK(mix_click.find("extension_bag_session_admitted_locked") <
          mix_click.find("mix_pending_tab = index"));
    CHECK(mix_install.find("extension_bag_session_admitted_locked") <
          mix_install.find("set_bag_size_word_locked"));
}

}  // namespace

int main() {
    test_draw_end_can_read_inventory_state_while_holding_runtime_lock();
    test_main_menu_cleanup_due_edges_and_retries();
    test_inventory_state_reader_uses_popup_state_entry();
    test_panel_edges_cannot_reset_loaded_state();
    test_unadmitted_panel_mutators_fail_before_touching_state();
    std::printf("extension_bag_draw_lock_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

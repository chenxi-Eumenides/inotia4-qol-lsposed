#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"

#include <cstdio>
#include <array>
#include <cstdint>
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

template <typename Work, typename Finish>
uint64_t run_event_work_then_finish(std::mutex& mutex, Work work, Finish finish) {
    uint64_t result = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        result = work();
    }
    return finish(result);
}

struct PendingTabHarness {
    std::mutex mutex;
    bool enabled = true;
    bool admitted = true;
    uint64_t inventory_generation = 1;
    uint64_t tab_generation = 1;
    uint64_t pending_generation = 0;
    int pending = -1;
    void* live_root = nullptr;
    void* saved_root = nullptr;
    std::array<void*, 5> buttons{};
    std::array<void*, 5> children{};
    int commits = 0;

    bool button_is_current(int index, void* control) const {
        if (!enabled || !admitted || index < 0 || index >= 5 || control == nullptr ||
            saved_root == nullptr || saved_root != live_root ||
            tab_generation != inventory_generation || buttons[index] != control) {
            return false;
        }
        for (void* child : children) {
            if (child == control) return true;
        }
        return false;
    }

    void install(void* root, const std::array<void*, 5>& controls) {
        std::lock_guard<std::mutex> lock(mutex);
        if (saved_root != nullptr && saved_root != root) {
            ++inventory_generation;
            pending = -1;
            pending_generation = 0;
        }
        live_root = root;
        saved_root = root;
        buttons = controls;
        children = controls;
        tab_generation = inventory_generation;
    }

    void disable() {
        std::lock_guard<std::mutex> lock(mutex);
        enabled = false;
        ++inventory_generation;
        pending = -1;
        pending_generation = 0;
        buttons.fill(nullptr);
        saved_root = nullptr;
        tab_generation = 0;
    }

    bool queue(int index, void* control) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!button_is_current(index, control)) return false;
        pending = index;
        pending_generation = inventory_generation;
        return true;
    }

    template <typename Callback>
    bool commit(Callback&& callback) {
        std::unique_lock<std::mutex> lock(mutex);
        const int index = pending;
        if (index < 0 || index >= 5 || pending_generation != inventory_generation ||
            !button_is_current(index, buttons[index])) {
            pending = -1;
            pending_generation = 0;
            return false;
        }
        const uint64_t generation = inventory_generation;
        void* const root = saved_root;
        void* const control = buttons[index];
        pending = -1;
        pending_generation = 0;
        lock.unlock();
        callback();
        lock.lock();
        if (generation != inventory_generation || root != saved_root ||
            !button_is_current(index, control)) {
            return false;
        }
        ++commits;
        return true;
    }
};

struct ModuleViewHarness {
    bool installed = false;
    int bound = 0;
    int cleared = 0;

    bool install(int child_count, int capacity, int fail_binding_slot) {
        installed = false;
        bound = 0;
        cleared = 0;
        if (child_count < capacity) return false;
        for (int slot = 0; slot < capacity; ++slot) {
            if (slot == fail_binding_slot) {
                cleared = bound;
                bound = 0;
                return false;
            }
            ++bound;
        }
        installed = true;
        return true;
    }
};

struct TabInstallHarness {
    bool installed = false;
    int created = 0;
    int reclaimed = 0;

    bool install(int fail_index) {
        installed = false;
        created = 0;
        reclaimed = 0;
        for (int index = 0; index < 5; ++index) {
            ++created;
            if (index == fail_index) {
                reclaimed = created;
                created = 0;
                return false;
            }
        }
        installed = created == 5;
        return installed;
    }
};

struct EventAdmissionHarness {
    int extension_calls = 0;
    int original_calls = 0;

    void dispatch(bool admitted) {
        if (!admitted) {
            ++original_calls;
            return;
        }
        ++extension_calls;
    }
};

void test_tab_event_finish_reenters_only_after_unlock() {
    std::mutex mutex;
    bool finish_acquired_lock = false;
    const uint64_t result = run_event_work_then_finish(
        mutex, [] { return uint64_t{1}; }, [&](uint64_t work_result) {
            std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
            finish_acquired_lock = lock.owns_lock();
            return work_result;
        });
    CHECK(result == 1);
    CHECK(finish_acquired_lock);
}

void test_pending_tab_rejects_unadmitted_and_stale_controls() {
    PendingTabHarness harness;
    std::array<void*, 5> first{{reinterpret_cast<void*>(1), reinterpret_cast<void*>(2),
                                reinterpret_cast<void*>(3), reinterpret_cast<void*>(4),
                                reinterpret_cast<void*>(5)}};
    harness.install(reinterpret_cast<void*>(10), first);
    CHECK(harness.queue(2, first[2]));
    harness.disable();
    CHECK(!harness.commit([] {}));
    CHECK(harness.commits == 0);

    harness.enabled = true;
    std::array<void*, 5> rebuilt{{reinterpret_cast<void*>(11), reinterpret_cast<void*>(12),
                                 reinterpret_cast<void*>(13), reinterpret_cast<void*>(14),
                                 reinterpret_cast<void*>(15)}};
    harness.install(reinterpret_cast<void*>(20), rebuilt);
    CHECK(harness.queue(1, rebuilt[1]));
    std::array<void*, 5> after_rebuild{{reinterpret_cast<void*>(21), reinterpret_cast<void*>(22),
                                       reinterpret_cast<void*>(23), reinterpret_cast<void*>(24),
                                       reinterpret_cast<void*>(25)}};
    harness.install(reinterpret_cast<void*>(21), after_rebuild);
    CHECK(!harness.commit([] {}));
    CHECK(harness.commits == 0);

    CHECK(!harness.queue(1, reinterpret_cast<void*>(99)));
}

void test_pending_tab_callback_runs_unlocked_and_revalidates_generation() {
    PendingTabHarness harness;
    std::array<void*, 5> controls{{reinterpret_cast<void*>(31), reinterpret_cast<void*>(32),
                                   reinterpret_cast<void*>(33), reinterpret_cast<void*>(34),
                                   reinterpret_cast<void*>(35)}};
    harness.install(reinterpret_cast<void*>(30), controls);
    CHECK(harness.queue(0, controls[0]));
    bool callback_acquired_lock = false;
    CHECK(harness.commit([&] {
        std::unique_lock<std::mutex> callback_lock(harness.mutex, std::try_to_lock);
        callback_acquired_lock = callback_lock.owns_lock();
    }));
    CHECK(callback_acquired_lock);
    CHECK(harness.commits == 1);

    CHECK(harness.queue(0, controls[0]));
    CHECK(!harness.commit([&] {
        std::array<void*, 5> replacement{{reinterpret_cast<void*>(41),
                                         reinterpret_cast<void*>(42),
                                         reinterpret_cast<void*>(43),
                                         reinterpret_cast<void*>(44),
                                         reinterpret_cast<void*>(45)}};
        harness.install(reinterpret_cast<void*>(40), replacement);
    }));
    CHECK(harness.commits == 1);
}

void test_tab_install_failure_reclaims_partial_controls() {
    TabInstallHarness harness;
    CHECK(harness.install(-1));
    CHECK(harness.installed);
    CHECK(harness.created == 5);
    CHECK(!harness.install(2));
    CHECK(!harness.installed);
    CHECK(harness.created == 0);
    CHECK(harness.reclaimed == 3);
}

void test_unadmitted_event_delegates_only_to_original_path() {
    EventAdmissionHarness harness;
    harness.dispatch(false);
    CHECK(harness.extension_calls == 0);
    CHECK(harness.original_calls == 1);
    harness.dispatch(true);
    CHECK(harness.extension_calls == 1);
    CHECK(harness.original_calls == 1);
}

void test_module_view_install_is_atomic_for_missing_children_and_bind_failure() {
    ModuleViewHarness harness;
    CHECK(!harness.install(3, 4, -1));
    CHECK(!harness.installed);
    CHECK(harness.install(4, 4, -1));
    CHECK(harness.installed);
    CHECK(!harness.install(5, 5, 2));
    CHECK(!harness.installed);
    CHECK(harness.bound == 0);
    CHECK(harness.cleared == 2);
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
    const std::string source = read_source(QOL_EXTENSION_BAG_PUBLIC_BRIDGE_SOURCE);
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
    const std::string draw_end = source_function(
        render, "void virtual_bag_draw_end_locked(int inventory_state, std::unique_lock<std::mutex>& lock) {");
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
    const std::string tab_click = source_function(
        runtime, "void handle_extension_tab_click_impl_locked(int extension_bag,\n                                            std::unique_lock<std::mutex>* unlockable_lock) {");
    CHECK(tab_click.find("extension_bag_session_admitted_locked") != std::string::npos);
    CHECK(tab_click.find("extension_bag_session_admitted_locked") <
          tab_click.find("g_virtual_bag_state"));

    const std::string public_runtime = read_source(QOL_EXTENSION_BAG_PUBLIC_BRIDGE_SOURCE);
    const std::string capacity = source_function(
        public_runtime, "bool virtual_bag_logical_capacity(int32_t logical_bag, int32_t* out_capacity) {");
    const std::string equip = source_function(public_runtime,
        "std::string data_virtual_bag_test_equip(int index, int bag_type) {");
    const std::string item = source_function(public_runtime,
        "std::string data_virtual_bag_test_item(int index, int slot, int category, int count) {");
    CHECK(capacity.find("extension_bag_session_admitted_locked") != std::string::npos);
    CHECK(equip.find("extension_bag_session_admitted_locked") < equip.find("set_test_equipped"));
    CHECK(item.find("extension_bag_session_admitted_locked") < item.find("set_item("));

    const std::string projection = read_source(QOL_EXTENSION_BAG_PROJECTION_SOURCE);
    const std::string install_view =
        source_function(projection, "bool install_module_view_locked(int bag) {");
    CHECK(install_view.find("extension_bag_session_admitted_locked") <
          install_view.find("set_bag_size_word_locked"));

    const std::string store_tabs = read_source(QOL_EXTENSION_BAG_STORE_TABS_SOURCE);
    const std::string store_click =
        source_function(store_tabs, "uint64_t store_tab_item_proc(void* ctrl, uint64_t event, void*, void*) {");
    const std::string store_projection =
        read_source(QOL_EXTENSION_BAG_STORE_PROJECTION_SOURCE);
    const std::string store_install =
        source_function(store_projection, "void store_install_module_view_locked(int bag) {");
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

void test_runtime_regression_guards_match_behavioral_models() {
    const std::string runtime = read_source(QOL_EXTENSION_BAG_RUNTIME_SOURCE);
    const std::string proc = source_function(runtime, "uint64_t extension_tab_item_proc(");
    CHECK(!proc.empty());
    CHECK(proc.find("return finish(1)") == std::string::npos);

    const std::string install_tabs =
        source_function(runtime, "void install_extension_tab_buttons_locked() {");
    CHECK(install_tabs.find("invalidate_extension_tab_buttons_locked()") != std::string::npos);
    CHECK(install_tabs.find("extension_tab_buttons_complete_locked()") != std::string::npos);
    CHECK(install_tabs.find("set_rect") != std::string::npos);
    CHECK(install_tabs.find("set_control_data") != std::string::npos);
    const size_t create_loop = install_tabs.find("bool complete = true;");
    const size_t create_loop_end = install_tabs.find(
        "if (extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer) != mount_parent)",
        create_loop);
    CHECK(create_loop != std::string::npos && create_loop_end != std::string::npos);
    CHECK(install_tabs.substr(create_loop, create_loop_end - create_loop).find("continue") ==
          std::string::npos);
    CHECK(install_tabs.find("g_extension_tab_buttons = created") >
          install_tabs.find("if (!complete)"));

    const std::string queue =
        source_function(runtime, "void queue_extension_tab_click_locked(int extension_bag) {");
    const std::string commit = source_function(
        runtime, "void commit_pending_extension_tab_locked(std::unique_lock<std::mutex>& lock) {");
    CHECK(queue.find("g_virtual_bag_enabled.load()") != std::string::npos);
    CHECK(queue.find("extension_tab_control_is_current_locked") != std::string::npos);
    CHECK(commit.find("extension_tab_control_is_current_locked") != std::string::npos);
    CHECK(commit.find("handle_pending_extension_tab_click_locked(lock") != std::string::npos);

    const std::string pending_click = source_function(
        runtime, "void handle_pending_extension_tab_click_locked(std::unique_lock<std::mutex>& lock,");
    CHECK(pending_click.find("handle_extension_tab_click_impl_locked(extension_bag, &lock)") !=
          std::string::npos);
    const std::string click_impl = source_function(
        runtime, "void handle_extension_tab_click_impl_locked(int extension_bag,\n                                            std::unique_lock<std::mutex>* unlockable_lock) {");
    const size_t unlock = click_impl.find("unlockable_lock->unlock()");
    CHECK(unlock != std::string::npos);
    const size_t pending_make = click_impl.find("g_write_port->ui.make_item_description(ctrl)", unlock);
    const size_t relock = click_impl.find("unlockable_lock->lock()", pending_make);
    CHECK(unlock < pending_make && pending_make < relock);
    CHECK(click_impl.find("g_inventory_generation == generation") != std::string::npos);

    const std::string orchestrator = read_source(QOL_EXTENSION_BAG_ORCHESTRATOR_SOURCE);
    const std::string persist = source_function(orchestrator, "bool persist_state_locked(bool force) {");
    CHECK(persist.find("does not report durable staging") != std::string::npos);

    const std::string lifecycle = read_source(QOL_EXTENSION_BAG_LIFECYCLE_SOURCE);
    const std::string event = source_function(
        lifecycle, "uint64_t virtual_bag_event(uint64_t event, uint64_t param, uint64_t param2) {");
    CHECK(event.find("if (!session_admitted)") < event.find("g_virtual_bag_enabled.load()"));
    CHECK(event.find("return g_orig_event != nullptr ? g_orig_event(event, param, param2) : 0") !=
          std::string::npos);
    const std::string main_menu = source_function(lifecycle, "bool prepare_main_menu_locked() {");
    CHECK(main_menu.find("teardown_extension_bag_hosts_locked()") != std::string::npos);

    const std::string public_runtime = read_source(QOL_EXTENSION_BAG_PUBLIC_BRIDGE_SOURCE);
    const std::string disable =
        source_function(public_runtime, "bool set_virtual_bag_enabled(bool enabled) {");
    CHECK(disable.find("teardown_extension_bag_hosts_locked()") != std::string::npos);

    const std::string projection = read_source(QOL_EXTENSION_BAG_PROJECTION_SOURCE);
    const std::string install_view =
        source_function(projection, "bool install_module_view_locked(int bag) {");
    CHECK(install_view.find("item_root_or_children_unavailable") != std::string::npos);
    CHECK(install_view.find("item_control_binding_failed") != std::string::npos);
    CHECK(install_view.find("g_module_view_installed = true") >
          install_view.find("item_root_replaced_before_commit"));
    const std::string refresh =
        source_function(projection, "void refresh_projection_if_overwritten_locked() {");
    CHECK(refresh.find("ensure_module_view_root_current_locked()") != std::string::npos);
    const std::string render = read_source(QOL_EXTENSION_BAG_RENDER_SOURCE);
    const std::string draw_wrapper =
        source_function(render, "void virtual_bag_draw_end_wrapper() {");
    CHECK(draw_wrapper.find("virtual_bag_inventory_state_id()") <
          draw_wrapper.find("std::unique_lock<std::mutex> lock(g_virtual_bag_mtx)"));
    CHECK(draw_wrapper.find("virtual_bag_draw_end_locked(inventory_state, lock)") !=
          std::string::npos);
}

}  // namespace

int main() {
    test_tab_event_finish_reenters_only_after_unlock();
    test_pending_tab_rejects_unadmitted_and_stale_controls();
    test_pending_tab_callback_runs_unlocked_and_revalidates_generation();
    test_tab_install_failure_reclaims_partial_controls();
    test_unadmitted_event_delegates_only_to_original_path();
    test_module_view_install_is_atomic_for_missing_children_and_bind_failure();
    test_draw_end_can_read_inventory_state_while_holding_runtime_lock();
    test_main_menu_cleanup_due_edges_and_retries();
    test_inventory_state_reader_uses_popup_state_entry();
    test_panel_edges_cannot_reset_loaded_state();
    test_unadmitted_panel_mutators_fail_before_touching_state();
    test_runtime_regression_guards_match_behavioral_models();
    std::printf("extension_bag_draw_lock_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

#include "core/native/game_cache.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "infra/frame/frame_task.h"
#include "infra/runtime.h"
#include "infra/scheduler.h"
#include "game/json/game_json.h"

// Compile the production scheduler into this standalone fixture so phase checks exercise its TLS.
#include "../infra/scheduler.cpp"

namespace {

constexpr char kNotReadyJson[] = "{\"error\":\"not ready\"}";
constexpr char kMapJson[] = "{\"map_id\":34,\"x\":-1,\"y\":-1}";

std::atomic<int64_t> g_frame{1};
std::atomic<int> g_clock_reads{0};
std::atomic<int> g_player_builds{0};
std::atomic<int> g_map_builds{0};
std::atomic<int> g_player_version{1};
std::atomic<bool> g_player_builder_not_ready{false};
std::atomic<bool> g_player_recursive_read{false};
std::atomic<bool> g_in_world{true};
std::atomic<bool> g_builder_on_wrong_thread{false};
std::thread::id g_main_thread;
std::mutex g_clock_mtx;
std::condition_variable g_clock_cv;
std::mutex g_reader_mtx;
std::condition_variable g_reader_cv;
int g_started_readers = 0;
std::string g_recursive_result;
int g_failures = 0;

void check(bool condition, const char* expression, int line);
#define CHECK(condition) check((condition), #condition, __LINE__)

std::string player_json(int version) {
    return "{\"player\":{\"version\":" + std::to_string(version) + "}}";
}

int64_t fake_frame_count() {
    g_clock_reads.fetch_add(1, std::memory_order_relaxed);
    g_clock_cv.notify_all();
    return g_frame.load(std::memory_order_relaxed);
}

bool fake_world_gate() {
    if (std::this_thread::get_id() != g_main_thread) {
        g_builder_on_wrong_thread.store(true, std::memory_order_relaxed);
    }
    return g_in_world.load(std::memory_order_relaxed);
}

bool fake_main_thread_gate() { return std::this_thread::get_id() == g_main_thread; }

std::string build_player() {
    if (std::this_thread::get_id() != g_main_thread) {
        g_builder_on_wrong_thread.store(true, std::memory_order_relaxed);
    }
    g_player_builds.fetch_add(1, std::memory_order_relaxed);
    if (g_player_recursive_read.exchange(false, std::memory_order_relaxed)) {
        g_recursive_result = data_player_json();
    }
    if (g_player_builder_not_ready.load(std::memory_order_relaxed)) return kNotReadyJson;
    return player_json(g_player_version.load(std::memory_order_relaxed));
}

std::string build_map() {
    if (std::this_thread::get_id() != g_main_thread) {
        g_builder_on_wrong_thread.store(true, std::memory_order_relaxed);
    }
    g_map_builds.fetch_add(1, std::memory_order_relaxed);
    return kMapJson;
}

struct ResultRead {
    std::string result;
    int calls = 0;
    bool force_refresh = true;
    bool logic_phase_active = false;
    bool render_phase_active = false;
};

bool logic_result_read(int64_t, void* context) {
    auto* request = static_cast<ResultRead*>(context);
    request->logic_phase_active =
        qol::infra::scheduler_is_dispatching(qol::infra::FramePoint::kLogicPre);
    request->render_phase_active =
        qol::infra::scheduler_is_dispatching(qol::infra::FramePoint::kRenderPre);
    if (request->force_refresh) frame_cache_force_refresh();
    request->result = data_player_json();
    ++request->calls;
    return false;
}

bool render_result_read(int64_t, void* context) {
    auto* request = static_cast<ResultRead*>(context);
    request->logic_phase_active =
        qol::infra::scheduler_is_dispatching(qol::infra::FramePoint::kLogicPre);
    request->render_phase_active =
        qol::infra::scheduler_is_dispatching(qol::infra::FramePoint::kRenderPre);
    frame_cache_force_refresh();
    request->result = data_player_json();
    ++request->calls;
    return false;
}

void mark_reader_started() {
    {
        std::lock_guard<std::mutex> lock(g_reader_mtx);
        ++g_started_readers;
    }
    g_reader_cv.notify_all();
}

void wait_for_readers(int expected) {
    std::unique_lock<std::mutex> lock(g_reader_mtx);
    const bool ready = g_reader_cv.wait_for(lock, std::chrono::milliseconds(250), [expected]() {
        return g_started_readers >= expected;
    });
    CHECK(ready);
}

void wait_for_clock_reads(int expected) {
    std::unique_lock<std::mutex> lock(g_clock_mtx);
    const bool ready = g_clock_cv.wait_for(lock, std::chrono::milliseconds(250), [expected]() {
        return g_clock_reads.load(std::memory_order_relaxed) >= expected;
    });
    CHECK(ready);
}

void check(bool condition, const char* expression, int line) {
    if (condition) return;
    ++g_failures;
    std::printf("FAIL test_game_cache.cpp:%d: %s\n", line, expression);
}

void dispatch_frame(int64_t frame) {
    g_frame.store(frame, std::memory_order_relaxed);
    frame_task_dispatch(kFramePointLogicPre);
}

void run_logic_result_read(int64_t frame, ResultRead* request) {
    g_frame.store(frame, std::memory_order_relaxed);
    CHECK(qol::infra::scheduler_add(qol::infra::FramePoint::kLogicPre, &logic_result_read,
                                    request, 1, 1) != 0);
    qol::infra::scheduler_dispatch(qol::infra::FramePoint::kLogicPre);
    CHECK(request->calls == 1);
    CHECK(request->logic_phase_active && !request->render_phase_active);
    CHECK(!qol::infra::scheduler_is_dispatching(qol::infra::FramePoint::kLogicPre));
}

}  // namespace

std::string build_gamestate_json() {
    if (std::this_thread::get_id() != g_main_thread) {
        g_builder_on_wrong_thread.store(true, std::memory_order_relaxed);
    }
    return "{\"screen\":\"main_menu\"}";
}

std::string build_snapshot_json() {
    if (std::this_thread::get_id() != g_main_thread) {
        g_builder_on_wrong_thread.store(true, std::memory_order_relaxed);
    }
    return "{\"screen\":\"main_menu\"}";
}

int main() {
    g_main_thread = std::this_thread::get_id();
    qol::infra::runtime_init({&fake_frame_count});
    qol::infra::scheduler_reset_for_test();
    frame_cache_set_world_gate(&fake_world_gate);
    frame_cache_set_main_thread_gate(&fake_main_thread_gate);
    frame_cache_set_json_builder(0, &build_player);
    frame_cache_set_json_builder(2, &build_map);
    CHECK(data_map_json() == kNotReadyJson);  // world input is unknown until a safe frame samples it
    CHECK(g_map_builds.load() == 0);

    // A first write-result read is cold: LogicPre samples world state before the cache task runs.
    ResultRead cold_write;
    run_logic_result_read(1, &cold_write);
    CHECK(cold_write.result == player_json(1));
    CHECK(frame_cache_ready());
    CHECK(g_player_builds.load() == 1);
    CHECK(data_player_json() == player_json(1));
    CHECK(qol::game::json::is_ready_state_snapshot(player_json(1)));
    CHECK(!qol::game::json::is_ready_state_snapshot(kNotReadyJson));
    CHECK(!qol::game::json::is_ready_state_snapshot("{\"error\":\"not in game\"}"));

    // A sticky main-thread identity is insufficient: outside LogicPre, dirty getters fail closed.
    g_player_version.store(2, std::memory_order_relaxed);
    frame_cache_force_refresh();
    const int before_unsafe_read = g_player_builds.load();
    const auto unsafe_read_started = std::chrono::steady_clock::now();
    CHECK(data_player_json() == kNotReadyJson);
    CHECK(std::chrono::steady_clock::now() - unsafe_read_started < std::chrono::milliseconds(50));
    CHECK(g_player_builds.load() == before_unsafe_read);

    // RenderPre on the same thread is also unsafe; only a real scheduler LogicPre callback qualifies.
    ResultRead render_read;
    g_frame.store(2, std::memory_order_relaxed);
    CHECK(qol::infra::scheduler_add(qol::infra::FramePoint::kRenderPre, &render_result_read,
                                    &render_read, 1, 1) != 0);
    qol::infra::scheduler_dispatch(qol::infra::FramePoint::kRenderPre);
    CHECK(render_read.calls == 1 && render_read.result == kNotReadyJson);
    CHECK(!render_read.logic_phase_active && render_read.render_phase_active);
    CHECK(!qol::infra::scheduler_is_dispatching(qol::infra::FramePoint::kRenderPre));
    CHECK(g_player_builds.load() == before_unsafe_read);

    // The same dirty slot is freshly built once from the actual LogicPre result callback.
    const int before_inline_refresh = g_player_builds.load();
    ResultRead inline_write;
    run_logic_result_read(2, &inline_write);
    CHECK(inline_write.result == player_json(2));
    CHECK(g_player_builds.load() == before_inline_refresh + 1);

    // Recursive dependency reads may observe the old snapshot, but the outer build publishes v3.
    g_player_version.store(3, std::memory_order_relaxed);
    g_player_recursive_read.store(true, std::memory_order_relaxed);
    const int before_recursive_build = g_player_builds.load();
    ResultRead recursive_write;
    run_logic_result_read(3, &recursive_write);
    CHECK(g_recursive_result == player_json(2));
    CHECK(recursive_write.result == player_json(3));
    CHECK(g_player_builds.load() == before_recursive_build + 1);

    // A failed synchronous build remains dirty and returns not-ready rather than the v3 body.
    g_player_version.store(4, std::memory_order_relaxed);
    g_player_builder_not_ready.store(true, std::memory_order_relaxed);
    const int before_failed_build = g_player_builds.load();
    ResultRead failed_write;
    run_logic_result_read(4, &failed_write);
    CHECK(failed_write.result == kNotReadyJson);
    CHECK(g_player_builds.load() == before_failed_build + 1);
    g_player_builder_not_ready.store(false, std::memory_order_relaxed);
    const int before_retry = g_player_builds.load();
    ResultRead retry_read;
    retry_read.force_refresh = false;
    run_logic_result_read(5, &retry_read);
    CHECK(retry_read.result == player_json(4));
    CHECK(g_player_builds.load() == before_retry + 1);

    // A non-main reader never invokes a builder; a later fake logic frame publishes its result.
    std::string lazy_result;
    int reads_before_lazy = g_clock_reads.load();
    std::thread lazy_reader([&lazy_result]() {
        mark_reader_started();
        lazy_result = data_map_json();
    });
    wait_for_readers(1);
    wait_for_clock_reads(reads_before_lazy + 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(g_map_builds.load() == 0);
    dispatch_frame(2);
    lazy_reader.join();
    CHECK(lazy_result == kMapJson);
    CHECK(g_map_builds.load() == 1);

    // Concurrent readers all observe the complete newly published snapshot; one slot is built once.
    g_player_version.store(5, std::memory_order_relaxed);
    frame_cache_force_refresh();
    const int before_concurrent_build = g_player_builds.load();
    const int before_concurrent_clock_reads = g_clock_reads.load();
    std::mutex readers_mtx;
    std::condition_variable readers_cv;
    int started_readers = 0;
    std::vector<std::string> reader_results(6);
    std::vector<std::thread> readers;
    for (size_t i = 0; i < reader_results.size(); ++i) {
        readers.emplace_back([&, i]() {
            {
                std::lock_guard<std::mutex> lock(readers_mtx);
                ++started_readers;
            }
            readers_cv.notify_one();
            reader_results[i] = data_player_json();
        });
    }
    {
        std::unique_lock<std::mutex> lock(readers_mtx);
        const bool all_started = readers_cv.wait_for(lock, std::chrono::milliseconds(250), [&]() {
            return started_readers == static_cast<int>(reader_results.size());
        });
        CHECK(all_started);
    }
    wait_for_clock_reads(before_concurrent_clock_reads + static_cast<int>(reader_results.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(g_player_builds.load() == before_concurrent_build);
    dispatch_frame(3);
    for (std::thread& reader : readers) reader.join();
    for (const std::string& result : reader_results) CHECK(result == player_json(5));
    CHECK(g_player_builds.load() == before_concurrent_build + 1);

    // Without a publishing frame, non-main readers wait only for the bounded interval and fail closed.
    frame_cache_force_refresh();
    const int before_bounded_wait = g_player_builds.load();
    const int clock_reads_before_wait = g_clock_reads.load();
    std::string timeout_result;
    const auto timeout_started = std::chrono::steady_clock::now();
    std::thread timeout_reader([&timeout_result]() { timeout_result = data_player_json(); });
    wait_for_clock_reads(clock_reads_before_wait + 1);
    timeout_reader.join();
    const auto timeout_duration = std::chrono::steady_clock::now() - timeout_started;
    CHECK(timeout_result == kNotReadyJson);
    CHECK(timeout_duration >= std::chrono::milliseconds(80));
    CHECK(timeout_duration < std::chrono::milliseconds(250));
    CHECK(g_player_builds.load() == before_bounded_wait);

    // stop removes the scheduled callback and prevents a later fake frame from building.
    const int before_stop = g_player_builds.load();
    frame_cache_stop();
    dispatch_frame(4);
    CHECK(g_player_builds.load() == before_stop);

    // Compatibility restart resumes frame-driven refresh without a worker thread.
    frame_cache_start();
    dispatch_frame(5);
    CHECK(g_player_builds.load() == before_stop + 1);

    // Leaving world clears world-only snapshots and preserves the existing fallback body.
    g_in_world.store(false, std::memory_order_relaxed);
    dispatch_frame(6);
    CHECK(data_player_json() == "{\"error\":\"not in game\"}");

    // Non-world system slots still build through the logic-frame task, not an HTTP thread.
    std::string main_menu_state;
    const int reads_before_menu = g_clock_reads.load();
    std::thread main_menu_reader([&main_menu_state]() {
        mark_reader_started();
        main_menu_state = data_gamestate_json();
    });
    wait_for_readers(2);
    wait_for_clock_reads(reads_before_menu + 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    dispatch_frame(7);
    main_menu_reader.join();
    CHECK(main_menu_state == "{\"screen\":\"main_menu\"}");

    std::string main_menu_snapshot;
    const int reads_before_snapshot = g_clock_reads.load();
    std::thread snapshot_reader([&main_menu_snapshot]() {
        mark_reader_started();
        main_menu_snapshot = data_snapshot_json();
    });
    wait_for_readers(3);
    wait_for_clock_reads(reads_before_snapshot + 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    dispatch_frame(8);
    snapshot_reader.join();
    CHECK(main_menu_snapshot == "{\"screen\":\"main_menu\"}");
    CHECK(!g_builder_on_wrong_thread.load());

    frame_cache_stop();
    if (g_failures != 0) {
        std::printf("game_cache: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("game_cache: pass");
    return 0;
}

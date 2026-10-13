// game_cache.cpp —— 由 game_data.cpp 拆分生成（纯搬代码，零逻辑变更）

#include "infra/runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "game_cache.h"
#include "infra/frame/frame_task.h"
#include "infra/scheduler.h"

// system 槽（gamestate/snapshot）builder 仍由 game_system 提供（T2l-integ 口径，system 槽留 game_system）；
// 此处仅前向声明，避免 infra include legacy/system 头（include 边消除，链接语义不变）。
std::string build_gamestate_json();
std::string build_snapshot_json();

namespace {

struct CacheSlot {
    const char* name;             // 端点名（日志用）
    int interval;                 // 刷新间隔：0=请求驱动；n>0=请求后的每 n 帧刷新
    uint64_t last_frame;          // 上次成功发布的帧号
    std::string json;             // 最近发布的不可变 JSON 快照
    std::string (*build)();       // 构造器：仅由逻辑帧主线程调用
    bool world_only;              // 非 world 时是否返回 not in game
    bool requested = false;       // 只有被读取过的槽才参与后续调度
    bool dirty = false;           // 等待逻辑帧构造
    bool published = false;
    bool building = false;
    std::thread::id building_thread{};
    uint64_t building_generation = 0;
    uint64_t change_version = 0;
    uint64_t publish_version = 0;
};

CacheSlot g_cache_slots[] = {
    // player/party/inventory/skills/mercenaries 与 world 五槽（map/units/drops/enemies/interactives）
    // 的 legacy builder 由 host 装配期注入（frame_cache_set_json_builder）；未注入前保持 nullptr，
    // 读侧降级为 not in game。
    {"player",      1, 0, "", nullptr,                  true},
    {"party",       1, 0, "", nullptr,                  true},
    {"map",         0, 0, "", nullptr,                  true},
    {"units",       1, 0, "", nullptr,                  true},
    {"gamestate",   1, 0, "", build_gamestate_json,   false},
    {"snapshot",    1, 0, "", build_snapshot_json,    false},
    {"inventory",   0, 0, "", nullptr,                  true},
    {"skills",      0, 0, "", nullptr,                  true},
    {"mercenaries", 0, 0, "", nullptr,                  true},
    {"drops",       1, 0, "", nullptr,                  true},
    {"enemies",     0, 0, "", nullptr,                  true},
    {"interactives",0, 0, "", nullptr,                  true},
};
constexpr int CACHE_SLOT_COUNT = sizeof(g_cache_slots) / sizeof(g_cache_slots[0]);
constexpr auto kReaderWait = std::chrono::milliseconds(100);
constexpr auto kBuilderTimeBudget = std::chrono::milliseconds(3);
constexpr size_t kBuilderCountBudget = 3;
constexpr char kNotReadyJson[] = "{\"error\":\"not ready\"}";
constexpr char kNotInGameJson[] = "{\"error\":\"not in game\"}";

std::mutex g_cache_mtx;
std::condition_variable g_cache_cv;
std::atomic<bool> g_cache_ready{false};
std::atomic<bool> g_cache_enabled{true};
std::atomic<bool> g_cache_world{false};
std::atomic<bool (*)()> g_world_gate{nullptr};
std::atomic<bool (*)()> g_main_thread_gate{nullptr};
uint64_t g_cache_generation = 1;  // 受 g_cache_mtx 保护；stop/start 使在途结果失效
FrameTaskId g_cache_task = 0;    // 受 g_cache_mtx 保护
bool g_world_state_known = false;
bool g_last_world_state = false;

bool on_game_main_thread() {
    bool (*fn)() = g_main_thread_gate.load(std::memory_order_acquire);
    return fn != nullptr && fn();
}

bool cached_world_state() { return g_cache_world.load(std::memory_order_acquire); }

void mark_dirty_locked(CacheSlot& slot) {
    slot.dirty = true;
    ++slot.change_version;
}

bool cache_logic_task(int64_t frame, void* /*ctx*/);

bool has_requested_builder_locked() {
    for (const CacheSlot& slot : g_cache_slots) {
        if (slot.requested && slot.build != nullptr) return true;
    }
    return false;
}

void schedule_cache_task_locked() {
    if (!g_cache_enabled.load(std::memory_order_acquire) || g_cache_task != 0 ||
        !has_requested_builder_locked()) {
        return;
    }
    // 任务只在有消费者请求缓存后注册；真正的 builder 仍由 frame_source 的逻辑帧派发。
    g_cache_task = frame_task_add(kFramePointLogicPre, &cache_logic_task, nullptr, 1, 0);
}

std::string fallback_locked(const CacheSlot& slot) {
    if (slot.published) return slot.json;
    if (slot.build == nullptr || (slot.world_only && !cached_world_state())) {
        return kNotInGameJson;
    }
    return kNotReadyJson;
}

bool builder_result_ready(const std::string& json) {
    return !json.empty() && json != kNotReadyJson && json != kNotInGameJson;
}

bool sample_logic_world_state() {
    bool (*world_gate)() = g_world_gate.load(std::memory_order_acquire);
    const bool in_world = world_gate != nullptr && world_gate();
    g_cache_world.store(in_world, std::memory_order_release);

    bool changed_world = false;
    {
        std::lock_guard<std::mutex> lock(g_cache_mtx);
        changed_world = !g_world_state_known || in_world != g_last_world_state;
        g_world_state_known = true;
        g_last_world_state = in_world;
        if (changed_world) {
            for (CacheSlot& slot : g_cache_slots) {
                if (!slot.requested || slot.build == nullptr) continue;
                if (!in_world && slot.world_only) {
                    if (slot.published || !slot.json.empty()) {
                        slot.json.clear();
                        slot.published = false;
                        slot.last_frame = 0;
                        ++slot.publish_version;
                    }
                    slot.dirty = false;
                } else {
                    mark_dirty_locked(slot);
                }
            }
        }
    }
    if (changed_world) g_cache_cv.notify_all();
    return in_world;
}

// The caller owns the logic-frame safety check. This common pipeline is shared by scheduled
// refresh and a one-slot read made from a verified logic-phase owner result callback.
bool build_slot(int index, int64_t frame) {
    std::string (*builder)() = nullptr;
    uint64_t generation = 0;
    uint64_t change_version = 0;
    const std::thread::id current_thread = std::this_thread::get_id();
    {
        std::lock_guard<std::mutex> lock(g_cache_mtx);
        CacheSlot& slot = g_cache_slots[index];
        if (!g_cache_enabled.load(std::memory_order_relaxed) || !slot.dirty ||
            slot.build == nullptr || slot.building) {
            return false;
        }
        generation = g_cache_generation;
        change_version = slot.change_version;
        builder = slot.build;
        slot.building = true;
        slot.building_thread = current_thread;
        slot.building_generation = generation;
    }

    std::string json = builder();
    const bool ready = builder_result_ready(json);
    bool published = false;
    {
        std::lock_guard<std::mutex> lock(g_cache_mtx);
        CacheSlot& slot = g_cache_slots[index];
        const bool owns_build = slot.building && slot.building_thread == current_thread &&
                                slot.building_generation == generation;
        if (owns_build) {
            slot.building = false;
            slot.building_thread = std::thread::id{};
            if (g_cache_enabled.load(std::memory_order_relaxed) &&
                g_cache_generation == generation && slot.build == builder && ready &&
                slot.change_version == change_version) {
                slot.json = std::move(json);
                slot.last_frame = frame > 0 ? static_cast<uint64_t>(frame) : 0;
                slot.published = true;
                slot.dirty = false;
                ++slot.publish_version;
                g_cache_ready.store(true, std::memory_order_release);
                published = true;
            } else if (g_cache_enabled.load(std::memory_order_relaxed) &&
                       g_cache_generation == generation && slot.build == builder) {
                // Keep the invalidation pending after a failed build or a concurrent write.
                slot.dirty = true;
            }
        }
    }
    g_cache_cv.notify_all();
    return published;
}

bool build_dirty_slot_inline(int index, int64_t frame) {
    if (!on_game_main_thread() ||
        !qol::infra::scheduler_is_dispatching(qol::infra::FramePoint::kLogicPre)) {
        return false;
    }
    return build_slot(index, frame);
}

bool cache_logic_task(int64_t frame, void* /*ctx*/) {
    if (!g_cache_enabled.load(std::memory_order_acquire)) return false;
    if (!on_game_main_thread()) return true;

    bool (*world_gate)() = g_world_gate.load(std::memory_order_acquire);
    const bool in_world = world_gate != nullptr && world_gate();
    g_cache_world.store(in_world, std::memory_order_release);

    struct PendingBuild {
        int index;
    };
    std::vector<PendingBuild> pending;
    uint64_t generation = 0;
    bool changed_world = false;
    {
        std::lock_guard<std::mutex> lock(g_cache_mtx);
        if (!g_cache_enabled.load(std::memory_order_relaxed)) return false;
        generation = g_cache_generation;
        changed_world = !g_world_state_known || in_world != g_last_world_state;
        g_world_state_known = true;
        g_last_world_state = in_world;

        for (int i = 0; i < CACHE_SLOT_COUNT; ++i) {
            CacheSlot& slot = g_cache_slots[i];
            if (slot.world_only && !in_world) {
                if (slot.published || !slot.json.empty()) {
                    slot.json.clear();
                    slot.published = false;
                    slot.last_frame = 0;
                    ++slot.publish_version;
                }
                slot.dirty = false;
                continue;
            }
            if (!slot.requested || slot.build == nullptr) continue;
            const bool periodic_refresh =
                slot.interval > 0 &&
                (!slot.published || frame <= 0 || slot.last_frame == 0 ||
                 (static_cast<uint64_t>(frame) >= slot.last_frame &&
                  static_cast<uint64_t>(frame) - slot.last_frame >=
                      static_cast<uint64_t>(slot.interval)));
            if (changed_world || (periodic_refresh && !slot.dirty)) {
                mark_dirty_locked(slot);
            }
            if (slot.dirty && !slot.building) pending.push_back(PendingBuild{i});
        }
    }
    if (changed_world) g_cache_cv.notify_all();

    const auto started = std::chrono::steady_clock::now();
    size_t attempted = 0;
    for (const PendingBuild& item : pending) {
        if (attempted >= kBuilderCountBudget ||
            std::chrono::steady_clock::now() - started >= kBuilderTimeBudget) {
            break;
        }
        {
            std::lock_guard<std::mutex> lock(g_cache_mtx);
            if (!g_cache_enabled.load(std::memory_order_relaxed) ||
                g_cache_generation != generation) {
                return false;
            }
        }
        ++attempted;
        (void)build_slot(item.index, frame);
    }

    return true;
}

std::string data_slot_json(int idx) {
    const int64_t frame = qol::infra::frame_count();
    const bool is_main_thread = on_game_main_thread();
    const bool in_logic_frame =
        is_main_thread && qol::infra::scheduler_is_dispatching(qol::infra::FramePoint::kLogicPre);
    if (in_logic_frame) (void)sample_logic_world_state();
    std::unique_lock<std::mutex> lock(g_cache_mtx);
    CacheSlot& slot = g_cache_slots[idx];
    slot.requested = true;
    schedule_cache_task_locked();

    // A builder may read its own slot as a dependency. Only that recursive read may observe the
    // last published body; external readers never turn an in-progress refresh into success.
    if (slot.building && slot.building_thread == std::this_thread::get_id()) {
        return fallback_locked(slot);
    }

    if (slot.build == nullptr) return fallback_locked(slot);
    const bool in_world = cached_world_state();
    if (slot.world_only && !in_world) {
        return g_world_state_known ? kNotInGameJson : kNotReadyJson;
    }

    if (slot.interval == 0) {
        if (!slot.dirty &&
            (!slot.published || (frame > 0 && slot.last_frame != static_cast<uint64_t>(frame)))) {
            mark_dirty_locked(slot);
        }
    } else if (!slot.dirty &&
               (!slot.published ||
                (frame > 0 && slot.last_frame != 0 &&
                 static_cast<uint64_t>(frame) >= slot.last_frame &&
                 static_cast<uint64_t>(frame) - slot.last_frame >=
                     static_cast<uint64_t>(slot.interval)))) {
        mark_dirty_locked(slot);
    }

    if (!slot.dirty) return fallback_locked(slot);
    const uint64_t observed_publish = slot.publish_version;
    if (slot.building && is_main_thread) return kNotReadyJson;

    if (is_main_thread) {
        lock.unlock();
        (void)build_dirty_slot_inline(idx, frame);
        lock.lock();
        if (!slot.dirty && slot.published) return slot.json;
        return kNotReadyJson;
    }

    g_cache_cv.wait_for(lock, kReaderWait, [&slot, observed_publish]() {
        return slot.publish_version != observed_publish ||
               !g_cache_enabled.load(std::memory_order_acquire);
    });
    if (!slot.dirty && slot.published) return slot.json;
    return kNotReadyJson;
}

}  // namespace

void frame_cache_set_json_builder(int32_t slot, std::string (*fn)()) {
    if (slot < 0 || slot >= CACHE_SLOT_COUNT) return;
    std::lock_guard<std::mutex> lock(g_cache_mtx);
    CacheSlot& cache_slot = g_cache_slots[slot];
    if (cache_slot.build == fn) return;
    cache_slot.build = fn;
    cache_slot.json.clear();
    cache_slot.last_frame = 0;
    cache_slot.published = false;
    if (cache_slot.requested && fn != nullptr) mark_dirty_locked(cache_slot);
    schedule_cache_task_locked();
}

void frame_cache_set_world_gate(bool (*fn)()) {
    g_world_gate.store(fn, std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_cache_mtx);
    g_world_state_known = false;
    for (CacheSlot& slot : g_cache_slots) {
        if (slot.requested && slot.build != nullptr) mark_dirty_locked(slot);
    }
    schedule_cache_task_locked();
}

void frame_cache_set_main_thread_gate(bool (*fn)()) {
    g_main_thread_gate.store(fn, std::memory_order_release);
}

// 兼容旧入口：只切换帧任务门禁，不创建预取线程。
void frame_cache_start() {
    std::lock_guard<std::mutex> lock(g_cache_mtx);
    ++g_cache_generation;
    g_cache_enabled.store(true, std::memory_order_release);
    for (CacheSlot& slot : g_cache_slots) {
        if (slot.requested && slot.build != nullptr && !slot.published) mark_dirty_locked(slot);
    }
    schedule_cache_task_locked();
}

void frame_cache_stop() {
    FrameTaskId task = 0;
    {
        std::lock_guard<std::mutex> lock(g_cache_mtx);
        ++g_cache_generation;
        g_cache_enabled.store(false, std::memory_order_release);
        task = g_cache_task;
        g_cache_task = 0;
        for (CacheSlot& slot : g_cache_slots) slot.dirty = false;
    }
    if (task != 0) frame_task_remove(task);
    g_cache_cv.notify_all();
}

void frame_cache_force_refresh() {
    std::lock_guard<std::mutex> lock(g_cache_mtx);
    for (CacheSlot& slot : g_cache_slots) {
        if (slot.requested && slot.build != nullptr) mark_dirty_locked(slot);
    }
    schedule_cache_task_locked();
}

bool frame_cache_ready() { return g_cache_ready.load(std::memory_order_acquire); }

std::string data_player_json() { return data_slot_json(0); }

std::string data_party_json() { return data_slot_json(1); }

std::string data_inventory_json() { return data_slot_json(6); }

std::string data_map_json() { return data_slot_json(2); }

std::string data_units_json() { return data_slot_json(3); }

std::string data_gamestate_json() { return data_slot_json(4); }

std::string data_snapshot_json() { return data_slot_json(5); }

std::string data_skills_json() { return data_slot_json(7); }

std::string data_mercenaries_json() { return data_slot_json(8); }

std::string data_drops_json() { return data_slot_json(9); }

std::string data_enemies_json() { return data_slot_json(10); }

std::string data_interactives_json() { return data_slot_json(11); }

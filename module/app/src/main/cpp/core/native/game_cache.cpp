// game_cache.cpp —— 由 game_data.cpp 拆分生成（纯搬代码，零逻辑变更）

#include "game_character.h"
#include "game_party.h"
#include "game_inventory.h"
#include "game_world.h"
#include "game_system.h"

#include "game_access.h"
#include "game_symbols.h"

#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define MOVE_TAG "Inotia4Move"
#define MOVE_LOG(...) __android_log_print(ANDROID_LOG_INFO, MOVE_TAG, __VA_ARGS__)
#include "game_cache.h"
#include "game_state.h"

struct CacheSlot {
    const char* name;             // 端点名（日志用）
    int interval;                 // 刷新间隔：0=惰性（请求驱动）；n>0=每 n 帧预取
    uint64_t last_frame;          // 上次刷新帧号
    std::string json;             // 缓存内容（world 时有效）
    std::string (*build)();       // 构造器：读游戏内存拼 JSON
    bool world_only;              // v0.5.16：非 world 态下是否报错（gamestate/snapshot=false 全状态可用）
    std::atomic<bool> refreshing{false};  // v0.4.58：有线程正在构造（并发请求直接返回旧缓存）
};

CacheSlot g_cache_slots[] = {
    {"player",      1, 0, "", build_player_json,      true},
    {"party",       1, 0, "", build_party_json,       true},
    {"map",         0, 0, "", build_map_json,         true},  // v0.4.61：瓦片矩阵静态数据（同图不变），惰性获取（切图才变，请求驱动）
    {"units",       1, 0, "", build_units_json,       true},
    {"gamestate",   1, 0, "", build_gamestate_json,   false},
    {"snapshot",    1, 0, "", build_snapshot_json,    false},
    {"inventory",   0, 0, "", build_inventory_json,   true},  // 惰性：偶发查看，请求驱动
    {"skills",      0, 0, "", build_skills_json,      true},  // 惰性
    {"mercenaries", 0, 0, "", build_mercenaries_json, true},  // 惰性
    {"drops",       1, 0, "", build_drops_json,       true},
    {"enemies",     0, 0, "", build_enemies_json,     true},  // 惰性：units 子集（type==1），请求驱动
    {"interactives",0, 0, "", build_interactives_json,true},  // 惰性：units 子集（type==2 且可交互），请求驱动
};
constexpr int CACHE_SLOT_COUNT = sizeof(g_cache_slots) / sizeof(g_cache_slots[0]);

std::mutex g_cache_mtx;                    // 保护缓存读写
std::atomic<bool> g_cache_ready{false};    // 已成功构造过至少一个槽
std::thread g_cache_thread;                // v0.4.59：预取线程（仅驱动 interval>0 槽）
std::atomic<bool> g_cache_stop{false};

// 等待帧边界：帧号变化（= 本帧 Draw 完成，数据稳定）即返回；超时兜底（游戏暂停/主菜单）
uint64_t wait_frame_boundary(uint64_t since) {
    for (int i = 0; i < 100; ++i) {
        int64_t f = data_frame_count();
        if (f > 0 && static_cast<uint64_t>(f) != since) return static_cast<uint64_t>(f);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    int64_t f = data_frame_count();
    return f > 0 ? static_cast<uint64_t>(f) : since;
}

// 构造单槽（调用方持锁）并更新帧号；非 world 清空缓存
void rebuild_slot_locked(CacheSlot& s, uint64_t frame) {
    if (game_in_world()) {
        s.json = s.build();
        s.last_frame = frame;
        g_cache_ready.store(true);
    } else {
        s.json.clear();
    }
}

// 惰性刷新（interval=0 槽）：同帧复用（本帧已构造 → 返回缓存），跨帧 → 单飞等帧边界构造；
// 构造期间并发请求返回旧缓存（不等帧，避免请求率>帧率时全部排队等帧）
std::string data_slot_lazy(int idx) {
    CacheSlot& s = g_cache_slots[idx];
    uint64_t f = data_frame_count();
    if (s.last_frame != 0 && f == s.last_frame) {
        std::lock_guard<std::mutex> lock(g_cache_mtx);
        return s.json.empty() ? s.build() : s.json;
    }
    if (s.refreshing.exchange(true)) {
        std::lock_guard<std::mutex> lock(g_cache_mtx);
        return s.json.empty() ? s.build() : s.json;
    }
    uint64_t boundary = wait_frame_boundary(f);
    std::lock_guard<std::mutex> lock(g_cache_mtx);
    if (s.last_frame != boundary) {
        rebuild_slot_locked(s, boundary);
    }
    s.refreshing.store(false);
    return s.json.empty() ? s.build() : s.json;
}

// 统一入口：interval>0 → 预取槽直接读缓存（预取线程保证新鲜）；interval=0 → 惰性
std::string data_slot_json(int idx) {
    // v0.5.16：非 world 态下 world 专属槽应诚实报错而非返回陈旧缓存
    //（预取线程在退出 world 后停止构造但不清缓存，此前会返回上一帧的 world 数据）。
    // gamestate/snapshot（world_only=false）全状态可用，直接现构。
    if (!game_in_world()) {
        CacheSlot& s = g_cache_slots[idx];
        if (s.world_only) {
            std::lock_guard<std::mutex> lock(g_cache_mtx);
            s.json.clear();   // 清陈旧缓存，回 world 后首帧重取，避免短暂脏数据
            return "{\"error\":\"not in game\"}";
        }
        return s.build();
    }
    if (g_cache_slots[idx].interval > 0) {
        std::lock_guard<std::mutex> lock(g_cache_mtx);
        if (!g_cache_slots[idx].json.empty()) return g_cache_slots[idx].json;
    }
    return data_slot_lazy(idx);   // 缓存未就绪/惰性槽：走惰性路径（锁外等帧构造）
}

// 预取线程：帧计数驱动，仅构造 interval>0 槽（每 n 帧一次）
void cache_prefetch_thread_fn() {
    uint64_t last_frame = 0;
    while (!g_cache_stop.load()) {
        int64_t f = data_frame_count();
        if (f > 0 && static_cast<uint64_t>(f) != last_frame) {
            last_frame = f;
            if (!game_in_world()) continue;
            for (int i = 0; i < CACHE_SLOT_COUNT; ++i) {
                CacheSlot& s = g_cache_slots[i];
                if (s.interval <= 0) continue;
                if (f >= s.last_frame &&
                    f - s.last_frame < static_cast<uint64_t>(s.interval)) continue;
                std::string j = s.build();   // 锁外构造（units BFS 等耗时操作不持锁）
                std::lock_guard<std::mutex> lock(g_cache_mtx);
                s.json = std::move(j);       // 锁内仅 move 交换（µs 级），不阻塞请求读
                s.last_frame = f;
                g_cache_ready.store(true);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void frame_cache_start() {
    if (g_cache_thread.joinable()) return;
    bool need_thread = false;
    for (int i = 0; i < CACHE_SLOT_COUNT; ++i) {
        if (g_cache_slots[i].interval > 0) { need_thread = true; break; }
    }
    if (!need_thread) return;
    g_cache_stop.store(false);
    g_cache_thread = std::thread(cache_prefetch_thread_fn);
}

// 停止预取线程（API 全局开关关闭时调用）：置停止标志并 join，join 后可再次 frame_cache_start()。
void frame_cache_stop() {
    g_cache_stop.store(true);
    if (g_cache_thread.joinable()) {
        g_cache_thread.join();
    }
}

void frame_cache_force_refresh() {
    if (!game_in_world()) return;
    std::lock_guard<std::mutex> lock(g_cache_mtx);
    uint64_t f = static_cast<uint64_t>(data_frame_count());
    for (int i = 0; i < CACHE_SLOT_COUNT; ++i) {
        CacheSlot& s = g_cache_slots[i];
        s.json = s.build();
        s.last_frame = f;
    }
    g_cache_ready.store(true);
}

bool frame_cache_ready() { return g_cache_ready.load(); }

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

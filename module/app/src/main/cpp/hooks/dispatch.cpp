#include "hooks/dispatch.h"

// 共享资源分派实现（见 hooks/dispatch.h 与 ARCHITECTURE.md §4.5）。
//
// 线程纪律：subscribe/unsubscribe/count 可来自任意线程；dispatch_run_text 在 owner 的
// wrapper 内（游戏主线程）。注册/注销持互斥锁；分派先持锁快照消费者，再锁外执行回调，
// 保证「本轮注销下轮生效」且不持锁调用外部代码（§4.4 死锁规则）。

#include <cstring>
#include <mutex>

namespace qol::hooks {
namespace {

constexpr uint32_t kMaxResources = 8;
constexpr uint32_t kMaxConsumersPerResource = 32;

struct Consumer {
    DispatchId id = 0;
    DispatchPhase phase = DispatchPhase::kBefore;
    ConsumerKind kind = ConsumerKind::kObserve;
    TextDispatchFn fn = nullptr;
    void* ctx = nullptr;
};

struct ResourceGroup {
    const char* id = nullptr;  // nullptr = 空槽
    Consumer consumers[kMaxConsumersPerResource];
    uint32_t count = 0;
};

ResourceGroup g_groups[kMaxResources];
std::mutex g_mtx;
uint64_t g_next_id = 1;  // 0 保留为无效句柄

// 布尔门型 Intercept（D2c-2b-1）：独立于文本链 ConsumerKind::kIntercept 的按资源登记表。
constexpr uint32_t kMaxInterceptGates = 8;

struct InterceptGate {
    const char* id = nullptr;  // nullptr = 空槽
    InterceptGateFn fn = nullptr;
    void* ctx = nullptr;
};

InterceptGate g_gates[kMaxInterceptGates];

int find_gate(const char* resource_id) {
    for (uint32_t i = 0; i < kMaxInterceptGates; ++i) {
        if (g_gates[i].id != nullptr && std::strcmp(g_gates[i].id, resource_id) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool is_empty(const char* text) { return text == nullptr || text[0] == '\0'; }

int find_group(const char* resource_id) {
    for (uint32_t i = 0; i < kMaxResources; ++i) {
        if (g_groups[i].id != nullptr && std::strcmp(g_groups[i].id, resource_id) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int find_consumer(const ResourceGroup& group, DispatchId id) {
    for (uint32_t i = 0; i < group.count; ++i) {
        if (group.consumers[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

// 同资源重入门禁（§4.5 幂等与重入）：回调内再次分派同资源直接返回 original。
constexpr int kMaxReentryDepth = 8;
thread_local const char* t_reentry[kMaxReentryDepth] = {};
thread_local int t_reentry_depth = 0;

bool in_reentry(const char* resource_id) {
    for (int i = 0; i < t_reentry_depth; ++i) {
        if (t_reentry[i] != nullptr && std::strcmp(t_reentry[i], resource_id) == 0) return true;
    }
    return false;
}

bool valid_kind(ConsumerKind kind) {
    return kind == ConsumerKind::kObserve || kind == ConsumerKind::kModify ||
           kind == ConsumerKind::kIntercept;
}

bool valid_phase(DispatchPhase phase) {
    return phase == DispatchPhase::kBefore || phase == DispatchPhase::kAfter;
}

}  // namespace

DispatchId dispatch_subscribe(const char* resource_id, DispatchPhase phase, ConsumerKind kind,
                              TextDispatchFn fn, void* ctx) {
    if (is_empty(resource_id) || fn == nullptr) return 0;
    if (!valid_phase(phase) || !valid_kind(kind)) return 0;

    std::lock_guard<std::mutex> lock(g_mtx);
    int index = find_group(resource_id);
    if (index < 0) {
        for (uint32_t i = 0; i < kMaxResources; ++i) {
            if (g_groups[i].id == nullptr) {
                g_groups[i].id = resource_id;
                index = static_cast<int>(i);
                break;
            }
        }
        if (index < 0) return 0;  // 资源槽满
    }
    ResourceGroup& group = g_groups[index];
    if (group.count >= kMaxConsumersPerResource) return 0;
    if (kind == ConsumerKind::kIntercept) {
        // 短路唯一性（§4.5）：同一资源最多一个 Intercept，跨阶段统一检查。
        for (uint32_t i = 0; i < group.count; ++i) {
            if (group.consumers[i].kind == ConsumerKind::kIntercept) return 0;
        }
    }

    const DispatchId id = g_next_id++;
    if (id == 0) return 0;
    Consumer& slot = group.consumers[group.count];
    slot.id = id;
    slot.phase = phase;
    slot.kind = kind;
    slot.fn = fn;
    slot.ctx = ctx;
    ++group.count;
    return id;
}

bool dispatch_unsubscribe(const char* resource_id, DispatchId id) {
    if (is_empty(resource_id) || id == 0) return false;
    std::lock_guard<std::mutex> lock(g_mtx);
    const int index = find_group(resource_id);
    if (index < 0) return false;
    ResourceGroup& group = g_groups[index];
    const int consumer = find_consumer(group, id);
    if (consumer < 0) return false;
    for (uint32_t i = static_cast<uint32_t>(consumer); i + 1 < group.count; ++i) {
        group.consumers[i] = group.consumers[i + 1];
    }
    --group.count;
    return true;
}

uint32_t dispatch_consumer_count(const char* resource_id, DispatchPhase phase, ConsumerKind kind) {
    if (is_empty(resource_id)) return 0;
    std::lock_guard<std::mutex> lock(g_mtx);
    const int index = find_group(resource_id);
    if (index < 0) return 0;
    const ResourceGroup& group = g_groups[index];
    uint32_t count = 0;
    for (uint32_t i = 0; i < group.count; ++i) {
        if (group.consumers[i].phase == phase && group.consumers[i].kind == kind) ++count;
    }
    return count;
}

const char* dispatch_run_text(const char* resource_id, uint32_t text_id, const char* original) {
    if (is_empty(resource_id)) return original;
    if (in_reentry(resource_id)) return original;  // 同资源重入：fail-safe 透传

    // 快照消费者：分派期间的注销/新增本轮不生效（§4.5 启动期确定，不运行期重排）。
    Consumer snapshot[kMaxConsumersPerResource];
    uint32_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        const int index = find_group(resource_id);
        if (index < 0) return original;  // 未注册资源
        const ResourceGroup& group = g_groups[index];
        for (uint32_t i = 0; i < group.count; ++i) {
            snapshot[count++] = group.consumers[i];
        }
    }
    if (count == 0) return original;

    if (t_reentry_depth < kMaxReentryDepth) {
        t_reentry[t_reentry_depth++] = resource_id;
    }

    const char* current = original;
    bool intercepted = false;
    // 短路后 after 阶段仅 Observe 生效时，反向扫描的起点（不含）。
    int after_observe_start = static_cast<int>(count);

    // before 阶段：注册顺序；Observe 观察 / Modify 链式累积 / Intercept 短路。
    for (uint32_t i = 0; i < count; ++i) {
        const Consumer& c = snapshot[i];
        if (c.phase != DispatchPhase::kBefore) continue;
        if (c.kind == ConsumerKind::kObserve) {
            (void)c.fn(text_id, current, c.ctx);
        } else if (c.kind == ConsumerKind::kModify) {
            const char* replaced = c.fn(text_id, current, c.ctx);
            if (replaced != nullptr) current = replaced;
        } else {  // kIntercept
            const char* decided = c.fn(text_id, current, c.ctx);
            if (decided != nullptr) current = decided;
            intercepted = true;
            break;
        }
    }

    // after 阶段：逆序；Observe 观察 / Modify 覆盖 / Intercept 短路。
    if (!intercepted) {
        for (int i = static_cast<int>(count) - 1; i >= 0; --i) {
            const Consumer& c = snapshot[i];
            if (c.phase != DispatchPhase::kAfter) continue;
            if (c.kind == ConsumerKind::kObserve) {
                (void)c.fn(text_id, current, c.ctx);
            } else if (c.kind == ConsumerKind::kModify) {
                const char* replaced = c.fn(text_id, current, c.ctx);
                if (replaced != nullptr) current = replaced;
            } else {  // kIntercept
                const char* decided = c.fn(text_id, current, c.ctx);
                if (decided != nullptr) current = decided;
                intercepted = true;
                after_observe_start = i;
                break;
            }
        }
    }

    // 短路后：跳过 original 与 after 阶段的 Modify/Intercept；after 阶段的 Observe 仍执行
    // （可观察，不可改）。after_observe_start 之前的 after 消费者尚未执行，只跑其中的 Observe。
    if (intercepted) {
        for (int i = after_observe_start - 1; i >= 0; --i) {
            const Consumer& c = snapshot[i];
            if (c.phase != DispatchPhase::kAfter || c.kind != ConsumerKind::kObserve) continue;
            (void)c.fn(text_id, current, c.ctx);
        }
    }

    if (t_reentry_depth > 0) --t_reentry_depth;
    return current;
}

bool dispatch_set_intercept(const char* resource_id, InterceptGateFn fn, void* ctx) {
    if (is_empty(resource_id) || fn == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_mtx);
    const int existing = find_gate(resource_id);
    if (existing >= 0) {
        // 同一资源最多 1 个；同 fn 幂等，异 fn 拒绝（§4.5 唯一性）。
        return g_gates[existing].fn == fn;
    }
    for (uint32_t i = 0; i < kMaxInterceptGates; ++i) {
        if (g_gates[i].id != nullptr) continue;
        g_gates[i].id = resource_id;
        g_gates[i].fn = fn;
        g_gates[i].ctx = ctx;
        return true;
    }
    return false;  // 门槽满
}

bool dispatch_clear_intercept(const char* resource_id) {
    if (is_empty(resource_id)) return false;
    std::lock_guard<std::mutex> lock(g_mtx);
    const int index = find_gate(resource_id);
    if (index < 0) return false;
    g_gates[index] = InterceptGate{};
    return true;
}

bool dispatch_intercept_blocks(const char* resource_id, void* ctx) {
    if (is_empty(resource_id)) return false;
    // 先持锁快照，再锁外调用回调（§4.4 死锁规则）。
    InterceptGateFn fn = nullptr;
    void* registered_ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        const int index = find_gate(resource_id);
        if (index < 0) return false;
        fn = g_gates[index].fn;
        registered_ctx = g_gates[index].ctx;
    }
    if (fn == nullptr) return false;
    return fn(ctx != nullptr ? ctx : registered_ctx);
}

void dispatch_reset_for_test() {
    std::lock_guard<std::mutex> lock(g_mtx);
    for (uint32_t i = 0; i < kMaxResources; ++i) {
        g_groups[i] = ResourceGroup{};
    }
    for (uint32_t i = 0; i < kMaxInterceptGates; ++i) {
        g_gates[i] = InterceptGate{};
    }
    g_next_id = 1;
}

}  // namespace qol::hooks

#include "infra/events.h"

// 类型化事件总线实现（见 infra/events.h 的接口契约）。
//
// 线程模型：订阅表由 g_mtx 保护，任意线程可 subscribe/unsubscribe；
// publish 仅在持锁期间复制一份订阅者快照，随后在锁外调用处理器。
//
// 为什么快照派发：处理器内可以再次 subscribe/unsubscribe，甚至 publish；
// 若在持有 g_mtx 时调用处理器，处理器内的订阅表操作会自锁（std::mutex
// 不可重入），或与派发期间的注销竞争导致迭代器失效。取快照后立即释放
// 锁，使本轮派发完全不读取实时订阅表，既避免自锁也避免迭代器失效。
//
// 快照语义：派发开始时复制的列表决定本轮调用集合，因此派发期间被注销的
// 订阅在本轮仍可能被调用一次；下一轮 publish 才会读取到最新订阅表。这是
// 刻意选择的代价——要求“注销后当轮立即生效”的插件应在处理器内自行检查
// 存活标志，而不是依赖总线在派发中途剔除订阅。
//
// 为什么容量有界：订阅表固定上限，防止插件泄漏订阅（忘记 unsubscribe）
// 导致无界内存增长；满时 subscribe 返回 0，由调用方决定降级策略。

#include <cstddef>
#include <mutex>
#include <vector>

namespace qol::infra {
namespace {

constexpr std::size_t kMaxSubscriptions = 256;

struct Entry {
    EventTypeId type;
    EventHandler handler;
    void* ctx;
    SubscriptionId id;
};

std::mutex g_mtx;
std::vector<Entry> g_entries;   // 按订阅顺序保存，保证派发按订阅顺序
SubscriptionId g_next_id = 1;   // 0 保留为无效 id，故从 1 起单调递增

}  // namespace

SubscriptionId subscribe(EventTypeId type, EventHandler handler, void* ctx) {
    // 先判空指针：与容量无关的参数错误优先返回 0。
    if (handler == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_entries.size() >= kMaxSubscriptions) {
        return 0;
    }
    const SubscriptionId id = g_next_id++;
    g_entries.push_back(Entry{type, handler, ctx, id});
    return id;
}

bool unsubscribe(SubscriptionId id) {
    if (id == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mtx);
    for (auto it = g_entries.begin(); it != g_entries.end(); ++it) {
        if (it->id == id) {
            g_entries.erase(it);
            return true;
        }
    }
    // 未知 id 或重复注销：均视为无效操作，返回 false。
    return false;
}

uint32_t publish(EventTypeId type, const void* payload) {
    std::vector<Entry> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        for (const Entry& e : g_entries) {
            if (e.type == type) {
                snapshot.push_back(e);
            }
        }
    }
    // 锁外派发：处理器可安全地 subscribe/unsubscribe/publish。
    for (const Entry& e : snapshot) {
        e.handler(e.ctx, payload);
    }
    return static_cast<uint32_t>(snapshot.size());
}

void clear_all() {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_entries.clear();
    g_next_id = 1;  // 同时重置 id 计数，避免测试与 deactivate 后 id 无界增长
}

void events_reset_for_test() {
    // 与 clear_all 同义：测试需要与插件 deactivate 相同的清理语义。
    clear_all();
}

uint32_t subscription_count() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return static_cast<uint32_t>(g_entries.size());
}

}  // namespace qol::infra

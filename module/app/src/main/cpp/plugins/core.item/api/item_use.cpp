#include "plugins/core.item/api/item_use.h"

#include <cstring>
#include <mutex>

// item.use 订阅表：与 hooks/dispatch 的共享资源模型一致（§4.5）。
// 注册/注销可来自任意线程；分派只在游戏线程（CHAR_UseItemEx wrapper 内）执行。
// 分派先持锁快照，再锁外调用回调，保证“本轮注销下轮生效”且不持锁调用外部代码。

namespace qol::features::core_item {
namespace {

using contracts::ItemUseApi;
using contracts::QolItemUseEvent;
using contracts::QolItemUsePostFn;

constexpr uint32_t kMaxSubscribers = 8;
constexpr const char* kInvalidArgument = "invalid argument";
constexpr const char* kConflict = "item.use subscription conflict";
constexpr const char* kBusy = "item.use subscription capacity full";
constexpr const char* kNotFound = "item.use subscription not found";

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

struct Subscriber {
    uint64_t id = 0;
    const char* owner = nullptr;
    QolItemUsePostFn fn = nullptr;
    void* ctx = nullptr;
};

Subscriber g_subscribers[kMaxSubscribers];
uint32_t g_count = 0;
uint64_t g_next_id = 1;
std::mutex g_mutex;

// 回调内再次触发物品使用时不递归分派；外层本次通知继续完成。
thread_local bool t_in_dispatch = false;

QolResult subscribe_post(const char* owner, QolItemUsePostFn fn, void* ctx,
                         uint64_t* out_subscription) {
    if (out_subscription != nullptr) *out_subscription = 0;
    if (owner == nullptr || owner[0] == '\0' || fn == nullptr || out_subscription == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t i = 0; i < g_count; ++i) {
        if (g_subscribers[i].fn == fn && std::strcmp(g_subscribers[i].owner, owner) == 0) {
            return error_result(kQolErrConflict, kConflict);
        }
    }
    if (g_count >= kMaxSubscribers) {
        return error_result(kQolErrBusy, kBusy);
    }

    const uint64_t id = g_next_id++;
    if (id == 0) return error_result(kQolErrInternal, kInvalidArgument);
    Subscriber& slot = g_subscribers[g_count];
    slot.id = id;
    slot.owner = owner;
    slot.fn = fn;
    slot.ctx = ctx;
    ++g_count;
    *out_subscription = id;
    return contracts::ok();
}

QolResult unsubscribe_post(uint64_t subscription) {
    if (subscription == 0) return error_result(kQolErrInvalidArgument, kInvalidArgument);

    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t i = 0; i < g_count; ++i) {
        if (g_subscribers[i].id != subscription) continue;
        g_subscribers[i] = g_subscribers[g_count - 1];
        --g_count;
        g_subscribers[g_count] = Subscriber{};
        return contracts::ok();
    }
    return error_result(kQolErrNotFound, kNotFound);
}

}  // namespace

void item_use_dispatch(const QolItemUseEvent& event) {
    if (t_in_dispatch) return;
    t_in_dispatch = true;

    Subscriber snapshot[kMaxSubscribers];
    uint32_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        count = g_count;
        for (uint32_t i = 0; i < count; ++i) snapshot[i] = g_subscribers[i];
    }
    for (uint32_t i = 0; i < count; ++i) {
        snapshot[i].fn(&event, snapshot[i].ctx);
    }
    t_in_dispatch = false;
}

void item_use_reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& subscriber : g_subscribers) subscriber = Subscriber{};
    g_count = 0;
}

extern const ItemUseApi kItemUseApi{
    sizeof(ItemUseApi),
    &subscribe_post,
    &unsubscribe_post,
};

}  // namespace qol::features::core_item

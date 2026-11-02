#include "save_pre_save.h"

#include <array>
#include <cstring>
#include <mutex>
#include <vector>

namespace qol::features::core_save::save_pre_save {
namespace {

using namespace qol::contracts;

struct Observer {
    QolOwnerToken owner = 0;
    SavePreSaveObserverFn fn = nullptr;
    void* user = nullptr;
    uint64_t id = 0;
};

std::mutex g_mutex;
std::array<Observer, kMaxObservers> g_observers{};
uint64_t g_next_id = 1;
// 主线程通知期间为 true；观察者内再次 notify 直接返回。
bool g_notifying = false;

QolResult fail(int32_t code, const char* text) {
    return error(code, text, static_cast<uint32_t>(std::strlen(text)));
}

}  // namespace

QolResult register_observer(QolOwnerToken owner, SavePreSaveObserverFn fn, void* user,
                            uint64_t* out_id) {
    if (out_id == nullptr) return fail(kQolErrInvalidArgument, "observer id is required");
    *out_id = 0;
    if (owner == 0 || fn == nullptr) return fail(kQolErrInvalidArgument, "observer invalid");
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const Observer& observer : g_observers) {
        if (observer.id == 0) continue;
        if (observer.owner == owner && observer.fn == fn && observer.user == user) {
            return fail(kQolErrBlocked, "observer already registered");
        }
    }
    for (Observer& observer : g_observers) {
        if (observer.id != 0) continue;
        const uint64_t id = g_next_id != 0 ? g_next_id++ : 0;
        if (id == 0) return fail(kQolErrInternal, "observer id exhausted");
        observer = Observer{owner, fn, user, id};
        *out_id = id;
        return ok();
    }
    return fail(kQolErrBlocked, "observer limit reached");
}

QolResult unregister_observer(QolOwnerToken owner, uint64_t id) {
    if (id == 0) return ok();  // 未知/已注销 id 幂等成功。
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Observer& observer : g_observers) {
        if (observer.id != id) continue;
        if (observer.owner != owner) return fail(kQolErrNotFound, "observer owner mismatch");
        observer = Observer{};
        return ok();
    }
    return ok();
}

void notify(int32_t slot) {
    std::vector<Observer> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_notifying) return;  // 重入通知不得嵌套。
        g_notifying = true;
        snapshot.reserve(kMaxObservers);
        for (const Observer& observer : g_observers) {
            if (observer.id != 0 && observer.fn != nullptr) snapshot.push_back(observer);
        }
    }
    for (const Observer& observer : snapshot) {
        // 观察型契约：消费者异常不得传播到原版保存路径。
        try {
            observer.fn(observer.user, slot);
        } catch (...) {
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_notifying = false;
    }
}

void unregister_owner(QolOwnerToken owner) {
    if (owner == 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Observer& observer : g_observers) {
        if (observer.id != 0 && observer.owner == owner) observer = Observer{};
    }
}

void reset_for_test() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_observers.fill(Observer{});
    g_next_id = 1;
    g_notifying = false;
}

}  // namespace qol::features::core_save::save_pre_save

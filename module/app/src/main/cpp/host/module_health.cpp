#include "host/module_health.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace qol::host {
namespace {

struct ObserverSlot {
    uint64_t id = 0;
    ModuleHealthObserver observer = nullptr;
    void* context = nullptr;
    bool active = true;
    uint64_t last_revision = 0;
    uint32_t in_flight = 0;
};

struct ModuleHealthState {
    std::mutex mutex;
    std::condition_variable observers_drained;
    uint64_t revision = 0;
    uint32_t known_mask = 0;
    uint32_t failed_mask = 0;
    uint8_t main_menu_seen = 0;
    uint64_t next_observer_id = 1;
    bool dispatching = false;
    std::vector<std::shared_ptr<ObserverSlot>> observers;
    std::deque<contracts::QolModuleHealthSnapshot> pending_snapshots;
};

ModuleHealthState& state() {
    static ModuleHealthState instance;
    return instance;
}

std::recursive_mutex& delivery_mutex() {
    static std::recursive_mutex instance;
    return instance;
}

thread_local const ObserverSlot* g_current_observer = nullptr;

uint32_t bit_mask(contracts::QolModuleHealthBit bit) {
    const uint32_t mask = static_cast<uint32_t>(bit);
    if (mask == 0 || (mask & (mask - 1)) != 0 ||
        (mask & ~contracts::kQolModuleHealthAllMask) != 0) return 0;
    return mask;
}

void increment_revision(ModuleHealthState& health) {
    if (health.revision != std::numeric_limits<uint64_t>::max()) ++health.revision;
}

bool set_fact_locked(ModuleHealthState& health, uint32_t mask, bool known, bool failed) {
    if (mask == 0) return false;
    const uint32_t before_known = health.known_mask;
    const uint32_t before_failed = health.failed_mask;
    if (known) {
        health.known_mask |= mask;
        if (failed) health.failed_mask |= mask;
        else health.failed_mask &= ~mask;
    } else {
        health.known_mask &= ~mask;
        health.failed_mask &= ~mask;
    }
    health.failed_mask &= health.known_mask;
    if (before_known != health.known_mask || before_failed != health.failed_mask) {
        increment_revision(health);
        return true;
    }
    return false;
}

contracts::QolModuleHealthSnapshot snapshot_locked(const ModuleHealthState& health) {
    return {sizeof(contracts::QolModuleHealthSnapshot), health.revision, health.known_mask,
            health.failed_mask, health.main_menu_seen};
}

void queue_snapshot_locked(ModuleHealthState& health) {
    health.pending_snapshots.push_back(snapshot_locked(health));
}

void finish_observer_call(ModuleHealthState& health,
                          const std::shared_ptr<ObserverSlot>& observer) {
    {
        std::lock_guard<std::mutex> lock(health.mutex);
        if (observer->in_flight > 0) --observer->in_flight;
    }
    health.observers_drained.notify_all();
}

void call_observer(ModuleHealthState& health,
                   const std::shared_ptr<ObserverSlot>& observer,
                   const contracts::QolModuleHealthSnapshot& snapshot) {
    ModuleHealthObserver callback = nullptr;
    void* context = nullptr;
    {
        std::lock_guard<std::mutex> lock(health.mutex);
        if (!observer->active || snapshot.revision <= observer->last_revision) return;
        observer->last_revision = snapshot.revision;
        ++observer->in_flight;
        callback = observer->observer;
        context = observer->context;
    }

    const ObserverSlot* previous_observer = g_current_observer;
    g_current_observer = observer.get();
    callback(context, snapshot);
    g_current_observer = previous_observer;
    finish_observer_call(health, observer);
}

void dispatch_pending_snapshots(ModuleHealthState& health) {
    {
        std::lock_guard<std::mutex> lock(health.mutex);
        if (health.dispatching) return;
        health.dispatching = true;
    }

    for (;;) {
        contracts::QolModuleHealthSnapshot snapshot{};
        std::vector<std::shared_ptr<ObserverSlot>> observers;
        {
            std::lock_guard<std::mutex> lock(health.mutex);
            if (health.pending_snapshots.empty()) {
                health.dispatching = false;
                return;
            }
            snapshot = health.pending_snapshots.front();
            health.pending_snapshots.pop_front();
            observers = health.observers;
        }
        for (const auto& observer : observers) call_observer(health, observer, snapshot);
    }
}

void apply_fact(contracts::QolModuleHealthBit bit, bool known, bool failed) {
    const uint32_t mask = bit_mask(bit);
    if (mask == 0) return;
    std::lock_guard<std::recursive_mutex> delivery_lock(delivery_mutex());
    ModuleHealthState& health = state();
    {
        std::lock_guard<std::mutex> lock(health.mutex);
        if (set_fact_locked(health, mask, known, failed)) queue_snapshot_locked(health);
    }
    dispatch_pending_snapshots(health);
}

QolResult read_facts(void*, contracts::QolModuleHealthSnapshot* out) {
    if (out == nullptr) return contracts::error(kQolErrInvalidArgument, nullptr, 0);
    ModuleHealthState& health = state();
    std::lock_guard<std::mutex> lock(health.mutex);
    *out = snapshot_locked(health);
    return contracts::ok();
}

const contracts::QolHostModuleHealthFactsApi kModuleHealthFactsApi{
    sizeof(contracts::QolHostModuleHealthFactsApi), nullptr, &read_facts};

}  // namespace

void module_health_set_fact(contracts::QolModuleHealthBit bit, bool known, bool failed) {
    apply_fact(bit, known, failed);
}

void module_health_observe_main_menu(bool is_main_menu) {
    if (!is_main_menu) return;
    ModuleHealthState& health = state();
    {
        std::lock_guard<std::mutex> lock(health.mutex);
        if (health.main_menu_seen != 0) return;
    }
    std::lock_guard<std::recursive_mutex> delivery_lock(delivery_mutex());
    {
        std::lock_guard<std::mutex> lock(health.mutex);
        if (health.main_menu_seen == 0) {
            health.main_menu_seen = 1;
            increment_revision(health);
            queue_snapshot_locked(health);
        }
    }
    dispatch_pending_snapshots(health);
}

void module_health_config_bootstrap_started() {
    std::lock_guard<std::recursive_mutex> delivery_lock(delivery_mutex());
    ModuleHealthState& health = state();
    {
        std::lock_guard<std::mutex> lock(health.mutex);
        if (set_fact_locked(health,
                            static_cast<uint32_t>(contracts::QolModuleHealthBit::kConfigStorage),
                            false, false)) queue_snapshot_locked(health);
    }
    dispatch_pending_snapshots(health);
}

void module_health_config_bootstrap_finished(bool durable_ready) {
    std::lock_guard<std::recursive_mutex> delivery_lock(delivery_mutex());
    ModuleHealthState& health = state();
    {
        std::lock_guard<std::mutex> lock(health.mutex);
        if (set_fact_locked(health,
                            static_cast<uint32_t>(contracts::QolModuleHealthBit::kConfigStorage),
                            true, !durable_ready)) queue_snapshot_locked(health);
    }
    dispatch_pending_snapshots(health);
}

const contracts::QolHostModuleHealthFactsApi& module_health_facts_api() {
    return kModuleHealthFactsApi;
}

QolResult module_health_subscribe(ModuleHealthObserver observer, void* context,
                                  uint64_t* out_subscription_id) {
    if (out_subscription_id != nullptr) *out_subscription_id = 0;
    if (observer == nullptr || out_subscription_id == nullptr)
        return contracts::error(kQolErrInvalidArgument, nullptr, 0);

    std::lock_guard<std::recursive_mutex> delivery_lock(delivery_mutex());
    ModuleHealthState& health = state();
    std::shared_ptr<ObserverSlot> slot;
    try {
        slot = std::make_shared<ObserverSlot>();
    } catch (const std::bad_alloc&) {
        return contracts::error(kQolErrBusy, nullptr, 0);
    }
    contracts::QolModuleHealthSnapshot snapshot{};
    {
        std::lock_guard<std::mutex> lock(health.mutex);
        if (health.next_observer_id == 0)
            return contracts::error(kQolErrBlocked, nullptr, 0);
        slot->id = health.next_observer_id;
        slot->observer = observer;
        slot->context = context;
        slot->last_revision = health.revision;
        slot->in_flight = 1;
        try {
            health.observers.push_back(slot);
        } catch (const std::bad_alloc&) {
            return contracts::error(kQolErrBusy, nullptr, 0);
        }
        ++health.next_observer_id;
        snapshot = snapshot_locked(health);
        *out_subscription_id = slot->id;
    }

    const ObserverSlot* previous_observer = g_current_observer;
    g_current_observer = slot.get();
    observer(context, snapshot);
    g_current_observer = previous_observer;
    finish_observer_call(health, slot);
    dispatch_pending_snapshots(health);
    return contracts::ok();
}

QolResult module_health_unsubscribe(uint64_t subscription_id) {
    if (subscription_id == 0) return contracts::error(kQolErrInvalidArgument, nullptr, 0);

    std::lock_guard<std::recursive_mutex> delivery_lock(delivery_mutex());
    ModuleHealthState& health = state();
    std::unique_lock<std::mutex> lock(health.mutex);
    const auto found = std::find_if(health.observers.begin(), health.observers.end(),
        [subscription_id](const auto& observer) { return observer->id == subscription_id; });
    if (found == health.observers.end())
        return contracts::error(kQolErrNotFound, nullptr, 0);

    const std::shared_ptr<ObserverSlot> observer = *found;
    observer->active = false;
    health.observers.erase(found);
    if (g_current_observer == observer.get() || g_current_observer != nullptr)
        return contracts::ok();
    health.observers_drained.wait(lock, [&observer] { return observer->in_flight == 0; });
    return contracts::ok();
}

void module_health_reset_for_test() {
    std::lock_guard<std::recursive_mutex> delivery_lock(delivery_mutex());
    ModuleHealthState& health = state();
    std::lock_guard<std::mutex> lock(health.mutex);
    for (const auto& observer : health.observers) observer->active = false;
    health.observers.clear();
    health.pending_snapshots.clear();
    health.dispatching = false;
    health.revision = 0;
    health.known_mask = 0;
    health.failed_mask = 0;
    health.main_menu_seen = 0;
    health.next_observer_id = 1;
}

}  // namespace qol::host

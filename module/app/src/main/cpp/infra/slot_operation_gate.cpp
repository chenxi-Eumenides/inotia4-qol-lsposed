#include "slot_operation_gate.h"
#include "infra/store.h"

#include <array>
#include <cstring>
#include <limits>

namespace qol::infra::slot_operation {
namespace {
std::mutex g_mutex;
std::array<contracts::QolSlotOperationLease, 3> g_leases{};
uint64_t g_next_id = 1;
QolResult fail(int32_t code, const char* text) {
    return contracts::error(code, text, static_cast<uint32_t>(std::strlen(text)));
}
contracts::QolSlotOperationLease* find(uint64_t id) {
    if (id == 0) return nullptr;
    for (auto& lease : g_leases) if (lease.id == id) return &lease;
    return nullptr;
}
QolResult validate(uint64_t owner, const contracts::QolSlotOperationLease* input) {
    if (input == nullptr || input->size < sizeof(*input) || owner == 0 || input->owner != owner ||
        input->id == 0 || input->slot_mask == 0 || input->slot_mask > 7)
        return fail(kQolErrInvalidArgument, "invalid operation lease");
    const auto* held = find(input->id);
    if (held == nullptr || held->owner != owner || held->revision != input->revision ||
        held->slot_mask != input->slot_mask) return fail(kQolErrBusy, "stale operation lease");
    return contracts::ok();
}
}
std::mutex& mutex() { return g_mutex; }
bool busy_locked(int32_t slot) {
    if (slot < 0 || slot > 2) return false;
    for (const auto& lease : g_leases) if (lease.id != 0 && (lease.slot_mask & (1u << slot))) return true;
    return false;
}
QolResult validate_locked(uint64_t owner, const contracts::QolSlotOperationLease* input, int32_t slot) {
    if (slot < 0 || slot > 2) return fail(kQolErrInvalidArgument, "invalid operation slot");
    const auto result = validate(owner, input);
    if (!contracts::succeeded(result)) return result;
    return (input->slot_mask & (1u << slot)) ? contracts::ok()
        : fail(kQolErrInvalidArgument, "slot outside operation mask");
}
bool reservation_admitted_locked(uint64_t owner, uint64_t operation, int32_t slot) {
    if (operation == 0) return !busy_locked(slot);
    const auto* held = find(operation);
    return held != nullptr && held->owner == owner && slot >= 0 && slot < 3 &&
           (held->slot_mask & (1u << slot)) != 0;
}
bool referenced_locked(uint64_t operation) {
    const auto* held = find(operation);
    return held != nullptr && held->referenced != 0;
}
QolResult acquire(uint64_t owner, uint32_t mask, contracts::QolSlotOperationLease* out) {
    if (owner == 0 || mask == 0 || mask > 7 || out == nullptr || out->size < sizeof(*out))
        return fail(kQolErrInvalidArgument, "invalid operation mask");
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int32_t slot = 0; slot < 3; ++slot) {
        if ((mask & (1u << slot)) && (busy_locked(slot) || store::reservation_busy_locked(slot)))
            return fail(kQolErrBusy, "operation slots occupied");
    }
    if (g_next_id == std::numeric_limits<uint64_t>::max()) return fail(kQolErrBusy, "operation id exhausted");
    for (auto& lease : g_leases) {
        if (lease.id != 0) continue;
        lease = {sizeof(lease), mask, g_next_id++, owner, 1, 0, 0};
        *out = lease;
        return contracts::ok();
    }
    return fail(kQolErrBusy, "operation capacity exhausted");
}
QolResult set_referenced(uint64_t owner, contracts::QolSlotOperationLease* input, uint32_t referenced) {
    if (referenced > 1) return fail(kQolErrInvalidArgument, "invalid reference value");
    std::lock_guard<std::mutex> lock(g_mutex);
    auto result = validate(owner, input);
    if (!contracts::succeeded(result)) return result;
    auto* held = find(input->id);
    if (held->referenced != referenced) {
        if (held->revision == UINT64_MAX) return fail(kQolErrBusy, "operation revision exhausted");
        held->referenced = referenced;
        ++held->revision;
    }
    *input = *held;
    return contracts::ok();
}
QolResult refresh(uint64_t owner, uint64_t id, contracts::QolSlotOperationLease* out) {
    if (owner == 0 || id == 0 || out == nullptr || out->size < sizeof(*out))
        return fail(kQolErrInvalidArgument, "invalid operation refresh");
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto* held = find(id);
    if (held == nullptr || held->owner != owner) return fail(kQolErrBlocked, "operation not owned");
    *out = *held;
    return contracts::ok();
}
QolResult release(uint64_t owner, const contracts::QolSlotOperationLease* input) {
    std::lock_guard<std::mutex> lock(g_mutex);
    // 已释放或未知 id 的重复收尾无副作用，按幂等语义返回 NotFound；id 仍在但
    // revision/owner 过期才 Busy，旧凭据不能借幂等收尾释放新状态。
    if (input != nullptr && input->size >= sizeof(*input) && owner != 0 &&
        input->owner == owner && input->id != 0 && find(input->id) == nullptr)
        return fail(kQolErrNotFound, "unknown operation");
    const auto result = validate(owner, input);
    if (!contracts::succeeded(result)) return result;
    if (store::operation_has_reservation_locked(input->id)) return fail(kQolErrBusy, "operation still has reservation");
    *find(input->id) = {};
    return contracts::ok();
}
void owner_stopped_locked(uint64_t owner) {
    for (auto& lease : g_leases) {
        if (lease.id == 0 || lease.owner != owner) continue;
        // 即使 operation 引用未置位，关联预算仍是有效引用，不能制造悬挂 reservation。
        if (lease.referenced == 0 && !store::operation_has_reservation_locked(lease.id)) lease = {};
    }
}
void owner_rebound_locked(uint64_t old_owner, uint64_t new_owner) {
    for (auto& lease : g_leases) {
        if (lease.id != 0 && lease.owner == old_owner && lease.revision != UINT64_MAX) {
            lease.owner = new_owner;
            ++lease.revision;
        }
    }
}
}  // namespace qol::infra::slot_operation

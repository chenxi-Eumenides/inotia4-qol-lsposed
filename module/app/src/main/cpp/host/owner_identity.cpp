#include "host/owner_identity.h"

#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>

namespace qol::host {
namespace {
struct Owner {
    void* identity = nullptr;
    contracts::QolOwnerToken token = 0;
    uint64_t generation = 1;
    uint64_t previous_token = 0;
    std::string store_namespace;
};
std::array<Owner, 32> g_owners;
std::mutex g_mutex;
uint64_t g_next_token = 1;
void (*g_store_stopped)(uint64_t) = nullptr;
void (*g_store_rebound)(uint64_t, uint64_t) = nullptr;

QolResult fail(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}
Owner* find_identity(void* identity) {
    for (auto& owner : g_owners) if (owner.identity == identity) return &owner;
    return nullptr;
}
Owner* find_token(contracts::QolOwnerToken token) {
    if (token == 0) return nullptr;
    for (auto& owner : g_owners) if (owner.identity != nullptr && owner.token == token) return &owner;
    return nullptr;
}
bool issue(Owner& owner) {
    if (owner.token != 0) return true;
    if (g_next_token == std::numeric_limits<uint64_t>::max()) return false;
    owner.token = g_next_token++;
    if (owner.previous_token != 0 && g_store_rebound != nullptr && !owner.store_namespace.empty())
        g_store_rebound(owner.previous_token, owner.token);
    return true;
}
QolResult query(void* identity, contracts::QolOwnerToken* token, uint64_t* generation) {
    if (identity == nullptr || token == nullptr || generation == nullptr)
        return fail(kQolErrInvalidArgument, "invalid owner query");
    std::lock_guard<std::mutex> lock(g_mutex);
    auto* owner = find_identity(identity);
    if (owner == nullptr) return fail(kQolErrNotFound, "owner not registered");
    *token = owner->token;
    *generation = owner->generation;
    return contracts::ok();
}
QolResult verify(void* identity, contracts::QolOwnerToken token) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto* owner = find_token(token);
    if (owner == nullptr || (identity != nullptr && owner->identity != identity))
        return fail(kQolErrBlocked, "owner token invalid");
    return contracts::ok();
}
const contracts::QolOwnerIdentityApi kApi{{sizeof(contracts::QolOwnerIdentityApi), 1, 0},
                                       &query, &verify};
}  // namespace

bool owner_register(void* identity) {
    if (identity == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (find_identity(identity) != nullptr) return true;
    for (auto& owner : g_owners) {
        if (owner.identity != nullptr) continue;
        owner.identity = identity;
        if (issue(owner)) return true;
        owner = Owner{};
        return false;
    }
    return false;
}
bool owner_resume(void* identity) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto* owner = identity == nullptr ? nullptr : find_identity(identity);
    return owner != nullptr && issue(*owner);
}
void owner_suspend(void* identity) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto* owner = identity == nullptr ? nullptr : find_identity(identity);
    if (owner == nullptr || owner->token == 0) return;
    owner->previous_token = owner->token;
    if (g_store_stopped != nullptr) g_store_stopped(owner->token);
    owner->token = 0;
    ++owner->generation;
}
bool owner_bind_store(void* identity, const char* store_namespace) {
    if (store_namespace == nullptr || *store_namespace == '\0') return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto* owner = identity == nullptr ? nullptr : find_identity(identity);
    if (owner == nullptr) return false;
    if (!owner->store_namespace.empty() && owner->store_namespace != store_namespace) return false;
    owner->store_namespace = store_namespace;
    return true;
}
QolResult owner_verify_token(contracts::QolOwnerToken token) { return verify(nullptr, token); }
QolResult owner_authorize_store(contracts::QolOwnerToken token, const char* store_namespace) {
    if (store_namespace == nullptr) return fail(kQolErrInvalidArgument, "invalid store namespace");
    std::lock_guard<std::mutex> lock(g_mutex);
    auto* owner = find_token(token);
    if (owner == nullptr || owner->store_namespace != store_namespace)
        return fail(kQolErrBlocked, "owner store not authorized");
    return contracts::ok();
}
const contracts::QolOwnerIdentityApi& owner_identity_api() { return kApi; }
void owner_identity_reset_for_test() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& owner : g_owners) owner = Owner{};
}
void owner_store_lifecycle_handlers(void (*stopped)(uint64_t), void (*rebound)(uint64_t, uint64_t)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_store_stopped = stopped;
    g_store_rebound = rebound;
}
}  // namespace qol::host

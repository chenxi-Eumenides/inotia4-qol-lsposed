#include "host/adapters.h"
#include "host/http_dispatch.h"
#include "host/http_registry.h"
#include "host/lifecycle_participants.h"
#include "host/module_health.h"
#include "host/owner_identity.h"
#include "host/save_route.h"
#include "host/settings_catalog_adapter.h"
#include "host/settings_config_bridge.h"
#include "host/config_namespace.h"
#include "host/features/production.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <limits>
#include <string>
#include <vector>

#include "infra/log/qol_log.h"
#include "game/ops/game_ops_common.h"
#include "contracts/module_save_port.h"
#include "hooks/dispatch.h"
#include "hooks/registry.h"
#include "hooks/popup_state.h"
#include "infra/capabilities.h"
#include "infra/commands.h"
#include "infra/request_policy.h"
#include "infra/config.h"
#include "infra/events.h"
#include "infra/panels.h"
#include "infra/scheduler.h"
#include "infra/snapshot_handle.h"
#include "infra/store.h"
#include "infra/slot_operation_gate.h"
#include "infra/text_scope.h"
#include "game/adapter_ports/system_port.h"
#include "game/access/ptr_slots.h"
#include "game/access/game_access.h"
#include "game/access/item_handle_registry.h"
#include "patch/ptr_slot.h"

extern bool module_save_register_participant(const ModuleSaveParticipant&) __attribute__((weak));
extern bool module_save_game() __attribute__((weak));

namespace qol::host {
namespace {

using QolResult = ::QolResult;

static_assert(sizeof(contracts::QolPopupStateEntry) == sizeof(hooks::PopupStateEntry));
static_assert(offsetof(contracts::QolPopupStateEntry, slot) ==
              offsetof(hooks::PopupStateEntry, slot));
static_assert(offsetof(contracts::QolPopupStateEntry, state_id) ==
              offsetof(hooks::PopupStateEntry, state_id));
static_assert(sizeof(contracts::QolPopupStateHooks) == sizeof(hooks::PopupStateHooks));

constexpr const char* kNullArgument = "null argument";
constexpr const char* kInvalidArgument = "invalid argument";
constexpr const char* kNotFound = "not found";
constexpr const char* kNotReady = "not ready";
constexpr const char* kUnsupported = "unsupported";
constexpr const char* kConflict = "conflict";
constexpr const char* kCapacity = "capacity exhausted";

QolResult store_result(infra::store::Status status);
QolResult result_error(int32_t code, const char* message);

QolResult sdk_in_flight_enter(contracts::InFlightHandle handle) {
    if (handle.resource_id == nullptr || !hooks::in_flight_enter(handle.resource_id)) {
        return result_error(kQolErrNotFound, kNotFound);
    }
    return contracts::ok();
}

QolResult sdk_in_flight_leave(contracts::InFlightHandle handle) {
    if (handle.resource_id == nullptr || !hooks::in_flight_leave(handle.resource_id)) {
        return result_error(kQolErrNotFound, kNotFound);
    }
    return contracts::ok();
}

QolResult result_error(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult result_from_bool(bool ok, int32_t code, const char* message) {
    return ok ? contracts::ok() : result_error(code, message);
}

QolResult write_json_result(const std::string& json, char* out, uint32_t capacity,
                           uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (out == nullptr || out_size == nullptr || json.size() > capacity) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!json.empty()) std::memcpy(out, json.data(), json.size());
    *out_size = static_cast<uint32_t>(json.size());
    return contracts::ok();
}

QolResult sdk_result_write_ok(char* out, uint32_t capacity, uint32_t* out_size) {
    return write_json_result(::op_ok(), out, capacity, out_size);
}

QolResult sdk_result_write_error(const char* message, char* out, uint32_t capacity,
                                 uint32_t* out_size) {
    if (message == nullptr) return result_error(kQolErrInvalidArgument, kNullArgument);
    return write_json_result(::op_err(message), out, capacity, out_size);
}

bool valid_log_level(int32_t level) { return level >= 0 && level <= 3; }

bool valid_log_domain(int32_t domain) {
    return domain >= 0 && domain <= static_cast<int32_t>(QolDomain::kSpecialEquip);
}

QolResult sdk_log_write(int32_t level, int32_t domain, const char* source, const char* message,
                        uint32_t message_len) {
    if (source == nullptr || message == nullptr) {
        return result_error(kQolErrInvalidArgument, kNullArgument);
    }
    if (!valid_log_level(level) || !valid_log_domain(domain)) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (message_len > contracts::kMessageCap) {
        return result_error(kQolErrInvalidArgument, "message too long");
    }

    thread_local std::array<char, contracts::kMessageCap + 1> buffer{};
    std::memcpy(buffer.data(), message, message_len);
    buffer[message_len] = '\0';
    qol_log_write_kotlin(static_cast<QolLogLevel>(level), static_cast<QolDomain>(domain), source,
                         buffer.data());
    return contracts::ok();
}

int32_t sdk_log_debug_enabled() {
    return qol_log_debug_enabled() ? 1 : 0;
}

QolResult sdk_config_get_bool(const char* key, int32_t default_value, int32_t* out_value) {
    if (key == nullptr || out_value == nullptr) {
        return result_error(kQolErrInvalidArgument, kNullArgument);
    }
    *out_value = infra::config_get_bool(key, default_value != 0) ? 1 : 0;
    return contracts::ok();
}

QolResult sdk_config_get_i32(const char* key, int32_t default_value, int32_t* out_value) {
    if (key == nullptr || out_value == nullptr) {
        return result_error(kQolErrInvalidArgument, kNullArgument);
    }
    *out_value = infra::config_get_int(key, default_value);
    return contracts::ok();
}

QolResult sdk_config_get_string(const char* key, const char* default_value, char* out_value,
                                uint32_t capacity, uint32_t* out_size) {
    if (key == nullptr || out_value == nullptr || capacity == 0 || out_size == nullptr) {
        return result_error(kQolErrInvalidArgument, kNullArgument);
    }
    const char* value = infra::config_get_string(key, default_value);
    const std::size_t length = std::strlen(value);
    if (length >= capacity) return result_error(kQolErrBlocked, "config string buffer too small");
    std::memcpy(out_value, value, length);
    out_value[length] = '\0';
    *out_size = static_cast<uint32_t>(length);
    return contracts::ok();
}

QolResult sdk_config_set_bool(const char*, int32_t) {
    return result_error(kQolErrUnsupported, "config writes require native config owner");
}

QolResult sdk_config_set_i32(const char*, int32_t) {
    return result_error(kQolErrUnsupported, "config writes require native config owner");
}

// SDK 边界显式快照当前策略；infra post 同时读取 ambient scope，覆盖内部直调。
QolResult sdk_commands_post(contracts::QolCommandTicket* out,
                            const contracts::QolCommand* command) {
    const infra::RequestPolicy policy = infra::current_request_policy();
    const infra::ScopedRequestPolicy captured_policy(policy);
    return infra::commands_port().post(out, command);
}

QolResult sdk_commands_post_typed(uint32_t handler_id, const void* payload, uint32_t size,
                                  contracts::QolCommandTicket* out) {
    const infra::RequestPolicy policy = infra::current_request_policy();
    const infra::ScopedRequestPolicy captured_policy(policy);
    return infra::commands_port().post_typed(handler_id, payload, size, out);
}

contracts::QolCommands sdk_commands_api() {
    const contracts::QolCommands& commands = infra::commands_port();
    return {&sdk_commands_post, commands.state, commands.cancel,
            commands.register_typed_handler, commands.unregister_typed_handler,
            &sdk_commands_post_typed};
}

struct SchedulerLease {
    bool used = false;
    infra::FrameTaskId id = 0;
    contracts::QolFrameHandler handler = nullptr;
    void* ctx = nullptr;
    bool count_limited = false;
    int remaining = 0;
};

constexpr std::size_t kSchedulerLeaseCapacity = 128;
std::array<SchedulerLease, kSchedulerLeaseCapacity> g_scheduler_leases{};
std::mutex g_scheduler_mtx;

bool scheduler_bridge(int64_t frame, void* raw) {
    auto* lease = static_cast<SchedulerLease*>(raw);
    if (lease == nullptr) return false;

    contracts::QolFrameHandler handler = nullptr;
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_scheduler_mtx);
        if (!lease->used || lease->handler == nullptr) return false;
        handler = lease->handler;
        ctx = lease->ctx;
        if (lease->count_limited) --lease->remaining;
    }
    const bool keep = handler(frame, ctx) != 0;
    bool exhausted = false;
    {
        std::lock_guard<std::mutex> lock(g_scheduler_mtx);
        exhausted = lease->count_limited && lease->remaining == 0;
    }
    if (!keep || exhausted) {
        std::lock_guard<std::mutex> lock(g_scheduler_mtx);
        lease->used = false;
        lease->id = 0;
        lease->handler = nullptr;
        lease->ctx = nullptr;
        lease->count_limited = false;
        lease->remaining = 0;
        return false;
    }
    return true;
}

QolResult sdk_scheduler_add(int32_t point, contracts::QolFrameHandler handler, void* ctx,
                             int32_t interval, int32_t count, uint64_t* out_id) {
    if (out_id != nullptr) *out_id = 0;
    if (out_id == nullptr || handler == nullptr || point < 0 || point > 1 || count < 0) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }

    std::lock_guard<std::mutex> lock(g_scheduler_mtx);
    SchedulerLease* lease = nullptr;
    for (SchedulerLease& candidate : g_scheduler_leases) {
        if (!candidate.used) {
            lease = &candidate;
            break;
        }
    }
    if (lease == nullptr) return result_error(kQolErrBlocked, kCapacity);
    lease->used = true;
    lease->id = 0;
    lease->handler = handler;
    lease->ctx = ctx;
    lease->count_limited = count > 0;
    lease->remaining = count;
    const infra::FrameTaskId id = infra::scheduler_add(static_cast<infra::FramePoint>(point),
                                                       &scheduler_bridge, lease, interval, count);
    if (id == 0) {
        *lease = SchedulerLease{};
        return result_error(kQolErrBlocked, kCapacity);
    }
    lease->id = id;
    *out_id = id;
    return contracts::ok();
}

QolResult sdk_scheduler_remove(uint64_t id) {
    if (id == 0) return result_error(kQolErrInvalidArgument, kInvalidArgument);
    const bool removed = infra::scheduler_remove(id);
    if (!removed) return result_error(kQolErrNotFound, kNotFound);
    std::lock_guard<std::mutex> lock(g_scheduler_mtx);
    for (SchedulerLease& lease : g_scheduler_leases) {
        if (lease.used && lease.id == id) {
            lease = SchedulerLease{};
            break;
        }
    }
    return contracts::ok();
}

QolResult sdk_events_subscribe(uint32_t type, contracts::QolEventHandler handler, void* ctx,
                               uint64_t* out_id) {
    if (out_id != nullptr) *out_id = 0;
    if (out_id == nullptr || handler == nullptr) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    const infra::SubscriptionId id = infra::subscribe(type, handler, ctx);
    if (id == 0) return result_error(kQolErrBlocked, kCapacity);
    *out_id = id;
    return contracts::ok();
}

QolResult sdk_events_unsubscribe(uint64_t id) {
    if (id == 0) return result_error(kQolErrInvalidArgument, kInvalidArgument);
    return result_from_bool(infra::unsubscribe(id), kQolErrNotFound, kNotFound);
}

struct CapabilityLease {
    bool used = false;
    uint64_t token = 0;
    const char* capability_id = nullptr;
    QolVersionRange range{};
};

constexpr std::size_t kCapabilityLeaseCapacity = 64;
std::array<CapabilityLease, kCapabilityLeaseCapacity> g_capability_leases{};
std::mutex g_capability_mtx;
uint64_t g_next_capability_lease = 1;

QolResult sdk_capability_require(const char* id, QolVersionRange range, const void** out_iface) {
    if (out_iface != nullptr) *out_iface = nullptr;
    if (id == nullptr || out_iface == nullptr) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    const void* iface = infra::require_capability(id, range);
    if (iface == nullptr) return result_error(kQolErrNotReady, kNotReady);
    *out_iface = iface;
    return contracts::ok();
}

QolResult sdk_capability_lease_enter(const char* id, QolVersionRange range, uint64_t* out_lease) {
    if (out_lease != nullptr) *out_lease = 0;
    if (id == nullptr || out_lease == nullptr) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    const void* iface = infra::require_capability(id, range);
    if (iface == nullptr) return result_error(kQolErrNotReady, kNotReady);

    std::lock_guard<std::mutex> lock(g_capability_mtx);
    for (CapabilityLease& lease : g_capability_leases) {
        if (!lease.used) {
            lease.used = true;
            lease.capability_id = id;
            lease.range = range;
            lease.token = g_next_capability_lease++;
            if (lease.token == 0) lease.token = g_next_capability_lease++;
            *out_lease = lease.token;
            return contracts::ok();
        }
    }
    return result_error(kQolErrBlocked, kCapacity);
}

QolResult sdk_capability_lease_leave(uint64_t lease_id) {
    if (lease_id == 0) return result_error(kQolErrInvalidArgument, kInvalidArgument);
    std::lock_guard<std::mutex> lock(g_capability_mtx);
    for (CapabilityLease& lease : g_capability_leases) {
        if (lease.used && lease.token == lease_id) {
            lease = CapabilityLease{};
            return contracts::ok();
        }
    }
    return result_error(kQolErrNotFound, kNotFound);
}

QolResult sdk_capability_set_available(const char*, int32_t) {
    return result_error(kQolErrUnsupported, kUnsupported);
}

QolResult sdk_store_read(int32_t slot, const char* name, contracts::QolStoreBuffer* out,
                         uint16_t* out_version) {
    if (out != nullptr) out->size = 0;
    if (slot < 0 || name == nullptr || out == nullptr || out_version == nullptr) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    std::vector<uint8_t> payload;
    uint16_t version = 0;
    const infra::store::Status status = infra::store::read_section(slot, name, &payload, &version);
    if (status != infra::store::Status::kOk) return store_result(status);
    if (payload.size() > out->capacity || (payload.size() > 0 && out->data == nullptr)) {
        out->size = static_cast<uint32_t>(payload.size());
        return result_error(kQolErrInvalidArgument, "buffer too small");
    }
    if (!payload.empty()) std::memcpy(out->data, payload.data(), payload.size());
    out->size = static_cast<uint32_t>(payload.size());
    *out_version = version;
    return contracts::ok();
}

QolResult sdk_store_write(int32_t slot, const char* name, uint16_t version, const uint8_t* data,
                          uint32_t size) {
    if (slot < 0 || name == nullptr || (size > 0 && data == nullptr) || version == 0) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    return store_result(infra::store::write_section(slot, name, version, data, size));
}

QolResult sdk_store_remove(int32_t slot, const char* name) {
    if (slot < 0 || name == nullptr) return result_error(kQolErrInvalidArgument, kInvalidArgument);
    return store_result(infra::store::remove_section(slot, name));
}

QolResult sdk_store_replace(int32_t slot, const contracts::QolStoreSection* replacements,
                            uint32_t replacement_count, const char* const* removals,
                            uint32_t removal_count) {
    if (slot < 0 || (replacement_count > 0 && replacements == nullptr) ||
        (removal_count > 0 && removals == nullptr)) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    infra::store::ReplaceMap replacement_map;
    infra::store::RemoveSet removal_set;
    replacement_map.reserve(replacement_count);
    removal_set.reserve(removal_count);
    for (uint32_t i = 0; i < replacement_count; ++i) {
        const contracts::QolStoreSection& replacement = replacements[i];
        if (replacement.name == nullptr || replacement.version == 0 ||
            (replacement.size > 0 && replacement.data == nullptr)) {
            return result_error(kQolErrInvalidArgument, kInvalidArgument);
        }
        infra::store::Section section;
        section.version = replacement.version;
        if (replacement.size > 0) {
            section.payload.assign(replacement.data, replacement.data + replacement.size);
        }
        replacement_map.emplace_back(replacement.name, std::move(section));
    }
    for (uint32_t i = 0; i < removal_count; ++i) {
        if (removals[i] == nullptr) return result_error(kQolErrInvalidArgument, kInvalidArgument);
        removal_set.emplace_back(removals[i]);
    }
    return store_result(infra::store::replace_sections(slot, replacement_map, removal_set));
}

QolResult owner_store_ensure(contracts::QolOwnerToken owner, int32_t slot) {
    const auto authorized = owner_authorize_store(owner, "module-saves");
    if (!contracts::succeeded(authorized)) return authorized;
    return store_result(infra::store::ensure_slot(slot));
}
QolResult owner_store_reset(contracts::QolOwnerToken owner, int32_t slot) {
    const auto authorized = owner_authorize_store(owner, "module-saves");
    if (!contracts::succeeded(authorized)) return authorized;
    return store_result(infra::store::reset_slot(slot));
}
QolResult owner_store_read(contracts::QolOwnerToken owner, int32_t slot, const char* section,
                          uint8_t* out, uint64_t capacity, uint64_t* size, uint16_t* version) {
    if (size != nullptr) *size = 0;
    if (version != nullptr) *version = 0;
    const auto authorized = owner_authorize_store(owner, "module-saves");
    if (!contracts::succeeded(authorized)) return authorized;
    if (section == nullptr || size == nullptr || version == nullptr ||
        (capacity != 0 && out == nullptr)) return result_error(kQolErrInvalidArgument, kInvalidArgument);
    std::vector<uint8_t> payload;
    const auto status = infra::store::read_section(slot, section, &payload, version);
    if (status != infra::store::Status::kOk) return store_result(status);
    *size = payload.size();
    if (payload.size() > capacity) return result_error(kQolErrInvalidArgument, "buffer too small");
    if (!payload.empty()) std::memcpy(out, payload.data(), payload.size());
    return contracts::ok();
}
QolResult owner_store_write(contracts::QolOwnerToken owner, int32_t slot, const char* section,
                           uint16_t version, const uint8_t* bytes, uint64_t size) {
    const auto authorized = owner_authorize_store(owner, "module-saves");
    if (!contracts::succeeded(authorized)) return authorized;
    if (section != nullptr && std::strcmp(section, "module.save.journal") == 0)
        return result_error(kQolErrBlocked, "journal requires reservation");
    if (size > std::numeric_limits<uint32_t>::max())
        return result_error(kQolErrInvalidArgument, "payload too large");
    return sdk_store_write(slot, section, version, bytes, static_cast<uint32_t>(size));
}
QolResult owner_store_remove(contracts::QolOwnerToken owner, int32_t slot, const char* section) {
    const auto authorized = owner_authorize_store(owner, "module-saves");
    if (!contracts::succeeded(authorized)) return authorized;
    if (section != nullptr && std::strcmp(section, "module.save.journal") == 0)
        return result_error(kQolErrBlocked, "journal requires reservation");
    return sdk_store_remove(slot, section);
}
QolResult owner_store_replace(contracts::QolOwnerToken owner, int32_t slot,
                             const contracts::QolStoreSection* replacements, uint32_t count,
                             const char* const* removals, uint32_t removal_count) {
    const auto authorized = owner_authorize_store(owner, "module-saves");
    if (!contracts::succeeded(authorized)) return authorized;
    if ((count != 0 && replacements == nullptr) || (removal_count != 0 && removals == nullptr))
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    for (uint32_t i = 0; i < count; ++i)
        if (replacements[i].name != nullptr && std::strcmp(replacements[i].name, "module.save.journal") == 0)
            return result_error(kQolErrBlocked, "journal requires reservation");
    for (uint32_t i = 0; i < removal_count; ++i)
        if (removals[i] != nullptr && std::strcmp(removals[i], "module.save.journal") == 0)
            return result_error(kQolErrBlocked, "journal requires reservation");
    return sdk_store_replace(slot, replacements, count, removals, removal_count);
}
QolResult owner_store_inspect(contracts::QolOwnerToken owner, int32_t slot,
                             contracts::QolOwnerStoreSlotState* out) {
    const auto allowed = owner_authorize_store(owner, "module-saves");
    return contracts::succeeded(allowed) ? store_result(infra::store::inspect_slot(slot, out)) : allowed;
}
QolResult owner_store_preflight(contracts::QolOwnerToken owner, int32_t slot,
    const contracts::QolOwnerStoreCandidate* candidates, uint32_t count, uint64_t* lengths) {
    const auto allowed = owner_authorize_store(owner, "module-saves");
    return contracts::succeeded(allowed) ? store_result(infra::store::preflight(slot, candidates, count, lengths)) : allowed;
}
QolResult owner_store_reserve(contracts::QolOwnerToken owner, int32_t slot,
    const contracts::QolOwnerStoreCandidate* candidates, uint32_t count,
    contracts::QolOwnerStoreReservation* out) {
    const auto allowed = owner_authorize_store(owner, "module-saves");
    if (!contracts::succeeded(allowed)) return allowed;
    owner_store_lifecycle_handlers(&infra::store::owner_stopped, &infra::store::owner_rebound);
    owner_store_lifecycle_try_handlers(&infra::store::owner_stopped_try, &infra::store::owner_rebound_try);
    const auto status = infra::store::reserve(owner, slot, candidates, count, out);
    if (status != infra::store::Status::kOk) return store_result(status);
    const auto still_allowed = owner_authorize_store(owner, "module-saves");
    if (!contracts::succeeded(still_allowed)) {
        // 预检期间 owner 可停用；不能在停用通知之后遗留新建的过期租约。
        (void)infra::store::release(owner, *out);
        return still_allowed;
    }
    return contracts::ok();
}
QolResult owner_store_release(contracts::QolOwnerToken owner, contracts::QolOwnerStoreReservation credential) {
    const auto allowed = owner_authorize_store(owner, "module-saves");
    return contracts::succeeded(allowed) ? store_result(infra::store::release(owner, credential)) : allowed;
}
QolResult owner_store_replace_reserved(contracts::QolOwnerToken owner, int32_t slot,
    contracts::QolOwnerStoreReservation* credential, const contracts::QolStoreSection* sections,
    uint32_t count, const char* const* removals, uint32_t removal_count) {
    const auto allowed = owner_authorize_store(owner, "module-saves");
    if (!contracts::succeeded(allowed)) return allowed;
    if ((count != 0 && sections == nullptr) || (removal_count != 0 && removals == nullptr))
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    infra::store::ReplaceMap replacements;
    infra::store::RemoveSet removed;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& section = sections[i];
        if (section.name == nullptr || section.version == 0 || section.size > 1024 * 1024 ||
            (section.size != 0 && section.data == nullptr)) return result_error(kQolErrInvalidArgument, kInvalidArgument);
        infra::store::Section copy;
        copy.version = section.version;
        if (section.size != 0) copy.payload.assign(section.data, section.data + section.size);
        replacements.emplace_back(section.name, std::move(copy));
    }
    for (uint32_t i = 0; i < removal_count; ++i) {
        if (removals[i] == nullptr) return result_error(kQolErrInvalidArgument, kInvalidArgument);
        removed.emplace_back(removals[i]);
    }
    // 字段错误是 InvalidArgument；释放/过期/同 owner 无凭据是 Busy，不能误当成功。
    return store_result(infra::store::replace_reserved(owner, slot, credential, replacements, removed));
}
QolResult owner_store_write_reserved(contracts::QolOwnerToken owner, int32_t slot,
    contracts::QolOwnerStoreReservation* credential, const char* name, uint16_t version,
    const uint8_t* bytes, uint64_t size) {
    if (size > 1024 * 1024) return result_error(kQolErrInvalidArgument, "payload too large");
    const contracts::QolStoreSection section{name, version, bytes, static_cast<uint32_t>(size)};
    return owner_store_replace_reserved(owner, slot, credential, &section, 1, nullptr, 0);
}
QolResult owner_store_remove_reserved(contracts::QolOwnerToken owner, int32_t slot,
    contracts::QolOwnerStoreReservation* credential, const char* name) {
    return owner_store_replace_reserved(owner, slot, credential, nullptr, 0, &name, 1);
}
QolResult owner_store_set_referenced(contracts::QolOwnerToken owner, int32_t slot,
    contracts::QolOwnerStoreReservation* credential, uint32_t referenced) {
    const auto allowed = owner_authorize_store(owner, "module-saves");
    return contracts::succeeded(allowed)
        ? store_result(infra::store::set_referenced(owner, slot, credential, referenced)) : allowed;
}
QolResult owner_store_reserve_in_operation(contracts::QolOwnerToken owner,
    const contracts::QolSlotOperationLease* operation, int32_t slot,
    const contracts::QolOwnerStoreCandidate* candidates, uint32_t count, contracts::QolOwnerStoreReservation* out) {
    const auto allowed = owner_authorize_store(owner, "module-saves");
    if (!contracts::succeeded(allowed)) return allowed;
    const auto operation_allowed = owner_authorize_slot_operation(owner);
    if (!contracts::succeeded(operation_allowed)) return operation_allowed;
    return store_result(infra::store::reserve_in_operation(owner, operation, slot, candidates, count, out));
}
QolResult owner_store_ensure_in_operation(contracts::QolOwnerToken owner,
    const contracts::QolSlotOperationLease* operation, int32_t slot) {
    const auto allowed = owner_authorize_store(owner, "module-saves");
    if (!contracts::succeeded(allowed)) return allowed;
    return store_result(infra::store::ensure_in_operation(owner, operation, slot));
}
QolResult owner_store_reset_in_operation(contracts::QolOwnerToken owner,
    const contracts::QolSlotOperationLease* operation, int32_t slot) {
    const auto allowed = owner_authorize_store(owner, "module-saves");
    if (!contracts::succeeded(allowed)) return allowed;
    return store_result(infra::store::reset_in_operation(owner, operation, slot));
}
const contracts::QolOwnerStoreApi kOwnerStoreApi{
    {sizeof(contracts::QolOwnerStoreApi), 1, 0}, &owner_store_ensure, &owner_store_reset,
    &owner_store_read, &owner_store_write, &owner_store_remove, &owner_store_replace,
    &owner_store_inspect, &owner_store_preflight, &owner_store_reserve, &owner_store_release,
    &owner_store_write_reserved, &owner_store_remove_reserved, &owner_store_replace_reserved,
    &owner_store_set_referenced, &owner_store_reserve_in_operation,
    &owner_store_ensure_in_operation, &owner_store_reset_in_operation};

QolResult operation_acquire(void*, contracts::QolOwnerToken owner, uint32_t mask,
                            contracts::QolSlotOperationLease* out) {
    const auto allowed = owner_authorize_slot_operation(owner);
    if (!contracts::succeeded(allowed)) return allowed;
    owner_store_lifecycle_handlers(&infra::store::owner_stopped, &infra::store::owner_rebound);
    owner_store_lifecycle_try_handlers(&infra::store::owner_stopped_try, &infra::store::owner_rebound_try);
    const auto result = infra::slot_operation::acquire(owner, mask, out);
    if (!contracts::succeeded(result)) return result;
    const auto still_allowed = owner_authorize_slot_operation(owner);
    if (!contracts::succeeded(still_allowed)) {
        (void)infra::slot_operation::release(owner, out);
        return still_allowed;
    }
    return result;
}
QolResult operation_set_referenced(void*, contracts::QolOwnerToken owner,
    contracts::QolSlotOperationLease* lease, uint32_t referenced) {
    const auto allowed = owner_authorize_slot_operation(owner);
    return contracts::succeeded(allowed) ? infra::slot_operation::set_referenced(owner, lease, referenced) : allowed;
}
QolResult operation_refresh(void*, contracts::QolOwnerToken owner, uint64_t id,
                            contracts::QolSlotOperationLease* out) {
    const auto allowed = owner_authorize_slot_operation(owner);
    return contracts::succeeded(allowed) ? infra::slot_operation::refresh(owner, id, out) : allowed;
}
QolResult operation_release(void*, contracts::QolOwnerToken owner, const contracts::QolSlotOperationLease* lease) {
    const auto allowed = owner_authorize_slot_operation(owner);
    return contracts::succeeded(allowed) ? infra::slot_operation::release(owner, lease) : allowed;
}
QolResult operation_import(void*, contracts::QolOwnerToken owner, const contracts::QolSlotOperationLease* lease,
    int32_t slot, const uint8_t* bytes, uint64_t count, contracts::QolSlotOperationApply* apply) {
    if (apply == nullptr || apply->size < sizeof(*apply)) return result_error(kQolErrInvalidArgument, kInvalidArgument);
    apply->apply = static_cast<uint32_t>(contracts::QolSlotOperationApplyState::kUnchanged);
    const auto allowed = owner_authorize_slot_operation(owner);
    if (!contracts::succeeded(allowed)) return allowed;
    if (count > 4 * 1024 * 1024) return result_error(kQolErrInvalidArgument, "container too large");
    return store_result(infra::store::import_in_operation(owner, lease, slot, bytes, static_cast<std::size_t>(count), apply));
}
QolResult operation_delete(void*, contracts::QolOwnerToken owner,
                           const contracts::QolSlotOperationLease* lease, int32_t slot) {
    const auto allowed = owner_authorize_slot_operation(owner);
    return contracts::succeeded(allowed) ? store_result(infra::store::delete_in_operation(owner, lease, slot)) : allowed;
}
const contracts::QolSlotOperationApi kSlotOperationApi{
    {sizeof(contracts::QolSlotOperationApi), 1, 0}, nullptr, &operation_acquire,
    &operation_set_referenced, &operation_refresh, &operation_release, &operation_import, &operation_delete};

// core.save 领域回调路由；授权与 drain 语义在 host/save_route 内实现。
const contracts::QolSaveRouteApi kSaveRouteApi = qol::host::save_route::save_route_api();

QolResult sdk_store_read_container(int32_t slot, uint8_t* out, uint32_t capacity,
                                   uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (slot < 0 || out_size == nullptr || (capacity > 0 && out == nullptr)) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }

    std::vector<uint8_t> bytes;
    const infra::store::Status status = infra::store::read_container(slot, &bytes);
    if (status != infra::store::Status::kOk) return store_result(status);
    if (bytes.size() > capacity) {
        *out_size = static_cast<uint32_t>(bytes.size());
        return result_error(kQolErrInvalidArgument, "buffer too small");
    }
    if (!bytes.empty()) std::memcpy(out, bytes.data(), bytes.size());
    *out_size = static_cast<uint32_t>(bytes.size());
    return contracts::ok();
}

QolResult sdk_store_import_container(int32_t slot, const uint8_t* data, uint32_t size) {
    if (slot < 0 || (size > 0 && data == nullptr)) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    return store_result(infra::store::import_container(slot, data, size));
}

QolResult sdk_store_delete_slot(int32_t slot) {
    if (slot < 0) return result_error(kQolErrInvalidArgument, kInvalidArgument);
    return store_result(infra::store::delete_slot(slot));
}

QolResult sdk_store_register_participant(const contracts::QolStoreParticipant* participant) {
    if (participant == nullptr || participant->name == nullptr || participant->prepare == nullptr ||
        participant->commit == nullptr || participant->abort == nullptr) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (module_save_register_participant == nullptr) return result_error(kQolErrNotReady, kNotReady);
    const ModuleSaveParticipant legacy{participant->name, participant->prepare, participant->commit,
                                       participant->abort};
    return result_from_bool(module_save_register_participant(legacy), kQolErrConflict, kConflict);
}

QolResult sdk_store_save_game() {
    if (module_save_game == nullptr) return result_error(kQolErrNotReady, kNotReady);
    return result_from_bool(module_save_game(), kQolErrBlocked, "save failed");
}

int32_t sdk_lifecycle_participant_register(
    const contracts::QolLifecycleParticipant* participant) {
    return host_lifecycle_participant_register(participant);
}

int32_t sdk_item_handle_register(void* item, uint32_t provider_id,
                                 contracts::QolItemHandle* out_handle) {
    if (item == nullptr || out_handle == nullptr) {
        return contracts::kQolItemHandleStatusInvalidArgument;
    }
    qol::game::ItemHandleError error{};
    *out_handle = qol::game::register_item(item, provider_id, &error);
    if (out_handle->slot != 0) return contracts::kQolItemHandleStatusOk;
    switch (error) {
        case qol::game::ItemHandleError::kInvalidArgument:
            return contracts::kQolItemHandleStatusInvalidArgument;
        case qol::game::ItemHandleError::kWrongThread:
            return contracts::kQolItemHandleStatusWrongThread;
        case qol::game::ItemHandleError::kFull:
            return contracts::kQolItemHandleStatusCapacity;
        case qol::game::ItemHandleError::kStale:
            return contracts::kQolItemHandleStatusStale;
        case qol::game::ItemHandleError::kType:
            return contracts::kQolItemHandleStatusType;
        case qol::game::ItemHandleError::kTransition:
            return contracts::kQolItemHandleStatusTransition;
        case qol::game::ItemHandleError::kNone:
            return contracts::kQolItemHandleStatusNotFound;
    }
    return contracts::kQolItemHandleStatusNotFound;
}

int32_t sdk_item_handle_lookup(void* item, contracts::QolItemHandle* out_handle,
                               uint32_t* out_provider_id) {
    if (item == nullptr || out_handle == nullptr) {
        return contracts::kQolItemHandleStatusInvalidArgument;
    }
    *out_handle = qol::game::lookup_item(item, out_provider_id);
    return out_handle->slot != 0 ? contracts::kQolItemHandleStatusOk
                                 : contracts::kQolItemHandleStatusNotFound;
}

int32_t sdk_item_handle_invalidate(void* item, uint32_t provider_id) {
    return qol::game::invalidate_item(item, provider_id)
               ? contracts::kQolItemHandleStatusOk
               : contracts::kQolItemHandleStatusNotFound;
}

int32_t sdk_item_handle_rebind(void* item, uint32_t provider_id) {
    return qol::game::rebind_item(item, provider_id)
               ? contracts::kQolItemHandleStatusOk
               : contracts::kQolItemHandleStatusNotFound;
}

int32_t sdk_item_handle_begin_invalidation(void* item, uint32_t provider_id,
                                           contracts::QolItemHandle* out_checkpoint) {
    if (out_checkpoint == nullptr) return contracts::kQolItemHandleStatusInvalidArgument;
    return qol::game::begin_item_invalidation(item, provider_id, out_checkpoint)
               ? contracts::kQolItemHandleStatusOk
               : contracts::kQolItemHandleStatusNotFound;
}

int32_t sdk_item_handle_finish_invalidation(contracts::QolItemHandle checkpoint,
                                            int32_t committed) {
    return qol::game::finish_item_invalidation(checkpoint, committed != 0)
               ? contracts::kQolItemHandleStatusOk
               : contracts::kQolItemHandleStatusStale;
}

QolResult sdk_panels_register_item(const contracts::PanelItemSpec* item) {
    if (item == nullptr) return result_error(kQolErrInvalidArgument, kNullArgument);
    return result_from_bool(infra::panels_register_item(*item), kQolErrConflict, kConflict);
}

QolResult sdk_panels_register_provider(const contracts::PanelProviderSpec* provider) {
    if (provider == nullptr) return result_error(kQolErrInvalidArgument, kNullArgument);
    return result_from_bool(infra::panels_register_provider(*provider), kQolErrConflict, kConflict);
}

QolResult sdk_panels_unregister_owner(const char* owner) {
    if (owner == nullptr) return result_error(kQolErrInvalidArgument, kNullArgument);
    infra::panels_unregister_owner(owner);
    return contracts::ok();
}

QolResult sdk_panels_find(const char* panel_id, const contracts::PanelProviderSpec** out_provider) {
    if (panel_id == nullptr || out_provider == nullptr) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_provider = infra::panels_find_provider(panel_id);
    return *out_provider == nullptr ? result_error(kQolErrNotFound, kNotFound) : contracts::ok();
}

QolResult sdk_panels_open(const char* panel_id) {
    const contracts::PanelProviderSpec* provider = nullptr;
    const QolResult found = sdk_panels_find(panel_id, &provider);
    if (!contracts::succeeded(found)) return found;
    if (provider->open == nullptr) return result_error(kQolErrUnsupported, kUnsupported);
    if (provider->open()) return contracts::ok();
    const char* reason = provider->block_reason == nullptr ? nullptr : provider->block_reason();
    return reason == nullptr ? result_error(kQolErrBlocked, "panel blocked")
                             : contracts::error(kQolErrBlocked, reason,
                                                static_cast<uint32_t>(std::strlen(reason)));
}

QolResult sdk_panels_close(const char* panel_id) {
    const contracts::PanelProviderSpec* provider = nullptr;
    const QolResult found = sdk_panels_find(panel_id, &provider);
    if (!contracts::succeeded(found)) return found;
    if (provider->close == nullptr) return result_error(kQolErrUnsupported, kUnsupported);
    provider->close();
    return contracts::ok();
}

QolResult sdk_panels_active(const char* panel_id, int32_t* out_active) {
    if (out_active != nullptr) *out_active = 0;
    if (panel_id == nullptr || out_active == nullptr) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    const contracts::PanelProviderSpec* provider = infra::panels_find_provider(panel_id);
    if (provider == nullptr) return result_error(kQolErrNotFound, kNotFound);
    if (provider->active == nullptr) return result_error(kQolErrUnsupported, kUnsupported);
    *out_active = provider->active() ? 1 : 0;
    return contracts::ok();
}

QolResult sdk_hooks_text_subscribe(const char* resource_id, int32_t phase, int32_t kind,
                                  contracts::QolTextDispatchFn fn, void* ctx, uint64_t* out_id) {
    if (out_id != nullptr) *out_id = 0;
    if (resource_id == nullptr || fn == nullptr || out_id == nullptr || phase < 0 || phase > 1 ||
        kind < 0 || kind > 2) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    const hooks::DispatchId id = hooks::dispatch_subscribe(
        resource_id, static_cast<hooks::DispatchPhase>(phase), static_cast<hooks::ConsumerKind>(kind),
        fn, ctx);
    if (id == 0) return result_error(kQolErrBlocked, kCapacity);
    *out_id = id;
    return contracts::ok();
}

QolResult sdk_hooks_text_unsubscribe(const char* resource_id, uint64_t id) {
    if (resource_id == nullptr || id == 0) return result_error(kQolErrInvalidArgument, kInvalidArgument);
    return result_from_bool(hooks::dispatch_unsubscribe(resource_id, id), kQolErrNotFound, kNotFound);
}

QolResult sdk_hooks_run_text(const char* resource_id, uint32_t text_id, const char* original,
                             const char** out_text) {
    if (resource_id == nullptr || original == nullptr || out_text == nullptr) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_text = hooks::dispatch_run_text(resource_id, text_id, original);
    return contracts::ok();
}

QolResult sdk_hooks_set_intercept(const char* resource_id, int32_t (*predicate)(void*), void* ctx) {
    if (resource_id == nullptr || predicate == nullptr) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    const auto gate = reinterpret_cast<hooks::InterceptGateFn>(predicate);
    return result_from_bool(hooks::dispatch_set_intercept(resource_id, gate, ctx), kQolErrConflict,
                            kConflict);
}

QolResult sdk_hooks_clear_intercept(const char* resource_id) {
    if (resource_id == nullptr) return result_error(kQolErrInvalidArgument, kInvalidArgument);
    return result_from_bool(hooks::dispatch_clear_intercept(resource_id), kQolErrNotFound,
                            kNotFound);
}

int32_t sdk_hooks_intercept_blocks(const char* resource_id, void* ctx) {
    if (resource_id == nullptr) return -kQolErrInvalidArgument;
    return hooks::dispatch_intercept_blocks(resource_id, ctx) ? 1 : 0;
}

QolResult sdk_hooks_popup_resolve(const char* symbol, contracts::QolPopupStateEntry* out_entry) {
    if (symbol == nullptr || out_entry == nullptr) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    return result_from_bool(hooks::popup_state_resolve_symbol(
                                symbol, reinterpret_cast<hooks::PopupStateEntry*>(out_entry)),
                            kQolErrNotFound, kNotFound);
}

QolResult sdk_hooks_popup_acquire(contracts::QolPopupStateEntry* entry,
                                  const contracts::QolPopupStateHooks* popup_hooks) {
    if (entry == nullptr || popup_hooks == nullptr) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    return result_from_bool(
        hooks::popup_state_acquire(reinterpret_cast<hooks::PopupStateEntry*>(entry),
                                   *reinterpret_cast<const hooks::PopupStateHooks*>(popup_hooks)),
        kQolErrConflict, kConflict);
}

QolResult sdk_hooks_popup_restore(contracts::QolPopupStateEntry* entry) {
    if (entry == nullptr) return result_error(kQolErrInvalidArgument, kNullArgument);
    hooks::popup_state_restore(reinterpret_cast<hooks::PopupStateEntry*>(entry));
    return contracts::ok();
}

struct DynamicDescBinding {
    bool active = false;
    void* slot = nullptr;
    void* original = nullptr;
    void* replacement = nullptr;
    void* button = nullptr;
};

std::mutex g_dynamic_desc_mtx;
std::array<DynamicDescBinding, 11> g_dynamic_desc_bindings{};

QolResult sdk_hooks_desc_bind(uint32_t index, void* replacement,
                             contracts::QolDynamicDescHook* out_hook) {
    if (replacement == nullptr || out_hook == nullptr || index >= g_dynamic_desc_bindings.size()) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    std::lock_guard<std::mutex> lock(g_dynamic_desc_mtx);
    auto& binding = g_dynamic_desc_bindings[index];
    if (binding.active) return result_error(kQolErrConflict, kConflict);
    qol::game::PtrSlotSite site{};
    if (!qol::game::extension_desc_button_slot(index, &site)) return result_error(kQolErrNotReady, kNotReady);
    void* button = nullptr;
    if (!qol::game::extension_desc_button_instance(index, &button)) {
        return result_error(kQolErrNotReady, kNotReady);
    }
    const auto write_ptr = &qol::patch::ptr_slot_write_raw;
    if (!write_ptr(site.slot_addr, replacement)) {
        return result_error(kQolErrInternal, "ptr slot write rejected");
    }
    binding = {true, reinterpret_cast<void*>(site.slot_addr), site.expected_value,
               replacement, button};
    out_hook->handle = static_cast<uint64_t>(index + 1);
    out_hook->original = site.expected_value;
    return contracts::ok();
}

DynamicDescBinding* dynamic_desc_binding(const contracts::QolDynamicDescHook* hook) {
    if (hook == nullptr || hook->handle == 0 || hook->handle > g_dynamic_desc_bindings.size()) {
        return nullptr;
    }
    auto& binding = g_dynamic_desc_bindings[hook->handle - 1];
    if (!binding.active || binding.original != hook->original) return nullptr;
    return &binding;
}

QolResult sdk_hooks_desc_unbind(contracts::QolDynamicDescHook* hook) {
    if (hook == nullptr) return result_error(kQolErrInvalidArgument, kInvalidArgument);
    std::lock_guard<std::mutex> lock(g_dynamic_desc_mtx);
    DynamicDescBinding* binding = dynamic_desc_binding(hook);
    if (binding == nullptr) return result_error(kQolErrNotFound, kNotFound);
    if (*reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(binding->slot)) != binding->replacement) {
        return result_error(kQolErrConflict, "ptr slot value mismatch");
    }
    const auto restore_ptr = &qol::patch::ptr_slot_restore_raw;
    if (!restore_ptr(reinterpret_cast<uintptr_t>(binding->slot), binding->original)) {
        return result_error(kQolErrInternal, "ptr slot restore rejected");
    }
    *binding = {};
    *hook = {};
    return contracts::ok();
}

QolResult sdk_hooks_desc_matches_button(const contracts::QolDynamicDescHook* hook,
                                        void* button, int32_t* out_matches) {
    if (out_matches == nullptr || button == nullptr) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    std::lock_guard<std::mutex> lock(g_dynamic_desc_mtx);
    DynamicDescBinding* binding = dynamic_desc_binding(hook);
    if (binding == nullptr) return result_error(kQolErrNotFound, kNotFound);
    *out_matches = binding->button == button ? 1 : 0;
    return contracts::ok();
}

QolResult sdk_hooks_desc_original_matches(const contracts::QolDynamicDescHook* hook,
                                          const char* symbol, int32_t* out_matches) {
    if (symbol == nullptr || out_matches == nullptr) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    std::lock_guard<std::mutex> lock(g_dynamic_desc_mtx);
    DynamicDescBinding* binding = dynamic_desc_binding(hook);
    if (binding == nullptr) return result_error(kQolErrNotFound, kNotFound);
    *out_matches = qol::game::extension_desc_button_original_matches(binding->original, symbol) ? 1 : 0;
    return contracts::ok();
}

QolResult sdk_hooks_desc_call_original(const contracts::QolDynamicDescHook* hook, void* button) {
    if (button == nullptr) return result_error(kQolErrInvalidArgument, kInvalidArgument);
    void* original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_dynamic_desc_mtx);
        DynamicDescBinding* binding = dynamic_desc_binding(hook);
        if (binding == nullptr) return result_error(kQolErrNotFound, kNotFound);
        original = binding->original;
    }
    if (original == nullptr) return result_error(kQolErrNotReady, kNotReady);
    using DescProc = void (*)(void*);
    reinterpret_cast<DescProc>(original)(button);
    return contracts::ok();
}

QolResult sdk_hooks_call_original_control_proc(void* control, uint64_t event, void* x2,
                                               void* param, uint64_t* out_result) {
    if (out_result == nullptr) return result_error(kQolErrInvalidArgument, kInvalidArgument);
    if (fn_ui_equip_inven_item_control_event_proc == nullptr) {
        return result_error(kQolErrNotReady, kNotReady);
    }
    *out_result = fn_ui_equip_inven_item_control_event_proc(control, event, x2, param);
    return contracts::ok();
}

std::mutex g_desc_button_mtx;
std::array<contracts::QolDynamicDescHook, 11> g_desc_button_bindings{};
std::array<contracts::QolDescButtonProc, 11> g_desc_button_procs{};

QolResult sdk_desc_button_bind(uint32_t index, contracts::QolDescButtonProc proc,
                               void** out_original) {
    if (out_original != nullptr) *out_original = nullptr;
    if (index >= g_desc_button_bindings.size() || proc == nullptr || out_original == nullptr) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    std::lock_guard<std::mutex> lock(g_desc_button_mtx);
    auto& binding = g_desc_button_bindings[index];
    if (binding.handle != 0) return result_error(kQolErrConflict, kConflict);
    const QolResult result = sdk_hooks_desc_bind(
        index, reinterpret_cast<void*>(proc), &binding);
    if (!contracts::succeeded(result)) return result;
    g_desc_button_procs[index] = proc;
    *out_original = binding.original;
    return contracts::ok();
}

QolResult sdk_desc_button_unbind(uint32_t index) {
    if (index >= g_desc_button_bindings.size()) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    std::lock_guard<std::mutex> lock(g_desc_button_mtx);
    auto& binding = g_desc_button_bindings[index];
    if (binding.handle == 0) return contracts::ok();
    const QolResult result = sdk_hooks_desc_unbind(&binding);
    if (contracts::succeeded(result)) g_desc_button_procs[index] = nullptr;
    return result;
}

QolResult sdk_desc_button_current_proc(uint32_t index, void** out_proc) {
    if (out_proc != nullptr) *out_proc = nullptr;
    if (index >= g_desc_button_bindings.size() || out_proc == nullptr) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    std::lock_guard<std::mutex> lock(g_desc_button_mtx);
    if (g_desc_button_bindings[index].handle != 0) {
        *out_proc = reinterpret_cast<void*>(g_desc_button_procs[index]);
        return contracts::ok();
    }
    return result_error(kQolErrNotFound, kNotFound);
}

QolResult sdk_desc_button_original_matches(void* original, const char* symbol,
                                           int32_t* out_match) {
    if (out_match != nullptr) *out_match = 0;
    if (original == nullptr || symbol == nullptr || out_match == nullptr) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    for (const auto& binding : g_desc_button_bindings) {
        if (binding.handle == 0 || binding.original != original) continue;
        return sdk_hooks_desc_original_matches(&binding, symbol, out_match);
    }
    return result_error(kQolErrNotFound, kNotFound);
}

QolResult sdk_desc_button_call_original(void* original, void* button) {
    if (original == nullptr || button == nullptr) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    for (const auto& binding : g_desc_button_bindings) {
        if (binding.handle == 0 || binding.original != original) continue;
        return sdk_hooks_desc_call_original(&binding, button);
    }
    return result_error(kQolErrNotFound, kNotFound);
}

thread_local std::array<infra::TextScope, 16> t_scope_stack{};
thread_local uint32_t t_scope_depth = 0;

QolResult sdk_text_scope_push(int32_t scope) {
    if (scope < 0 || scope > static_cast<int32_t>(infra::TextScope::kUimixPageTab)) {
        return result_error(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (t_scope_depth >= t_scope_stack.size()) return result_error(kQolErrBlocked, kCapacity);
    t_scope_stack[t_scope_depth++] = infra::active_text_scope();
    infra::t_active_text_scope = static_cast<infra::TextScope>(scope);
    return contracts::ok();
}

QolResult sdk_text_scope_pop() {
    if (t_scope_depth == 0) return result_error(kQolErrConflict, "scope stack empty");
    infra::t_active_text_scope = t_scope_stack[--t_scope_depth];
    return contracts::ok();
}

QolResult store_result(infra::store::Status status) {
    switch (status) {
        case infra::store::Status::kBusy: return result_error(kQolErrBusy, "slot reserved");
        case infra::store::Status::kBlocked: return result_error(kQolErrBlocked, "section reserved");
        case infra::store::Status::kOk:
            return contracts::ok();
        case infra::store::Status::kNotFound:
            return result_error(kQolErrNotFound, kNotFound);
        case infra::store::Status::kUnavailable:
            return result_error(kQolErrNotReady, kNotReady);
        case infra::store::Status::kBadSlot:
        case infra::store::Status::kBadVersion:
        case infra::store::Status::kBadName:
            return result_error(kQolErrInvalidArgument, kInvalidArgument);
        case infra::store::Status::kCorrupt:
        case infra::store::Status::kIoError:
            return result_error(kQolErrInternal, "store failure");
        case infra::store::Status::kTooLarge:
            return result_error(kQolErrBlocked, "store payload too large");
    }
    return result_error(kQolErrInternal, "unknown store status");
}

// 模块身份版本经 JNI 下发到 game 层，此处只做仅读转发；未下发返回 nullptr。
const char* sdk_module_version() { return qol::game::system_port_version(); }

contracts::QolHostSdk make_host_sdk() {
    contracts::QolHostSdk sdk{};
    sdk.abi = {sizeof(contracts::QolHostSdk), contracts::kHostSdkMajor, contracts::kHostSdkMinor};
    sdk.log = {&sdk_log_write, &sdk_log_debug_enabled};
    sdk.config = {&sdk_config_get_bool, &sdk_config_get_i32, &sdk_config_set_bool,
                  &sdk_config_set_i32, &sdk_config_get_string};
    sdk.scheduler = {&sdk_scheduler_add, &sdk_scheduler_remove};
    sdk.commands = sdk_commands_api();
    sdk.events = {&sdk_events_subscribe, &sdk_events_unsubscribe};
    sdk.capabilities = {&sdk_capability_require, &sdk_capability_lease_enter,
                        &sdk_capability_lease_leave, &sdk_capability_set_available};
    sdk.store = {&sdk_store_read, &sdk_store_write, &sdk_store_remove, &sdk_store_replace,
                 &sdk_store_read_container, &sdk_store_import_container, &sdk_store_delete_slot,
                 &sdk_store_register_participant, &sdk_store_save_game};
    sdk.panels = {&sdk_panels_register_item, &sdk_panels_register_provider,
                  &sdk_panels_unregister_owner, &sdk_panels_find, &sdk_panels_open,
                  &sdk_panels_close, &sdk_panels_active};
    sdk.result = {&sdk_result_write_ok, &sdk_result_write_error};
    sdk.hooks = {&sdk_hooks_text_subscribe, &sdk_hooks_text_unsubscribe, &sdk_hooks_run_text,
                 &sdk_hooks_set_intercept, &sdk_hooks_clear_intercept, &sdk_hooks_intercept_blocks,
                 &sdk_hooks_popup_resolve, &sdk_hooks_popup_acquire, &sdk_hooks_popup_restore,
                 &sdk_hooks_desc_bind, &sdk_hooks_desc_unbind, &sdk_hooks_desc_matches_button,
                 &sdk_hooks_desc_original_matches, &sdk_hooks_desc_call_original,
                 &sdk_hooks_call_original_control_proc};
    sdk.text_scope = {&sdk_text_scope_push, &sdk_text_scope_pop};
    sdk.ui = nullptr;
    sdk.desc_button = {&sdk_desc_button_bind, &sdk_desc_button_unbind,
                       &sdk_desc_button_current_proc, &sdk_desc_button_original_matches,
                       &sdk_desc_button_call_original};
    sdk.in_flight = {&sdk_in_flight_enter, &sdk_in_flight_leave};
    sdk.http = http_registry_api();
    sdk.http_dispatch = http_dispatch_api();
    // 票据结果回取面（尾部追加）：注册/注销结果处理器、读结果、细化状态。
    sdk.ticket_result = contracts::QolTicketResultApi{
        &infra::commands_register_typed_result_handler,
        &infra::commands_unregister_typed_result_handler,
        &infra::commands_read_result,
        &infra::commands_ticket_state,
        &infra::commands_defer_current_result,
        &infra::commands_current_attempt,
    };
    sdk.lifecycle_participant_register = &sdk_lifecycle_participant_register;
    sdk.item_handle = {&sdk_item_handle_register, &sdk_item_handle_lookup,
                       &sdk_item_handle_invalidate,
                       &sdk_item_handle_rebind,
                       &sdk_item_handle_begin_invalidation,
                       &sdk_item_handle_finish_invalidation};
    sdk.snapshot_create = &infra::snapshot_handle::create;
    sdk.snapshot_destroy = &infra::snapshot_handle::destroy;
    sdk.snapshot_publish = &infra::snapshot_handle::publish;
    sdk.snapshot_acquire = &infra::snapshot_handle::acquire;
    sdk.snapshot_release = &infra::snapshot_handle::release;
    sdk.settings_catalog = &settings_catalog_api();
    sdk.settings_config = &settings_config_api();
    sdk.module_version = &sdk_module_version;
    sdk.owner_verify_token = &owner_verify_token;
    sdk.owner_store = &kOwnerStoreApi;
    sdk.slot_operation = &kSlotOperationApi;
    sdk.save_route = &kSaveRouteApi;
    sdk.config_namespace = &config_namespace_api();
    sdk.feature = &production_feature_api();
    sdk.feature_control = nullptr;
    // Runtime effects and generic OP permission are shared through the production Host APIs.
    // A size/minor mismatch leaves both optional tails unavailable.
    if (!production_feature_bind_runtime_sdk(sdk)) {
        sdk.runtime_effects = nullptr;
        sdk.security = nullptr;
    }
    sdk.module_health = &module_health_facts_api();
    return sdk;
}

}  // namespace

extern const contracts::QolHostSdk g_host_sdk = make_host_sdk();

}  // namespace qol::host

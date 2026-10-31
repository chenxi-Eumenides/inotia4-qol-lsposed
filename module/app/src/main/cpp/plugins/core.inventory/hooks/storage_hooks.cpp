#include "storage_hooks.h"

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

#include "contracts/bag.h"
#include "plugins/core.inventory/api/bag_source.h"
#include "plugins/core.inventory/hooks/inventory_logging.h"

namespace qol::features::core_inventory::hooks {
namespace {

using HaveItemFn = int (*)(int32_t);
using GetItemCountFn = int (*)(int32_t);
using GetCumulateCountFn = int (*)(void*);
using IsHavingEmptySlotFn = int (*)(int32_t, int32_t);

constexpr const char* kClaimOwner = "core.inventory";

const qol::contracts::QolHostSdk* g_sdk = nullptr;
const qol::contracts::QolGameHost* g_game = nullptr;
const qol::contracts::QolItemPort* g_port = nullptr;
std::atomic<bool> g_support{true};

HaveItemFn g_backup_have_item = nullptr;
GetItemCountFn g_backup_get_item_count = nullptr;
GetCumulateCountFn g_backup_get_cumulate_count = nullptr;
IsHavingEmptySlotFn g_backup_is_having_empty_slot = nullptr;

thread_local bool g_in_have_item = false;
thread_local bool g_in_get_item_count = false;

void backup_missing(const char* hook_id) {
    QOL_LOG_ERROR(QolDomain::kInventory, "hook=%s backup missing",
                  hook_id == nullptr ? "unknown" : hook_id);
}

template <typename Function, typename... Args>
auto call_backup(const char* hook_id, Function backup, Args&&... args)
    -> decltype(backup(std::forward<Args>(args)...)) {
    using Return = decltype(backup(std::forward<Args>(args)...));
    if (backup == nullptr) {
        backup_missing(hook_id);
        if constexpr (std::is_void<Return>::value) {
            return;
        } else {
            return Return{};
        }
    }
    return backup(std::forward<Args>(args)...);
}

struct ExtensionCountContext {
    int32_t category;
    int64_t count;
};

int32_t count_source_item(const qol::contracts::QolInventoryItemRef* item, void* raw) {
    auto* context = static_cast<ExtensionCountContext*>(raw);
    if (item != nullptr && item->category == context->category && item->count > 0) {
        context->count += item->count;
    }
    return 0;
}

int registered_source_item_count(int32_t category) {
    ExtensionCountContext context{category, 0};
    for (uint32_t index = 0; index < registered_bag_source_count(); ++index) {
        const auto* source = registered_bag_source_at(index);
        if (source != nullptr && source->for_each_item != nullptr) {
            (void)source->for_each_item(&count_source_item, &context);
        }
    }
    return context.count > INT32_MAX ? INT32_MAX : static_cast<int>(context.count);
}

int stage_have_item(int32_t category, HaveItemFn backup) {
    if (g_in_have_item) return backup == nullptr ? 0 : backup(category);
    g_in_have_item = true;
    const int original = backup == nullptr ? 0 : backup(category);
    const int extension = registered_source_item_count(category);
    g_in_have_item = false;
    return original != 0 ? original : (extension > 0 ? 1 : 0);
}

int stage_get_item_count(int32_t category, GetItemCountFn backup) {
    if (g_in_get_item_count) return backup == nullptr ? 0 : backup(category);
    g_in_get_item_count = true;
    const int original = backup == nullptr ? 0 : backup(category);
    const int extension = registered_source_item_count(category);
    g_in_get_item_count = false;
    return original + extension;
}

int stage_get_cumulate_count(void* item, const qol::contracts::QolItemCountApi* item_count,
                             GetCumulateCountFn backup) {
    if (item != nullptr && item_count != nullptr && item_count->effective != nullptr) {
        uint32_t count = 0;
        if (qol::contracts::succeeded(item_count->effective(item, &count))) {
            return static_cast<int>(count);
        }
    }
    return backup == nullptr ? 0 : backup(item);
}

int64_t count_empty_slots_all(int32_t needed) {
    int64_t empty = 0;
    if (g_port != nullptr && qol::contracts::qol_item_port_bag_space_size_valid(g_port) &&
        g_port->read.bag_count != nullptr && g_port->read.bag_info != nullptr &&
        g_port->read.item_at != nullptr) {
        for (int32_t space = qol::contracts::kQolBagSpaceOriginal;
             space <= qol::contracts::kQolBagSpaceVariant; ++space) {
            int32_t bag_count = 0;
            if (!qol::contracts::succeeded(g_port->read.bag_count(space, &bag_count)) ||
                bag_count < 0) {
                continue;
            }
            for (int32_t bag = 0; bag < bag_count; ++bag) {
                if (space == qol::contracts::kQolBagSpaceOriginal &&
                    bag == qol::contracts::kQolTaskBagIndex) {
                    continue;
                }
                qol::contracts::QolBagRef ref{space, bag};
                qol::contracts::QolBagInfo info{};
                if (!qol::contracts::succeeded(g_port->read.bag_info(&ref, &info)) ||
                    info.capacity < 0) {
                    continue;
                }
                for (int32_t slot = 0; slot < info.capacity; ++slot) {
                    qol::contracts::QolInventoryItemRef item{};
                    const QolResult result = g_port->read.item_at(&ref, slot, &item);
                    if (result.code == kQolErrNotFound) ++empty;
                    if (empty >= needed) return empty;
                }
            }
        }
    }
    for (uint32_t index = 0; index < registered_bag_source_count(); ++index) {
        const auto* source = registered_bag_source_at(index);
        if (source == nullptr || source->count == nullptr || source->info == nullptr ||
            source->item_at == nullptr) {
            continue;
        }
        const int32_t count = source->count();
        if (count < 0) continue;
        for (int32_t bag = 0; bag < count; ++bag) {
            qol::contracts::QolBagInfo info{};
            if (!qol::contracts::succeeded(source->info(bag, &info)) || info.capacity < 0) {
                continue;
            }
            for (int32_t slot = 0; slot < info.capacity; ++slot) {
                qol::contracts::QolInventoryItemRef item{};
                const QolResult result = source->item_at(bag, slot, &item);
                if (result.code == kQolErrNotFound) ++empty;
                if (empty >= needed) return empty;
            }
        }
    }
    return empty;
}

void* find_item_wrapper(int32_t category) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight, {"hook:INVEN_FindItem"});
    if (g_game == nullptr || g_game->item_abi.find_item_wrapper == nullptr) {
        QOL_LOG_WARN(QolDomain::kInventory,
                     "INVEN_FindItem Game Host dispatch unavailable category=%d", category);
        return nullptr;
    }
    return g_game->item_abi.find_item_wrapper(category);
}

int have_item_wrapper(int32_t category) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight, {"hook:INVEN_HaveItem"});
    if (!support_enabled()) {
        return call_backup("hook:INVEN_HaveItem", g_backup_have_item, category);
    }
    if (g_backup_have_item == nullptr) {
        backup_missing("hook:INVEN_HaveItem");
        return 0;
    }
    return stage_have_item(category, g_backup_have_item);
}

int get_item_count_wrapper(int32_t category) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight, {"hook:INVEN_GetItemCount"});
    if (!support_enabled()) {
        return call_backup("hook:INVEN_GetItemCount", g_backup_get_item_count, category);
    }
    if (g_backup_get_item_count == nullptr) {
        backup_missing("hook:INVEN_GetItemCount");
        return 0;
    }
    return stage_get_item_count(category, g_backup_get_item_count);
}

int get_cumulate_count_wrapper(void* item) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight, {"hook:ITEM_GetCumulateCount"});
    if (!support_enabled()) {
        return call_backup("hook:ITEM_GetCumulateCount", g_backup_get_cumulate_count, item);
    }
    if (g_backup_get_cumulate_count == nullptr) {
        backup_missing("hook:ITEM_GetCumulateCount");
        return 0;
    }
    const auto* item_count = g_game == nullptr ? nullptr : &g_game->item_count;
    return stage_get_cumulate_count(item, item_count, g_backup_get_cumulate_count);
}

int is_having_empty_slot_wrapper(int32_t needed, int32_t include_task_bag) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight, {"hook:INVEN_IsHavingEmptySlot"});
    if (!support_enabled()) {
        return call_backup("hook:INVEN_IsHavingEmptySlot", g_backup_is_having_empty_slot,
                           needed, include_task_bag);
    }
    if (g_backup_is_having_empty_slot == nullptr) {
        backup_missing("hook:INVEN_IsHavingEmptySlot");
        return 0;
    }
    if (needed <= 0) return 1;
    if (include_task_bag != 0) {
        return call_backup("hook:INVEN_IsHavingEmptySlot", g_backup_is_having_empty_slot,
                           needed, include_task_bag);
    }
    if (g_port == nullptr || !qol::contracts::qol_item_port_bag_space_size_valid(g_port) ||
        g_port->read.bag_count == nullptr || g_port->read.bag_info == nullptr ||
        g_port->read.item_at == nullptr) {
        return call_backup("hook:INVEN_IsHavingEmptySlot", g_backup_is_having_empty_slot,
                           needed, include_task_bag);
    }
    return count_empty_slots_all(needed) >= needed ? 1 : 0;
}

#define STORAGE_HOOK_CLAIM(id, symbol, handler)                                           \
    {id, symbol, qol::contracts::ResourceMechanism::kEntryHook,                           \
     qol::contracts::ResourcePolicy::kPrivate,                                            \
     qol::contracts::ResourceRequirement::kRequired,                                      \
     qol::contracts::ResourceFailure::kFailClosed, true, kClaimOwner, "storage hook",      \
     nullptr, 120, reinterpret_cast<void*>(&handler)}

const qol::contracts::ResourceClaim kClaims[] = {
    STORAGE_HOOK_CLAIM("hook:INVEN_FindItem", "F_FIND_ITEM_VMA", find_item_wrapper),
    STORAGE_HOOK_CLAIM("hook:INVEN_HaveItem", "F_INVEN_HAVE_ITEM_VMA", have_item_wrapper),
    STORAGE_HOOK_CLAIM("hook:INVEN_GetItemCount", "F_INVEN_GET_ITEM_COUNT_VMA",
                       get_item_count_wrapper),
    STORAGE_HOOK_CLAIM("hook:ITEM_GetCumulateCount", "F_GET_CUMULATE_COUNT_VMA",
                       get_cumulate_count_wrapper),
    STORAGE_HOOK_CLAIM("hook:INVEN_IsHavingEmptySlot", "F_INVEN_IS_HAVING_EMPTY_SLOT_VMA",
                       is_having_empty_slot_wrapper),
};

#undef STORAGE_HOOK_CLAIM

void** backup_slot_for_handler(void* handler) {
    if (handler == reinterpret_cast<void*>(&have_item_wrapper)) {
        return reinterpret_cast<void**>(&g_backup_have_item);
    }
    if (handler == reinterpret_cast<void*>(&get_item_count_wrapper)) {
        return reinterpret_cast<void**>(&g_backup_get_item_count);
    }
    if (handler == reinterpret_cast<void*>(&get_cumulate_count_wrapper)) {
        return reinterpret_cast<void**>(&g_backup_get_cumulate_count);
    }
    if (handler == reinterpret_cast<void*>(&is_having_empty_slot_wrapper)) {
        return reinterpret_cast<void**>(&g_backup_is_having_empty_slot);
    }
    return nullptr;
}

}  // namespace

bool attach(const qol::contracts::QolHostSdk* sdk, const qol::contracts::QolGameHost* game,
            const qol::contracts::QolItemPort* port) {
    if (sdk == nullptr || game == nullptr || port == nullptr) return false;
    g_sdk = sdk;
    g_game = game;
    g_port = port;
    core_inventory_log_bind_sdk(sdk);
    if (g_game->item_abi.bind_find_item_backup == nullptr) return false;
    QOL_LOG_INFO(QolDomain::kInventory, "storage hooks attached read=5");
    return true;
}

void detach() {
    g_sdk = nullptr;
    g_game = nullptr;
    g_port = nullptr;
    g_backup_have_item = nullptr;
    g_backup_get_item_count = nullptr;
    g_backup_get_cumulate_count = nullptr;
    g_backup_is_having_empty_slot = nullptr;
    core_inventory_log_bind_sdk(nullptr);
}

const qol::contracts::ResourceClaim* claims(uint32_t* out_count) {
    if (out_count != nullptr) *out_count = sizeof(kClaims) / sizeof(kClaims[0]);
    return kClaims;
}

int32_t bind_backup(const char* resource_id, void* backup) {
    if (resource_id == nullptr || backup == nullptr) return 1;
    for (const auto& claim : kClaims) {
        if (std::strcmp(claim.id, resource_id) != 0) continue;
        if (std::strcmp(claim.id, "hook:INVEN_FindItem") == 0) {
            if (g_game == nullptr || g_game->item_abi.bind_find_item_backup == nullptr) return 1;
            const auto result = g_game->item_abi.bind_find_item_backup(
                reinterpret_cast<qol::contracts::QolFindItemAbiFn>(backup));
            return qol::contracts::succeeded(result) ? 0 : 1;
        }
        void** slot = backup_slot_for_handler(claim.handler);
        if (slot == nullptr) return 1;
        *slot = backup;
        return 0;
    }
    QOL_LOG_ERROR(QolDomain::kInventory, "unknown storage resource id=%s", resource_id);
    return 1;
}

void set_support(bool enabled) { g_support.store(enabled, std::memory_order_release); }

bool support_enabled() { return g_support.load(std::memory_order_acquire); }

}  // namespace qol::features::core_inventory::hooks

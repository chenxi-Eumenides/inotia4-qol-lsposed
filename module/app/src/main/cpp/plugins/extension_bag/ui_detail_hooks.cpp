#include "game_ui_virtbag.h"
#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/extension_bag_equip_rules.h"
#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/extension_bag_internal.h"
#include "plugins/extension_bag/extension_bag_logging.h"
#include "plugins/extension_bag/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/plugin.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>

void reset_extension_desc_item_hooks_locked() {
    const auto* api = qol::contracts::desc_hook_v2_if_available(g_sdk);
    if (api == nullptr) {
        g_extension_desc_item_bound.fill(false);
        g_extension_desc_item_ids.fill(0);
        return;
    }
    for (size_t index = 0; index < g_extension_desc_item_bound.size(); ++index) {
        const uint64_t id = g_extension_desc_item_ids[index];
        if (!g_extension_desc_item_bound[index] || id == 0) {
            g_extension_desc_item_bound[index] = false;
            g_extension_desc_item_ids[index] = 0;
            continue;
        }
        const QolResult result = api->unbind(id);
        if (!qol::contracts::succeeded(result) && result.code != kQolErrNotFound &&
            result.code != kQolErrNotReady) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "extension desc hook unbind failed index=%d id=%llu reason=host_unbind_error code=%d message=%.*s",
                         static_cast<int>(index), static_cast<unsigned long long>(id), result.code,
                         static_cast<int>(result.message_len),
                         result.message != nullptr ? result.message : "");
        }
        g_extension_desc_item_bound[index] = false;
        g_extension_desc_item_ids[index] = 0;
    }
}

namespace {
void extension_desc_item_execute_0(void* button) { extension_desc_item_execute_at(0, button); }
void extension_desc_item_execute_1(void* button) { extension_desc_item_execute_at(1, button); }
void extension_desc_item_execute_2(void* button) { extension_desc_item_execute_at(2, button); }
void extension_desc_item_execute_3(void* button) { extension_desc_item_execute_at(3, button); }
void extension_desc_item_execute_4(void* button) { extension_desc_item_execute_at(4, button); }
void extension_desc_item_execute_5(void* button) { extension_desc_item_execute_at(5, button); }
void extension_desc_item_execute_6(void* button) { extension_desc_item_execute_at(6, button); }
void extension_desc_item_execute_7(void* button) { extension_desc_item_execute_at(7, button); }
void extension_desc_item_execute_8(void* button) { extension_desc_item_execute_at(8, button); }
void extension_desc_item_execute_9(void* button) { extension_desc_item_execute_at(9, button); }
void extension_desc_item_execute_10(void* button) { extension_desc_item_execute_at(10, button); }

constexpr std::array<qol::contracts::QolDescButtonProc, 11> kDescButtonProcs = {
    &extension_desc_item_execute_0, &extension_desc_item_execute_1,
    &extension_desc_item_execute_2, &extension_desc_item_execute_3,
    &extension_desc_item_execute_4, &extension_desc_item_execute_5,
    &extension_desc_item_execute_6, &extension_desc_item_execute_7,
    &extension_desc_item_execute_8, &extension_desc_item_execute_9,
    &extension_desc_item_execute_10,
};
}  // namespace

bool extension_desc_original_matches(uint64_t id, const char* symbol) {
    const auto* api = qol::contracts::desc_hook_v2_if_available(g_sdk);
    if (api == nullptr || id == 0 || symbol == nullptr) return false;
    int32_t match = 0;
    return qol::contracts::succeeded(api->original_matches(id, symbol, &match)) && match != 0;
}

bool extension_desc_call_original(uint64_t id, void* button) {
    const auto* api = qol::contracts::desc_hook_v2_if_available(g_sdk);
    return api != nullptr && id != 0 && button != nullptr &&
           qol::contracts::succeeded(api->call_original(id, button));
}

void install_extension_desc_item_hooks_locked(void* item) {
    if (item == nullptr) return;
    static bool unavailable_logged = false;
    const auto* api = qol::contracts::desc_hook_v2_if_available(g_sdk);
    if (api == nullptr) {
        if (!unavailable_logged) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "extension desc hook skipped reason=desc hook v2 unavailable");
            unavailable_logged = true;
        }
        return;
    }
    unavailable_logged = false;

    std::array<void*, 11> resolved_execute_slots{};
    size_t resolved_execute_slot_count = 0;
    for (size_t index = 0; index < g_extension_desc_item_bound.size(); ++index) {
        if (index == 2) continue;
        if (g_extension_desc_item_bound[index] || g_extension_desc_item_ids[index] != 0) {
            QOL_LOG_DEBUG(QolDomain::kExtensionBag,
                          "extension desc hook skip index=%d reason=already_bound id=%llu",
                          static_cast<int>(index),
                          static_cast<unsigned long long>(g_extension_desc_item_ids[index]));
            continue;
        }

        qol::contracts::QolDescButtonTarget target{};
        const QolResult resolve_result = api->resolve(static_cast<uint32_t>(index), &target);
        if (!qol::contracts::succeeded(resolve_result)) {
            QOL_LOG_DEBUG(QolDomain::kExtensionBag,
                          "extension desc hook resolve skip index=%d code=%d",
                          static_cast<int>(index), resolve_result.code);
            continue;
        }
        if (target.execute_slot == nullptr) {
            QOL_LOG_DEBUG(QolDomain::kExtensionBag,
                          "extension desc hook resolve skip index=%d reason=null_execute_slot",
                          static_cast<int>(index));
            continue;
        }

        bool duplicate_execute_slot = false;
        for (size_t seen = 0; seen < resolved_execute_slot_count; ++seen) {
            if (resolved_execute_slots[seen] == target.execute_slot) {
                duplicate_execute_slot = true;
                break;
            }
        }
        if (duplicate_execute_slot) {
            QOL_LOG_DEBUG(QolDomain::kExtensionBag,
                          "extension desc hook resolve skip index=%d reason=duplicate_execute_slot execute=%p",
                          static_cast<int>(index), target.execute_slot);
            continue;
        }

        uint64_t id = 0;
        const QolResult bind_result = api->bind(static_cast<uint32_t>(index), &target,
                                                 kDescButtonProcs[index], &id);
        if (!qol::contracts::succeeded(bind_result) || id == 0) {
            QOL_LOG_DEBUG(QolDomain::kExtensionBag,
                          "extension desc hook bind skip index=%d code=%d id=%llu",
                          static_cast<int>(index), bind_result.code,
                          static_cast<unsigned long long>(id));
            continue;
        }
        // only successful bindings occupy an execute slot for dedupe; a failed
        // bind must not hide a later alias that can still be installed.
        resolved_execute_slots[resolved_execute_slot_count++] = target.execute_slot;

        int32_t equip_match = 0;
        int32_t unequip_match = 0;
        const QolResult equip_result = api->original_matches(id, "desc.button.equip", &equip_match);
        const QolResult unequip_result =
            api->original_matches(id, "desc.button.unequip", &unequip_match);
        if (!qol::contracts::succeeded(equip_result) ||
            !qol::contracts::succeeded(unequip_result)) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "extension desc hook classify failed index=%d id=%llu equip_code=%d unequip_code=%d reason=original_matches_error",
                         static_cast<int>(index), static_cast<unsigned long long>(id),
                         equip_result.code, unequip_result.code);
            const QolResult unbind_result = api->unbind(id);
            if (!qol::contracts::succeeded(unbind_result)) {
                QOL_LOG_WARN(QolDomain::kExtensionBag,
                             "extension desc hook cleanup failed index=%d id=%llu reason=host_unbind_error code=%d",
                             static_cast<int>(index), static_cast<unsigned long long>(id),
                             unbind_result.code);
                if (unbind_result.code != kQolErrNotFound &&
                    unbind_result.code != kQolErrNotReady &&
                    unbind_result.code != kQolErrConflict) {
                    g_extension_desc_item_ids[index] = id;
                    g_extension_desc_item_bound[index] = true;
                }
            }
            continue;
        }
        if (equip_match != 0 || unequip_match != 0) {
            const QolResult unbind_result = api->unbind(id);
            if (!qol::contracts::succeeded(unbind_result)) {
                QOL_LOG_WARN(QolDomain::kExtensionBag,
                             "extension desc hook equip/unequip unbind failed index=%d id=%llu equip=%d unequip=%d reason=host_unbind_error code=%d",
                             static_cast<int>(index), static_cast<unsigned long long>(id),
                             equip_match, unequip_match, unbind_result.code);
                if (unbind_result.code != kQolErrNotFound &&
                    unbind_result.code != kQolErrNotReady &&
                    unbind_result.code != kQolErrConflict) {
                    g_extension_desc_item_ids[index] = id;
                    g_extension_desc_item_bound[index] = true;
                }
            }
            continue;
        }

        g_extension_desc_item_ids[index] = id;
        g_extension_desc_item_bound[index] = true;
        QOL_LOG_DEBUG(QolDomain::kExtensionBag,
                      "extension desc hook installed index=%d id=%llu button=%p data=%p execute_slot=%p original=%p",
                      static_cast<int>(index), static_cast<unsigned long long>(id),
                      target.button, target.data, target.execute_slot, target.original_proc);
    }
}

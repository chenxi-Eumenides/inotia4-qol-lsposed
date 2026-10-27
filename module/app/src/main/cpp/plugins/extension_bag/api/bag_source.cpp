#include "plugins/extension_bag/api/bag_source.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "plugins/extension_bag/api/api_impl.h"
#include "plugins/extension_bag/core/projection.h"
#include "plugins/extension_bag/core/ui_state.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/runtime/entry_guard.h"
#include "plugins/extension_bag/runtime/extension_bag_port.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/runtime/object_state_port.h"
#include "plugins/extension_bag/runtime/ownership_port.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"
#include "plugins/extension_bag/runtime/session.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/state_access.h"

namespace qol::features::extension_bag {
namespace {

const contracts::QolBagSourceRegistryApi* g_registry = nullptr;
bool g_registered = false;

QolResult source_error(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

bool source_ready() {
    return ::qol::extension_bag::access::enabled().load(std::memory_order_acquire) && read_world_in_world() &&
           extension_bag_session_admitted_locked();
}

bool valid_bag(int bag) {
    return bag >= 0 && bag < virtual_bag::active_extension_bags();
}

bool valid_slot(int bag, int slot) {
    return valid_bag(bag) && slot >= 0 &&
           slot < ::qol::extension_bag::access::bag().capacities[bag];
}

bool descriptor_empty(const virtual_bag::Item& item) {
    return item.category <= 0 || item.count <= 0;
}

// During an EB-owned ApplyStuff call, core.item consumes the material through
// QolBagSource::remove. That call must bind the active ModuleUseToken so the
// outer apply can observe consumption and defer releasing a whole stack.
ModuleUseToken* active_use_token_for(int bag, int slot, void* item) {
    ModuleUseToken* token = g_active_module_use_token;
    if (token == nullptr || token->bag != bag || token->slot != slot || token->item != item) {
        return nullptr;
    }
    const ModuleUseState& use =
        qol::extension_bag::ui_state().objects.module_object_use[bag][slot];
    if (!use.active || use.item != item || use.generation != token->generation ||
        use.owner != token->owner) {
        return nullptr;
    }
    return token;
}

int encoded_category(int category) {
    return static_cast<int>(virtual_bag_category_uses_stack_count(category));
}

void* materialize_item_locked(int bag, int slot, contracts::QolItemHandle* out_handle) {
    if (out_handle == nullptr) return nullptr;
    *out_handle = contracts::kInvalidItemHandle;
    void* item = module_item_locked(bag, slot);
    if (item == nullptr) return nullptr;
    if (!extension_bag_register_item_handle(item) ||
        !extension_bag_item_handle_for_pointer(item, out_handle)) {
        return nullptr;
    }
    return item;
}

int32_t source_count() {
    if (!source_ready()) return 0;
    return virtual_bag::active_extension_bags();
}

QolResult source_info(int32_t bag, contracts::QolBagInfo* out) {
    if (out == nullptr) return source_error(kQolErrInvalidArgument, "invalid bag info");
    if (!source_ready()) return source_error(kQolErrNotReady, "extension bag not ready");
    if (!valid_bag(bag)) return source_error(kQolErrInvalidArgument, "invalid extension bag");
    ::qol::extension_bag::ReentryLock lock;
    int32_t occupied_slots = 0;
    const int capacity = ::qol::extension_bag::access::bag().capacities[bag];
    for (int slot = 0; slot < capacity; ++slot) {
        if (!descriptor_empty(::qol::extension_bag::access::bag().items[bag][slot])) ++occupied_slots;
    }
    *out = contracts::QolBagInfo{capacity, occupied_slots};
    return contracts::ok();
}

QolResult source_item_at(int32_t bag, int32_t slot, contracts::QolInventoryItemRef* out) {
    if (out == nullptr) return source_error(kQolErrInvalidArgument, "invalid item ref");
    *out = {};
    if (!source_ready()) return source_error(kQolErrNotReady, "extension bag not ready");
    if (!valid_slot(bag, slot)) {
        return source_error(kQolErrInvalidArgument, "invalid extension bag slot");
    }
    ::qol::extension_bag::ReentryLock lock;
    const virtual_bag::Item& descriptor = ::qol::extension_bag::access::bag().items[bag][slot];
    if (descriptor_empty(descriptor)) return source_error(kQolErrNotFound, "extension slot empty");
    contracts::QolItemHandle handle{};
    void* item = materialize_item_locked(bag, slot, &handle);
    if (item == nullptr) return source_error(kQolErrNotReady, "extension item unavailable");
    out->kind = static_cast<int32_t>(contracts::QolInventoryItemKind::kProvider);
    out->bag = bag;
    out->slot = slot;
    out->category = descriptor.category;
    out->count = static_cast<int32_t>(
        virtual_bag_effective_count(item, static_cast<uint32_t>(descriptor.count)));
    out->item = handle;
    out->space_id = kExtensionBagSpaceId;
    return contracts::ok();
}

QolResult source_for_each_item(contracts::QolInventoryBagSlotFn fn, void* ctx) {
    if (fn == nullptr) return source_error(kQolErrInvalidArgument, "invalid item callback");
    if (!source_ready()) return source_error(kQolErrNotReady, "extension bag not ready");
    ::qol::extension_bag::ReentryLock lock;
    const int active = virtual_bag::active_extension_bags();
    for (int bag = 0; bag < active; ++bag) {
        const int capacity = ::qol::extension_bag::access::bag().capacities[bag];
        for (int slot = 0; slot < capacity; ++slot) {
            const virtual_bag::Item& descriptor = ::qol::extension_bag::access::bag().items[bag][slot];
            if (descriptor_empty(descriptor)) continue;
            contracts::QolItemHandle handle{};
            void* item = materialize_item_locked(bag, slot, &handle);
            if (item == nullptr) {
                QOL_LOG_WARN(QolDomain::kExtensionBag,
                             "bag source enumerate skipped unmaterializable item bag=%d slot=%d",
                             bag, slot);
                continue;
            }
            contracts::QolInventoryItemRef ref{};
            ref.kind = static_cast<int32_t>(contracts::QolInventoryItemKind::kProvider);
            ref.bag = bag;
            ref.slot = slot;
            ref.category = descriptor.category;
            ref.count = static_cast<int32_t>(
                virtual_bag_effective_count(item, static_cast<uint32_t>(descriptor.count)));
            ref.item = handle;
            ref.space_id = kExtensionBagSpaceId;
            if (fn(&ref, ctx) != 0) return contracts::ok();
        }
    }
    return contracts::ok();
}

QolResult source_export_item(contracts::QolItemHandle item_handle, int32_t count,
                             contracts::QolBagTransfer* out) {
    if (out == nullptr || count <= 0) {
        return source_error(kQolErrInvalidArgument, "invalid transfer");
    }
    *out = {};
    if (!source_ready()) return source_error(kQolErrNotReady, "extension bag not ready");
    ::qol::extension_bag::ReentryLock lock;
    void* item = nullptr;
    if (!extension_bag_private_item_for_handle(item_handle, &item) || item == nullptr) {
        return source_error(kQolErrNotFound, "extension item not found");
    }
    int bag = -1;
    int slot = -1;
    if (!module_slot_of_item_locked(item, &bag, &slot)) {
        return source_error(kQolErrNotFound, "extension item not found");
    }
    if (bag < 0 || slot < 0 || !valid_slot(bag, slot)) {
        return source_error(kQolErrNotFound, "extension item not found");
    }
    if (qol::extension_bag::ui_state().objects.module_objects[bag][slot] != item) {
        return source_error(kQolErrNotFound, "extension item not found");
    }
    const virtual_bag::Item& descriptor = ::qol::extension_bag::access::bag().items[bag][slot];
    if (descriptor_empty(descriptor)) return source_error(kQolErrNotFound, "extension slot empty");
    const int32_t visible = static_cast<int32_t>(
        virtual_bag_effective_count(item, static_cast<uint32_t>(descriptor.count)));
    if (count > visible) {
        return source_error(kQolErrInvalidArgument, "transfer count exceeds item count");
    }
    if (descriptor.payload_size == 0 ||
        descriptor.payload_size > contracts::kQolBagTransferPayloadMaxSize ||
        !virtual_bag::valid_payload(descriptor)) {
        return source_error(kQolErrConflict, "extension item payload invalid");
    }
    out->category = descriptor.category;
    out->count = count;
    out->payload_size = descriptor.payload_size;
    std::memset(out->payload, 0, sizeof(out->payload));
    std::memcpy(out->payload, descriptor.payload.data(), descriptor.payload_size);
    return contracts::ok();
}

QolResult source_import_item(const contracts::QolBagTransfer* transfer, int32_t preferred_bag,
                             int32_t preferred_slot, contracts::QolBagMutationResult* out) {
    if (transfer == nullptr || out == nullptr || transfer->category <= 0 ||
        transfer->count <= 0 || transfer->payload_size == 0 ||
        transfer->payload_size > contracts::kQolBagTransferPayloadMaxSize) {
        return source_error(kQolErrInvalidArgument, "invalid transfer");
    }
    *out = {};
    if (!source_ready()) return source_error(kQolErrNotReady, "extension bag not ready");
    if (virtual_bag::validate_serialized_payload_buffer(
            transfer->payload, sizeof(transfer->payload), static_cast<int>(transfer->payload_size)) !=
        virtual_bag::PayloadValidation::kOk) {
        return source_error(kQolErrInvalidArgument, "invalid transfer payload");
    }

    virtual_bag::Item candidate{};
    candidate.category = transfer->category;
    candidate.count = transfer->count;
    candidate.payload_size = transfer->payload_size;
    std::memcpy(candidate.payload.data(), transfer->payload, transfer->payload_size);
    const bool stackable =
        encoded_category(transfer->category) ==
        static_cast<int>(qol::contracts::QolItemCountEncoding::kEncoded);
    if (stackable &&
        static_cast<uint32_t>(transfer->count) > virtual_bag_count_limit(true)) {
        return source_error(kQolErrInvalidArgument, "transfer count exceeds stack limit");
    }
    if (!stackable) {
        // 非堆叠装备/袋类物品的序列化 payload 可能没有 count 位段，不能因为
        // payload_count 为 0 就拒绝单件转移。
        candidate.count = transfer->count;
    } else if (virtual_bag::payload_count(candidate) !=
               static_cast<uint32_t>(transfer->count)) {
        if (!virtual_bag::patch_payload_count(&candidate,
                                              static_cast<uint32_t>(transfer->count),
                                              &encoded_category)) {
            if (transfer->count != 1 ||
                virtual_bag::payload_count(candidate) != 1) {
                return source_error(kQolErrInvalidArgument,
                                    "transfer count does not match payload");
            }
        }
    }
    if (!virtual_bag::valid_payload(candidate)) {
        return source_error(kQolErrInvalidArgument, "invalid transfer payload");
    }

    ::qol::extension_bag::ReentryLock lock;
    const int active = virtual_bag::active_extension_bags();
    int bag = -1;
    int slot = -1;
    if (preferred_bag < 0 && preferred_slot >= 0) {
        return source_error(kQolErrInvalidArgument, "preferred slot requires preferred bag");
    }
    if (preferred_bag >= 0) {
        if (!valid_bag(preferred_bag)) {
            return source_error(kQolErrInvalidArgument, "invalid preferred bag");
        }
        bag = preferred_bag;
        if (preferred_slot >= 0) {
            if (preferred_slot >= ::qol::extension_bag::access::bag().capacities[bag]) {
                return source_error(kQolErrInvalidArgument, "invalid preferred slot");
            }
            slot = preferred_slot;
        } else {
            for (int candidate_slot = 0; candidate_slot < ::qol::extension_bag::access::bag().capacities[bag];
                 ++candidate_slot) {
                if (module_slot_is_assignable_locked(bag, candidate_slot)) {
                    slot = candidate_slot;
                    break;
                }
            }
        }
    } else {
        for (int candidate_bag = 0; candidate_bag < active && slot < 0; ++candidate_bag) {
            for (int candidate_slot = 0;
                 candidate_slot < ::qol::extension_bag::access::bag().capacities[candidate_bag]; ++candidate_slot) {
                if (module_slot_is_assignable_locked(candidate_bag, candidate_slot)) {
                    bag = candidate_bag;
                    slot = candidate_slot;
                    break;
                }
            }
        }
    }
    if (bag < 0 || slot < 0) {
        return source_error(kQolErrConflict, "extension bag has no free slot");
    }
    auto& bag_items = ::qol::extension_bag::access::bag().items[bag][slot];
    if (!descriptor_empty(bag_items)) {
        if (bag_items.category != candidate.category || bag_items.count < 0) {
            return source_error(kQolErrConflict, "extension slot contains another item");
        }
        const uint32_t merged_count =
            static_cast<uint32_t>(bag_items.count) + static_cast<uint32_t>(transfer->count);
        if (merged_count > virtual_bag_count_limit(true)) {
            return source_error(kQolErrBlocked, "extension target stack full");
        }
        virtual_bag::Item merged = bag_items;
        if (!virtual_bag::patch_payload_count(&merged, merged_count, &encoded_category) ||
            !virtual_bag::valid_payload(merged)) {
            return source_error(kQolErrInternal, "extension merge payload failed");
        }
        merged.count = static_cast<int>(merged_count);
        const virtual_bag::Item previous = bag_items;
        const uint32_t previous_hash =
            qol::extension_bag::ui_state().objects.module_object_hashes[bag][slot];
        bag_items = merged;
        qol::extension_bag::ui_state().objects.module_object_hashes[bag][slot] =
            virtual_bag::payload_hash(merged);
        extension_bag_mark_dirty_locked();
        if (!persist_state_locked()) {
            bag_items = previous;
            qol::extension_bag::ui_state().objects.module_object_hashes[bag][slot] = previous_hash;
            return source_error(kQolErrInternal, "extension item persist failed");
        }
        if (module_item_locked(bag, slot) != nullptr &&
            !free_module_object_locked(bag, slot, nullptr)) {
            bag_items = previous;
            qol::extension_bag::ui_state().objects.module_object_hashes[bag][slot] = previous_hash;
            (void)persist_state_locked();
            return source_error(kQolErrConflict, "extension item busy");
        }
        contracts::QolItemHandle handle{};
        if (materialize_item_locked(bag, slot, &handle) == nullptr) {
            bag_items = previous;
            qol::extension_bag::ui_state().objects.module_object_hashes[bag][slot] = previous_hash;
            (void)persist_state_locked();
            return source_error(kQolErrInternal, "extension item materialization failed");
        }
        if (qol::extension_bag::ui_state().objects.module_view_installed &&
            qol::extension_bag::ui_state().objects.module_view_index == bag) {
            refresh_module_item_area_locked(bag);
        }
        out->ref = contracts::QolBagRef{kExtensionBagSpaceId, bag};
        out->item = handle;
        out->count = transfer->count;
        return contracts::ok();
    }
    if (!module_slot_is_assignable_locked(bag, slot)) {
        return source_error(kQolErrConflict, "extension slot unavailable");
    }

    const virtual_bag::Item previous = ::qol::extension_bag::access::bag().items[bag][slot];
    ::qol::extension_bag::access::bag().items[bag][slot] = candidate;
    contracts::QolItemHandle handle{};
    void* item = materialize_item_locked(bag, slot, &handle);
    if (item == nullptr) {
        ::qol::extension_bag::access::bag().items[bag][slot] = previous;
        (void)free_module_object_locked(bag, slot);
        return source_error(kQolErrInternal, "extension item materialization failed");
    }
    extension_bag_mark_dirty_locked();
    if (!persist_state_locked()) {
        ::qol::extension_bag::access::bag().items[bag][slot] = previous;
        qol::extension_bag::ui_state().objects.module_object_hashes[bag][slot] =
            virtual_bag::payload_hash(previous);
        qol::extension_bag::ui_state().objects.module_object_categories[bag][slot] =
            previous.category;
        if (!free_module_object_locked(bag, slot)) {
            QOL_LOG_ERROR(QolDomain::kExtensionBag,
                          "bag source import rollback could not release object bag=%d slot=%d",
                          bag, slot);
        }
        return source_error(kQolErrInternal, "extension item persist failed");
    }
    if (qol::extension_bag::ui_state().objects.module_view_installed &&
        qol::extension_bag::ui_state().objects.module_view_index == bag) {
        refresh_module_item_area_locked(bag);
    }
    out->ref = contracts::QolBagRef{kExtensionBagSpaceId, bag};
    out->item = handle;
    out->count = transfer->count;
    return contracts::ok();
}

QolResult source_remove(contracts::QolItemHandle expected, int32_t count,
                        contracts::QolBagMutationResult* out) {
    if (out == nullptr || count <= 0) {
        return source_error(kQolErrInvalidArgument, "invalid remove request");
    }
    *out = {};
    if (!source_ready()) return source_error(kQolErrNotReady, "extension bag not ready");
    ::qol::extension_bag::ReentryLock lock;
    void* item = nullptr;
    if (!extension_bag_private_item_for_handle(expected, &item) || item == nullptr) {
        return source_error(kQolErrNotFound, "stale item handle");
    }
    int bag = -1;
    int slot = -1;
    if (!module_slot_of_item_locked(item, &bag, &slot) || !valid_slot(bag, slot) ||
        qol::extension_bag::ui_state().objects.module_objects[bag][slot] != item) {
        return source_error(kQolErrNotFound, "stale item handle");
    }
    const virtual_bag::Item& descriptor = ::qol::extension_bag::access::bag().items[bag][slot];
    if (descriptor_empty(descriptor)) {
        return source_error(kQolErrNotFound, "extension slot empty");
    }
    const int32_t visible = static_cast<int32_t>(
        virtual_bag_effective_count(item, static_cast<uint32_t>(descriptor.count)));
    if (count > visible) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "bag source remove count rejected bag=%d slot=%d count=%d visible=%d",
                     bag, slot, count, visible);
        return source_error(kQolErrInvalidArgument, "remove count exceeds item count");
    }
    ModuleUseToken* active_use = active_use_token_for(bag, slot, item);
    if (count >= visible || count >= descriptor.count) {
        if (!module_object_release_allowed_locked(bag, slot, item, "bag source remove", true,
                                                  active_use)) {
            return source_error(kQolErrConflict, "extension item remove rejected");
        }
        const virtual_bag::Item previous = descriptor;
        ::qol::extension_bag::access::bag().items[bag][slot] = {};
        if (!free_module_object_locked(bag, slot, active_use)) {
            ::qol::extension_bag::access::bag().items[bag][slot] = previous;
            return source_error(kQolErrConflict, "extension item remove rejected");
        }
        extension_bag_mark_dirty_locked();
        if (!persist_state_locked()) {
            // 整堆 remove 的持久化失败必须恢复内存状态，否则 core.item 会认为源物品
            // 已删除，而下次读档又回到旧状态。对象已释放，按旧 descriptor 重新物化；
            // 物化失败时 descriptor 仍保留，后续读取会再次尝试。
            ::qol::extension_bag::access::bag().items[bag][slot] = previous;
            qol::extension_bag::ui_state().objects.module_object_hashes[bag][slot] =
                virtual_bag::payload_hash(previous);
            qol::extension_bag::ui_state().objects.module_object_categories[bag][slot] =
                previous.category;
            if (active_use != nullptr) {
                (void)module_use_abort_locked(*active_use);
            }
            extension_bag_mark_dirty_locked();
            if (module_item_locked(bag, slot) == nullptr) {
                QOL_LOG_ERROR(QolDomain::kExtensionBag,
                              "bag source remove persist failed and restore materialization failed "
                              "bag=%d slot=%d",
                              bag, slot);
            }
            if (qol::extension_bag::ui_state().objects.module_view_installed &&
                qol::extension_bag::ui_state().objects.module_view_index == bag) {
                refresh_module_item_area_locked(bag);
            }
            return source_error(kQolErrInternal, "extension item persist failed");
        }
        if (qol::extension_bag::ui_state().objects.module_view_installed &&
            qol::extension_bag::ui_state().objects.module_view_index == bag) {
            refresh_module_item_area_locked(bag);
        }
        out->ref = contracts::QolBagRef{kExtensionBagSpaceId, bag};
        out->item = expected;
        out->count = count;
        return contracts::ok();
    }

    virtual_bag::Item candidate = descriptor;
    candidate.count = descriptor.count - count;
    if (!virtual_bag::patch_payload_count(&candidate,
                                          static_cast<uint32_t>(candidate.count),
                                          &encoded_category)) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "bag source remove patch rejected bag=%d slot=%d category=%d count=%d",
                     bag, slot, descriptor.category, candidate.count);
        return source_error(kQolErrConflict, "extension item quantity update rejected");
    }
    if (!virtual_bag::valid_payload(candidate)) {
        return source_error(kQolErrConflict, "extension item payload invalid");
    }
    if (!virtual_bag_write_effective(item, static_cast<uint32_t>(candidate.count))) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "bag source remove write rejected bag=%d slot=%d category=%d count=%d",
                     bag, slot, descriptor.category, candidate.count);
        return source_error(kQolErrConflict, "extension item quantity update rejected");
    }
    ::qol::extension_bag::access::bag().items[bag][slot] = candidate;
    qol::extension_bag::ui_state().objects.module_object_hashes[bag][slot] =
        virtual_bag::payload_hash(candidate);
    extension_bag_mark_dirty_locked();
    if (!persist_state_locked()) {
        ::qol::extension_bag::access::bag().items[bag][slot] = descriptor;
        if (!virtual_bag_write_effective(item, static_cast<uint32_t>(descriptor.count))) {
            QOL_LOG_ERROR(QolDomain::kExtensionBag,
                          "bag source remove rollback count write failed bag=%d slot=%d",
                          bag, slot);
        }
        qol::extension_bag::ui_state().objects.module_object_hashes[bag][slot] =
            virtual_bag::payload_hash(descriptor);
        return source_error(kQolErrInternal, "extension item persist failed");
    }
    if (active_use != nullptr) {
        qol::extension_bag::ui_state().objects.module_object_use[bag][slot].consumed = true;
    }
    if (qol::extension_bag::ui_state().objects.module_view_installed &&
        qol::extension_bag::ui_state().objects.module_view_index == bag) {
        refresh_module_item_area_locked(bag);
    }
    out->ref = contracts::QolBagRef{kExtensionBagSpaceId, bag};
    out->item = expected;
    out->count = count;
    return contracts::ok();
}

QolResult source_identify(contracts::QolItemHandle item_handle, contracts::QolBagRef* out,
                          int32_t* out_slot) {
    if (out == nullptr || out_slot == nullptr) {
        return source_error(kQolErrInvalidArgument, "invalid identify output");
    }
    *out = {};
    *out_slot = -1;
    if (!source_ready()) return source_error(kQolErrNotReady, "extension bag not ready");
    ::qol::extension_bag::ReentryLock lock;
    void* item = nullptr;
    if (!extension_bag_private_item_for_handle(item_handle, &item) || item == nullptr) {
        return source_error(kQolErrNotFound, "extension item not found");
    }
    int bag = -1;
    int slot = -1;
    if (!module_slot_of_item_locked(item, &bag, &slot) || !valid_slot(bag, slot) ||
        descriptor_empty(::qol::extension_bag::access::bag().items[bag][slot])) {
        return source_error(kQolErrNotFound, "extension item not found");
    }
    out->space_id = kExtensionBagSpaceId;
    out->bag = bag;
    *out_slot = slot;
    return contracts::ok();
}

QolResult source_use(contracts::QolBagRef ref, int32_t slot) {
    if (ref.space_id != kExtensionBagSpaceId || slot < 0) {
        return source_error(kQolErrInvalidArgument, "invalid use request");
    }
    if (!source_ready()) return source_error(kQolErrNotReady, "extension bag not ready");
    if (!valid_slot(ref.bag, slot)) {
        return source_error(kQolErrInvalidArgument, "invalid extension slot");
    }
    const int logical_bag = virtual_bag::kExtensionLogicalBagFirst + ref.bag;
    const std::string body = extension_bag_api_use_item_impl(logical_bag, slot);
    if (body.rfind("{\"ok\":true", 0) == 0) {
        extension_bag_mark_dirty_locked();
        return contracts::ok();
    }
    VIRTBAG_LOG("bag source use rejected bag=%d slot=%d body=%s", ref.bag, slot, body.c_str());
    if (body.find("not in game") != std::string::npos ||
        body.find("slot data not ready") != std::string::npos) {
        return source_error(kQolErrNotReady, "extension bag not ready");
    }
    if (body.find("no space") != std::string::npos) {
        return source_error(kQolErrBlocked, "extension bag full");
    }
    if (body.find("unpack failed") != std::string::npos) {
        return source_error(kQolErrInternal, "unpack failed");
    }
    if (body.find("already in use") != std::string::npos ||
        body.find("on cooldown") != std::string::npos) {
        return source_error(kQolErrBusy, "extension item busy");
    }
    if (body.find("save busy") != std::string::npos ||
        body.find("mercenary slots full") != std::string::npos) {
        return source_error(kQolErrBlocked, "extension item blocked");
    }
    if (body.find("slot empty") != std::string::npos ||
        body.find("extension item unavailable") != std::string::npos) {
        return source_error(kQolErrNotFound, "extension item not found");
    }
    if (body.find("item not usable") != std::string::npos) {
        return source_error(kQolErrUnsupported, "extension item not usable");
    }
    if (body.find("bad extension bag") != std::string::npos ||
        body.find("bad slot") != std::string::npos ||
        body.find("slot beyond") != std::string::npos) {
        return source_error(kQolErrInvalidArgument, "invalid extension slot");
    }
    return source_error(kQolErrInternal, "extension item use failed");
}

const contracts::QolBagSource kBagSource{
    contracts::kBagSourceVersion,
    kExtensionBagSpaceId,
    &source_count,
    &source_info,
    &source_item_at,
    &source_for_each_item,
    &source_export_item,
    &source_import_item,
    &source_remove,
    &source_identify,
    &source_use,
};

}  // namespace

const contracts::QolBagSource* extension_bag_bag_source() { return &kBagSource; }

bool extension_bag_bag_source_register(
    const contracts::QolBagSourceRegistryApi* registry) {
    if (registry == nullptr || registry->register_source == nullptr) return false;
    if (g_registered) return true;
    if (!::qol::extension_bag::extension_bag_on_main_thread()) return false;
    const QolResult result = registry->register_source(&kBagSource);
    if (!contracts::succeeded(result)) return false;
    g_registry = registry;
    g_registered = true;
    return true;
}

void extension_bag_bag_source_unregister() {
    if (g_registered && g_registry != nullptr && g_registry->unregister_source != nullptr) {
        (void)g_registry->unregister_source(kExtensionBagSpaceId);
    }
    g_registered = false;
    g_registry = nullptr;
}

bool extension_bag_bag_source_registered() { return g_registered; }

}  // namespace qol::features::extension_bag

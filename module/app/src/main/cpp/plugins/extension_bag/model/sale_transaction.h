#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "contracts/extension_bag.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"

namespace qol::features::extension_bag::model {

namespace contracts = qol::contracts;

enum class SaleRemovalResult : uint8_t {
    kRemoved,
    kNotAdmitted,
    kInvalidIdentity,
    kActiveUse,
    kProjectionFailed,
    kObjectReleaseFailed,
    kRollbackFailed,
    kLedgerInvariantFailed,
    kRemovedLedgerDiagnostic,
};

struct SaleSlotState {
    int32_t logical_bag = -1;
    int32_t slot = -1;
    int32_t capacity = 0;
    bool session_admitted = false;
    bool descriptor_matches_object = false;
    bool descriptor_hash_matches = false;
    bool handle_custody_matches = false;
    contracts::QolItemHandle expected_item{};
    contracts::QolItemHandle current_item{};
    virtual_bag::Item* descriptor = nullptr;
    void** object = nullptr;
    int* object_category = nullptr;
    uint32_t* object_hash = nullptr;
    uint32_t* object_handle = nullptr;
    uint32_t ledger_handle = 0;
    ownership::Ledger* ledger = nullptr;
    bool* use_active = nullptr;
    bool* use_consumed = nullptr;
    bool* use_pending_release = nullptr;
    void** use_item = nullptr;
    uint64_t* use_generation = nullptr;
    std::thread::id* use_owner = nullptr;
    bool* dirty = nullptr;
    std::atomic<uint64_t>* revision = nullptr;
    void* projection_control = nullptr;
};

struct SaleRemovalOps {
    void* ctx = nullptr;
    bool (*clear_projection)(void* ctx, void* control, void* expected_item) = nullptr;
    bool (*restore_projection)(void* ctx, void* control, void* expected_item) = nullptr;
    // A failed release must leave the external handle registry, custody and object intact.
    bool (*release_object)(void* ctx, void* item,
                           contracts::QolItemHandle expected_item) = nullptr;
};

inline SaleRemovalResult remove_for_sale(SaleSlotState slot, const SaleRemovalOps& ops) {
    if (!slot.session_admitted) return SaleRemovalResult::kNotAdmitted;
    if (slot.logical_bag < virtual_bag::kExtensionLogicalBagFirst ||
        slot.logical_bag > virtual_bag::kExtensionLogicalBagLast || slot.slot < 0 ||
        slot.slot >= slot.capacity || slot.slot >= virtual_bag::kSlotCount || slot.descriptor == nullptr ||
        slot.object == nullptr || *slot.object == nullptr || slot.object_category == nullptr ||
        slot.object_hash == nullptr || slot.object_handle == nullptr || slot.ledger == nullptr ||
        slot.use_active == nullptr || slot.use_consumed == nullptr ||
        slot.use_pending_release == nullptr || slot.use_item == nullptr ||
        slot.use_generation == nullptr || slot.use_owner == nullptr || slot.dirty == nullptr ||
        slot.revision == nullptr || slot.descriptor->category <= 0 || slot.descriptor->count <= 0 ||
        *slot.object_category != slot.descriptor->category ||
        *slot.object_handle != slot.ledger_handle || slot.ledger_handle == 0 ||
        !slot.descriptor_matches_object || !slot.descriptor_hash_matches ||
        !slot.handle_custody_matches || slot.expected_item.slot == 0 ||
        slot.expected_item.generation == 0 || slot.expected_item.slot != slot.current_item.slot ||
        slot.expected_item.generation != slot.current_item.generation) {
        return SaleRemovalResult::kInvalidIdentity;
    }
    if (*slot.use_active || *slot.use_pending_release ||
        (*slot.use_item != nullptr && *slot.use_item != *slot.object)) {
        return SaleRemovalResult::kActiveUse;
    }
    const bool borrowed = ownership::live_state(*slot.ledger, slot.ledger_handle,
                                                ownership::State::kBorrowedForView);
    if (!borrowed && !ownership::live_state(*slot.ledger, slot.ledger_handle,
                                            ownership::State::kModuleOwned)) {
        return SaleRemovalResult::kInvalidIdentity;
    }

    void* const item = *slot.object;
    bool projection_detached = false;
    if (slot.projection_control != nullptr) {
        if (ops.clear_projection == nullptr ||
            !ops.clear_projection(ops.ctx, slot.projection_control, item)) {
            return SaleRemovalResult::kProjectionFailed;
        }
        projection_detached = true;
    }

    if (borrowed && ownership::return_from_view(slot.ledger, slot.ledger_handle) !=
                        ownership::Outcome::kOk) {
        const bool restored = !projection_detached ||
            (ops.restore_projection != nullptr &&
             ops.restore_projection(ops.ctx, slot.projection_control, item));
        return restored ? SaleRemovalResult::kLedgerInvariantFailed
                        : SaleRemovalResult::kRollbackFailed;
    }
    if (ownership::reserve_release(slot.ledger, slot.ledger_handle) != ownership::Outcome::kOk) {
        if (borrowed && ownership::borrow_for_view(slot.ledger, slot.ledger_handle) !=
                            ownership::Outcome::kOk) {
            return SaleRemovalResult::kRollbackFailed;
        }
        const bool restored = !projection_detached ||
            (ops.restore_projection != nullptr &&
             ops.restore_projection(ops.ctx, slot.projection_control, item));
        return restored ? SaleRemovalResult::kLedgerInvariantFailed
                        : SaleRemovalResult::kRollbackFailed;
    }
    if (ops.release_object == nullptr ||
        !ops.release_object(ops.ctx, item, slot.expected_item)) {
        if (ownership::cancel_reserved_release(slot.ledger, slot.ledger_handle) !=
            ownership::Outcome::kOk) {
            return SaleRemovalResult::kRollbackFailed;
        }
        if (borrowed && ownership::borrow_for_view(slot.ledger, slot.ledger_handle) !=
                            ownership::Outcome::kOk) {
            return SaleRemovalResult::kRollbackFailed;
        }
        const bool restored = !projection_detached ||
            (ops.restore_projection != nullptr &&
             ops.restore_projection(ops.ctx, slot.projection_control, item));
        return restored ? SaleRemovalResult::kObjectReleaseFailed
                        : SaleRemovalResult::kRollbackFailed;
    }
    const bool ledger_committed = ownership::commit_reserved_release(
        slot.ledger, slot.ledger_handle) == ownership::Outcome::kOk;

    *slot.descriptor = {};
    *slot.object = nullptr;
    *slot.object_category = 0;
    *slot.object_hash = 0;
    *slot.object_handle = 0;
    *slot.use_active = false;
    *slot.use_consumed = false;
    *slot.use_pending_release = false;
    *slot.use_item = nullptr;
    *slot.use_owner = {};
    ++*slot.use_generation;
    if (*slot.use_generation == 0) ++*slot.use_generation;
    *slot.dirty = true;
    slot.revision->fetch_add(1, std::memory_order_release);
    return ledger_committed ? SaleRemovalResult::kRemoved
                            : SaleRemovalResult::kRemovedLedgerDiagnostic;
}

}  // namespace qol::features::extension_bag::model

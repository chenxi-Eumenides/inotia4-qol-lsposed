#pragma once

#include <thread>
#include <atomic>
#include <cstdint>

#include "contracts/item_handle.h"
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
};

}  // namespace qol::features::extension_bag::model

#pragma once

#include "plugins/extension_bag/model/virtual_bag_state.h"

namespace qol::features::extension_bag::usecases {

enum class Error : int {
    kNone = 0,
    kDisabled,
    kNotInGame,
    kBadBag,
    kBadSlot,
    kInvalidBagCount,
    kNotInExtensionView,
    kBagNotSelected,
    kSlotBeyondCapacity,
    kBagEmpty,
    kBagAlreadySelected,
    kAlreadyInExtensionView,
    kExitInProgress,
    kInventoryPanelNotOpen,
    kEnterViewFailed,
    kExitViewFailed,
    kSelectBagFailed,
    kMovementDisabled,
    kTaskBagExcluded,
    kBadSourceBag,
    kBadDestinationBag,
    kUseInventoryMove,
    kMoveFailed,
    kTransactionPending,
    kDragActive,
    kBagNotEmpty,
    kInactiveBagHasRuntimeData,
    kBagCountChangeRejected,
    kPersistFailed,
    kNotEquipped,
    kNoSpace,
    kUnequipFailed,
};

inline constexpr const char* error_text(Error error) {
    switch (error) {
        case Error::kNone: return "";
        case Error::kDisabled: return "extension bag disabled";
        case Error::kNotInGame: return "not in game";
        case Error::kBadBag: return "bad extension bag (6-10)";
        case Error::kBadSlot: return "bad slot";
        case Error::kInvalidBagCount: return "invalid bag count";
        case Error::kNotInExtensionView: return "not in extension view";
        case Error::kBagNotSelected: return "bag not selected";
        case Error::kSlotBeyondCapacity: return "slot beyond derived capacity";
        case Error::kBagEmpty: return "slot empty";
        case Error::kBagAlreadySelected: return "bag already selected";
        case Error::kAlreadyInExtensionView: return "already in extension view";
        case Error::kExitInProgress: return "exit in progress";
        case Error::kInventoryPanelNotOpen: return "inventory panel not open";
        case Error::kEnterViewFailed: return "enter extension view failed";
        case Error::kExitViewFailed: return "exit extension view failed";
        case Error::kSelectBagFailed: return "select bag failed";
        case Error::kMovementDisabled: return "extension view open; movement disabled (P2)";
        case Error::kTaskBagExcluded: return "task bag excluded";
        case Error::kBadSourceBag: return "bad from bag";
        case Error::kBadDestinationBag: return "bad to bag";
        case Error::kUseInventoryMove: return "use /api/item/inventory/move_item";
        case Error::kMoveFailed: return "move failed";
        case Error::kTransactionPending: return "transaction pending";
        case Error::kDragActive: return "drag active";
        case Error::kBagNotEmpty: return "bag not empty";
        case Error::kInactiveBagHasRuntimeData: return "inactive bag has runtime data";
        case Error::kBagCountChangeRejected: return "bag count change rejected";
        case Error::kPersistFailed: return "persist failed";
        case Error::kNotEquipped: return "bag not equipped";
        case Error::kNoSpace: return "no space";
        case Error::kUnequipFailed: return "unequip failed";
    }
    return "unknown error";
}

struct OpResult {
    bool ok = false;
    Error error = Error::kNone;
};

struct MoveResult {
    bool ok = false;
    Error error = Error::kNone;
};

struct SetBagCountResult {
    bool ok = false;
    Error error = Error::kNone;
    int count = 0;
};

struct ClickItemResult {
    bool ok = false;
    Error error = Error::kNone;
    bool has_item = false;
    int logical_bag = -1;
    int slot = -1;
    int effective_count = 0;
    virtual_bag::Item item{};
};

inline constexpr OpResult success() { return {true, Error::kNone}; }
inline constexpr OpResult failure(Error error) { return {false, error}; }

}  // namespace qol::features::extension_bag::usecases

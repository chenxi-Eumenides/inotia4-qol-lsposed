#pragma once

#include <cstdint>

namespace qol::contracts {

// Authoritative item classification returned by item.facts.item_type.
// Consumers dispatch by this enum only; category and game predicates are
// implementation details of the item.facts provider.
enum class QolItemType : int32_t {
    kUnknown = 0,
    // Other use-effect item not covered by a more specific type below. Its
    // native path is still CHAR_UseItemEx.
    kConsumable = 1,
    kEquipment = 2,
    kBackpack = 3,
    kJewel = 4,
    kEnchantScroll = 5,
    kMercenarySeal = 6,
    kSkillBook = 7,
    kItemBox = 8,
    kSynthSet = 9,
    kPotion = 10,
    kDice = 11,
    kReviveScroll = 12,
    kLamp = 13,
    kUseAfterConfirm = 14,
    kOther = 15,
};

// Native use paths:
// - kPotion/kReviveScroll/kLamp/kSkillBook/kSynthSet/kConsumable: CHAR_UseItemEx.
//   The game dispatches internally by category/use-effect; callers share one entry.
// - kDice: STATUSDICE_Roll + pending state.
// - kUseAfterConfirm: UI confirmation, then CHAR_UseItemEx.
// - kMercenarySeal: MERCENARYSYSTEM_MakeMercenary.
// - kItemBox: ITEMSYSTEM_OpenItemBox or ITEMSYSTEM_ProcessUnpack.
// - kEquipment/kBackpack/kJewel/kEnchantScroll: equip/apply, not use.

}  // namespace qol::contracts

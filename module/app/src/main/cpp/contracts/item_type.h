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
    // monster 改版宝石拆卸器（永久）：拖到装备上走 UIEquip_ApplyStuff 的
    // category 0x3fa 专属分支，取出已镶宝石且不消耗自身。
    kGemRemover = 16,
};

// Native use paths:
// - kPotion/kReviveScroll/kLamp/kSkillBook/kSynthSet/kConsumable: CHAR_UseItemEx.
//   The game dispatches internally by category/use-effect; callers share one entry.
// - kDice: STATUSDICE_Roll + pending state.
// - kUseAfterConfirm: UI confirmation, then CHAR_UseItemEx.
// - kMercenarySeal: MERCENARYSYSTEM_MakeMercenary.
// - kItemBox: ITEMSYSTEM_OpenItemBox or ITEMSYSTEM_ProcessUnpack.
// - kEquipment/kBackpack/kJewel/kEnchantScroll/kGemRemover: equip/apply, not use.
//   kGemRemover 只存在于 monster 变体，apply 成功后材料不消耗。

}  // namespace qol::contracts

#pragma once

#include <cstdint>

namespace qol::contracts {

// Authoritative item classification returned by item.facts.item_type.
// Consumers dispatch by this enum only; category and game predicates are
// implementation details of the item.facts provider.
enum class QolItemType : int32_t {
    kUnknown = 0,
    kConsumable = 1,
    kEquipment = 2,
    kBackpack = 3,
    kJewel = 4,
    kEnchantScroll = 5,
    kMercenarySeal = 6,
    kSkillBook = 7,
    kItemBox = 8,
    kSynthSet = 9,
    kOther = 10,
};

}  // namespace qol::contracts

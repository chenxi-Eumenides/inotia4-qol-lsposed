#include "item_handle_guard.h"

#include "../model/item_identity.h"

namespace qol::features::core_item::bags {

QolResult recheck_expected_handle(const contracts::QolItemHandle& expected,
                                  const contracts::QolItemHandle& current) {
    if (model::expected_handle_matches(expected, current)) return contracts::ok();
    constexpr char kMessage[] = "stale item handle";
    return contracts::error(kQolErrNotFound, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

}  // namespace qol::features::core_item::bags

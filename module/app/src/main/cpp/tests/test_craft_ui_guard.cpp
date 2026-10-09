#include <cassert>

#include "plugins/craft_ui/feature.h"

int main() {
    using qol::features::craft_ui::craft_result_unavailable_is_fail_closed;
    using qol::features::craft_ui::CraftPlaceDisposition;
    using qol::features::craft_ui::craft_place_disposition;
    using qol::contracts::PlaceOutcome;
    assert(craft_result_unavailable_is_fail_closed(true));
    assert(!craft_result_unavailable_is_fail_closed(false));
    assert(craft_place_disposition(PlaceOutcome::kWriteFailed) ==
           CraftPlaceDisposition::kWriteFailed);
    assert(craft_place_disposition(PlaceOutcome::kWriteFailed) !=
           CraftPlaceDisposition::kAdvance);
    assert(craft_place_disposition(PlaceOutcome::kWriteFailed) !=
           CraftPlaceDisposition::kStop);
    assert(craft_place_disposition(PlaceOutcome::kPlaced) == CraftPlaceDisposition::kAdvance);
}

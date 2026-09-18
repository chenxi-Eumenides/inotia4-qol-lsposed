#include <cassert>

#include "plugins/craft_ui/feature.h"

int main() {
    using qol::features::craft_ui::craft_result_unavailable_is_fail_closed;
    assert(craft_result_unavailable_is_fail_closed(true));
    assert(!craft_result_unavailable_is_fail_closed(false));
    return 0;
}

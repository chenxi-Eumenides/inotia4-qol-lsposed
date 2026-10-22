#include "settings_contribution.h"

namespace qol::features::craft_lottery {
// Settings metadata comes exclusively from descriptor.features; this compatibility entry registers nothing.
QolResult register_settings(const contracts::QolHostSdk*) { return contracts::ok(); }
}  // namespace qol::features::craft_lottery

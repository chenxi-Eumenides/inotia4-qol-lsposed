#include "settings_contribution.h"

namespace qol::features::core_settings {
// Legacy assembly calls may remain during integration. Metadata now comes exclusively
// from descriptor.features; neither entry registers or writes the old catalog.
QolResult register_settings(const contracts::QolHostSdk*) { return contracts::ok(); }
QolResult register_categories(const contracts::QolHostSdk*) { return contracts::ok(); }
}  // namespace qol::features::core_settings

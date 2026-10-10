#include "settings_contribution.h"

namespace qol::features::move_merge {
// Settings metadata now comes exclusively from descriptor.features; the legacy catalog
// contribution is retired and registers nothing.
QolResult register_settings(const contracts::QolHostSdk*) { return contracts::ok(); }
}  // namespace qol::features::move_merge

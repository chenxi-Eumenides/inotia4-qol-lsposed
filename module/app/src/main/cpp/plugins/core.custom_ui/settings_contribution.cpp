#include "settings_contribution.h"

namespace qol::features::core_ui {
// The debug entry is now the descriptor feature `core.custom_ui.open_debug_components`.
// Settings metadata comes exclusively from descriptor.features; the legacy catalog
// category/item contributions are retired and register nothing.
QolResult register_settings_category(const contracts::QolHostSdk*) { return contracts::ok(); }
QolResult register_settings(const contracts::QolHostSdk*) { return contracts::ok(); }
}  // namespace qol::features::core_ui

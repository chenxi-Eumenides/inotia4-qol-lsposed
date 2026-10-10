#include "settings_contribution.h"

namespace qol::features::save_backup {
// The open-manager action is now the descriptor entry feature `save_backup.open_manager`;
// the legacy settings catalog contribution is retired and registers nothing.
QolResult register_settings(const contracts::QolHostSdk*) { return contracts::ok(); }
}  // namespace qol::features::save_backup

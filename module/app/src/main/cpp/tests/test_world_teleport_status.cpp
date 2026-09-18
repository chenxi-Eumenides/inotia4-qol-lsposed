#include <cassert>
#include <cstring>

#include "plugins/world_teleport/feature.h"

int main() {
    using qol::features::world_teleport::InstallDisposition;
    using qol::features::world_teleport::install_error;
    using qol::features::world_teleport::install_succeeded;

    assert(install_succeeded(InstallDisposition::kInstalled));
    assert(!install_succeeded(InstallDisposition::kUnsupported));
    assert(!install_succeeded(InstallDisposition::kFailed));
    assert(std::strcmp(install_error(InstallDisposition::kUnsupported),
                       "unsupported: world teleport entry instruction fingerprint") == 0);
    assert(std::strcmp(install_error(InstallDisposition::kFailed),
                       "install failed: world teleport entry patch unavailable") == 0);
    assert(install_error(InstallDisposition::kInstalled) == nullptr);
    return 0;
}

#pragma once

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

using QolItemDiceRejectFn = QolResult (*)();

struct QolItemDiceWritePort {
    QolVersion abi_version;
    QolItemDiceRejectFn reject;
};

constexpr QolVersion kQolPortItemDiceWriteVersion{1, 0};

}  // namespace qol::contracts

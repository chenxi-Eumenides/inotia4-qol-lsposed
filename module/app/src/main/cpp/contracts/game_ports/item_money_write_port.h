#pragma once

#include <cstdint>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

using QolItemMoneyWriteFn = QolResult (*)(int64_t money);

struct QolItemMoneyWritePort {
    QolVersion abi_version;
    QolItemMoneyWriteFn set_money;
};

constexpr QolVersion kQolPortItemMoneyWriteVersion{1, 0};

}  // namespace qol::contracts

#pragma once

// 调用方先引入固定宽度整数类型；本头不依赖任何其他契约。
namespace qol::contracts {
using QolOwnerToken = uint64_t;
constexpr QolOwnerToken kQolOwnerTokenInvalid = 0;
}  // namespace qol::contracts

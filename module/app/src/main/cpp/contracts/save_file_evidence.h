#pragma once
#include <cstdint>
namespace qol::contracts {
struct QolSaveFileEvidence {
    uint64_t file_length;
    uint8_t sha256[32];
};
}  // namespace qol::contracts

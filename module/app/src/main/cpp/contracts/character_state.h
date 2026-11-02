#pragma once

#include <cstdint>
#include <type_traits>

#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kCharacterStateCapabilityId = "character.state";
constexpr QolVersion kCharacterStateVersion{1, 0};

// Character owners assemble their domain projection. Cross-domain JSON may only be filtered or
// reordered; its fields must not be reassembled by another owner.
enum class QolCharacterStateKind : int32_t {
    kParty = 0,
    kPlayer = 1,
    kSkills = 2,
};

// The buffer contains the owner-assembled, byte-compatible JSON projection for the fixed kind.
using QolCharacterStateWriteJsonFn = QolResult (*)(QolCharacterStateKind kind, char* out,
                                                   uint32_t capacity, uint32_t* out_size);

struct CharacterStateApi {
    QolCharacterStateWriteJsonFn write_state_json;
};

static_assert(std::is_standard_layout<CharacterStateApi>::value,
              "CharacterStateApi must remain standard layout");
static_assert(std::is_trivially_copyable<CharacterStateApi>::value,
              "CharacterStateApi must remain trivially copyable");

}  // namespace qol::contracts

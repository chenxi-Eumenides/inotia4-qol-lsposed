#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

struct QolQuestReadOnlyApi {
    QolResult (*state)(int32_t* out_state);
    QolResult (*active_slot_count)(uint32_t* out_count);
    QolResult (*active_slot_id)(uint32_t index, uint32_t* out_id);
    QolResult (*total_count)(uint32_t* out_count);
    QolResult (*quest_state)(uint32_t quest_id, int32_t* out_state);
    QolResult (*is_complete)(uint32_t quest_id, int32_t* out_complete);
};

struct QolQuestWriteApi {
    QolResult (*set_state)(uint32_t quest_id, int32_t state);
};

struct QolQuestPort {
    QolVersion abi_version;
    QolQuestReadOnlyApi read;
    QolQuestWriteApi write;
};

static_assert(std::is_standard_layout<QolQuestPort>::value,
              "QolQuestPort must remain standard layout");
static_assert(std::is_trivially_copyable<QolQuestPort>::value,
              "QolQuestPort must remain trivially copyable");

}  // namespace qol::contracts

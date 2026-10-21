#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

struct QolChoiceReadApi {
    QolResult (*state_id)(int32_t* out_state_id);
    QolResult (*selected_index)(int32_t* out_index);
    QolResult (*text_owns)(const char* const* items, uint32_t count, int32_t* out_owned);
};

struct QolChoiceWriteApi {
    QolResult (*write_items)(const char* const* items, uint32_t count);
    QolResult (*write_title)(const char* title);
    QolResult (*write_focus)(int32_t index);
    QolResult (*push)();
    QolResult (*close_popup)();
    QolResult (*play_sound)(int32_t sound_id);
};

// v1.1 tail: primitives for the module-choice first-frame de-dim draw hook.
// The wrapper owns the sequence; each slot maps to one game primitive so the
// order (save -> clear flag -> GRPX start -> UIChoice_Draw -> GRPX end) stays
// testable without the game image.
struct QolChoiceDrawApi {
    QolResult (*refresh_flag)(uint8_t* out_flag);
    QolResult (*save_lcd)();
    QolResult (*clear_refresh_flag)();
    QolResult (*start_frame)();
    QolResult (*draw_choice)();
    QolResult (*end_frame)();
};

struct QolChoicePort {
    QolVersion abi_version;
    QolChoiceReadApi read;
    QolChoiceWriteApi write;
    QolChoiceDrawApi draw;
};

static_assert(std::is_standard_layout<QolChoicePort>::value,
              "QolChoicePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolChoicePort>::value,
              "QolChoicePort must remain trivially copyable");

}  // namespace qol::contracts

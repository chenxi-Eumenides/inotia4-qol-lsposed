#include "game/adapter_ports/choice_port.h"

#include <cstring>

#include "game/access/ui_choice.h"
#include "contracts/game_ports/ports.h"

namespace qol::game {
namespace {

using QolResult = ::QolResult;

[[gnu::used]] constexpr char kChoicePortBuildMarker[] = "choice_port:v1";

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult not_ready() { return error_result(kQolErrNotReady, "not ready"); }

QolResult read_state_id(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    const int value = ui_choice_state_id();
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_selected_index(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    return ui_choice_selected_index(out) ? contracts::ok() : not_ready();
}

QolResult read_text_owns(const char* const* items, uint32_t count, int32_t* out) {
    if (items == nullptr || out == nullptr || count == 0 || count > 6) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    *out = ui_choice_text_owns(items, count) ? 1 : 0;
    return contracts::ok();
}

QolResult write_items(const char* const* items, uint32_t count) {
    if (items == nullptr || count == 0 || count > 6) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    return ui_choice_write_items(items, count) ? contracts::ok() : not_ready();
}

QolResult write_title(const char* title) {
    if (title == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    return ui_choice_write_title(title) ? contracts::ok() : not_ready();
}

QolResult write_focus(int32_t index) {
    if (index < 0 || index > 255) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    return ui_choice_write_focus(index) ? contracts::ok() : not_ready();
}

QolResult push() { return ui_choice_push() ? contracts::ok() : not_ready(); }

QolResult close_popup() { return ui_choice_close_popup() ? contracts::ok() : not_ready(); }

QolResult play_sound(int32_t sound_id) {
    return ui_choice_play_sound(sound_id) ? contracts::ok() : not_ready();
}

const contracts::QolChoicePort kChoicePort{
    contracts::kQolPortChoiceVersion,
    {&read_state_id, &read_selected_index, &read_text_owns},
    {&write_items, &write_title, &write_focus, &push, &close_popup, &play_sound},
};

}  // namespace

const contracts::QolChoicePort& choice_port() { return kChoicePort; }

}  // namespace qol::game

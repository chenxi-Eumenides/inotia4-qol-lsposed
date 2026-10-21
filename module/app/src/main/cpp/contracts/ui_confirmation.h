#pragma once

#include <cstdint>

#include "result.h"

// Shared confirmation dialogs are owned by core.custom_ui. A dialog is opened only
// when no UIPopupMsg is active; callbacks run on the game main thread.
namespace qol::contracts {

constexpr const char* kUiConfirmationCapabilityId = "ui_confirmation";
constexpr int32_t kUiConfirmationVersionMajor = 1;
constexpr int32_t kUiConfirmationVersionMinor = 0;

using UiConfirmationCallback = void (*)(void* user);

struct UiConfirmationSpec {
    uint32_t text_id;       // TEXTDATABASE text id.
    uint32_t display_type;  // Native UIPopupMsg display type.
    uint32_t format_arg;   // Native text formatting argument.
    uint32_t price;        // 0..INT32_MAX; numeric native callback param, never a user pointer.
    UiConfirmationCallback on_confirm;
    UiConfirmationCallback on_cancel;
    void* user;             // Held by core.custom_ui; must live through callback or successful close.
};

struct UiConfirmationApi {
    // out_token is nonzero on success. On failure it is normally zero, but is nonzero when
    // failed-open cleanup retains owner state for retry; close it with that token.
    QolResult (*open)(const UiConfirmationSpec* spec, uint64_t* out_token);
    QolResult (*active)(uint64_t token, uint8_t* out_active);
    QolResult (*close)(uint64_t token);
};

}  // namespace qol::contracts

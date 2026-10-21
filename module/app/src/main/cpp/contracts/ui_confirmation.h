#pragma once

#include <cstdint>

#include "result.h"

// Shared confirmation dialogs are owned by core.custom_ui. A dialog is opened only
// when no UIPopupMsg is active; callbacks run on the game main thread.
namespace qol::contracts {

constexpr const char* kUiConfirmationCapabilityId = "ui_confirmation";
constexpr int32_t kUiConfirmationVersionMajor = 1;
constexpr int32_t kUiConfirmationVersionMinor = 1;

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

// v1.1 size-versioned cross-backend spec. Strings are NUL-terminated UTF-8 fixed arrays;
// an empty message selects TextData fallback only on native screens.
struct UiConfirmationSpecV2 {
    uint32_t size;
    uint32_t text_id;
    uint32_t display_type;
    uint32_t format_arg;
    uint32_t price;
    UiConfirmationCallback on_confirm;
    UiConfirmationCallback on_cancel;
    void* user;
    char message[256];
    char confirm_label[64];
    char cancel_label[64];
    char price_unit[64];
};

struct UiConfirmationApi {
    // out_token is nonzero on success. On failure it is normally zero, but is nonzero when
    // failed-open cleanup retains owner state for retry; close it with that token.
    QolResult (*open)(const UiConfirmationSpec* spec, uint64_t* out_token);
    QolResult (*active)(uint64_t token, uint8_t* out_active);
    QolResult (*close)(uint64_t token);
    // v1.1 tail: selects the backend from the live custom-panel stack.
    QolResult (*open_v2)(const UiConfirmationSpecV2* spec, uint64_t* out_token);
};

}  // namespace qol::contracts

#pragma once

#include "../../contracts/ui.h"
#include "../../contracts/ui_confirmation.h"
#include "../../contracts/game_host.h"
#include "../../contracts/host_sdk.h"

namespace qol::features::core_ui {

bool ui_confirmation_bind(const contracts::QolUiAdapter* adapter,
                          const contracts::QolGameHost* game = nullptr,
                          const contracts::QolHostSdk* sdk = nullptr);
void ui_confirmation_unbind();
bool ui_confirmation_set_installed(bool installed);
QolResult ui_confirmation_close_owned();
const contracts::UiConfirmationApi* ui_confirmation_api();

// Internal dual-backend support. These declarations are not part of the capability contract.
bool ui_confirmation_register_frame_overlay(contracts::QolUiFrameOverlayDrawFn draw, void* user);
bool ui_confirmation_register_builtin_frame_overlay();
bool ui_confirmation_modal_overlay_token(uint64_t* out_token);
bool ui_confirmation_modal_input_active();
bool ui_confirmation_consume_return_suppression();
void ui_confirmation_overlay_draw_succeeded(uint64_t token);
bool ui_confirmation_cancel_for_return();
uint32_t ui_confirmation_dispatch_custom_event(uint64_t event, uint64_t param, uint64_t param2);
void ui_confirmation_overlay_scope_finished(uint64_t token);
uint64_t ui_confirmation_overlay_scope_begin(uint64_t token);
bool ui_confirmation_overlay_scope_finish(uint64_t scope_id);
void ui_confirmation_overlay_scope_release(uint64_t scope_id);
bool ui_confirmation_overlay_scope_finish_current(uint64_t token);

}  // namespace qol::features::core_ui

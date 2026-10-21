#pragma once

#include "../../contracts/ui.h"
#include "../../contracts/ui_confirmation.h"

namespace qol::features::core_ui {

bool ui_confirmation_bind(const contracts::QolUiAdapter* adapter);
void ui_confirmation_unbind();
void ui_confirmation_set_installed(bool installed);
QolResult ui_confirmation_close_owned();
const contracts::UiConfirmationApi* ui_confirmation_api();

}  // namespace qol::features::core_ui

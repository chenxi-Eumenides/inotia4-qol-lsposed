#pragma once

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"

namespace qol::host {

extern const contracts::QolHostSdk g_host_sdk;
extern const contracts::QolGameHost g_game_host;

void game_host_mark_main_thread();

}  // namespace qol::host

#pragma once

#include "../../../contracts/host_sdk.h"
#include "../../../contracts/http.h"

namespace qol::features::core_ui {
QolResult debug_help_http_command(const contracts::HttpReq* request,
                                  contracts::HttpRes* response, void* ctx);
bool debug_help_register(const contracts::QolHostSdk* sdk);
void debug_help_unregister(const contracts::QolHostSdk* sdk);
void debug_help_set_enabled(bool enabled);
}  // namespace qol::features::core_ui

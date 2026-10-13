#pragma once

#include "../../../contracts/host_sdk.h"
#include "../../../contracts/http.h"

namespace qol::features::core_ui {

QolResult debug_components_http_command(const contracts::HttpReq* request,
                                        contracts::HttpRes* response, void* ctx);
bool debug_components_register(const contracts::QolHostSdk* sdk);
void debug_components_unregister(const contracts::QolHostSdk* sdk);
void debug_components_set_enabled(bool enabled);

}  // namespace qol::features::core_ui

#pragma once
#include "contracts/http.h"
namespace qol::host {
// Resident config/lifecycle routes bypass plugin registry and owner leases.
bool feature_control_http_route(const contracts::HttpReq&);
QolResult feature_control_http_dispatch(const contracts::HttpReq&, contracts::HttpRes&);
}  // namespace qol::host

#pragma once

#include <cstddef>

#include "contracts/host_sdk.h"

namespace qol::bridge {

inline QolResult read_module_health_snapshot(
    const void* provider_iface, const contracts::QolHostSdk* resident_sdk,
    contracts::QolModuleHealthSnapshot* out) {
    if (out == nullptr) return contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out = {};
    out->struct_size = sizeof(*out);

    const auto* const provider_api = provider_iface == nullptr
        ? nullptr
        : static_cast<const contracts::QolHostModuleHealthFactsApi*>(provider_iface);
    const auto* const resident_api = contracts::module_health_if_available(resident_sdk);
    const auto* const selected_api = provider_api != nullptr ? provider_api : resident_api;
    if (selected_api == nullptr) return contracts::ok();

    constexpr uint32_t required_api_size =
        offsetof(contracts::QolHostModuleHealthFactsApi, read) +
        sizeof(contracts::QolHostModuleHealthFactsApi::read);
    const auto read_api = [required_api_size](
                              const contracts::QolHostModuleHealthFactsApi* api,
                              contracts::QolModuleHealthSnapshot* snapshot) {
        if (api == nullptr || api->struct_size < required_api_size || api->read == nullptr)
            return contracts::error(kQolErrNotReady, nullptr, 0);
        return api->read(api->context, snapshot);
    };

    QolResult result = read_api(selected_api, out);
    if (!contracts::succeeded(result) && selected_api != resident_api && resident_api != nullptr) {
        *out = {};
        out->struct_size = sizeof(*out);
        result = read_api(resident_api, out);
    }
    return result;
}

}  // namespace qol::bridge

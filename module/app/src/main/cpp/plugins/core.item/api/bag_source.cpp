#include "bag_source.h"

#include "contracts/host_sdk.h"

namespace qol::features::core_item {
namespace {

const contracts::QolHostSdk* g_bag_source_sdk = nullptr;
const contracts::QolBagSourceRegistryApi* g_bag_source_registry = nullptr;

const contracts::QolBagSourceRegistryApi* registry() {
    if (g_bag_source_registry != nullptr) return g_bag_source_registry;
    if (g_bag_source_sdk == nullptr || g_bag_source_sdk->capabilities.require == nullptr) return nullptr;

    const void* iface = nullptr;
    const QolResult result = g_bag_source_sdk->capabilities.require(
        contracts::kBagSourceCapabilityId,
        {contracts::kBagSourceVersion.major, contracts::kBagSourceVersion.minor}, &iface);
    if (!contracts::succeeded(result) || iface == nullptr) return nullptr;

    const auto* candidate =
        static_cast<const contracts::QolBagSourceRegistryApi*>(iface);
    if (!contracts::qol_bag_source_registry_api_valid(candidate)) return nullptr;
    g_bag_source_registry = candidate;
    return g_bag_source_registry;
}

}  // namespace

void bind_bag_source_sdk(const contracts::QolHostSdk* sdk) {
    g_bag_source_sdk = sdk;
    g_bag_source_registry = nullptr;
}

void bind_bag_source_registry_for_test(const contracts::QolBagSourceRegistryApi* api) {
    g_bag_source_registry = api;
}

const contracts::QolBagSource* find_bag_source(int32_t space_id) {
    const auto* api = registry();
    if (api == nullptr || api->source_count == nullptr || api->source_at == nullptr) {
        return nullptr;
    }
    const uint32_t count = api->source_count();
    for (uint32_t index = 0; index < count; ++index) {
        const contracts::QolBagSource* source = nullptr;
        if (!contracts::succeeded(api->source_at(index, &source)) || source == nullptr) continue;
        if (source->space_id == space_id) return source;
    }
    return nullptr;
}

uint32_t registered_bag_source_count() {
    const auto* api = registry();
    return api != nullptr && api->source_count != nullptr ? api->source_count() : 0;
}

const contracts::QolBagSource* registered_bag_source_at(uint32_t index) {
    const auto* api = registry();
    if (api == nullptr || api->source_at == nullptr) return nullptr;
    const contracts::QolBagSource* source = nullptr;
    if (!contracts::succeeded(api->source_at(index, &source))) return nullptr;
    return source;
}

void reset_bag_sources() {
    g_bag_source_sdk = nullptr;
    g_bag_source_registry = nullptr;
}

}  // namespace qol::features::core_item

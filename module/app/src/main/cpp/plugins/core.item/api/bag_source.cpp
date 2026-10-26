#include "plugins/core.item/api/bag_source.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "plugins/core.item/runtime/core_item_internal.h"

namespace qol::features::core_item {
namespace {

std::vector<const contracts::QolBagSource*> g_bag_sources;

QolResult registry_error(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult register_source(const contracts::QolBagSource* source) {
    if (!contracts::qol_bag_source_valid(source) ||
        source->space_id < contracts::kQolBagSpaceFirstRegistered) {
        return registry_error(kQolErrInvalidArgument, "invalid bag source");
    }
    if (find_bag_source(source->space_id) != nullptr) {
        return registry_error(kQolErrConflict, "bag source exists");
    }
    g_bag_sources.push_back(source);
    return contracts::ok();
}

QolResult unregister_source(int32_t space_id) {
    const auto found = std::find_if(
        g_bag_sources.begin(), g_bag_sources.end(), [space_id](const auto* source) {
            return source != nullptr && source->space_id == space_id;
        });
    if (found == g_bag_sources.end()) {
        return registry_error(kQolErrNotFound, "bag source not found");
    }
    g_bag_sources.erase(found);
    return contracts::ok();
}

uint32_t source_count() { return registered_bag_source_count(); }

QolResult source_at(uint32_t index, const contracts::QolBagSource** out_source) {
    if (out_source == nullptr) {
        return registry_error(kQolErrInvalidArgument, "invalid bag source output");
    }
    *out_source = nullptr;
    const contracts::QolBagSource* source = registered_bag_source_at(index);
    if (source == nullptr) {
        return registry_error(kQolErrNotFound, "bag source not found");
    }
    *out_source = source;
    return contracts::ok();
}

}  // namespace

const contracts::QolBagSourceRegistryApi kBagSourceRegistryApi{
    sizeof(contracts::QolBagSourceRegistryApi), &register_source, &unregister_source,
    &source_count, &source_at};

const contracts::QolBagSource* find_bag_source(int32_t space_id) {
    for (const contracts::QolBagSource* source : g_bag_sources) {
        if (source != nullptr && source->space_id == space_id) return source;
    }
    return nullptr;
}

uint32_t registered_bag_source_count() {
    return static_cast<uint32_t>(g_bag_sources.size());
}

const contracts::QolBagSource* registered_bag_source_at(uint32_t index) {
    return index < g_bag_sources.size() ? g_bag_sources[index] : nullptr;
}

void reset_bag_sources() { g_bag_sources.clear(); }

}  // namespace qol::features::core_item

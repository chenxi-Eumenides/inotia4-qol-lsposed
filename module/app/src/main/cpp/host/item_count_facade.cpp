#include "host/item_count_facade.h"

#include <cstring>

#include "contracts/result.h"

namespace qol::host {
namespace {

const contracts::QolItemCountApi* g_provider = nullptr;

QolResult not_ready() {
    static constexpr char kMessage[] = "not ready";
    return contracts::error(kQolErrNotReady, kMessage, static_cast<uint32_t>(std::strlen(kMessage)));
}

QolResult invalid_argument() {
    static constexpr char kMessage[] = "invalid argument";
    return contracts::error(kQolErrInvalidArgument, kMessage,
                            static_cast<uint32_t>(std::strlen(kMessage)));
}

QolResult canonical(const void* item, uint32_t* out_count) {
    if (item == nullptr || out_count == nullptr) return invalid_argument();
    if (g_provider == nullptr) return not_ready();
    return g_provider->canonical == nullptr ? not_ready() : g_provider->canonical(item, out_count);
}

QolResult effective(const void* item, uint32_t* out_count) {
    if (item == nullptr || out_count == nullptr) return invalid_argument();
    if (g_provider == nullptr) return not_ready();
    return g_provider->effective == nullptr ? not_ready() : g_provider->effective(item, out_count);
}

QolResult is_encoded(const void* item, contracts::QolItemCountEncoding* out_encoding) {
    if (item == nullptr || out_encoding == nullptr) return invalid_argument();
    if (g_provider == nullptr) return not_ready();
    return g_provider->is_encoded == nullptr ? not_ready()
                                               : g_provider->is_encoded(item, out_encoding);
}

QolResult write_effective(void* item, uint32_t count) {
    if (item == nullptr) return invalid_argument();
    if (g_provider == nullptr) return not_ready();
    return g_provider->write_effective == nullptr ? not_ready()
                                                    : g_provider->write_effective(item, count);
}

QolResult writeback_needed(const void* item, uint32_t count, int32_t* out_needed) {
    if (item == nullptr || out_needed == nullptr) return invalid_argument();
    if (g_provider == nullptr) return not_ready();
    return g_provider->writeback_needed == nullptr
               ? not_ready()
               : g_provider->writeback_needed(item, count, out_needed);
}

const contracts::QolItemCountApi kFacade = {
    &canonical,
    &effective,
    &is_encoded,
    &write_effective,
    &writeback_needed,
};

}  // namespace

void host_item_count_set_provider(const contracts::QolItemCountApi* api) { g_provider = api; }

const contracts::QolItemCountApi& host_item_count_api() { return kFacade; }

const contracts::QolItemCountApi* host_item_count_api_if_resident() {
    return g_provider == nullptr ? nullptr : &kFacade;
}

}  // namespace qol::host

namespace qol::contracts {

const QolItemCountApi* item_count_api() { return host::host_item_count_api_if_resident(); }

}  // namespace qol::contracts

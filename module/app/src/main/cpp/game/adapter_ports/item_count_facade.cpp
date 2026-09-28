#include "game/adapter_ports/item_count_facade.h"

#include <cstring>

#include "contracts/result.h"

namespace qol::game {
namespace {

const contracts::QolItemCountApi* g_provider = nullptr;
ItemCountActiveProbe g_active_probe = nullptr;

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

QolResult field_from_canonical(uint32_t field, uint32_t canonical_count, int32_t extended,
                               uint32_t* out_field) {
    if (out_field == nullptr) return invalid_argument();
    if (g_provider == nullptr) return not_ready();
    return g_provider->field_from_canonical == nullptr
               ? not_ready()
               : g_provider->field_from_canonical(field, canonical_count, extended, out_field);
}

QolResult canonical_from_field(uint32_t field, uint32_t* out_canonical) {
    if (out_canonical == nullptr) return invalid_argument();
    if (g_provider == nullptr) return not_ready();
    return g_provider->canonical_from_field == nullptr
               ? not_ready()
               : g_provider->canonical_from_field(field, out_canonical);
}

QolResult view_from_canonical(uint32_t canonical_count, int32_t extended, uint32_t* out_view) {
    if (out_view == nullptr) return invalid_argument();
    if (g_provider == nullptr) return not_ready();
    return g_provider->view_from_canonical == nullptr
               ? not_ready()
               : g_provider->view_from_canonical(canonical_count, extended, out_view);
}

QolResult view_limit(int32_t extended, uint32_t* out_limit) {
    if (out_limit == nullptr) return invalid_argument();
    if (g_provider == nullptr) return not_ready();
    return g_provider->view_limit == nullptr ? not_ready()
                                              : g_provider->view_limit(extended, out_limit);
}

QolResult read_encoded_field(const void* item, uint32_t* out_field) {
    if (item == nullptr || out_field == nullptr) return invalid_argument();
    if (g_provider == nullptr) return not_ready();
    return g_provider->read_encoded_field == nullptr ? not_ready()
                                                       : g_provider->read_encoded_field(item, out_field);
}

QolResult write_encoded_field(void* item, uint32_t field) {
    if (item == nullptr) return invalid_argument();
    if (g_provider == nullptr) return not_ready();
    return g_provider->write_encoded_field == nullptr
               ? not_ready()
               : g_provider->write_encoded_field(item, field);
}

const contracts::QolItemCountApi kFacade = {
    &canonical,
    &effective,
    &is_encoded,
    &write_effective,
    &writeback_needed,
    &field_from_canonical,
    &canonical_from_field,
    &view_from_canonical,
    &view_limit,
    &read_encoded_field,
    &write_encoded_field,
};

}  // namespace

void item_count_set_provider(const contracts::QolItemCountApi* api) { g_provider = api; }

void item_count_set_active_probe(ItemCountActiveProbe probe) {
    if (g_active_probe == nullptr && probe != nullptr) g_active_probe = probe;
}

bool item_count_active() {
    return g_provider != nullptr && g_active_probe != nullptr && g_active_probe() != 0;
}

const contracts::QolItemCountApi& item_count_api() { return kFacade; }

const contracts::QolItemCountApi* item_count_api_if_resident() {
    return g_provider == nullptr ? nullptr : &kFacade;
}

}  // namespace qol::game

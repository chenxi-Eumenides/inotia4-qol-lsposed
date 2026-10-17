#include "game/adapter_ports/item_count_facade.h"

#include <atomic>
#include <cstring>

#include "contracts/result.h"
#include "game/access/patches.h"
#include "game/adapter_ports/item_count_codec.h"

namespace qol::game {
namespace {

namespace codec = qol::game::item_count_codec;

std::atomic<uint8_t> g_extended_mode{static_cast<uint8_t>(ItemCountExtendedMode::kOff)};

ItemCountExtendedMode snapshot_mode() {
    return static_cast<ItemCountExtendedMode>(g_extended_mode.load(std::memory_order_acquire));
}

bool extended_applied() { return snapshot_mode() == ItemCountExtendedMode::kApplied; }

QolResult not_ready() {
    static constexpr char kMessage[] = "not ready";
    return contracts::error(kQolErrNotReady, kMessage, static_cast<uint32_t>(std::strlen(kMessage)));
}

QolResult invalid_argument() {
    static constexpr char kMessage[] = "invalid argument";
    return contracts::error(kQolErrInvalidArgument, kMessage,
                            static_cast<uint32_t>(std::strlen(kMessage)));
}

QolResult unsupported() {
    static constexpr char kMessage[] = "unsupported";
    return contracts::error(kQolErrUnsupported, kMessage,
                            static_cast<uint32_t>(std::strlen(kMessage)));
}

// 与旧 provider 语义一致：非编码类别原版 getter 恒为 1。
QolResult canonical_impl(const void* item, uint32_t* out_count) {
    const contracts::QolItemCountEncoding encoding = item_count_encoding(item);
    if (encoding == contracts::QolItemCountEncoding::kUnknown) return unsupported();
    if (encoding == contracts::QolItemCountEncoding::kEncoded) {
        uint32_t raw = 0;
        if (!read_item_count_field(item, &raw)) return not_ready();
        *out_count = codec::s2_read_count(raw);
        return contracts::ok();
    }
    *out_count = 1;
    return contracts::ok();
}

QolResult canonical(const void* item, uint32_t* out_count) {
    if (item == nullptr || out_count == nullptr) return invalid_argument();
    return canonical_impl(item, out_count);
}

QolResult effective(const void* item, uint32_t* out_count) {
    if (item == nullptr || out_count == nullptr) return invalid_argument();
    const ItemCountExtendedMode mode = snapshot_mode();
    if (mode == ItemCountExtendedMode::kNotReady) return not_ready();
    const bool usable = mode == ItemCountExtendedMode::kApplied;
    const contracts::QolItemCountEncoding encoding = item_count_encoding(item);
    if (encoding == contracts::QolItemCountEncoding::kUnknown) return unsupported();
    if (encoding != contracts::QolItemCountEncoding::kEncoded) {
        return canonical_impl(item, out_count);
    }
    uint32_t raw = 0;
    if (!read_item_count_field(item, &raw)) return not_ready();
    const uint32_t canonical_count = codec::s2_read_count(raw);
    *out_count = codec::effective_clamp(codec::effective_view_count(canonical_count, usable), usable);
    return contracts::ok();
}

QolResult is_encoded(const void* item, contracts::QolItemCountEncoding* out_encoding) {
    if (item == nullptr || out_encoding == nullptr) return invalid_argument();
    *out_encoding = item_count_encoding(item);
    return contracts::ok();
}

QolResult write_effective(void* item, uint32_t count) {
    if (item == nullptr) return invalid_argument();
    const ItemCountExtendedMode mode = snapshot_mode();
    if (mode == ItemCountExtendedMode::kNotReady) return not_ready();
    const bool usable = mode == ItemCountExtendedMode::kApplied;
    const contracts::QolItemCountEncoding encoding = item_count_encoding(item);
    if (encoding == contracts::QolItemCountEncoding::kUnknown) return unsupported();
    if (encoding != contracts::QolItemCountEncoding::kEncoded) return unsupported();
    uint32_t raw = 0;
    if (!read_item_count_field(item, &raw)) return not_ready();
    const uint32_t clamped = codec::effective_clamp(count, usable);
    const uint32_t written = codec::field_from_canonical(raw, clamped, usable);
    return write_item_count_field(item, written) ? contracts::ok() : not_ready();
}

QolResult writeback_needed(const void* item, uint32_t count, int32_t* out_needed) {
    if (item == nullptr || out_needed == nullptr) return invalid_argument();
    const ItemCountExtendedMode mode = snapshot_mode();
    if (mode == ItemCountExtendedMode::kNotReady) return not_ready();
    const bool usable = mode == ItemCountExtendedMode::kApplied;
    const contracts::QolItemCountEncoding encoding = item_count_encoding(item);
    if (encoding == contracts::QolItemCountEncoding::kUnknown) return unsupported();
    if (encoding != contracts::QolItemCountEncoding::kEncoded) {
        *out_needed = 0;
        return contracts::ok();
    }
    uint32_t raw = 0;
    if (!read_item_count_field(item, &raw)) return not_ready();
    const uint32_t canonical_count = codec::s2_read_count(raw);
    const uint32_t current =
        codec::effective_clamp(codec::effective_view_count(canonical_count, usable), usable);
    *out_needed = current != codec::effective_clamp(count, usable) ? 1 : 0;
    return contracts::ok();
}

QolResult field_from_canonical(uint32_t field, uint32_t canonical_count, int32_t extended,
                               uint32_t* out_field) {
    if (out_field == nullptr) return invalid_argument();
    *out_field = codec::field_from_canonical(field, canonical_count, extended != 0);
    return contracts::ok();
}

QolResult canonical_from_field(uint32_t field, uint32_t* out_canonical) {
    if (out_canonical == nullptr) return invalid_argument();
    *out_canonical = codec::canonical_from_field(field);
    return contracts::ok();
}

QolResult view_from_canonical(uint32_t canonical_count, int32_t extended, uint32_t* out_view) {
    if (out_view == nullptr) return invalid_argument();
    *out_view = codec::view_from_canonical(canonical_count, extended != 0);
    return contracts::ok();
}

QolResult view_limit(int32_t extended, uint32_t* out_limit) {
    if (out_limit == nullptr) return invalid_argument();
    *out_limit = codec::view_limit(extended != 0);
    return contracts::ok();
}

QolResult read_encoded_field(const void* item, uint32_t* out_field) {
    if (item == nullptr || out_field == nullptr) return invalid_argument();
    return read_item_count_field(item, out_field) ? contracts::ok() : not_ready();
}

QolResult write_encoded_field(void* item, uint32_t field) {
    if (item == nullptr) return invalid_argument();
    return write_item_count_field(item, field) ? contracts::ok() : not_ready();
}

QolResult read_extended_mode(uint8_t* out_extended) {
    if (out_extended == nullptr) return invalid_argument();
    switch (snapshot_mode()) {
        case ItemCountExtendedMode::kOff:
            *out_extended = 0;
            return contracts::ok();
        case ItemCountExtendedMode::kApplied:
            *out_extended = 1;
            return contracts::ok();
        case ItemCountExtendedMode::kNotReady:
            return not_ready();
    }
    return not_ready();
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
    &read_extended_mode,
};

}  // namespace

void item_count_set_extended_mode(ItemCountExtendedMode mode) {
    g_extended_mode.store(static_cast<uint8_t>(mode), std::memory_order_release);
}

ItemCountExtendedMode item_count_extended_mode() { return snapshot_mode(); }

bool item_count_active() { return extended_applied(); }

const contracts::QolItemCountApi& item_count_api() { return kFacade; }

const contracts::QolItemCountApi* item_count_api_if_resident() { return &kFacade; }

}  // namespace qol::game

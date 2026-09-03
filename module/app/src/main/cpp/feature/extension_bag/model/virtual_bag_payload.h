#pragma once

// Included inside namespace virtual_bag by virtual_bag_state.h.
enum class PayloadValidation {
    kOk,
    kMissing,
    kLengthOutOfRange,
    kLengthPrefixMismatch,
    kNonZeroTail,
    kUnavailable,
};

inline const char* payload_validation_reason(PayloadValidation validation) {
    switch (validation) {
        case PayloadValidation::kOk: return "ok";
        case PayloadValidation::kMissing: return "payload_missing";
        case PayloadValidation::kLengthOutOfRange: return "payload_length_out_of_range";
        case PayloadValidation::kLengthPrefixMismatch: return "payload_length_prefix_mismatch";
        case PayloadValidation::kNonZeroTail: return "payload_nonzero_tail";
        case PayloadValidation::kUnavailable: return "payload_bridge_unavailable";
    }
    return "payload_validation_unknown";
}

using SaveItemPayloadFn = int (*)(uint8_t*, void*);
using LoadItemPayloadFn = int (*)(const uint8_t*, void**, int*);
using FreeItemPayloadFn = void (*)(void*);

inline PayloadValidation validate_serialized_payload_buffer(const uint8_t* payload,
                                                             size_t buffer_size,
                                                             int payload_size) {
    if (payload == nullptr || payload_size == 0) return PayloadValidation::kMissing;
    if (payload_size < static_cast<int>(kPayloadHeaderSize) ||
        payload_size > static_cast<int>(kMaxSerializedItem) ||
        static_cast<size_t>(payload_size) > buffer_size) {
        return PayloadValidation::kLengthOutOfRange;
    }
    if (payload[0] != static_cast<uint8_t>(payload_size - 1)) {
        return PayloadValidation::kLengthPrefixMismatch;
    }
    for (size_t index = static_cast<size_t>(payload_size); index < buffer_size; ++index) {
        if (payload[index] != 0) return PayloadValidation::kNonZeroTail;
    }
    return PayloadValidation::kOk;
}

inline PayloadValidation save_item_payload(SaveItemPayloadFn save_item, void* item,
                                            std::array<uint8_t, kSerializedItemBuffer>* out,
                                            int* out_size) {
    if (save_item == nullptr) return PayloadValidation::kUnavailable;
    if (item == nullptr || out == nullptr || out_size == nullptr) return PayloadValidation::kMissing;

    std::array<uint8_t, kSerializedItemProbeBuffer> probe{};
    const int payload_size = save_item(probe.data(), item);
    const PayloadValidation validation =
        validate_serialized_payload_buffer(probe.data(), probe.size(), payload_size);
    if (validation != PayloadValidation::kOk) return validation;

    out->fill(0);
    std::memcpy(out->data(), probe.data(), static_cast<size_t>(payload_size));
    *out_size = payload_size;
    return PayloadValidation::kOk;
}

enum class ManagedLoadFailure {
    kNone,
    kInvalidPayload,
    kUnavailable,
    kLoadFailed,
    kMissingOutput,
    kConsumedMismatch,
    kReserializeRejected,
    kRoundTripMismatch,
};

inline const char* managed_load_failure_reason(ManagedLoadFailure failure) {
    switch (failure) {
        case ManagedLoadFailure::kNone: return "ok";
        case ManagedLoadFailure::kInvalidPayload: return "invalid_payload";
        case ManagedLoadFailure::kUnavailable: return "payload_bridge_unavailable";
        case ManagedLoadFailure::kLoadFailed: return "load_failed";
        case ManagedLoadFailure::kMissingOutput: return "load_missing_output";
        case ManagedLoadFailure::kConsumedMismatch: return "load_consumed_mismatch";
        case ManagedLoadFailure::kReserializeRejected: return "load_reserialize_rejected";
        case ManagedLoadFailure::kRoundTripMismatch: return "load_round_trip_mismatch";
    }
    return "load_failure_unknown";
}

struct ManagedLoadResult {
    void* item = nullptr;
    PayloadValidation validation = PayloadValidation::kOk;
    ManagedLoadFailure failure = ManagedLoadFailure::kNone;
};

inline ManagedLoadResult load_item_payload_exact(const uint8_t* payload, int payload_size,
                                                  SaveItemPayloadFn save_item,
                                                  LoadItemPayloadFn load_item,
                                                  FreeItemPayloadFn free_item) {
    ManagedLoadResult result{};
    result.validation =
        validate_serialized_payload_buffer(payload, kSerializedItemBuffer, payload_size);
    if (result.validation != PayloadValidation::kOk) {
        result.failure = ManagedLoadFailure::kInvalidPayload;
        return result;
    }
    if (save_item == nullptr || load_item == nullptr || free_item == nullptr) {
        result.validation = PayloadValidation::kUnavailable;
        result.failure = ManagedLoadFailure::kUnavailable;
        return result;
    }

    void* item = nullptr;
    int consumed = 0;
    const int loaded = load_item(payload, &item, &consumed);
    if (loaded != 1) {
        if (item != nullptr) free_item(item);
        result.failure = ManagedLoadFailure::kLoadFailed;
        return result;
    }
    if (item == nullptr) {
        result.failure = ManagedLoadFailure::kMissingOutput;
        return result;
    }
    if (consumed != payload_size) {
        free_item(item);
        result.failure = ManagedLoadFailure::kConsumedMismatch;
        return result;
    }

    std::array<uint8_t, kSerializedItemBuffer> round_trip{};
    int round_trip_size = 0;
    result.validation = save_item_payload(save_item, item, &round_trip, &round_trip_size);
    if (result.validation != PayloadValidation::kOk) {
        free_item(item);
        result.failure = ManagedLoadFailure::kReserializeRejected;
        return result;
    }
    if (round_trip_size != payload_size ||
        std::memcmp(round_trip.data(), payload, static_cast<size_t>(payload_size)) != 0) {
        free_item(item);
        result.failure = ManagedLoadFailure::kRoundTripMismatch;
        return result;
    }

    result.item = item;
    return result;
}

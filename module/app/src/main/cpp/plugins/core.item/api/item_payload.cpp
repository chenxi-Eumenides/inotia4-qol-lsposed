#include "plugins/core.item/api/item_payload.h"

#include <array>
#include <cstring>

#include "plugins/core.item/runtime/core_item_internal.h"

namespace qol::features::core_item {
namespace {

constexpr uint32_t kPayloadHeaderSize = 19;
constexpr uint16_t kSerializedOptionSemanticMask = 0xe07f;

QolResult payload_error(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

bool valid_handle(contracts::QolItemHandle handle) {
    return handle.slot != 0 && handle.generation != 0;
}

bool length_prefix_valid(const uint8_t* payload, uint32_t size) {
    return payload != nullptr && size >= kPayloadHeaderSize &&
           size <= contracts::kQolBagTransferPayloadMaxSize &&
           static_cast<uint32_t>(payload[0]) + 1u == size;
}

bool round_trip_semantically_matches(const uint8_t* source, const uint8_t* round_trip,
                                     uint32_t size) {
    if (source == nullptr || round_trip == nullptr || size < kPayloadHeaderSize) return false;
    if (std::memcmp(source + 1, round_trip + 1, kPayloadHeaderSize - 1) != 0) return false;
    const uint32_t option_bytes = size - kPayloadHeaderSize;
    if (option_bytes % 4 != 0) return false;
    const uint32_t option_count = option_bytes / 4;
    for (uint32_t index = 0; index < option_count; ++index) {
        const uint8_t* source_option =
            source + kPayloadHeaderSize + static_cast<size_t>(index) * 4;
        const uint8_t* round_option =
            round_trip + kPayloadHeaderSize + static_cast<size_t>(index) * 4;
        const uint16_t source_code = static_cast<uint16_t>(source_option[0]) |
                                     (static_cast<uint16_t>(source_option[1]) << 8);
        const uint16_t round_code = static_cast<uint16_t>(round_option[0]) |
                                    (static_cast<uint16_t>(round_option[1]) << 8);
        if (((source_code ^ round_code) & kSerializedOptionSemanticMask) != 0 ||
            std::memcmp(source_option + 2, round_option + 2, 2) != 0) {
            return false;
        }
    }
    return true;
}

QolResult serialize_handle(contracts::QolItemHandle handle, uint8_t* out, uint32_t capacity,
                           uint32_t* out_size) {
    if (g_port == nullptr || !contracts::qol_item_port_transfer_size_valid(g_port) ||
        g_port->read.serialize_item == nullptr) {
        return payload_error(kQolErrUnsupported, "item serialization unavailable");
    }
    return g_port->read.serialize_item(handle, out, capacity, out_size);
}

QolResult validate_payload(const uint8_t* payload, uint32_t size,
                           contracts::QolItemPayloadValidation* out) {
    if (out != nullptr) *out = {};
    if (!length_prefix_valid(payload, size)) {
        return payload_error(kQolErrInvalidArgument, "invalid payload structure");
    }
    if (g_item_state_write == nullptr || g_item_state_write->load_item == nullptr ||
        g_item_state_write->free_item == nullptr) {
        return payload_error(kQolErrUnsupported, "item payload loader unavailable");
    }
    contracts::QolItemHandle temp{};
    QolResult result = g_item_state_write->load_item(payload, size, &temp);
    if (!contracts::succeeded(result)) {
        return payload_error(kQolErrInvalidArgument, "payload load rejected");
    }
    std::array<uint8_t, contracts::kQolBagTransferPayloadCapacity> round_trip{};
    uint32_t round_trip_size = 0;
    result = serialize_handle(temp, round_trip.data(), static_cast<uint32_t>(round_trip.size()),
                              &round_trip_size);
    const QolResult freed = g_item_state_write->free_item(temp);
    if (!contracts::succeeded(result)) return result;
    if (round_trip_size != size ||
        !round_trip_semantically_matches(payload, round_trip.data(), size)) {
        return payload_error(kQolErrInvalidArgument, "payload round trip mismatch");
    }
    if (!contracts::succeeded(freed) && freed.code != kQolErrNotFound) return freed;
    if (out != nullptr) {
        out->struct_size = sizeof(contracts::QolItemPayloadValidation);
        out->consumed = size;
        out->option_count = (size - kPayloadHeaderSize) / 4;
    }
    return contracts::ok();
}

QolResult update_payload(const contracts::QolItemPayloadUpdateSpec* spec,
                         contracts::QolItemPayload* out) {
    if (out == nullptr || spec == nullptr ||
        spec->struct_size < sizeof(contracts::QolItemPayloadUpdateSpec)) {
        return payload_error(kQolErrInvalidArgument, "invalid payload update request");
    }
    *out = {};
    out->struct_size = sizeof(contracts::QolItemPayload);
    if (g_item_state_write == nullptr || g_item_state_write->load_item == nullptr ||
        g_item_state_write->free_item == nullptr ||
        g_item_state_write->apply_payload_mutation == nullptr) {
        return payload_error(kQolErrUnsupported, "item payload ports unavailable");
    }
    if (g_port == nullptr) {
        return payload_error(kQolErrUnsupported, "item port unavailable");
    }
    if (spec->mutation_count != 0 && spec->mutations == nullptr) {
        return payload_error(kQolErrInvalidArgument, "invalid payload mutations");
    }
    const bool has_source = valid_handle(spec->source);
    const bool has_base = spec->base_payload != nullptr || spec->base_payload_size != 0;
    if (has_source == has_base) {
        return payload_error(kQolErrInvalidArgument, "payload source/base must be exclusive");
    }

    std::array<uint8_t, contracts::kQolBagTransferPayloadCapacity> base{};
    uint32_t base_size = 0;
    if (has_source) {
        uint32_t required = 0;
        QolResult result = serialize_handle(spec->source, nullptr, 0, &required);
        if (!contracts::succeeded(result)) return result;
        if (required < kPayloadHeaderSize ||
            required > contracts::kQolBagTransferPayloadMaxSize) {
            return payload_error(kQolErrInternal, "source payload size invalid");
        }
        result = serialize_handle(spec->source, base.data(),
                                  static_cast<uint32_t>(base.size()), &base_size);
        if (!contracts::succeeded(result)) return result;
        if (base_size != required) {
            return payload_error(kQolErrInternal, "source payload size changed");
        }
    } else {
        if (!length_prefix_valid(spec->base_payload, spec->base_payload_size)) {
            return payload_error(kQolErrInvalidArgument, "invalid base payload");
        }
        base_size = spec->base_payload_size;
        std::memcpy(base.data(), spec->base_payload, base_size);
    }

    contracts::QolItemHandle temp{};
    QolResult result = g_item_state_write->load_item(base.data(), base_size, &temp);
    if (!contracts::succeeded(result)) return result;
    for (uint32_t index = 0; index < spec->mutation_count; ++index) {
        result = g_item_state_write->apply_payload_mutation(temp, &spec->mutations[index]);
        if (!contracts::succeeded(result)) {
            (void)g_item_state_write->free_item(temp);
            return result;
        }
    }

    uint32_t required = 0;
    result = serialize_handle(temp, nullptr, 0, &required);
    if (!contracts::succeeded(result)) {
        (void)g_item_state_write->free_item(temp);
        return result;
    }
    if (required < kPayloadHeaderSize ||
        required > contracts::kQolBagTransferPayloadMaxSize) {
        (void)g_item_state_write->free_item(temp);
        return payload_error(kQolErrInternal, "updated payload size invalid");
    }
    result = serialize_handle(temp, out->payload,
                              static_cast<uint32_t>(sizeof(out->payload)), &out->payload_size);
    if (!contracts::succeeded(result)) {
        (void)g_item_state_write->free_item(temp);
        return result;
    }
    if (out->payload_size != required) {
        (void)g_item_state_write->free_item(temp);
        return payload_error(kQolErrInternal, "updated payload size changed");
    }

    // 有 source 时以源对象为权威读取 category/count：临时对象只用于生成 payload，
    // 其元数据在部分变体/加载路径上可能未完全回填。
    const contracts::QolItemHandle metadata_source = has_source ? spec->source : temp;
    int32_t category = 0;
    if (g_port->read.category != nullptr) {
        result = g_port->read.category(metadata_source, &category);
        if (!contracts::succeeded(result)) {
            (void)g_item_state_write->free_item(temp);
            return result;
        }
    }
    int32_t count = 1;
    if (g_port->read.effective_cumulate_count != nullptr) {
        const QolResult read_count =
            g_port->read.effective_cumulate_count(metadata_source, &count);
        if (!contracts::succeeded(read_count)) count = 1;
    } else if (g_port->read.cumulate_count != nullptr) {
        const QolResult read_count = g_port->read.cumulate_count(metadata_source, &count);
        if (!contracts::succeeded(read_count)) count = 1;
    }
    if (count <= 0) count = 1;
    const QolResult freed = g_item_state_write->free_item(temp);
    if (!contracts::succeeded(freed) && freed.code != kQolErrNotFound) return freed;

    if ((spec->flags & contracts::kItemPayloadUpdateValidateStructure) != 0) {
        const QolResult validated = validate_payload(out->payload, out->payload_size, nullptr);
        if (!contracts::succeeded(validated)) return validated;
    }
    // 有 source 时把同一组 mutation 应用到源对象：craft UI/UseStuff 仍持有原对象指针，
    // 必须保留对象身份；provider 的权威 payload 由后续 inventory.transfer.replace_item 提交。
    if (has_source) {
        for (uint32_t index = 0; index < spec->mutation_count; ++index) {
            result = g_item_state_write->apply_payload_mutation(spec->source,
                                                                &spec->mutations[index]);
            if (!contracts::succeeded(result)) return result;
        }
    }
    out->category = static_cast<uint32_t>(category);
    out->count = static_cast<uint32_t>(count);
    return contracts::ok();
}

QolResult api_update(const contracts::QolItemPayloadUpdateSpec* spec,
                     contracts::QolItemPayload* out) {
    return update_payload(spec, out);
}

QolResult api_validate(const uint8_t* payload, uint32_t payload_size,
                       contracts::QolItemPayloadValidation* out) {
    return validate_payload(payload, payload_size, out);
}

}  // namespace

const contracts::ItemPayloadApi& item_payload_api() {
    static const contracts::ItemPayloadApi api = {
        sizeof(contracts::ItemPayloadApi),
        contracts::kItemPayloadVersionMajor,
        contracts::kItemPayloadVersionMinor,
        &api_update,
        &api_validate,
    };
    return api;
}

}  // namespace qol::features::core_item

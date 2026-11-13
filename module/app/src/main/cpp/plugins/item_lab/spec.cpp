#include "spec.h"
namespace qol::features::item_lab {
bool build_equipment_payload(contracts::QolItemPayloadBlob* out) {
    if (out == nullptr) return false;
    const contracts::QolItemPayloadHeader header{0, static_cast<uint16_t>((462u << 6) | (4u << 2)),
                                                   0, 100, 0x30, 0};
    const contracts::QolItemPayloadOption options[] = {{0, 15}, {1, 12}, {2, 10}};
    if (!contracts::item_payload_build(header, options, 3, out->payload,
                                       sizeof(out->payload), &out->payload_size)) return false;
    out->struct_size = sizeof(*out);
    return true;
}
}

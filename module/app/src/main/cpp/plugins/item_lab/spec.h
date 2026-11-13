#pragma once
#include "../../contracts/item_payload.h"
namespace qol::features::item_lab {
constexpr int32_t kEquipmentCategory = 462;
constexpr int32_t kEquipmentCount = 1;
bool build_equipment_payload(contracts::QolItemPayloadBlob* out);
}

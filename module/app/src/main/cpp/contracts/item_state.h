#pragma once

#include <cstdint>
#include <type_traits>

#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kItemStateCapabilityId = "item.state";
constexpr QolVersion kItemStateVersion{1, 1};

// Item owners assemble their domain projection. Cross-domain JSON may only be filtered or
// reordered; its fields must not be reassembled by another owner (ADR 0002).
enum class QolItemStateKind : int32_t {
    // Raw inventory 形状 = 迁移前 legacy build_inventory_json 字节。
    kRawInventory = 0,
    // Enriched inventory 与 GET /api/item/inventory 响应体同源，字段由 item owner 组装。
    kEnrichedInventory = 1,
};

// 单槽 raw inventory 的宿主侧缓冲上限（对齐 legacy 最坏尺寸；见 T2h-B2 容量论证）。
constexpr uint32_t kQolItemStateRawInventoryCapacity = 262144;

// The buffer contains the owner-assembled, byte-compatible JSON projection for the fixed kind.
using QolItemStateWriteJsonFn = QolResult (*)(int32_t kind, char* out, uint32_t capacity,
                                              uint32_t* out_size);

struct ItemStateApi {
    QolItemStateWriteJsonFn write_state_json;
};

static_assert(std::is_standard_layout<ItemStateApi>::value,
              "ItemStateApi must remain standard layout");
static_assert(std::is_trivially_copyable<ItemStateApi>::value,
              "ItemStateApi must remain trivially copyable");

}  // namespace qol::contracts

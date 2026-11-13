#pragma once

// 物品 payload 纯数据契约（core.item 提供）。
//
// 约束：
// - 纯 payload 操作：不写库存槽、不修改源对象、不释放库存对象。对象/槽更新由
//   inventory.transfer.replace_item 负责。
// - 所有回调只在游戏主线程调用。
// - 结构校验只保证 loader/serializer 可接受且往返结构等价；不校验词条 ID、
//   数值、品质、UID 唯一性或堆叠上限，这些语义由调用方负责。

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "bag.h"
#include "item_payload_codec.h"
#include "item_handle.h"
#include "result.h"

namespace qol::contracts {

constexpr const char* kItemPayloadCapabilityId = "item.payload";
constexpr uint16_t kItemPayloadVersionMajor = 1;
constexpr uint16_t kItemPayloadVersionMinor = 1;

// 与 game 层字段写入一一对应；值语义由 game 层按物品结构解释。
enum class QolItemPayloadMutationKind : int32_t {
    kJewelWord = 1,
    kNativeOptionValue = 2,
    kCount = 3,
    kSocketLevel = 4,
    kSocketFilled = 5,
    kEnchantLevel = 6,
    kEnchantRemaining = 7,
    kMagicRate = 8,
};

struct QolItemPayloadMutation {
    int32_t kind;
    uint32_t index;
    int64_t value;
};

constexpr uint32_t kItemPayloadUpdateValidateStructure = 1u << 0;
// base_payload 路径没有 source 句柄可复用；payload 自身已携带 UID，本标志仅作调用方
// 意图声明，当前实现不额外改写 UID。
constexpr uint32_t kItemPayloadUpdatePreserveUid = 1u << 1;

struct QolItemPayloadUpdateSpec {
    uint32_t struct_size;
    QolItemHandle source;
    const uint8_t* base_payload;
    uint32_t base_payload_size;
    const QolItemPayloadMutation* mutations;
    uint32_t mutation_count;
    uint32_t flags;
};

struct QolItemPayload {
    uint32_t struct_size;
    uint32_t category;
    uint32_t count;
    uint32_t payload_size;
    uint8_t payload[kQolBagTransferPayloadCapacity];
};

struct QolItemPayloadValidation {
    uint32_t struct_size;
    uint32_t consumed;
    uint32_t option_count;
};

struct QolItemPayloadBuildSpec {
    uint32_t struct_size;
    QolItemPayloadHeader header;
    const QolItemPayloadOption* options;
    uint32_t option_count;
    uint32_t flags;
};

struct QolItemPayloadBlob {
    uint32_t struct_size;
    uint32_t payload_size;
    uint8_t payload[kQolBagTransferPayloadCapacity];
};

struct QolItemPayloadDecoded {
    uint32_t struct_size;
    uint32_t payload_size;
    QolItemPayloadHeader header;
    uint32_t option_count;
    QolItemPayloadOption options[kItemPayloadMaxOptionCount];
    uint32_t flags;
};

struct ItemPayloadApi {
    uint32_t struct_size;
    uint16_t version_major;
    uint16_t version_minor;
    QolResult (*update)(const QolItemPayloadUpdateSpec* spec, QolItemPayload* out);
    QolResult (*validate)(const uint8_t* payload, uint32_t payload_size,
                          QolItemPayloadValidation* out);
    QolResult (*build)(const QolItemPayloadBuildSpec* spec, QolItemPayloadBlob* out);
    QolResult (*decode)(const uint8_t* payload, uint32_t payload_size,
                        QolItemPayloadDecoded* out);
};

static_assert(std::is_standard_layout<QolItemPayloadMutation>::value,
              "QolItemPayloadMutation must remain standard layout");
static_assert(std::is_trivially_copyable<QolItemPayloadMutation>::value,
              "QolItemPayloadMutation must remain trivially copyable");
static_assert(std::is_standard_layout<QolItemPayloadUpdateSpec>::value,
              "QolItemPayloadUpdateSpec must remain standard layout");
static_assert(std::is_trivially_copyable<QolItemPayloadUpdateSpec>::value,
              "QolItemPayloadUpdateSpec must remain trivially copyable");
static_assert(std::is_standard_layout<QolItemPayload>::value,
              "QolItemPayload must remain standard layout");
static_assert(std::is_trivially_copyable<QolItemPayload>::value,
              "QolItemPayload must remain trivially copyable");
static_assert(std::is_standard_layout<QolItemPayloadValidation>::value,
              "QolItemPayloadValidation must remain standard layout");
static_assert(std::is_trivially_copyable<QolItemPayloadValidation>::value,
              "QolItemPayloadValidation must remain trivially copyable");
static_assert(std::is_standard_layout<QolItemPayloadBuildSpec>::value,
              "QolItemPayloadBuildSpec must remain standard layout");
static_assert(std::is_trivially_copyable<QolItemPayloadBuildSpec>::value,
              "QolItemPayloadBuildSpec must remain trivially copyable");
static_assert(std::is_standard_layout<QolItemPayloadBlob>::value,
              "QolItemPayloadBlob must remain standard layout");
static_assert(std::is_trivially_copyable<QolItemPayloadBlob>::value,
              "QolItemPayloadBlob must remain trivially copyable");
static_assert(std::is_standard_layout<QolItemPayloadDecoded>::value,
              "QolItemPayloadDecoded must remain standard layout");
static_assert(std::is_trivially_copyable<QolItemPayloadDecoded>::value,
              "QolItemPayloadDecoded must remain trivially copyable");

inline bool item_payload_api_valid(const ItemPayloadApi* api) {
    constexpr uint32_t kV10Size = static_cast<uint32_t>(
        offsetof(ItemPayloadApi, validate) + sizeof(decltype(ItemPayloadApi::validate)));
    return api != nullptr && api->struct_size >= kV10Size &&
           api->version_major == kItemPayloadVersionMajor &&
           api->update != nullptr && api->validate != nullptr;
}

inline bool item_payload_codec_api_valid(const ItemPayloadApi* api) {
    constexpr uint32_t kCodecSize = static_cast<uint32_t>(
        offsetof(ItemPayloadApi, decode) + sizeof(decltype(ItemPayloadApi::decode)));
    return api != nullptr && api->struct_size >= kCodecSize &&
           api->version_major == kItemPayloadVersionMajor && api->version_minor >= 1 &&
           api->build != nullptr && api->decode != nullptr;
}

}  // namespace qol::contracts

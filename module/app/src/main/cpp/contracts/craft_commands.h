#pragma once

#include <cstdint>
#include <type_traits>

#include "commands.h"
#include "item_handle.h"

namespace qol::contracts {

constexpr uint32_t kCraftWriteStuffNeedHandlerId = 0x43524601;
constexpr uint32_t kCraftSetStuffItemHandlerId = 0x43524602;

struct CraftWriteStuffNeedPayload {
    uint32_t index;
    int32_t category;
    int32_t count;
};

struct CraftSetStuffItemPayload {
    uint32_t index;
    QolItemHandle item;
};

static_assert(sizeof(CraftWriteStuffNeedPayload) <= kTypedCommandPayloadCapacity,
              "craft write command payload exceeds capacity");
static_assert(std::is_trivially_copyable<CraftWriteStuffNeedPayload>::value,
              "craft write command payload must remain trivially copyable");
static_assert(sizeof(CraftSetStuffItemPayload) <= kTypedCommandPayloadCapacity,
              "craft set item command payload exceeds capacity");
static_assert(std::is_trivially_copyable<CraftSetStuffItemPayload>::value,
              "craft set item command payload must remain trivially copyable");

}  // namespace qol::contracts

#pragma once

#include <cstdint>
#include <type_traits>

namespace qol::contracts {

// A zero slot or generation is invalid. Handles contain no game address and are process-local.
struct QolItemHandle {
    uint32_t slot;
    uint32_t generation;
};

struct QolUiControlHandle {
    uint32_t slot;
    uint32_t generation;
};

static_assert(std::is_standard_layout<QolItemHandle>::value, "item handle must be standard layout");
static_assert(std::is_trivially_copyable<QolItemHandle>::value,
              "item handle must be trivially copyable");
static_assert(std::is_standard_layout<QolUiControlHandle>::value,
              "UI control handle must be standard layout");
static_assert(std::is_trivially_copyable<QolUiControlHandle>::value,
              "UI control handle must be trivially copyable");

constexpr QolItemHandle kInvalidItemHandle{0, 0};
constexpr QolUiControlHandle kInvalidUiControlHandle{0, 0};

constexpr uint32_t kQolItemHandleProviderNative = 0;
constexpr uint32_t kQolItemHandleProviderExtensionBag = 1;
constexpr uint32_t kQolItemHandleProviderAny = UINT32_MAX;

enum QolItemHandleStatus : int32_t {
    kQolItemHandleStatusOk = 0,
    kQolItemHandleStatusInvalidArgument = -1,
    kQolItemHandleStatusWrongThread = -2,
    kQolItemHandleStatusCapacity = -3,
    kQolItemHandleStatusStale = -4,
    kQolItemHandleStatusType = -5,
    kQolItemHandleStatusTransition = -6,
    kQolItemHandleStatusNotFound = -7,
};

struct QolItemHandleApi {
    // Registration and ownership callbacks return QolItemHandleStatus.
    int32_t (*register_item)(void* item, uint32_t provider_id, QolItemHandle* out_handle);
    int32_t (*invalidate_item)(void* item, uint32_t provider_id);
    int32_t (*rebind_item)(void* item, uint32_t provider_id);
    int32_t (*begin_invalidation)(void* item, uint32_t provider_id,
                                  QolItemHandle* out_checkpoint);
    int32_t (*finish_invalidation)(QolItemHandle checkpoint, int32_t committed);
};

static_assert(std::is_standard_layout<QolItemHandleApi>::value,
              "item handle API must be standard layout");
static_assert(std::is_trivially_copyable<QolItemHandleApi>::value,
              "item handle API must be trivially copyable");

}  // namespace qol::contracts

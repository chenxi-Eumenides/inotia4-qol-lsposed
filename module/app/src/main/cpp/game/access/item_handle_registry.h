#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "contracts/item_handle.h"

namespace qol::game {

enum class ItemHandleError : uint8_t {
    kNone, kInvalidArgument, kWrongThread, kFull, kStale, kType, kTransition
};

struct ItemHandleResolution {
    void* item = nullptr;
    ItemHandleError error = ItemHandleError::kNone;
};

class ItemHandleRegistry {
public:
    static constexpr size_t kCapacity = 1024;
    using MainThreadCheck = bool (*)();

    explicit ItemHandleRegistry(MainThreadCheck is_main_thread = nullptr);
    contracts::QolItemHandle register_item(void* item, uint32_t provider_id = 0,
                                           ItemHandleError* error = nullptr);
    ItemHandleResolution resolve_item(contracts::QolItemHandle handle,
                                      uint32_t provider_id = 0) const;
    bool invalidate_item(void* item, uint32_t provider_id = contracts::kQolItemHandleProviderAny);
    bool invalidate_item(contracts::QolItemHandle handle);
    bool rebind_item(void* item, uint32_t provider_id);
    bool begin_invalidation(void* item, uint32_t provider_id,
                            contracts::QolItemHandle* out_checkpoint);
    bool finish_invalidation(contracts::QolItemHandle checkpoint, bool committed);
    void invalidate_all();
    void invalidate_provider(uint32_t provider_id);
    bool begin_lifecycle_transition();
    void finish_lifecycle_transition(bool committed);
    size_t high_watermark() const { return high_watermark_; }
#ifdef QOL_ITEM_HANDLE_TESTING
    void set_generation_for_test(uint32_t slot, uint32_t generation);
#endif

private:
    struct Slot {
        void* item = nullptr;
        uint32_t provider_id = 0;
        uint32_t generation = 1;
        bool active = false;
        bool retired = false;
        bool suspended = false;
    };

    bool on_main_thread() const;
    std::array<Slot, kCapacity> slots_{};
    std::array<Slot, kCapacity> transition_snapshot_{};
    MainThreadCheck is_main_thread_;
    size_t active_count_ = 0;
    size_t high_watermark_ = 0;
    bool transitioning_ = false;
};

ItemHandleRegistry& item_handle_registry();
contracts::QolItemHandle register_item(void* item, uint32_t provider_id = 0,
                                      ItemHandleError* error = nullptr);
ItemHandleResolution resolve_item(contracts::QolItemHandle handle,
                                  uint32_t provider_id = 0);
bool invalidate_item(void* item,
                     uint32_t provider_id = contracts::kQolItemHandleProviderAny);
bool invalidate_item(contracts::QolItemHandle handle);
bool rebind_item(void* item, uint32_t provider_id);
bool begin_item_invalidation(void* item, uint32_t provider_id,
                             contracts::QolItemHandle* out_checkpoint);
bool finish_item_invalidation(contracts::QolItemHandle checkpoint, bool committed);
void invalidate_all_item_handles();
void invalidate_item_handles_for_provider(uint32_t provider_id);
bool begin_item_handle_lifecycle_transition();
void finish_item_handle_lifecycle_transition(bool committed);

}  // namespace qol::game

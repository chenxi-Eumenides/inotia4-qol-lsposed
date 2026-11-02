#include "game/access/item_handle_registry.h"

#include "game/adapter_ports/game_host_port.h"
#include "game/access/qol_log_seam.h"

namespace qol::game {
namespace {

bool game_main_thread() {
    return game_host_is_main_thread();
}

void set_error(ItemHandleError* out, ItemHandleError error) {
    if (out != nullptr) *out = error;
}

}  // namespace

ItemHandleRegistry::ItemHandleRegistry(MainThreadCheck is_main_thread)
    : is_main_thread_(is_main_thread) {}

bool ItemHandleRegistry::on_main_thread() const {
    return is_main_thread_ == nullptr || is_main_thread_();
}

contracts::QolItemHandle ItemHandleRegistry::register_item(void* item,
                                                           uint32_t provider_id,
                                                           ItemHandleError* error) {
    set_error(error, ItemHandleError::kNone);
    if (item == nullptr) {
        set_error(error, ItemHandleError::kInvalidArgument);
        return contracts::kInvalidItemHandle;
    }
    if (!on_main_thread()) {
        set_error(error, ItemHandleError::kWrongThread);
        return contracts::kInvalidItemHandle;
    }
    if (transitioning_) {
        set_error(error, ItemHandleError::kTransition);
        return contracts::kInvalidItemHandle;
    }
    for (size_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        if (slot.suspended && slot.item == item) {
            set_error(error, ItemHandleError::kTransition);
            return contracts::kInvalidItemHandle;
        }
        if (slot.active && slot.item == item) {
            if (slot.provider_id == provider_id) {
                return {static_cast<uint32_t>(i + 1), slot.generation};
            }
            set_error(error, ItemHandleError::kType);
            return contracts::kInvalidItemHandle;
        }
    }
    for (size_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        if (slot.active || slot.suspended || slot.retired) continue;
        slot.item = item;
        slot.provider_id = provider_id;
        slot.active = true;
        slot.suspended = false;
        ++active_count_;
        if (active_count_ > high_watermark_) high_watermark_ = active_count_;
        return {static_cast<uint32_t>(i + 1), slot.generation};
    }
    set_error(error, ItemHandleError::kFull);
    QOL_LOG_SEAM_ERROR(kQolLogDomainPlatform, "item handle table full capacity=%zu", kCapacity);
    return contracts::kInvalidItemHandle;
}

contracts::QolItemHandle ItemHandleRegistry::lookup_item(void* item,
                                                         uint32_t* out_provider_id) const {
    if (out_provider_id != nullptr) *out_provider_id = 0;
    if (item == nullptr || !on_main_thread() || transitioning_) {
        return contracts::kInvalidItemHandle;
    }
    for (size_t i = 0; i < slots_.size(); ++i) {
        const Slot& slot = slots_[i];
        if (!slot.active || slot.item != item) continue;
        if (out_provider_id != nullptr) *out_provider_id = slot.provider_id;
        return {static_cast<uint32_t>(i + 1), slot.generation};
    }
    return contracts::kInvalidItemHandle;
}

ItemHandleResolution ItemHandleRegistry::resolve_item(contracts::QolItemHandle handle,
                                                       uint32_t provider_id) const {
    if (handle.slot == 0 || handle.slot > slots_.size() || handle.generation == 0) {
        return {nullptr, ItemHandleError::kStale};
    }
    if (!on_main_thread()) return {nullptr, ItemHandleError::kWrongThread};
    const Slot& slot = slots_[handle.slot - 1];
    if (!slot.active || slot.generation != handle.generation) {
        return {nullptr, ItemHandleError::kStale};
    }
    if (provider_id != contracts::kQolItemHandleOwnerAny && slot.provider_id != provider_id) {
        return {nullptr, ItemHandleError::kType};
    }
    return {slot.item, ItemHandleError::kNone};
}

bool ItemHandleRegistry::invalidate_item(void* item, uint32_t provider_id) {
    contracts::QolItemHandle checkpoint{};
    if (!begin_invalidation(item, provider_id, &checkpoint)) return false;
    return finish_invalidation(checkpoint, true);
}

bool ItemHandleRegistry::invalidate_item(contracts::QolItemHandle handle) {
    if (handle.slot == 0 || handle.slot > slots_.size() || handle.generation == 0 ||
        !on_main_thread() || transitioning_) return false;
    Slot& slot = slots_[handle.slot - 1];
    if (!slot.active || slot.generation != handle.generation) return false;
    slot.item = nullptr;
    slot.provider_id = 0;
    slot.active = false;
    slot.suspended = false;
    --active_count_;
    if (slot.generation == UINT32_MAX) slot.retired = true;
    else ++slot.generation;
    return true;
}

bool ItemHandleRegistry::rebind_item(void* item, uint32_t provider_id) {
    if (item == nullptr || !on_main_thread() || transitioning_) return false;
    for (Slot& slot : slots_) {
        if (!slot.active || slot.item != item) continue;
        slot.provider_id = provider_id;
        return true;
    }
    return false;
}

bool ItemHandleRegistry::begin_invalidation(void* item, uint32_t provider_id,
                                            contracts::QolItemHandle* out_checkpoint) {
    if (out_checkpoint == nullptr || item == nullptr || !on_main_thread() || transitioning_) {
        return false;
    }
    *out_checkpoint = contracts::kInvalidItemHandle;
    for (size_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        if (!slot.active || slot.item != item ||
            (provider_id != contracts::kQolItemHandleOwnerAny && slot.provider_id != provider_id)) {
            continue;
        }
        slot.active = false;
        slot.suspended = true;
        --active_count_;
        *out_checkpoint = {static_cast<uint32_t>(i + 1), slot.generation};
        return true;
    }
    return false;
}

bool ItemHandleRegistry::finish_invalidation(contracts::QolItemHandle checkpoint,
                                             bool committed) {
    if (checkpoint.slot == 0 || checkpoint.slot > slots_.size() || checkpoint.generation == 0 ||
        !on_main_thread() || transitioning_) return false;
    Slot& slot = slots_[checkpoint.slot - 1];
    if (!slot.suspended || slot.generation != checkpoint.generation) return false;
    slot.suspended = false;
    if (!committed) {
        slot.active = true;
        ++active_count_;
        return true;
    }
    slot.item = nullptr;
    slot.provider_id = 0;
    slot.active = false;
    if (slot.generation == UINT32_MAX) slot.retired = true;
    else ++slot.generation;
    return true;
}

void ItemHandleRegistry::invalidate_all() {
    if (!on_main_thread() || transitioning_) return;
    for (Slot& slot : slots_) {
        if (!slot.active) continue;
        slot.item = nullptr;
        slot.provider_id = 0;
        slot.active = false;
        slot.suspended = false;
        if (slot.generation == UINT32_MAX) slot.retired = true;
        else ++slot.generation;
    }
    active_count_ = 0;
}

void ItemHandleRegistry::invalidate_provider(uint32_t provider_id) {
    if (!on_main_thread() || transitioning_) return;
    for (Slot& slot : slots_) {
        if (!slot.active || slot.provider_id != provider_id) continue;
        slot.item = nullptr;
        slot.provider_id = 0;
        slot.active = false;
        slot.suspended = false;
        --active_count_;
        if (slot.generation == UINT32_MAX) slot.retired = true;
        else ++slot.generation;
    }
}

bool ItemHandleRegistry::begin_lifecycle_transition() {
    if (!on_main_thread() || transitioning_) return false;
    transition_snapshot_ = slots_;
    transitioning_ = true;
    for (Slot& slot : slots_) {
        if (!slot.active) continue;
        slot.item = nullptr;
        slot.provider_id = 0;
        slot.active = false;
        slot.suspended = false;
        if (slot.generation == UINT32_MAX) slot.retired = true;
        else ++slot.generation;
    }
    active_count_ = 0;
    return true;
}

void ItemHandleRegistry::finish_lifecycle_transition(bool committed) {
    if (!on_main_thread() || !transitioning_) return;
    if (!committed) {
        slots_ = transition_snapshot_;
        active_count_ = 0;
        for (const Slot& slot : slots_) active_count_ += slot.active ? 1u : 0u;
    }
    transition_snapshot_ = {};
    transitioning_ = false;
}

#ifdef QOL_ITEM_HANDLE_TESTING
void ItemHandleRegistry::set_generation_for_test(uint32_t slot, uint32_t generation) {
    if (slot == 0 || slot > slots_.size() || slots_[slot - 1].active || generation == 0) return;
    slots_[slot - 1].generation = generation;
}
#endif

ItemHandleRegistry& item_handle_registry() {
    static ItemHandleRegistry registry(&game_main_thread);
    return registry;
}

contracts::QolItemHandle register_item(void* item, uint32_t provider_id,
                                      ItemHandleError* error) {
    return item_handle_registry().register_item(item, provider_id, error);
}

contracts::QolItemHandle lookup_item(void* item, uint32_t* out_provider_id) {
    return item_handle_registry().lookup_item(item, out_provider_id);
}

ItemHandleResolution resolve_item(contracts::QolItemHandle handle, uint32_t provider_id) {
    return item_handle_registry().resolve_item(handle, provider_id);
}

bool invalidate_item(void* item, uint32_t provider_id) {
    return item_handle_registry().invalidate_item(item, provider_id);
}

bool invalidate_item(contracts::QolItemHandle handle) {
    return item_handle_registry().invalidate_item(handle);
}

bool rebind_item(void* item, uint32_t provider_id) {
    return item_handle_registry().rebind_item(item, provider_id);
}

bool begin_item_invalidation(void* item, uint32_t provider_id,
                             contracts::QolItemHandle* out_checkpoint) {
    return item_handle_registry().begin_invalidation(item, provider_id, out_checkpoint);
}

bool finish_item_invalidation(contracts::QolItemHandle checkpoint, bool committed) {
    return item_handle_registry().finish_invalidation(checkpoint, committed);
}

void invalidate_all_item_handles() { item_handle_registry().invalidate_all(); }

void invalidate_item_handles_for_provider(uint32_t provider_id) {
    item_handle_registry().invalidate_provider(provider_id);
}

bool begin_item_handle_lifecycle_transition() {
    return item_handle_registry().begin_lifecycle_transition();
}

void finish_item_handle_lifecycle_transition(bool committed) {
    item_handle_registry().finish_lifecycle_transition(committed);
}

}  // namespace qol::game

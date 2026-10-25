#pragma once

#include "plugins/extension_bag/model/ownership_ledger.h"

#include <cstdint>
#include <thread>

struct ModuleUseState {
    bool active = false;
    bool consumed = false;
    bool pending_release = false;
    void* item = nullptr;
    uint64_t generation = 0;
    std::thread::id owner{};
};

struct ModuleUseToken {
    int bag = -1;
    int slot = -1;
    void* item = nullptr;
    uint64_t generation = 0;
    std::thread::id owner{};
};

struct ModuleUseReleasePlan {
    bool pending = false;
    int bag = -1;
    int slot = -1;
    void* item = nullptr;
    uint32_t handle = 0;
    uint64_t generation = 0;
};

bool module_slot_of_item_locked(void* item, int* out_bag, int* out_slot);
bool defer_item_free_locked(void* item);
void drain_deferred_frees_locked();
int32_t extension_bag_deferred_free_tick(int64_t frame, void* ctx);
void log_ownership_audit_locked(const char* context);
bool object_touch_window_active_locked();
bool retire_custody_item_locked(uint32_t handle, void* item, const char* context);
void* load_item_payload_tracked_locked(const uint8_t* payload, int payload_size,
                                       const char* context, uint32_t* out_handle);
void handover_tracked_item_locked(uint32_t handle, void* item, const char* context);
void release_tracked_item_locked(uint32_t handle, void* item, const char* context);
bool module_slot_is_assignable_locked(int bag, int slot);
bool module_object_replacement_allowed_locked(int bag, int slot, const char* context);
bool module_object_release_allowed_locked(int bag, int slot, void* item, const char* context,
                                          bool allow_owner, const ModuleUseToken* token);
bool module_object_generation_advance_locked(int bag, int slot, const char* context);
bool module_use_begin_locked(int bag, int slot, void* item, ModuleUseToken* out_token);
bool module_use_finish_locked(const ModuleUseToken& token, bool* out_consumed,
                              ModuleUseReleasePlan* out_release);
bool module_use_abort_locked(const ModuleUseToken& token);
bool module_use_abort_isolate_locked(const ModuleUseToken& token);
bool module_use_release_native(const ModuleUseReleasePlan& release);
bool module_use_finalize_release_locked(const ModuleUseReleasePlan& release);
bool free_module_object_locked(int bag, int slot, void* raw_use_token = nullptr);

// Transactions use these ports rather than reaching through the UI-state owner.
ownership::Ledger ownership_ledger_snapshot();
void ownership_ledger_restore(const ownership::Ledger& snapshot);
bool ownership_handle_live(uint32_t handle, ownership::State expected);
ownership::Outcome ownership_borrow_for_view(uint32_t handle);
ownership::Outcome ownership_return_from_view(uint32_t handle);

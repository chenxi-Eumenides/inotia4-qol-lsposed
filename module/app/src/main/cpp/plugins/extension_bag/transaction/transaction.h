#pragma once

#include "plugins/extension_bag/runtime/state.h"

bool move_original_to_extension_slot_locked(
    std::unique_lock<std::mutex>& lock, int src_bag, int src_slot, int dst_bag,
    qol::contracts::QolItemHandle expected_item = qol::contracts::kInvalidItemHandle);
bool move_original_to_extension_locked(std::unique_lock<std::mutex>& lock, int dst_bag,
                                       void* moving_control);
bool move_extension_to_original_locked(std::unique_lock<std::mutex>& lock, int src_bag,
                                       int src_slot, int target_bag);
bool move_extension_to_extension_locked(int src_bag, int src_slot, int dst_bag,
                                        int requested_dst_slot = -1);
bool route_projected_session_to_tab_locked(std::unique_lock<std::mutex>& lock, int target_bag);
bool extension_save_item_on_empty(void* item, int bag);
bool serialize_item_payload_locked(
    void* item, std::array<uint8_t, virtual_bag::kSerializedItemBuffer>* out, int* out_size);
void* load_item_payload_locked(const uint8_t* payload, int payload_size, const char* context);
void generate_transaction_id_locked(char* out);
bool txn_record_pending_locked(virtual_bag::TransactionContext* txn);
void txn_abort_locked(const virtual_bag::TransactionContext& txn,
                      virtual_bag::TxnStage failed_at, const char* reason);
void isolate_pending_locked(const char* reason, const char* detail);
void isolate_ext2orig_failure_locked(const virtual_bag::TransactionContext& txn,
                                    virtual_bag::TxnStage phase, const char* reason,
                                    const char* detail);
bool original_inventory_contains_payload_locked(const uint8_t* payload, int payload_size,
                                                int target_bag);
int original_inventory_payload_slot_locked(const uint8_t* payload, int payload_size,
                                           int target_bag);
int original_inventory_handle_slot_locked(qol::contracts::QolItemHandle expected,
                                          int target_bag);
void recover_pending_transaction_locked(std::unique_lock<std::mutex>& lock);
bool route_deferred_extension_to_original_locked(std::unique_lock<std::mutex>& lock);
bool route_projected_session_drop_locked(std::unique_lock<std::mutex>& lock,
                                         int64_t x, int64_t y);
bool handle_bag_drop_release_locked(std::unique_lock<std::mutex>& lock,
                                    int64_t x, int64_t y);
bool apply_extension_material_to_slot_locked(std::unique_lock<std::mutex>& lock,
                                             int src_bag, int src_slot, int dst_bag,
                                             int dst_slot, void* material, void* target_item);
bool remove_item_direct_unlocked(std::unique_lock<std::mutex>& lock, int bag, int slot,
                                 void* expected_item, const char* context);

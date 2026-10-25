#pragma once

#include "contracts/slot_data.h"

#include <cstdint>

void extension_bag_session_set_blocked(bool blocked);
void extension_bag_session_set_admitted_epoch(uint64_t epoch);
void extension_bag_session_set_handle(qol::contracts::QolSlotDataHandle handle);
qol::contracts::QolSlotDataHandle extension_bag_session_exchange_handle();
qol::contracts::QolSlotDataHandle extension_bag_session_handle();
uint64_t extension_bag_session_epoch();
bool extension_bag_session_admitted_locked();

bool persist_state_locked(bool force = false);
bool extension_bag_ready_locked();
int extension_storage_index(int logical_bag);
bool virtual_bag_module_save_validate_locked();
bool extension_bag_module_slot_has_native_object(int bag, int slot);
bool virtual_bag_prepare_save_slot_load();
uint64_t extension_bag_isolation_now_ms();
void extension_bag_flush_dirty_staging(const qol::contracts::QolSlotDataApi* api);
bool extension_bag_slot_data_register(qol::contracts::QolOwnerToken owner,
                                      const qol::contracts::QolSlotDataApi* api);
qol::contracts::QolSlotDataRequest extension_bag_slot_data_unregister(
    const qol::contracts::QolSlotDataApi* api);
bool extension_bag_slot_data_request_settled(const qol::contracts::QolSlotDataApi* api,
                                              qol::contracts::QolSlotDataRequest request);

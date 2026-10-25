#pragma once

#include "contracts/slot_data.h"

#include <cstdint>
#include <string>

void extension_bag_session_set_blocked(bool blocked);
void extension_bag_session_set_admitted_epoch(uint64_t epoch);
void extension_bag_session_set_handle(qol::contracts::QolSlotDataHandle handle);
qol::contracts::QolSlotDataHandle extension_bag_session_exchange_handle();
qol::contracts::QolSlotDataHandle extension_bag_session_handle();
uint64_t extension_bag_session_epoch();
bool extension_bag_session_admitted_locked();

void ensure_state_loaded_locked();
bool persist_state_locked(bool force = false);
bool extension_bag_ready_locked();
std::string extension_bag_not_ready_error_locked();
int extension_internal_bag(int logical_bag);
bool virtual_bag_module_save_validate_locked();
bool virtual_bag_prepare_save_slot_load();

std::string extension_bag_status_json_locked_bridge();
std::string extension_bag_not_ready_error_locked_bridge();
bool extension_bag_ready_locked_bridge();
int extension_internal_bag_bridge(int logical_bag);
std::string extension_bag_view_result_json_bridge(bool ok, const char* error);

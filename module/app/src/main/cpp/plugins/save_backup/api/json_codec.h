#pragma once

#include <string>
#include <vector>

#include "contracts/host_sdk.h"
#include "plugins/save_backup/model/types.h"

namespace save_backup::api {

std::string result_ok(const qol::contracts::QolHostSdk* sdk);
std::string result_error(const qol::contracts::QolHostSdk* sdk, const char* message);
std::string entry_json(const Entry& entry);
std::string entry_response(const Entry& entry);
std::string export_response(const Entry& entry, bool deduplicated);
std::string list_response(const std::vector<Entry>& entries);
std::string files_response(const std::vector<BackupFile>& files);
std::string null_backup_response();
std::string delete_response();
std::string deleted_slot_response(int slot);

}  // namespace save_backup::api

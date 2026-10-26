#pragma once

#include <cstdint>
#include <string>

#include "../../../contracts/game_host.h"
#include "../../../contracts/host_sdk.h"
#include "../../../contracts/save_facts.h"
#include "../../../contracts/world_facts.h"
#include "../../../contracts/game_ports/save_call_port.h"
#include "plugins/save_backup/model/types.h"

// save_backup L4：备份用例的公共入口。
// 当前保留字符串结果适配以维持既有 HTTP/UI 契约。

namespace save_backup {
namespace usecases {

void save_backup_bind_context(const qol::contracts::QolHostSdk* sdk,
                              const qol::contracts::SaveFactsApi* save_facts,
                              const qol::contracts::QolSaveCallPort* save_call,
                              const qol::contracts::QolGameHost* game,
                              qol::contracts::QolOwnerToken owner_token);
void save_backup_bind_world_facts(const qol::contracts::WorldFactsApi* world_facts);
const qol::contracts::QolHostSdk* save_backup_host_sdk();
const qol::contracts::QolGameHost* save_backup_game_host();
const qol::contracts::SaveFactsApi* save_backup_save_facts();
const qol::contracts::QolSaveCallPort* save_backup_save_call();
std::string save_backup_result_ok();
std::string save_backup_result_error(const char* message);
void save_backup_init(const char* data_dir, const char* external_files_dir);
void save_backup_refresh_slot_meta_cache();
bool save_backup_export_slot_to_dir(int32_t slot, const char* dir, const char* file_name,
                                    std::string* out_error);
void save_backup_autobackup_pre_save(int32_t slot);
void save_backup_set_map_names(const char* json);
bool save_backup_slot_file_exists(int32_t slot);
std::string save_backup_map_name(int map_id);
const char* save_backup_class_name(int class_idx);
std::string save_backup_list_json();
std::string save_backup_list_files_json();
std::string save_backup_entry_json(const char* name);
std::string save_backup_export_json(int slot);
std::string save_backup_import_json(const char* checksum, int slot);
std::string save_backup_auto_entry_json(int slot);
std::string save_backup_import_auto_json(int source_slot, int target_slot);

// L4 typed query APIs（面板直接消费；JSON 适配仍保留给 HTTP/测试）。
Outcome<BackupFilesResult> list_backup_files();
Outcome<EntryResult> describe_backup(const std::string& name);
Outcome<EntryResult> auto_entry(int slot);

}  // namespace usecases
}  // namespace save_backup

#include "feature.h"

#include "transaction/transactions.h"
#include "usecases/backup_usecases.h"

void save_backup_bind_context(const qol::contracts::QolHostSdk* sdk,
                              const qol::contracts::SaveFactsApi* save_facts,
                              const qol::contracts::QolSaveCallPort* save_call,
                              const qol::contracts::QolGameHost* game,
                              qol::contracts::QolOwnerToken owner_token) {
    save_backup::usecases::save_backup_bind_context(sdk, save_facts, save_call, game, owner_token);
}
void save_backup_bind_world_facts(const qol::contracts::WorldFactsApi* world_facts) {
    save_backup::usecases::save_backup_bind_world_facts(world_facts);
}
const qol::contracts::QolHostSdk* save_backup_host_sdk() { return save_backup::usecases::save_backup_host_sdk(); }
const qol::contracts::QolGameHost* save_backup_game_host() { return save_backup::usecases::save_backup_game_host(); }
const qol::contracts::SaveFactsApi* save_backup_save_facts() { return save_backup::usecases::save_backup_save_facts(); }
const qol::contracts::QolSaveCallPort* save_backup_save_call() { return save_backup::usecases::save_backup_save_call(); }
std::string save_backup_result_ok() { return save_backup::usecases::save_backup_result_ok(); }
std::string save_backup_result_error(const char* message) { return save_backup::usecases::save_backup_result_error(message); }
void save_backup_init(const char* data_dir, const char* external_files_dir) { save_backup::usecases::save_backup_init(data_dir, external_files_dir); }
void save_backup_recover_deletes() { save_backup::usecases::transaction::recover_deletes(); }
void save_backup_refresh_slot_meta_cache() { save_backup::usecases::save_backup_refresh_slot_meta_cache(); }
bool save_backup_export_slot_to_dir(int32_t slot, const char* dir, const char* file_name, std::string* out_error) { return save_backup::usecases::save_backup_export_slot_to_dir(slot, dir, file_name, out_error); }
void save_backup_autobackup_pre_save(int32_t slot) { save_backup::usecases::save_backup_autobackup_pre_save(slot); }
void save_backup_set_map_names(const char* json) { save_backup::usecases::save_backup_set_map_names(json); }
bool save_backup_slot_file_exists(int32_t slot) { return save_backup::usecases::save_backup_slot_file_exists(slot); }
std::string save_backup_map_name(int map_id) { return save_backup::usecases::save_backup_map_name(map_id); }
const char* save_backup_class_name(int class_idx) { return save_backup::usecases::save_backup_class_name(class_idx); }
std::string save_backup_list_json() { return save_backup::usecases::save_backup_list_json(); }
std::string save_backup_list_files_json() { return save_backup::usecases::save_backup_list_files_json(); }
std::string save_backup_entry_json(const char* name) { return save_backup::usecases::save_backup_entry_json(name); }
std::string save_backup_export_json(int slot) { return save_backup::usecases::save_backup_export_json(slot); }
std::string save_backup_import_json(const char* checksum, int slot) { return save_backup::usecases::save_backup_import_json(checksum, slot); }
std::string save_backup_auto_entry_json(int slot) { return save_backup::usecases::save_backup_auto_entry_json(slot); }
std::string save_backup_import_auto_json(int source_slot, int target_slot) { return save_backup::usecases::save_backup_import_auto_json(source_slot, target_slot); }
std::string save_backup_delete_json(const char* checksum) { return save_backup::usecases::transaction::delete_json(checksum); }
void save_backup_slot_deleted_sync(int32_t slot) { save_backup::usecases::transaction::slot_deleted_sync(slot); }
std::string save_backup_delete_slot_json(int slot) { return save_backup::usecases::transaction::delete_slot_json(slot); }

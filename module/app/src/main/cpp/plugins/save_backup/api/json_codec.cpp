#include "json_codec.h"
#include "contracts/result.h"

namespace save_backup::api {

std::string result_ok(const qol::contracts::QolHostSdk* sdk) {
    char buffer[64] = {};
    uint32_t size = 0;
    if (sdk != nullptr && sdk->result.write_ok != nullptr &&
        qol::contracts::succeeded(sdk->result.write_ok(buffer, sizeof(buffer), &size))) {
        return std::string(buffer, size);
    }
    return "{\"ok\":false,\"error\":\"result unavailable\"}";
}

std::string result_error(const qol::contracts::QolHostSdk* sdk, const char* message) {
    char buffer[256] = {};
    uint32_t size = 0;
    if (sdk != nullptr && sdk->result.write_error != nullptr && message != nullptr &&
        qol::contracts::succeeded(sdk->result.write_error(message, buffer, sizeof(buffer), &size))) {
        return std::string(buffer, size);
    }
    return "{\"ok\":false,\"error\":\"result unavailable\"}";
}

std::string entry_json(const Entry& entry) { return ::save_backup::entry_json(entry); }

std::string entry_response(const Entry& entry) {
    return "{\"ok\":true,\"backup\":" + ::save_backup::entry_json(entry) + "}";
}

std::string export_response(const Entry& entry, bool deduplicated) {
    std::string out = "{\"ok\":true,\"backup\":" + ::save_backup::entry_json(entry);
    if (deduplicated) out += ",\"deduplicated\":true";
    return out + "}";
}

std::string list_response(const std::vector<Entry>& entries) {
    std::string out = "{\"ok\":true,\"backups\":[";
    bool first = true;
    for (const Entry& entry : entries) {
        if (!first) out += ",";
        out += ::save_backup::entry_json(entry);
        first = false;
    }
    return out + "]}";
}

std::string files_response(const std::vector<BackupFile>& files) {
    std::string out = "{\"ok\":true,\"files\":[";
    for (size_t i = 0; i < files.size(); ++i) {
        if (i != 0) out += ",";
        out += "{\"name\":\"" + ::save_backup::json_escape(files[i].name) +
               "\",\"size\":" + std::to_string(files[i].size) +
               ",\"mtime_ms\":" + std::to_string(files[i].mtime_ms) + "}";
    }
    return out + "]}";
}

std::string null_backup_response() { return "{\"ok\":true,\"backup\":null}"; }
std::string delete_response() { return "{\"ok\":true}"; }
std::string deleted_slot_response(int slot) {
    return "{\"ok\":true,\"deleted_slot\":" + std::to_string(slot) + "}";
}

}  // namespace save_backup::api

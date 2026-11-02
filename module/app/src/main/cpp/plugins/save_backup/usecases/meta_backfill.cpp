#include "meta_backfill.h"

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "../log.h"
#include "../persistence/persistence.h"
#include "../state/state.h"

namespace save_backup::usecases::meta_backfill {

bool backfill_entry_meta_from_plain(const std::string& path,
                                   const std::vector<uint8_t>& bytes,
                                   save_backup::ParsedBundle& parsed) {
    long long level = -1;
    long long class_idx = -1;
    const bool has_level = save_backup::json_find_int(parsed.meta_json, "hero_level", level);
    const bool has_class = save_backup::json_find_int(parsed.meta_json, "class_idx", class_idx);
    int plain_level = -1;
    int plain_class = -1;
    if (!save_backup::parse_plain_main_character(parsed.orig_plain, plain_level, plain_class) ||
        plain_level < 0 || plain_class < 0) {
        return false;
    }
    // 已与明文一致则不动；缺字段/旧值 -1/运行期槽结构写错的旧值都按明文修正。
    const char* cls = save_backup::state::className(plain_class);
    const std::string expected_class_name = cls != nullptr ? cls : std::string();
    std::string meta_class_name;
    save_backup::json_find_string(parsed.meta_json, "class_name", meta_class_name);
    if (has_level && level == plain_level && has_class && class_idx == plain_class &&
        meta_class_name == expected_class_name) {
        return false;
    }

    long long map_id = 0;
    long long hero_index = -1;
    long long save_version = 0;
    long long save_time = 0;
    long long warehouse_count = 0;
    long long module_state = -1;
    std::string original_sha256;
    std::string module_sha256;
    std::string checksum;
    std::string warehouse_sha256;
    save_backup::json_find_int(parsed.meta_json, "map_id", map_id);
    save_backup::json_find_int(parsed.meta_json, "hero_index", hero_index);
    save_backup::json_find_int(parsed.meta_json, "save_version", save_version);
    save_backup::json_find_int(parsed.meta_json, "save_time", save_time);
    save_backup::json_find_int(parsed.meta_json, "warehouse_count", warehouse_count);
    save_backup::json_find_int(parsed.meta_json, "module_state", module_state);
    save_backup::json_find_string(parsed.meta_json, "original_sha256", original_sha256);
    save_backup::json_find_string(parsed.meta_json, "module_sha256", module_sha256);
    save_backup::json_find_string(parsed.meta_json, "checksum", checksum);
    save_backup::json_find_string(parsed.meta_json, "warehouse_sha256", warehouse_sha256);
    const std::string new_meta = save_backup::build_meta_json(
        parsed.source_slot, parsed.export_time_ms, static_cast<int>(map_id), plain_level,
        static_cast<int>(hero_index), static_cast<int>(save_version), save_time, original_sha256,
        module_sha256, checksum, plain_class, expected_class_name,
        static_cast<int>(warehouse_count), warehouse_sha256, static_cast<int>(module_state));
    if (new_meta.size() > 0xFFFFu || bytes.size() < 6 + parsed.meta_json.size()) return false;

    // 原文件布局：... | u16 metaLen | metaJson | u32 crc32；前缀与尾部分段逐字节保留。
    const size_t prefix_len = bytes.size() - 4 - 2 - parsed.meta_json.size();
    std::vector<uint8_t> out;
    out.reserve(prefix_len + 2 + new_meta.size() + 4);
    out.insert(out.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(prefix_len));
    out.push_back(static_cast<uint8_t>(new_meta.size() >> 8));
    out.push_back(static_cast<uint8_t>(new_meta.size()));
    out.insert(out.end(), new_meta.begin(), new_meta.end());
    const uint32_t crc = save_backup::crc32_ieee(out.data(), out.size());
    out.push_back(static_cast<uint8_t>(crc >> 24));
    out.push_back(static_cast<uint8_t>(crc >> 16));
    out.push_back(static_cast<uint8_t>(crc >> 8));
    out.push_back(static_cast<uint8_t>(crc));
    {
        // 与删除/导入的 bundle 操作串行，避免 backfill 的 rename 把刚删除的文件重新写回。
        std::lock_guard<std::mutex> lk(save_backup::persistence::file_mutex());
        if (!save_backup::persistence::write_file_bytes_atomic(path, out.data(), out.size())) {
            SAVE_BACKUP_LOG_WARN("backfill entry meta write failed: %s", path.c_str());
            return false;
        }
    }
    parsed.meta_json = new_meta;
    parsed.size_bytes = static_cast<long long>(out.size());
    SAVE_BACKUP_LOG_INFO("backfill entry meta: %s level=%d class=%d", path.c_str(), plain_level,
                         plain_class);
    return true;
}

}  // namespace save_backup::usecases::meta_backfill

#include "state.h"

#include <atomic>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace save_backup {
namespace state {
namespace detail {

std::shared_ptr<const std::unordered_map<int, std::string>> parseMapNames(
    const std::string& json) {
    auto table = std::make_shared<std::unordered_map<int, std::string>>();
    if (json.empty()) return table;
    // 极简扁平解析：仅识别 "数字": "字符串" 对（Kotlin JSONObject.toString 输出）。
    // 字符级扫描足够（n ≤ 416 条记录，体积 < 32KB），不引入第三方依赖。
    size_t i = 0;
    const size_t n = json.size();
    while (i < n) {
        while (i < n && (json[i] == '{' || json[i] == '}' || json[i] == ',' ||
                         json[i] == ' ' || json[i] == '\t' || json[i] == '\n' ||
                         json[i] == '\r')) {
            ++i;
        }
        if (i >= n) break;
        size_t key_start = i;
        if (json[i] == '"') ++key_start;
        i = key_start;
        while (i < n && json[i] != ':' && json[i] != ',') ++i;
        if (i >= n || json[i] != ':') break;
        std::string key = json.substr(key_start, i - key_start);
        while (!key.empty() &&
               (key.back() == '"' || key.back() == ' ' || key.back() == '\t')) {
            key.pop_back();
        }
        ++i;
        while (i < n && (json[i] == ' ' || json[i] == '\t')) ++i;
        std::string value;
        if (i < n && json[i] == '"') {
            ++i;
            const size_t value_start = i;
            while (i < n && json[i] != '"') ++i;
            value = json.substr(value_start, i - value_start);
            if (i < n) ++i;
        } else {
            const size_t value_start = i;
            while (i < n && json[i] != ',' && json[i] != '}') ++i;
            value = json.substr(value_start, i - value_start);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.pop_back();
            }
        }
        int map_id = -1;
        try {
            map_id = std::stoi(key);
        } catch (...) {
            map_id = -1;
        }
        if (map_id >= 0 && !value.empty()) (*table)[map_id] = std::move(value);
    }
    return table;
}

}  // namespace detail

std::shared_ptr<const Context> g_context = std::make_shared<const Context>();
std::shared_ptr<const std::unordered_map<int, std::string>> g_map_names =
    std::make_shared<const std::unordered_map<int, std::string>>();
std::shared_ptr<const std::array<SlotMeta, 3>> g_slot_meta =
    std::make_shared<const std::array<SlotMeta, 3>>();

Context snapshot() { return *std::atomic_load(&g_context); }

void bind(Context context) {
    std::atomic_store(&g_context, std::make_shared<const Context>(std::move(context)));
}

void unbind() { bind(Context{}); }

void setDirs(const std::string& data_dir, const std::string& external_dir) {
    Context next = snapshot();
    next.data_dir = data_dir;
    next.external_dir = external_dir;
    bind(std::move(next));
}

void setMapNames(const std::string& json) {
    std::atomic_store(&g_map_names, detail::parseMapNames(json));
}

std::string mapName(int map_id) {
    const auto table = std::atomic_load(&g_map_names);
    if (table == nullptr) return std::string();
    const auto it = table->find(map_id);
    return it == table->end() ? std::string() : it->second;
}

std::unordered_map<int, std::string> mapNamesSnapshot() {
    const auto table = std::atomic_load(&g_map_names);
    return table == nullptr ? std::unordered_map<int, std::string>() : *table;
}

void publishSlotMeta(const std::array<SlotMeta, 3>& meta) {
    std::atomic_store(&g_slot_meta,
                      std::make_shared<const std::array<SlotMeta, 3>>(meta));
}

std::array<SlotMeta, 3> slotMeta() { return *std::atomic_load(&g_slot_meta); }

SlotMeta slotMeta(int slot) {
    if (slot < 0 || slot > 2) return SlotMeta{};
    return slotMeta()[static_cast<size_t>(slot)];
}

const char* className(int class_idx) {
    static const char* const kNames[6] = {"黑暗骑士", "忍者", "黑魔导", "祭司",
                                          "暗影猎手", "狂战士"};
    // 与迁移前一致：非法/未知返回空串（非 nullptr），调用方按“无职业名”退化。
    if (class_idx < 0 || class_idx >= 6) return "";
    return kNames[class_idx];
}

void refreshSlotMetaOnGameThread() {
    const Context ctx = snapshot();
    if (ctx.save_facts == nullptr || ctx.save_facts->slot_exists == nullptr) return;
    std::array<SlotMeta, 3> fresh{};
    for (int slot = 0; slot < 3; ++slot) {
        fresh[static_cast<size_t>(slot)].valid = true;
        int32_t exists = 0;
        if (!qol::contracts::succeeded(ctx.save_facts->slot_exists(slot, &exists)) ||
            exists == 0) {
            continue;
        }
        int32_t value = 0;
        if (ctx.save_facts->slot_hero_index != nullptr &&
            qol::contracts::succeeded(ctx.save_facts->slot_hero_index(slot, &value))) {
            fresh[static_cast<size_t>(slot)].hero_index = value;
        }
        if (ctx.save_facts->hero_level != nullptr &&
            qol::contracts::succeeded(ctx.save_facts->hero_level(slot, &value))) {
            fresh[static_cast<size_t>(slot)].hero_level = value;
        }
        if (ctx.save_facts->hero_class != nullptr &&
            qol::contracts::succeeded(ctx.save_facts->hero_class(slot, &value))) {
            fresh[static_cast<size_t>(slot)].class_idx = value;
        }
    }
    publishSlotMeta(fresh);
}

}  // namespace state
}  // namespace save_backup

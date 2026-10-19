#include "plugin.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#include "contracts/http.h"
#include "contracts/static_data.h"
#include "json.h"

namespace qol::features::core_data {
namespace {

constexpr char kPluginId[] = "core.data";
constexpr char kPluginVersion[] = "0.1.0";
constexpr uint32_t kMaxAssetBytes = 16 * 1024 * 1024;
constexpr uint32_t kMaxTotalAssetBytes = 32 * 1024 * 1024;
constexpr uint32_t kMaxAssetCount = 128;
constexpr char kNotFound[] = "{\"ok\":false,\"error\":\"not found\"}";
constexpr char kNotImplemented[] = "{\"ok\":false,\"error\":\"not implemented\"}";

const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kHttpRegistryCapabilityId,
     {contracts::kHttpRegistryVersion.major, contracts::kHttpRegistryVersion.minor}},
    {contracts::kHttpDispatchCapabilityId,
     {contracts::kHttpDispatchVersion.major, contracts::kHttpDispatchVersion.minor}},
};

std::mutex g_catalog_mutex;
std::map<std::string, std::string> g_assets;
std::map<std::string, std::string> g_staging_assets;
uint32_t g_staging_expected_files = 0;
uint32_t g_staging_expected_bytes = 0;
uint32_t g_staging_bytes = 0;
bool g_import_in_progress = false;
bool g_catalog_ready = false;
const contracts::QolHostSdk* g_sdk = nullptr;
contracts::QolStaticDataApi g_static_data_api{};

// maps/tiles.json 体积较大且 static.data 调用只读，首次调用时整体解码一次后复用，
// 避免每次 map_info/map_tiles 都复制并解析原始资产。
struct CachedMap {
    int32_t width = contracts::kStaticMapTileDim;
    int32_t height = contracts::kStaticMapTileDim;
    std::array<uint8_t, contracts::kStaticMapTileSize> tiles{};
};

std::mutex g_map_cache_mutex;
std::unordered_map<int32_t, CachedMap> g_map_cache;
bool g_map_cache_ready = false;

QolResult invalid_argument(const char* message) {
    return contracts::error(kQolErrInvalidArgument, message,
                            static_cast<uint32_t>(std::strlen(message)));
}

QolResult write_response(contracts::HttpRes* response, uint16_t status,
                         const std::string& body) {
    if (response == nullptr) return invalid_argument("null response");
    response->status = status;
    if (response->body == nullptr || response->body_capacity < body.size()) {
        response->body_size = static_cast<uint32_t>(body.size());
        return contracts::error(kQolErrBlocked, "http response buffer too small", 31);
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    return contracts::ok();
}

QolResult error_response(contracts::HttpRes* response, uint16_t status, const char* body) {
    return write_response(response, status, body);
}

QolResult not_found(contracts::HttpRes* response) {
    {
        std::lock_guard<std::mutex> lock(g_catalog_mutex);
        if (!g_catalog_ready) return error_response(response, 503, "{\"ok\":false,\"error\":\"not ready\"}");
    }
    return error_response(response, 404, kNotFound);
}

std::string path_parameter(const contracts::HttpReq& request, const char* wanted) {
    for (uint32_t i = 0; i < request.path_param_count; ++i) {
        const contracts::HttpPathParam& param = request.path_params[i];
        if (param.name != nullptr && std::string(param.name, param.name_len) == wanted) {
            return std::string(param.value, param.value_len);
        }
    }
    return {};
}

int32_t parse_int(const std::string& text, int32_t* out) {
    if (text.empty() || out == nullptr) return 0;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || end == nullptr || *end != '\0' || value < INT32_MIN ||
        value > INT32_MAX) return 0;
    *out = static_cast<int32_t>(value);
    return 1;
}

int64_t scalar_int(const JsonValue* value, int64_t fallback) {
    if (value == nullptr || value->type != JsonValue::Type::kNumber) return fallback;
    char* end = nullptr;
    const long long parsed = std::strtoll(value->text.c_str(), &end, 10);
    return end != value->text.c_str() && end != nullptr && *end == '\0' ? parsed : fallback;
}

bool valid_asset_name(const char* path, uint32_t size) {
    if (path == nullptr || size == 0 || size > 255 || path[0] == '/' || path[size - 1] == '/')
        return false;
    uint32_t segment_start = 0;
    for (uint32_t i = 0; i <= size; ++i) {
        if (i < size && (path[i] == '\\' || path[i] == '\0')) return false;
        if (i == size || path[i] == '/') {
            const std::string segment(path + segment_start, i - segment_start);
            if (segment.empty() || segment == "." || segment == "..") return false;
            segment_start = i + 1;
        }
    }
    return true;
}

bool copy_asset(const char* path, std::string* out) {
    if (out == nullptr || path == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_catalog_mutex);
    if (!g_catalog_ready) return false;
    const auto found = g_assets.find(path);
    if (found == g_assets.end()) return false;
    *out = found->second;
    return true;
}

bool copy_asset(const std::string& path, std::string* out) {
    return copy_asset(path.c_str(), out);
}

bool object_records(const std::string& json, JsonValue* root, const JsonValue** records) {
    if (!parse_json(json, root) || root->type != JsonValue::Type::kObject) return false;
    const JsonValue* values = root->find("records");
    if (values == nullptr || values->type != JsonValue::Type::kArray) return false;
    *records = values;
    return true;
}

std::string lowercase_ascii(std::string value) {
    for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

std::string trim_ascii(const std::string& value) {
    size_t first = 0;
    size_t last = value.size();
    while (first < last && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    return value.substr(first, last - first);
}

bool base64_decode(const std::string& input, std::vector<uint8_t>* output) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (output == nullptr) return false;
    output->clear();
    uint32_t accumulator = 0;
    uint32_t bits = 0;
    bool padding = false;
    for (const unsigned char ch : input) {
        if (std::isspace(ch)) continue;
        if (ch == '=') {
            padding = true;
            continue;
        }
        if (padding) return false;
        const char* found = std::strchr(kAlphabet, ch);
        if (found == nullptr) return false;
        accumulator = (accumulator << 6) | static_cast<uint32_t>(found - kAlphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output->push_back(static_cast<uint8_t>((accumulator >> bits) & 0xff));
        }
    }
    return bits < 6;
}

bool ensure_map_cache() {
    {
        std::lock_guard<std::mutex> lock(g_map_cache_mutex);
        if (g_map_cache_ready) return true;
    }
    // 缓存发布与资产导入共用 catalog 锁：导入完成会清空缓存，避免发布上一版资产。
    std::lock_guard<std::mutex> catalog_lock(g_catalog_mutex);
    if (!g_catalog_ready) return false;
    const auto asset = g_assets.find("maps/tiles.json");
    if (asset == g_assets.end()) return false;
    JsonValue root;
    if (!parse_json(asset->second, &root) || root.type != JsonValue::Type::kObject) return false;
    std::unordered_map<int32_t, CachedMap> parsed;
    for (size_t i = 0; i < root.keys.size() && i < root.values.size(); ++i) {
        const std::string& key = root.keys[i];
        if (key.size() < 2 || key[0] != 'm') continue;
        int32_t map_id = 0;
        if (!parse_int(key.substr(1), &map_id)) continue;
        const JsonValue& entry = root.values[i];
        if (entry.type != JsonValue::Type::kObject) return false;
        std::vector<uint8_t> bytes;
        if (!base64_decode(entry.opt_string("tiles"), &bytes) ||
            bytes.size() < contracts::kStaticMapTileSize)
            return false;
        const int64_t width = scalar_int(entry.find("width"), contracts::kStaticMapTileDim);
        const int64_t height = scalar_int(entry.find("height"), contracts::kStaticMapTileDim);
        if (width <= 0 || width > INT32_MAX || height <= 0 || height > INT32_MAX) return false;
        CachedMap map;
        map.width = static_cast<int32_t>(width);
        map.height = static_cast<int32_t>(height);
        std::memcpy(map.tiles.data(), bytes.data(), contracts::kStaticMapTileSize);
        parsed[map_id] = map;
    }
    std::lock_guard<std::mutex> cache_lock(g_map_cache_mutex);
    g_map_cache.swap(parsed);
    g_map_cache_ready = true;
    return true;
}

void invalidate_map_cache() {
    std::lock_guard<std::mutex> lock(g_map_cache_mutex);
    g_map_cache.clear();
    g_map_cache_ready = false;
}

QolResult map_list(const contracts::HttpReq*, contracts::HttpRes* response, void*) {
    std::string source;
    JsonValue root;
    if (!copy_asset("tables/MAPINFOBASE.json", &source) || !parse_json(source, &root) ||
        root.type != JsonValue::Type::kObject) return not_found(response);
    const JsonValue* records = root.find("records");
    if (records == nullptr || records->type != JsonValue::Type::kArray) return not_found(response);
    std::string body = "{\"maps\":[";
    bool first = true;
    for (size_t i = 0; i < records->items.size(); ++i) {
        const JsonValue& record = records->items[i];
        if (record.type != JsonValue::Type::kObject) continue;
        const JsonValue* u16 = record.find("u16");
        const int64_t map_id = u16 == nullptr ? -1 : u16->opt_int(0, -1);
        if (map_id < 0 || map_id > INT32_MAX) continue;
        if (!first) body.push_back(',');
        first = false;
        body += "{\"map_id\":" + std::to_string(map_id) + ",\"name\":" +
                quote_json(record.opt_string("text_0")) + "}";
    }
    body += "]}";
    return write_response(response, 200, body);
}

QolResult map_detail(const contracts::HttpReq& request, contracts::HttpRes* response) {
    int32_t map_id = 0;
    if (!parse_int(path_parameter(request, "map_id"), &map_id)) return not_found(response);
    std::string source;
    JsonValue root;
    if (!copy_asset("tables/MAPINFOBASE.json", &source) || !parse_json(source, &root) ||
        root.type != JsonValue::Type::kObject) return not_found(response);
    const JsonValue* records = root.find("records");
    if (records == nullptr || records->type != JsonValue::Type::kArray) return not_found(response);
    if (map_id >= 0 && static_cast<size_t>(map_id) < records->items.size()) {
        const JsonValue& record = records->items[static_cast<size_t>(map_id)];
        if (record.type != JsonValue::Type::kObject) return not_found(response);
        const JsonValue* u16 = record.find("u16");
        const int64_t text_id = u16 == nullptr ? -1 : u16->opt_int(0, -1);
        return write_response(response, 200,
            "{\"map_id\":" + std::to_string(map_id) + ",\"text_id\":" +
            std::to_string(text_id) + ",\"name\":" + quote_json(record.opt_string("text_0")) +
            ",\"raw\":" + stringify_json(record) + "}");
    }
    for (size_t i = 0; i < records->items.size(); ++i) {
        const JsonValue& record = records->items[i];
        if (record.type != JsonValue::Type::kObject) continue;
        const JsonValue* u16 = record.find("u16");
        if (u16 != nullptr && u16->opt_int(0, -1) == map_id) {
            return write_response(response, 200,
                "{\"map_id\":" + std::to_string(map_id) + ",\"index\":" +
                std::to_string(i) + ",\"text_id\":" + std::to_string(map_id) +
                ",\"name\":" + quote_json(record.opt_string("text_0")) + ",\"raw\":" +
                stringify_json(record) + "}");
        }
    }
    return not_found(response);
}

QolResult map_tiles(const contracts::HttpReq& request, contracts::HttpRes* response) {
    int32_t map_id = 0;
    if (!parse_int(path_parameter(request, "map_id"), &map_id)) return not_found(response);
    std::string source;
    JsonValue root;
    if (!copy_asset("maps/tiles.json", &source) || !parse_json(source, &root) ||
        root.type != JsonValue::Type::kObject) return not_found(response);
    const std::string key = "m" + std::to_string(map_id);
    const JsonValue* entry = root.find(key.c_str());
    if (entry == nullptr || entry->type != JsonValue::Type::kObject) return not_found(response);
    const std::string encoded = entry->opt_string("tiles");
    if (encoded.empty()) return not_found(response);
    std::vector<uint8_t> bytes;
    if (!base64_decode(encoded, &bytes) || bytes.size() < 4096) return not_found(response);
    const JsonValue* width_value = entry->find("width");
    const JsonValue* height_value = entry->find("height");
    const int64_t width = scalar_int(width_value, 64);
    const int64_t height = scalar_int(height_value, 64);
    std::string body = "{\"map_id\":" + std::to_string(map_id) +
                       ",\"src\":\"static\",\"width\":" + std::to_string(width) +
                       ",\"height\":" + std::to_string(height) +
                       ",\"size\":64,\"encoding\":\"array\",\"tiles\":[";
    for (uint32_t y = 0; y < 64; ++y) {
        if (y != 0) body.push_back(',');
        body.push_back('[');
        for (uint32_t x = 0; x < 64; ++x) {
            if (x != 0) body.push_back(',');
            body += std::to_string(bytes[y * 64 + x]);
        }
        body.push_back(']');
    }
    body += "]}";
    return write_response(response, 200, body);
}

QolResult map_exits(const contracts::HttpReq& request, contracts::HttpRes* response) {
    int32_t map_id = 0;
    if (!parse_int(path_parameter(request, "map_id"), &map_id)) return not_found(response);
    std::string source;
    JsonValue root;
    if (!copy_asset("maps/exits.json", &source) || !parse_json(source, &root) ||
        root.type != JsonValue::Type::kObject) return not_found(response);
    const std::string key = "m" + std::to_string(map_id);
    const JsonValue* map = root.find(key.c_str());
    if (map == nullptr || map->type != JsonValue::Type::kObject) return not_found(response);
    const JsonValue* exits = map->find("exits");
    if (exits == nullptr || exits->type != JsonValue::Type::kArray) return not_found(response);
    return write_response(response, 200,
        "{\"map_id\":" + std::to_string(map_id) +
        ",\"src\":\"static\",\"exits\":" +
        stringify_json(*exits) + "}");
}

QolResult table_list(const contracts::HttpReq*, contracts::HttpRes* response, void*) {
    std::string source;
    JsonValue root;
    if (!copy_asset("manifest.json", &source) || !parse_json(source, &root) ||
        root.type != JsonValue::Type::kObject) return not_found(response);
    const JsonValue* tables = root.find("tables");
    const std::string raw = tables != nullptr && tables->type == JsonValue::Type::kArray
                                ? stringify_json(*tables) : "[]";
    return write_response(response, 200, "{\"tables\":" + raw + "}");
}

QolResult table_get(const contracts::HttpReq& request, contracts::HttpRes* response) {
    const std::string table = path_parameter(request, "table");
    if (table.empty() || table.find('/') != std::string::npos || table.find("..") != std::string::npos)
        return not_found(response);
    std::string source;
    if (!copy_asset("tables/" + table + ".json", &source)) return not_found(response);
    return write_response(response, 200, source);
}

QolResult table_search(const contracts::HttpReq& request, contracts::HttpRes* response) {
    std::string table = path_parameter(request, "table");
    if (table.empty() || table.find('/') != std::string::npos || table.find("..") != std::string::npos)
        return not_found(response);
    std::string query;
    const int32_t query_status = decode_query_value(request.query, request.query_len, "q", &query);
    if (query_status != 1) return error_response(response, 400, "{\"ok\":false,\"error\":\"bad request\"}");
    std::string source;
    JsonValue root;
    const JsonValue* records = nullptr;
    if (!copy_asset("tables/" + table + ".json", &source) ||
        !object_records(source, &root, &records)) return not_found(response);
    const std::string keyword = lowercase_ascii(trim_ascii(query));
    if (keyword.empty()) {
        return write_response(response, 200,
            "{\"items\":" + stringify_json(*records) + "}");
    }
    std::string body = "{\"items\":[";
    bool first = true;
    for (size_t i = 0; i < records->items.size(); ++i) {
        const JsonValue& record = records->items[i];
        if (record.type != JsonValue::Type::kObject) continue;
        const std::string name = record.opt_string("text_0");
        if (lowercase_ascii(name).find(keyword) == std::string::npos) continue;
        if (!first) body.push_back(',');
        first = false;
        body += "{\"index\":" + std::to_string(i) + ",\"name\":" + quote_json(name) +
                ",\"raw\":" + stringify_json(record) + "}";
    }
    body += "]}";
    return write_response(response, 200, body);
}

QolResult table_download(const contracts::HttpReq*, contracts::HttpRes* response, void*) {
    return error_response(response, 501, kNotImplemented);
}

QolResult events_get(const contracts::HttpReq*, contracts::HttpRes* response, void*) {
    std::string source;
    return copy_asset("reverse/events.json", &source) ? write_response(response, 200, source)
                                                       : not_found(response);
}

QolResult text_get(const contracts::HttpReq& request, contracts::HttpRes* response) {
    std::string lang;
    if (decode_query_value(request.query, request.query_len, "lang", &lang) != 1)
        return error_response(response, 400, "{\"ok\":false,\"error\":\"bad request\"}");
    if (lang.empty() || lang.find('/') != std::string::npos || lang.find("..") != std::string::npos)
        return not_found(response);
    std::string source;
    return copy_asset("text/" + lang + ".json", &source) ? write_response(response, 200, source)
                                                           : not_found(response);
}

QolResult help_get(const contracts::HttpReq*, contracts::HttpRes* response, void*) {
    return error_response(response, 501, kNotImplemented);
}

QolResult download_all(const contracts::HttpReq*, contracts::HttpRes* response, void*) {
    return error_response(response, 501, kNotImplemented);
}

const contracts::HttpEndpointDescriptor kHttpEndpoints[] = {
    {contracts::HttpMethod::kGet, "/api/world/maps/list", kPluginId,
     contracts::HttpHandlerKind::kQuery, 0, 1, &map_list, nullptr, nullptr},
    {contracts::HttpMethod::kGet, "/api/world/maps/{map_id}", kPluginId,
     contracts::HttpHandlerKind::kQuery, 0, 1,
     [](const contracts::HttpReq* request, contracts::HttpRes* response, void*) {
          return request == nullptr ? invalid_argument("null request") : map_detail(*request, response);
      }, nullptr, nullptr},
    {contracts::HttpMethod::kGet, "/api/world/maps/{map_id}/tiles", kPluginId,
     contracts::HttpHandlerKind::kQuery, 0, 1,
     [](const contracts::HttpReq* request, contracts::HttpRes* response, void*) {
          return request == nullptr ? invalid_argument("null request") : map_tiles(*request, response);
      }, nullptr, nullptr},
    {contracts::HttpMethod::kGet, "/api/world/maps/{map_id}/exits", kPluginId,
     contracts::HttpHandlerKind::kQuery, 0, 1,
     [](const contracts::HttpReq* request, contracts::HttpRes* response, void*) {
          return request == nullptr ? invalid_argument("null request") : map_exits(*request, response);
      }, nullptr, nullptr},
    {contracts::HttpMethod::kGet, "/api/system/tables", kPluginId,
     contracts::HttpHandlerKind::kQuery, 0, 1, &table_list, nullptr, nullptr},
    {contracts::HttpMethod::kGet, "/api/system/tables/story-events", kPluginId,
     contracts::HttpHandlerKind::kQuery, 0, 1, &events_get, nullptr, nullptr},
    {contracts::HttpMethod::kGet, "/api/system/tables/text", kPluginId,
     contracts::HttpHandlerKind::kQuery, 0, 1,
     [](const contracts::HttpReq* request, contracts::HttpRes* response, void*) {
          return request == nullptr ? invalid_argument("null request") : text_get(*request, response);
      }, nullptr, nullptr},
    {contracts::HttpMethod::kGet, "/api/system/tables/{table}", kPluginId,
     contracts::HttpHandlerKind::kQuery, 0, 1,
     [](const contracts::HttpReq* request, contracts::HttpRes* response, void*) {
          return request == nullptr ? invalid_argument("null request") : table_get(*request, response);
      }, nullptr, nullptr},
    {contracts::HttpMethod::kGet, "/api/system/tables/{table}/search", kPluginId,
     contracts::HttpHandlerKind::kQuery, 0, 1,
     [](const contracts::HttpReq* request, contracts::HttpRes* response, void*) {
          return request == nullptr ? invalid_argument("null request") : table_search(*request, response);
      }, nullptr, nullptr},
    {contracts::HttpMethod::kGet, "/api/system/tables/{table}/download", kPluginId,
     contracts::HttpHandlerKind::kQuery, 0, 1, &table_download, nullptr, nullptr},
    {contracts::HttpMethod::kGet, "/api/system/help", kPluginId,
     contracts::HttpHandlerKind::kQuery, 0, 1, &help_get, nullptr, nullptr},
    {contracts::HttpMethod::kGet, "/api/system/download", kPluginId,
     contracts::HttpHandlerKind::kQuery, 0, 1, &download_all, nullptr, nullptr},
};

QolResult static_map_name(int32_t map_id, char* out, uint32_t capacity, uint32_t* out_size) {
    std::string source;
    JsonValue root;
    const JsonValue* records = nullptr;
    if (!copy_asset("tables/MAPINFOBASE.json", &source) ||
        !object_records(source, &root, &records) || map_id < 0 ||
        static_cast<size_t>(map_id) >= records->items.size())
        return contracts::error(kQolErrNotFound, "map not found", 13);
    const JsonValue& record = records->items[static_cast<size_t>(map_id)];
    if (record.type != JsonValue::Type::kObject) return contracts::error(kQolErrNotFound, "map not found", 13);
    const std::string name = record.opt_string("text_0");
    if (out_size == nullptr) return invalid_argument("null output size");
    *out_size = static_cast<uint32_t>(name.size());
    if (name.size() > capacity || (name.size() > 0 && out == nullptr))
        return contracts::error(kQolErrBlocked, "static output buffer too small", 29);
    if (!name.empty()) std::memcpy(out, name.data(), name.size());
    return contracts::ok();
}

QolResult static_map_exits(int32_t map_id, contracts::QolStaticMapExit* out,
                           uint32_t capacity, uint32_t* out_count) {
    std::string source;
    JsonValue root;
    if (!copy_asset("maps/exits.json", &source) || !parse_json(source, &root) ||
        root.type != JsonValue::Type::kObject)
        return contracts::error(kQolErrNotReady, "static exits unavailable", 24);
    const std::string key = "m" + std::to_string(map_id);
    const JsonValue* map = root.find(key.c_str());
    const JsonValue* exits = map == nullptr ? nullptr : map->find("exits");
    if (exits == nullptr || exits->type != JsonValue::Type::kArray)
        return contracts::error(kQolErrNotFound, "map exits not found", 19);
    if (out_count == nullptr) return invalid_argument("null output count");
    *out_count = static_cast<uint32_t>(exits->items.size());
    if (exits->items.size() > capacity || (!exits->items.empty() && out == nullptr))
        return contracts::error(kQolErrBlocked, "static output buffer too small", 29);
    for (size_t i = 0; i < exits->items.size(); ++i) {
        const JsonValue& value = exits->items[i];
        const JsonValue* x = value.find("x");
        const JsonValue* y = value.find("y");
        const JsonValue* target_map = value.find("targetMapId");
        const JsonValue* target_x = value.find("targetX");
        const JsonValue* target_y = value.find("targetY");
        if (x == nullptr || y == nullptr || target_map == nullptr || target_x == nullptr || target_y == nullptr)
            return contracts::error(kQolErrInvalidArgument, "invalid map exits", 17);
        // exits.json 的字段是对象成员，不是数组元素：必须用 scalar_int 读取。
        // 哨兵取 INT64_MIN，避免把缺失/非数字与合法 int32 值混淆。
        constexpr int64_t kInvalidScalar = INT64_MIN;
        const int64_t values[5] = {
            scalar_int(x, kInvalidScalar), scalar_int(y, kInvalidScalar),
            scalar_int(target_map, kInvalidScalar), scalar_int(target_x, kInvalidScalar),
            scalar_int(target_y, kInvalidScalar)};
        for (const int64_t field : values) {
            if (field == kInvalidScalar || field < INT32_MIN || field > INT32_MAX)
                return contracts::error(kQolErrInvalidArgument, "invalid map exits", 17);
        }
        out[i] = {static_cast<int32_t>(values[0]), static_cast<int32_t>(values[1]),
                  static_cast<int32_t>(values[2]), static_cast<int32_t>(values[3]),
                  static_cast<int32_t>(values[4])};
    }
    return contracts::ok();
}

QolResult static_map_info(int32_t map_id, contracts::QolStaticMapInfo* out) {
    if (out == nullptr) return invalid_argument("null map info");
    if (!ensure_map_cache()) return contracts::error(kQolErrNotReady, "static tiles unavailable", 25);
    std::lock_guard<std::mutex> lock(g_map_cache_mutex);
    const auto found = g_map_cache.find(map_id);
    if (found == g_map_cache.end())
        return contracts::error(kQolErrNotFound, "map tiles not found", 19);
    *out = {map_id, found->second.width, found->second.height};
    return contracts::ok();
}

QolResult static_map_tiles(int32_t map_id, uint8_t* out_tiles, uint32_t capacity,
                           uint32_t* out_size) {
    if (out_tiles == nullptr || out_size == nullptr ||
        capacity < contracts::kStaticMapTileSize)
        return invalid_argument("invalid map tiles output");
    if (!ensure_map_cache()) return contracts::error(kQolErrNotReady, "static tiles unavailable", 25);
    std::lock_guard<std::mutex> lock(g_map_cache_mutex);
    const auto found = g_map_cache.find(map_id);
    if (found == g_map_cache.end())
        return contracts::error(kQolErrNotFound, "map tiles not found", 19);
    std::memcpy(out_tiles, found->second.tiles.data(), contracts::kStaticMapTileSize);
    *out_size = contracts::kStaticMapTileSize;
    return contracts::ok();
}

contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kStaticDataCapabilityId, contracts::kStaticDataVersion, &g_static_data_api},
};

int plugin_install(contracts::PluginContext& context) {
    if (context.sdk == nullptr) return 1;
    g_static_data_api = {&static_map_name, &static_map_exits, &static_map_info, &static_map_tiles};
    g_sdk = context.sdk;
    return 0;
}

void plugin_deactivate(contracts::PluginContext& context) {
    (void)context;
    g_sdk = nullptr;
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_sdk != nullptr;
    out.error = nullptr;
}

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"core.data.catalog", 1, contracts::FeatureCategory::kHidden,
     contracts::FeatureType::kToggle, "静态数据服务", "",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId, kPluginVersion,
    0, nullptr,
    static_cast<uint32_t>(sizeof(kOptionalRequires) / sizeof(kOptionalRequires[0])),
    kOptionalRequires,
    1, kProvides,
    0, nullptr,
    0, nullptr,
    &plugin_install, &plugin_deactivate, nullptr, nullptr, &plugin_fill_status, nullptr,
    nullptr, 0, nullptr, 0,
    kFeatures, 1,
    kHttpEndpoints, static_cast<uint32_t>(sizeof(kHttpEndpoints) / sizeof(kHttpEndpoints[0])),
    nullptr,
    nullptr, 0,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

QolResult begin_asset_import(uint32_t expected_files, uint32_t expected_bytes) {
    if (expected_files > kMaxAssetCount || expected_bytes > kMaxTotalAssetBytes)
        return invalid_argument("static data import exceeds limits");
    std::lock_guard<std::mutex> lock(g_catalog_mutex);
    g_staging_assets.clear();
    g_staging_expected_files = expected_files;
    g_staging_expected_bytes = expected_bytes;
    g_staging_bytes = 0;
    g_import_in_progress = true;
    return contracts::ok();
}

QolResult import_asset(const char* path, uint32_t path_size, const uint8_t* bytes,
                       uint32_t byte_size) {
    if (!valid_asset_name(path, path_size) || byte_size > kMaxAssetBytes ||
        (bytes == nullptr && byte_size != 0)) return invalid_argument("invalid static asset");
    std::lock_guard<std::mutex> lock(g_catalog_mutex);
    if (!g_import_in_progress) return contracts::error(kQolErrNotReady, "static import not started", 25);
    const std::string name(path, path_size);
    if (g_staging_assets.find(name) != g_staging_assets.end() ||
        g_staging_assets.size() >= kMaxAssetCount ||
        byte_size > kMaxTotalAssetBytes - g_staging_bytes)
        return invalid_argument("static asset import limit exceeded");
    const char* data = byte_size == 0 ? "" : reinterpret_cast<const char*>(bytes);
    g_staging_assets.emplace(name, std::string(data, byte_size));
    g_staging_bytes += byte_size;
    return contracts::ok();
}

QolResult finish_asset_import() {
    std::lock_guard<std::mutex> lock(g_catalog_mutex);
    if (!g_import_in_progress) return contracts::error(kQolErrNotReady, "static import not started", 25);
    if (g_staging_assets.size() != g_staging_expected_files ||
        g_staging_bytes != g_staging_expected_bytes) {
        g_staging_assets.clear();
        g_import_in_progress = false;
        g_catalog_ready = false;
        invalidate_map_cache();
        return invalid_argument("static asset import size mismatch");
    }
    g_assets.swap(g_staging_assets);
    g_staging_assets.clear();
    g_staging_bytes = 0;
    g_import_in_progress = false;
    g_catalog_ready = true;
    invalidate_map_cache();
    return contracts::ok();
}

void cancel_asset_import() {
    std::lock_guard<std::mutex> lock(g_catalog_mutex);
    g_staging_assets.clear();
    g_staging_bytes = 0;
    g_import_in_progress = false;
    g_catalog_ready = false;
    invalidate_map_cache();
}

}  // namespace qol::features::core_data

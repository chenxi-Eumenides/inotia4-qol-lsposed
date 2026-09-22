#pragma once

#include <cstdint>
#include <string>
#include <vector>

// item_facts 私有极简 JSON 解析器：只用于解析既有 native 库存/商店原始 JSON（数值/布尔/数组/
// 对象），不承载静态数据；插件不得依赖 infra/host 的解析实现（ARCHITECTURE.md §3.2）。
namespace qol::features::item_facts {

struct JsonValue {
    enum class Type : uint8_t { kNull, kBool, kNumber, kString, kArray, kObject };

    Type type = Type::kNull;
    bool boolean = false;
    long long integer = 0;
    std::string string;
    std::vector<JsonValue> items;
    // 对象成员用两个平行数组表示：pair<string, JsonValue> 会在 JsonValue 自身未完整时非法。
    std::vector<std::string> member_keys;
    std::vector<JsonValue> member_values;

    // 返回同名成员的第一个值；不存在返回 nullptr。仅用于扁平对象。
    const JsonValue* find(const char* key) const;
    bool as_int(long long* out) const;
    bool as_bool(bool* out) const;
};

// 解析 text[0,size)；失败返回 false 且不修改 out 之外的调用方状态。
bool json_parse(const char* text, uint32_t size, JsonValue* out);

}  // namespace qol::features::item_facts

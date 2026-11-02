#pragma once

#include <cstdint>

// character_facts 私有极简 JSON 切片器：只用于在既有 native 原始/富化 JSON 上按顶层
// 键名或数组下标取原样子串（保持转义与字段序，直接拼进响应体）。不承载静态数据；
// 插件不得依赖 infra/host 的解析实现（ARCHITECTURE.md §3.2）。
namespace qol::features::core_character {

struct JsonSlice {
    const char* data = nullptr;
    uint32_t size = 0;
};

// 对象顶层成员 key 的值切片（含嵌套括号/引号原样）。找不到或非对象返回 false。
bool json_member(const char* json, uint32_t size, const char* key, JsonSlice* out);

// 数组顶层第 index 个元素切片。越界或非数组返回 false。
bool json_element(const char* json, uint32_t size, int32_t index, JsonSlice* out);

// 数组顶层元素个数。非数组返回 false。
bool json_array_size(const char* json, uint32_t size, int32_t* out);

// 对象顶层整数字段。字段不存在或非整数返回 false。
bool json_int_member(const char* json, uint32_t size, const char* key, long long* out);

// 切片是否为字面量 null。
bool json_slice_is_null(const JsonSlice& slice);

}  // namespace qol::features::core_character

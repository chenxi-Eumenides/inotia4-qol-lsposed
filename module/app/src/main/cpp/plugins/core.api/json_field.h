#pragma once

#include <cstdint>
#include <string>

namespace qol::features::core_api {

// 从 JSON 对象文本中提取两个字符串字段（lifecycle body 的 id/action）。
// 返回 false 表示 body 不是合法 JSON 对象，对应 Kotlin JsonUtil.parseBody 失败；
// 字段缺失或值为非字符串标量时 found=false、值保持为空，对应 Kotlin optString 缺省空串。
// 解析在插件内自持，是为了不把 command handler 的 body 解析经契约外通道上抛。
bool extract_string_fields(const uint8_t* body, uint32_t body_len, const char* key_a,
                           std::string* out_a, bool* found_a, const char* key_b,
                           std::string* out_b, bool* found_b);

}  // namespace qol::features::core_api

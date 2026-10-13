#pragma once

#include <string>

// 纯 JSON 工具层：无游戏内存依赖，仅字符串构造辅助。

std::string json_escape(const char* s);

namespace qol::game::json {

inline bool is_ready_state_snapshot(const std::string& json) {
    return !json.empty() && json != "{\"error\":\"not ready\"}" &&
           json != "{\"error\":\"not in game\"}";
}

}  // namespace qol::game::json

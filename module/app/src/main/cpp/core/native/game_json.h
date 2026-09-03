#pragma once

#include <string>

// 纯 JSON 工具层：无游戏内存依赖，仅字符串构造辅助。

std::string json_escape(const char* s);

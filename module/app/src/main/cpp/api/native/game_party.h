#pragma once

#include <cstdint>
#include <string>

// 队伍域（parse 域）：队伍编成操作。

// 数据读取（read 域拆分）：队伍/佣兵 JSON 构造。
std::string build_party_json();
std::string build_mercenaries_json();

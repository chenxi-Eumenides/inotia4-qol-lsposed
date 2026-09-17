#include "host/variant_json.h"

#include <cstdint>

#include "core/native/game_json.h"
#include "core/native/game_variant.h"

namespace qol::host {

std::string host_variant_json() {
    // 幂等初始化：未命中表时 game_variant() 仍有合法默认值，但先初始化才能拿到
    // 真实分类（首次调用会解析 maps 并查表，结果缓存）。
    game_variant_init();
    const GameVariant& variant = game_variant();

    std::string out = "{\"series\":\"";
    out += json_escape(game_series_name(variant.series));
    out += "\",\"version\":\"";
    out += json_escape(variant.version_label);
    out += "\",\"known\":";
    out += variant.known ? "true" : "false";
    out += ",\"capabilities\":";
    out += std::to_string(variant.capabilities);
    out += "}";
    return out;
}

}  // namespace qol::host

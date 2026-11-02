#include "plugins/extension_bag/runtime/response.h"

#include "contracts/game_cache.h"

std::string extension_bag_op_ok() {
    frame_cache_force_refresh();
    return "{\"ok\":true}";
}

std::string extension_bag_op_err(const char* message) {
    return std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
}

#include "plugins/core.item/runtime/core_item_internal.h"

#include <cstring>
#include <limits>
#include <string>

#include "plugins/core.item/http/response.h"

namespace qol::features::core_item {

Query kQueries[] = {
    Query::kInventory, Query::kMoney,        Query::kItems,
    Query::kBagInfo,   Query::kBagSlot,      Query::kShopItems,
};

QolResult response_error(contracts::HttpRes* response, uint16_t status, const char* message) {
    const std::string body = std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
    if (response == nullptr || response->body == nullptr || response->body_capacity < body.size()) {
        return contracts::error(kQolErrInternal, "http response buffer too small", 31);
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = status;
    return contracts::error(status == 503 ? kQolErrNotReady : kQolErrInvalidArgument, message,
                            static_cast<uint32_t>(std::strlen(message)));
}

QolResult response_body(contracts::HttpRes* response, const char* body, uint32_t size,
                        uint16_t status) {
    if (response == nullptr || response->body == nullptr || body == nullptr ||
        response->body_capacity < size) {
        return response_error(response, 500, "http response buffer too small");
    }
    std::memcpy(response->body, body, size);
    response->body_size = size;
    response->status = status;
    return contracts::ok();
}

bool parse_path_int(const contracts::HttpReq& request, const char* name, int32_t* out) {
    if (request.path_params == nullptr || name == nullptr || out == nullptr) return false;
    for (uint32_t i = 0; i < request.path_param_count; ++i) {
        const contracts::HttpPathParam& param = request.path_params[i];
        if (param.name == nullptr || param.name_len != std::strlen(name) ||
            std::strncmp(param.name, name, param.name_len) != 0) {
            continue;
        }
        if (param.value == nullptr || param.value_len == 0) return false;
        if (param.value_len > 10) return false;
        int32_t parsed = 0;
        for (uint32_t k = 0; k < param.value_len; ++k) {
            const char c = param.value[k];
            if (c < '0' || c > '9') return false;
            const int32_t digit = c - '0';
            if (parsed > (std::numeric_limits<int32_t>::max() - digit) / 10) return false;
            parsed = parsed * 10 + digit;
        }
        *out = parsed;
        return true;
    }
    return false;
}

QolResult query_handler(const contracts::HttpReq* request, contracts::HttpRes* response, void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return response_error(response, 400, "bad request");
    }
    ItemSnapshot snapshot{};
    if (!g_snapshot.read(snapshot)) return response_error(response, 503, "snapshot not ready");
    if (snapshot.state == SnapshotState::kNotInGame) {
        return response_body(response, kNotInGameBody,
                             static_cast<uint32_t>(sizeof(kNotInGameBody) - 1));
    }
    if (snapshot.state != SnapshotState::kReady) {
        return response_error(response, 503, "snapshot not ready");
    }

    const Query query = *static_cast<const Query*>(ctx);
    switch (query) {
        case Query::kInventory: {
            std::string body;
            if (!enrich_inventory(snapshot.inventory, snapshot.inventory_size, g_port, &body)) {
                return response_error(response, 500, "internal error");
            }
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kMoney: {
            const std::string body = "{\"money\":" + std::to_string(snapshot.money) + "}";
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kItems: {
            std::string body;
            if (!enrich_items_list(snapshot.inventory, snapshot.inventory_size, g_port, &body)) {
                return response_error(response, 500, "internal error");
            }
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kBagInfo: {
            int32_t bag = 0;
            if (!parse_path_int(*request, "bag", &bag)) {
                return response_error(response, 400, "bad request");
            }
            std::string body;
            bool found = false;
            if (!enrich_bag_info(snapshot.inventory, snapshot.inventory_size, bag, &body, &found)) {
                return response_error(response, 500, "internal error");
            }
            if (!found) return response_error(response, 404, "not found");
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kBagSlot: {
            int32_t bag = 0;
            int32_t slot = 0;
            if (!parse_path_int(*request, "bag", &bag) ||
                !parse_path_int(*request, "slot", &slot)) {
                return response_error(response, 400, "bad request");
            }
            if (slot < 0 || slot >= 16) return response_error(response, 404, "not found");
            std::string body;
            bool bag_found = false;
            if (!enrich_bag_slot(snapshot.inventory, snapshot.inventory_size, bag, slot, g_port,
                                 &body, &bag_found)) {
                return response_error(response, 500, "internal error");
            }
            if (!bag_found) return response_error(response, 404, "not found");
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kShopItems: {
            std::string body;
            if (!enrich_shop_items(snapshot.shop, snapshot.shop_size, g_port, &body)) {
                return response_error(response, 500, "internal error");
            }
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
    }
    return response_error(response, 500, "internal error");
}


}  // namespace qol::features::core_item

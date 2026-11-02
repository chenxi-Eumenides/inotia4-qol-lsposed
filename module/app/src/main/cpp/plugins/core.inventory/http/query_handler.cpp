#include "query_handler.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "plugins/core.inventory/bags/snapshot_store.h"
#include "plugins/core.inventory/http/response.h"
#include "plugins/core.inventory/runtime/context.h"

namespace qol::features::core_inventory {
namespace {

struct Cursor {
    int32_t space;
    int32_t bag;
    int32_t slot;
};

using InventoryItem = std::pair<const bags::BagSnapshot*, const bags::SlotRecord*>;

bool query_value(const contracts::HttpReq& request, const char* name, std::string_view* out) {
    if (name == nullptr || out == nullptr || request.query == nullptr) return false;
    const std::string_view query(request.query, request.query_len);
    const std::string_view wanted(name);
    size_t begin = 0;
    while (begin <= query.size()) {
        const size_t end = query.find('&', begin);
        const size_t part_end = end == std::string_view::npos ? query.size() : end;
        const std::string_view part = query.substr(begin, part_end - begin);
        const size_t equals = part.find('=');
        const std::string_view key = part.substr(0, equals);
        if (key == wanted) {
            *out = equals == std::string_view::npos ? std::string_view{} :
                                                      part.substr(equals + 1);
            return true;
        }
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return false;
}

std::string decode_value(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t index = 0; index < raw.size(); ++index) {
        const char character = raw[index];
        if (character == '+') {
            out.push_back(' ');
            continue;
        }
        if (character == '%' && index + 2 < raw.size()) {
            const auto hex = [](char digit) -> int {
                if (digit >= '0' && digit <= '9') return digit - '0';
                if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
                if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
                return -1;
            };
            const int high = hex(raw[index + 1]);
            const int low = hex(raw[index + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>(high * 16 + low));
                index += 2;
                continue;
            }
        }
        out.push_back(character);
    }
    return out;
}

bool parse_nonnegative(std::string_view text, int32_t* out) {
    if (out == nullptr || text.empty()) return false;
    int32_t parsed = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') return false;
        const int32_t digit = character - '0';
        if (parsed > (std::numeric_limits<int32_t>::max() - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    *out = parsed;
    return true;
}

bool parse_optional_index(const contracts::HttpReq& request, const char* name, bool* present,
                          int32_t* out) {
    std::string_view value;
    *present = query_value(request, name, &value);
    if (!*present) return true;
    const std::string decoded = decode_value(value);
    return parse_nonnegative(decoded, out);
}

bool parse_cursor(std::string_view value, Cursor* out) {
    if (out == nullptr) return false;
    const size_t first = value.find(':');
    if (first == std::string_view::npos) return false;
    const size_t second = value.find(':', first + 1);
    if (second == std::string_view::npos || value.find(':', second + 1) != std::string_view::npos) {
        return false;
    }
    Cursor parsed{};
    if (!parse_nonnegative(value.substr(0, first), &parsed.space) ||
        !parse_nonnegative(value.substr(first + 1, second - first - 1), &parsed.bag) ||
        !parse_nonnegative(value.substr(second + 1), &parsed.slot)) {
        return false;
    }
    *out = parsed;
    return true;
}

bool parse_path_int(const contracts::HttpReq& request, const char* name, int32_t* out) {
    if (request.path_params == nullptr || name == nullptr || out == nullptr) return false;
    for (uint32_t i = 0; i < request.path_param_count; ++i) {
        const contracts::HttpPathParam& param = request.path_params[i];
        if (param.name == nullptr || param.name_len != std::strlen(name) ||
            std::strncmp(param.name, name, param.name_len) != 0) {
            continue;
        }
        if (param.value == nullptr || param.value_len == 0 || param.value_len > 10) return false;
        return parse_nonnegative(std::string_view(param.value, param.value_len), out);
    }
    return false;
}

bool find_space_bag(const bags::InventorySnapshot& snapshot, int32_t space, int32_t bag,
                    const bags::BagSnapshot** out) {
    if (out == nullptr) return false;
    for (const bags::BagSnapshot& entry : snapshot.bags) {
        if (entry.space_id == space && entry.bag == bag) {
            *out = &entry;
            return true;
        }
    }
    return false;
}

bool resolve_flat_bag(const bags::InventorySnapshot& snapshot, int32_t flat,
                      const bags::BagSnapshot** out) {
    if (flat < 0 || out == nullptr) return false;
    int32_t index = 0;
    for (const bags::BagSnapshot& entry : snapshot.bags) {
        if (index == flat) {
            *out = &entry;
            return true;
        }
        if (index == std::numeric_limits<int32_t>::max()) return false;
        ++index;
    }
    return false;
}

bool parse_page_options(const contracts::HttpReq& request, int32_t* space, bool* has_space,
                        int32_t* bag, bool* has_bag, int32_t* limit,
                        bool* has_cursor, Cursor* cursor, const char** error) {
    if (!parse_optional_index(request, "space", has_space, space) ||
        !parse_optional_index(request, "bag", has_bag, bag)) {
        *error = "bad request";
        return false;
    }

    *limit = 100;
    std::string_view limit_value;
    if (query_value(request, "limit", &limit_value) &&
        (!parse_nonnegative(decode_value(limit_value), limit) || *limit <= 0 || *limit > 500)) {
        *error = "bad limit";
        return false;
    }

    *has_cursor = query_value(request, "cursor", &limit_value);
    if (*has_cursor) {
        const std::string decoded_cursor = decode_value(limit_value);
        if (decoded_cursor.empty()) {
            *has_cursor = false;
        } else if (!parse_cursor(decoded_cursor, cursor)) {
            *error = "bad cursor";
            return false;
        }
    }
    return true;
}

bool item_after_cursor(const bags::BagSnapshot& bag, const bags::SlotRecord& item,
                       const Cursor& cursor) {
    return std::tie(bag.space_id, bag.bag, item.slot) >
           std::tie(cursor.space, cursor.bag, cursor.slot);
}

QolResult inventory_page(const contracts::HttpReq& request, contracts::HttpRes* response,
                         bool include_bags, const bags::InventorySnapshot& snapshot) {
    int32_t space = 0;
    int32_t bag = 0;
    int32_t limit = 100;
    bool has_space = false;
    bool has_bag = false;
    bool has_cursor = false;
    Cursor cursor{};
    const char* option_error = nullptr;
    if (!parse_page_options(request, &space, &has_space, &bag, &has_bag, &limit,
                            &has_cursor, &cursor, &option_error)) {
        return response_error(response, 400, option_error);
    }

    const bags::BagSnapshot* selected_bag = nullptr;
    if (has_bag) {
        const bool found = has_space ? find_space_bag(snapshot, space, bag, &selected_bag)
                                     : resolve_flat_bag(snapshot, bag, &selected_bag);
        if (!found) return response_error(response, 404, "bag not found");
        space = selected_bag->space_id;
        bag = selected_bag->bag;
        has_space = true;
    }

    std::vector<InventoryItem> page;
    page.reserve(static_cast<size_t>(limit) + 1);
    for (const bags::BagSnapshot& entry : snapshot.bags) {
        if (has_space && entry.space_id != space) continue;
        if (has_bag && entry.bag != bag) continue;
        for (const bags::SlotRecord& item : entry.items) {
            if (has_cursor && !item_after_cursor(entry, item, cursor)) continue;
            page.emplace_back(&entry, &item);
            if (page.size() > static_cast<size_t>(limit)) break;
        }
        if (page.size() > static_cast<size_t>(limit)) break;
    }
    const bool has_more = page.size() > static_cast<size_t>(limit);
    if (has_more) page.pop_back();

    std::string body;
    if (include_bags) {
        body = "{\"bags\":[";
        bool first_bag = true;
        for (const bags::BagSnapshot& entry : snapshot.bags) {
            if (!first_bag) body.push_back(',');
            first_bag = false;
            body += "{\"space\":" + std::to_string(entry.space_id) +
                    ",\"bag\":" + std::to_string(entry.bag) +
                    ",\"capacity\":" + std::to_string(entry.capacity) +
                    ",\"occupied_slots\":" + std::to_string(entry.occupied_slots) + "}";
        }
        body += "],\"items\":[";
    } else {
        body = "{\"items\":[";
    }

    bool first_item = true;
    for (const InventoryItem& selected : page) {
        if (!first_item) body.push_back(',');
        first_item = false;
        std::string item_json;
        if (!enrich_snapshot_item(selected.first->space_id, selected.first->bag,
                                  *selected.second, g_port, true, &item_json)) {
            return response_error(response, 500, "internal error");
        }
        body += item_json;
    }
    body += "],\"next_cursor\":";
    if (has_more && !page.empty()) {
        const InventoryItem& last = page.back();
        body += "\"" + std::to_string(last.first->space_id) + ":" +
                std::to_string(last.first->bag) + ":" + std::to_string(last.second->slot) +
                "\"";
    } else {
        body += "null";
    }
    body += has_more ? ",\"has_more\":true}" : ",\"has_more\":false}";
    return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
}

QolResult bag_info(const contracts::HttpReq& request, contracts::HttpRes* response,
                   const bags::InventorySnapshot& snapshot) {
    int32_t bag = 0;
    if (!parse_path_int(request, "bag", &bag)) return response_error(response, 400, "bad request");
    int32_t space = 0;
    bool has_space = false;
    if (!parse_optional_index(request, "space", &has_space, &space)) {
        return response_error(response, 400, "bad request");
    }
    const bags::BagSnapshot* selected = nullptr;
    const bool found = has_space ? find_space_bag(snapshot, space, bag, &selected)
                                 : resolve_flat_bag(snapshot, bag, &selected);
    if (!found) return response_error(response, 404, "bag not found");
    const std::string body = "{\"space\":" + std::to_string(selected->space_id) +
                             ",\"bag\":" + std::to_string(selected->bag) +
                             ",\"capacity\":" + std::to_string(selected->capacity) +
                             ",\"occupied_slots\":" + std::to_string(selected->occupied_slots) +
                             "}";
    return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
}

QolResult bag_slot(const contracts::HttpReq& request, contracts::HttpRes* response,
                   const bags::InventorySnapshot& snapshot) {
    int32_t bag = 0;
    int32_t slot = 0;
    if (!parse_path_int(request, "bag", &bag) || !parse_path_int(request, "slot", &slot)) {
        return response_error(response, 400, "bad request");
    }
    int32_t space = 0;
    bool has_space = false;
    if (!parse_optional_index(request, "space", &has_space, &space)) {
        return response_error(response, 400, "bad request");
    }
    const bags::BagSnapshot* selected = nullptr;
    const bool found = has_space ? find_space_bag(snapshot, space, bag, &selected)
                                 : resolve_flat_bag(snapshot, bag, &selected);
    if (!found) return response_error(response, 404, "bag not found");
    if (slot >= selected->capacity) return response_error(response, 404, "slot not found");

    const auto item = std::lower_bound(
        selected->items.begin(), selected->items.end(), slot,
        [](const bags::SlotRecord& record, int32_t value) { return record.slot < value; });
    if (item == selected->items.end() || item->slot != slot) {
        return response_body(response, "null", 4);
    }
    std::string body;
    if (!enrich_snapshot_item(selected->space_id, selected->bag, *item, g_port, true, &body)) {
        return response_error(response, 500, "internal error");
    }
    return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
}

}  // namespace

Query kQueries[] = {
    Query::kInventory, Query::kMoney,        Query::kItems,
    Query::kBagInfo,   Query::kBagSlot,
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

QolResult query_handler(const contracts::HttpReq* request, contracts::HttpRes* response, void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return response_error(response, 400, "bad request");
    }
    const Query query = *static_cast<const Query*>(ctx);
    const std::shared_ptr<const bags::InventorySnapshot> snapshot =
        bags::inventory_snapshot_store().read();
    if (snapshot == nullptr) return response_error(response, 503, "not ready");
    switch (query) {
        case Query::kInventory:
            return inventory_page(*request, response, true, *snapshot);
        case Query::kItems:
            return inventory_page(*request, response, false, *snapshot);
        case Query::kMoney: {
            const std::string body = "{\"money\":" + std::to_string(snapshot->money) + "}";
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kBagInfo:
            return bag_info(*request, response, *snapshot);
        case Query::kBagSlot:
            return bag_slot(*request, response, *snapshot);
    }
    return response_error(response, 500, "internal error");
}

}  // namespace qol::features::core_inventory

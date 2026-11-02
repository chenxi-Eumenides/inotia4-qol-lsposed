#include "plugins/core.item/shop/buy.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "plugins/core.item/http/json_lite.h"
#include "plugins/core.item/http/write_result_inventory.h"
#include "plugins/core.item/runtime/core_item_internal.h"

namespace qol::features::core_item::shop {
namespace {

const JsonValue* member_last(const JsonValue& object, const char* key) {
    if (key == nullptr) return nullptr;
    for (size_t i = object.member_keys.size(); i > 0; --i) {
        if (object.member_keys[i - 1] == key) return &object.member_values[i - 1];
    }
    return nullptr;
}

bool parse_double_string(const std::string& text, double* out) {
    if (out == nullptr) return false;
    size_t begin = 0;
    size_t end_index = text.size();
    while (begin < end_index && static_cast<unsigned char>(text[begin]) <= 0x20) ++begin;
    while (end_index > begin && static_cast<unsigned char>(text[end_index - 1]) <= 0x20) {
        --end_index;
    }
    if (begin == end_index) return false;
    std::string trimmed = text.substr(begin, end_index - begin);
    const bool special = trimmed == "NaN" || trimmed == "+NaN" || trimmed == "-NaN" ||
                         trimmed == "Infinity" || trimmed == "+Infinity" ||
                         trimmed == "-Infinity";
    if (!special && !trimmed.empty() &&
        (trimmed.back() == 'f' || trimmed.back() == 'F' || trimmed.back() == 'd' ||
         trimmed.back() == 'D')) {
        trimmed.pop_back();
        if (trimmed.empty()) return false;
    }
    const size_t sign_offset = !trimmed.empty() &&
                                       (trimmed.front() == '+' || trimmed.front() == '-')
                                   ? 1
                                   : 0;
    if (trimmed.size() >= sign_offset + 2 && trimmed[sign_offset] == '0' &&
        (trimmed[sign_offset + 1] == 'x' || trimmed[sign_offset + 1] == 'X') &&
        trimmed.find_first_of("pP") == std::string::npos) {
        return false;
    }
    char* end = nullptr;
    const double parsed = std::strtod(trimmed.c_str(), &end);
    if (end == trimmed.c_str() || *end != '\0' ||
        ((std::isnan(parsed) || std::isinf(parsed)) && !special)) {
        return false;
    }
    *out = parsed;
    return true;
}

int32_t narrow_int(double value) {
    if (std::isnan(value)) return 0;
    if (value <= static_cast<double>(std::numeric_limits<int32_t>::min())) {
        return std::numeric_limits<int32_t>::min();
    }
    if (value >= static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(value);
}

int32_t opt_int(const JsonValue& object, const char* key, int32_t fallback) {
    const JsonValue* value = member_last(object, key);
    if (value == nullptr) return fallback;
    long long number = 0;
    if (value->type == JsonValue::Type::kNumber && value->as_int(&number)) {
        if (number < std::numeric_limits<int32_t>::min()) return std::numeric_limits<int32_t>::min();
        if (number > std::numeric_limits<int32_t>::max()) return std::numeric_limits<int32_t>::max();
        return static_cast<int32_t>(number);
    }
    double parsed = 0;
    if (value->type != JsonValue::Type::kString || !parse_double_string(value->string, &parsed)) {
        return fallback;
    }
    return narrow_int(parsed);
}

bool has_invalid_number(const JsonValue& object, const char* key) {
    const JsonValue* value = member_last(object, key);
    if (value == nullptr || value->type != JsonValue::Type::kNumber) return false;
    long long parsed = 0;
    return !value->as_int(&parsed);
}

bool parse_write_body(const std::string& body, JsonValue* object) {
    return object != nullptr && json_parse(body.data(), static_cast<uint32_t>(body.size()), object) &&
           object->type == JsonValue::Type::kObject;
}

bool copy_truncated(char* out, uint32_t capacity, uint32_t* out_size,
                    const std::string& body) {
    if (out == nullptr || out_size == nullptr) return false;
    const uint32_t copied = static_cast<uint32_t>(std::min<std::size_t>(body.size(), capacity));
    if (copied != 0) std::memcpy(out, body.data(), copied);
    *out_size = static_cast<uint32_t>(body.size());
    return true;
}

bool write_shop_buy_result(const contracts::QolItemWriteArgs& args, char* out,
                           uint32_t capacity, uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr || g_item_state_write == nullptr) return false;
    *out_size = 0;
    write_result::InventorySnapshot inventory_before{};
    (void)write_result::capture_inventory_snapshot(&inventory_before);

    contracts::QolItemInventoryStateWriteResult operation_data{};
    const QolResult operation = g_item_state_write->shop_buy(args.i0, &operation_data);
    if (!contracts::succeeded(operation)) {
        const char* message = operation.message != nullptr ? operation.message : "item operation failed";
        const uint32_t message_size = operation.message != nullptr
                                          ? operation.message_len
                                          : static_cast<uint32_t>(std::strlen(message));
        const std::string body = std::string("{\"ok\":false,\"error\":\"") +
                                 std::string(message, message_size) + "\"}";
        (void)copy_buffer(out, capacity, out_size, body.data(),
                          static_cast<uint32_t>(body.size()));
        return false;
    }
    request_inventory_refresh();

    write_result::InventorySnapshot inventory_after{};
    if (!write_result::capture_inventory_snapshot(&inventory_after)) {
        constexpr char kInventoryUnavailable[] =
            "{\"ok\":false,\"error\":\"inventory state unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kInventoryUnavailable,
                          sizeof(kInventoryUnavailable) - 1);
        return false;
    }
    int64_t money_after = 0;
    if (!contracts::succeeded(money(&money_after))) {
        constexpr char kMoneyUnavailable[] = "{\"ok\":false,\"error\":\"money unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kMoneyUnavailable,
                          sizeof(kMoneyUnavailable) - 1);
        return false;
    }

    std::string body = "{\"ok\":true,\"price\":" + std::to_string(operation_data.value) +
                       ",\"money\":" + std::to_string(money_after) + ",\"slots\":[";
    write_result::append_changed_slots(body, inventory_before, inventory_after);
    body += "]}";
    return copy_truncated(out, capacity, out_size, body);
}

}  // namespace

const ItemWriteRoute kBuyRoute{
    "/api/item/shop/buy_item", contracts::QolItemWriteKind::kShopBuy, &buy_command};

QolResult buy_command(const contracts::HttpReq* request, contracts::HttpRes* response,
                      void* ctx) {
    (void)ctx;
    if (request == nullptr || response == nullptr) return response_error(response, 400, "bad request");

    const std::string body(request->body != nullptr
                               ? reinterpret_cast<const char*>(request->body)
                               : "",
                           request->body_len);
    JsonValue body_object{};
    if (!parse_write_body(body, &body_object)) return response_error(response, 400, "bad request");
    if (has_invalid_number(body_object, "slot")) {
        return response_error(response, 400, "bad value for slot");
    }

    contracts::QolItemWriteArgs args{};
    args.kind = static_cast<int32_t>(contracts::QolItemWriteKind::kShopBuy);
    args.i0 = -1;
    args.i1 = -1;
    args.i2 = -1;
    args.i3 = -1;
    args.i4 = -1;
    args.i5 = -1;
    args.i0 = opt_int(body_object, "slot", -1);
    if (args.i0 < 0) return response_error(response, 400, "slot required");

    if (g_sdk == nullptr || g_sdk->commands.post_typed == nullptr) {
        return response_error(response, 503, "command port unavailable");
    }
    contracts::QolCommandTicket ticket = 0;
    if (!contracts::succeeded(g_sdk->commands.post_typed(
            kItemWriteHandlerId, &args, sizeof(args), &ticket)) ||
        ticket == 0) {
        return response_error(response, 503, "command queue unavailable");
    }
    std::string ack = "{\"ok\":true,\"ticket\":";
    ack += std::to_string(ticket);
    ack += "}";
    return response_body(response, ack.data(), static_cast<uint32_t>(ack.size()));
}

bool write_result_if_requested(const contracts::QolItemWriteArgs& args, char* out,
                               uint32_t capacity, uint32_t* out_size, bool* handled) {
    if (handled == nullptr) return false;
    *handled = args.kind == static_cast<int32_t>(contracts::QolItemWriteKind::kShopBuy);
    if (!*handled) return false;
    return write_shop_buy_result(args, out, capacity, out_size);
}

}  // namespace qol::features::core_item::shop

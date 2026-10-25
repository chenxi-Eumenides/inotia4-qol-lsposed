#include "plugins/core.item/runtime/core_item_internal.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include "plugins/core.item/http/json_lite.h"

namespace qol::features::core_item {
namespace {

const JsonValue* write_member_last(const JsonValue& object, const char* key) {
    if (key == nullptr) return nullptr;
    for (size_t i = object.member_keys.size(); i > 0; --i) {
        if (object.member_keys[i - 1] == key) return &object.member_values[i - 1];
    }
    return nullptr;
}

// 复刻 Android JSONObject.optXxx 语义：Double.parseDouble(trim 后字符串)，向零截断并饱和窄化。
// Kotlin 迁移前先校验字段；C++ 若按解析失败拒绝，会新增 400 并破坏逐字行为一致性。
bool write_parse_double_string(const std::string& text, double* out) {
    if (out == nullptr) {
        return false;
    }
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

int64_t write_narrow_long(double value) {
    constexpr double kLongLower = -9223372036854775808.0;
    constexpr double kLongUpperExclusive = 9223372036854775808.0;
    if (std::isnan(value)) return 0;
    if (value <= kLongLower) return std::numeric_limits<int64_t>::min();
    if (value >= kLongUpperExclusive) return std::numeric_limits<int64_t>::max();
    return static_cast<int64_t>(value);
}

int32_t write_narrow_int(double value) {
    if (std::isnan(value)) return 0;
    if (value <= static_cast<double>(std::numeric_limits<int32_t>::min())) {
        return std::numeric_limits<int32_t>::min();
    }
    if (value >= static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(value);
}

int32_t write_opt_int(const JsonValue& object, const char* key, int32_t fallback) {
    const JsonValue* value = write_member_last(object, key);
    if (value == nullptr) return fallback;
    long long number = 0;
    if (value->type == JsonValue::Type::kNumber && value->as_int(&number)) {
        if (number < std::numeric_limits<int32_t>::min()) return std::numeric_limits<int32_t>::min();
        if (number > std::numeric_limits<int32_t>::max()) return std::numeric_limits<int32_t>::max();
        return static_cast<int32_t>(number);
    }
    double parsed = 0;
    if (value->type != JsonValue::Type::kString ||
        !write_parse_double_string(value->string, &parsed)) {
        return fallback;
    }
    return write_narrow_int(parsed);
}

bool write_has_invalid_number(const JsonValue& object, const char* key) {
    const JsonValue* value = write_member_last(object, key);
    if (value == nullptr || value->type != JsonValue::Type::kNumber) return false;
    long long parsed = 0;
    return !value->as_int(&parsed);
}

bool write_has_member(const JsonValue& object, const char* key) {
    return write_member_last(object, key) != nullptr;
}

QolResult invalid_write_number_error(const char* key, contracts::HttpRes* response) {
    const std::string message = std::string("bad value for ") + key;
    return response_error(response, 400, message.c_str());
}

int64_t write_opt_long(const JsonValue& object, const char* key, int64_t fallback) {
    const JsonValue* value = write_member_last(object, key);
    if (value == nullptr) return fallback;
    long long number = 0;
    if (value->type == JsonValue::Type::kNumber && value->as_int(&number)) return number;
    double parsed = 0;
    if (value->type == JsonValue::Type::kString &&
        write_parse_double_string(value->string, &parsed)) {
        return write_narrow_long(parsed);
    }
    return fallback;
}

bool parse_write_body(const std::string& body, JsonValue* object) {
    return object != nullptr && json_parse(body.data(), static_cast<uint32_t>(body.size()), object) &&
           object->type == JsonValue::Type::kObject;
}


bool write_path_int(const contracts::HttpReq* request, const char* name, int32_t* out) {
    if (request == nullptr || request->path_params == nullptr || name == nullptr || out == nullptr) {
        return false;
    }
    const std::string target(name);
    for (uint32_t i = 0; i < request->path_param_count; ++i) {
        const contracts::HttpPathParam& param = request->path_params[i];
        if (param.name == nullptr || param.value == nullptr) continue;
        if (target.size() != param.name_len ||
            target.compare(0, target.size(), param.name, param.name_len) != 0) {
            continue;
        }
        if (param.value_len > 10) return false;
        int32_t value = 0;
        for (uint32_t k = 0; k < param.value_len; ++k) {
            const char c = param.value[k];
            if (c < '0' || c > '9') return false;
            const int32_t digit = c - '0';
            if (value > (std::numeric_limits<int32_t>::max() - digit) / 10) return false;
            value = value * 10 + digit;
        }
        *out = value;
        return true;
    }
    return false;
}

}  // namespace

ItemWriteRoute kItemWriteRoutes[kItemWriteRouteCount] = {
    {"/api/item/inventory/use_item", contracts::QolItemWriteKind::kUseItem},
    {"/api/item/inventory/sell_item", contracts::QolItemWriteKind::kSellItem},
    {"/api/item/inventory/move_item", contracts::QolItemWriteKind::kMoveItem},
    {"/api/item/inventory/discard_item", contracts::QolItemWriteKind::kDiscardItem},
    {"/api/item/inventory/accept_dice", contracts::QolItemWriteKind::kAcceptDice},
    {"/api/item/inventory/reject_dice", contracts::QolItemWriteKind::kRejectDice},
    {"/api/item/inventory/{role}/equip_item", contracts::QolItemWriteKind::kEquipItem},
    {"/api/item/inventory/{role}/unequip_item", contracts::QolItemWriteKind::kUnequipItem},
    {"/api/item/inventory/{role}/put_jewel", contracts::QolItemWriteKind::kPutJewel},
    {"/api/item/inventory/{role}/enchant", contracts::QolItemWriteKind::kEnchant},
    {"/api/item/shop/buy_item", contracts::QolItemWriteKind::kShopBuy},
    {"/api/op/inventory/add", contracts::QolItemWriteKind::kAddItem},
    {"/api/op/inventory/money", contracts::QolItemWriteKind::kSetMoney},
};

QolResult item_write_command(const contracts::HttpReq* request, contracts::HttpRes* response,
                             void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return response_error(response, 400, "bad request");
    }
    const contracts::QolItemWriteKind kind = *static_cast<const contracts::QolItemWriteKind*>(ctx);
    const std::string body(request->body != nullptr
                               ? reinterpret_cast<const char*>(request->body)
                               : "",
                           request->body_len);
    JsonValue body_object{};
    const bool body_valid = parse_write_body(body, &body_object);
    contracts::QolItemWriteArgs args{};
    args.kind = static_cast<int32_t>(kind);
    args.i0 = -1;
    args.i1 = -1;
    args.i2 = -1;
    args.i3 = -1;
    args.i4 = -1;
    args.i5 = -1;
    int32_t path_value = -1;
    if (kind != contracts::QolItemWriteKind::kAcceptDice &&
        kind != contracts::QolItemWriteKind::kRejectDice && !body_valid) {
        return response_error(response, 400, "bad request");
    }
    switch (kind) {
        case contracts::QolItemWriteKind::kUseItem:
        case contracts::QolItemWriteKind::kSellItem:
        case contracts::QolItemWriteKind::kDiscardItem:
            if (write_has_invalid_number(body_object, "bag")) return invalid_write_number_error("bag", response);
            if (write_has_invalid_number(body_object, "slot")) return invalid_write_number_error("slot", response);
            args.i0 = write_opt_int(body_object, "bag", -1);
            args.i1 = write_opt_int(body_object, "slot", -1);
            if (args.i0 < 0 || args.i1 < 0) return response_error(response, 400, "bag/slot required");
            break;
        case contracts::QolItemWriteKind::kMoveItem:
            if (write_has_invalid_number(body_object, "bag")) return invalid_write_number_error("bag", response);
            if (write_has_invalid_number(body_object, "slot")) return invalid_write_number_error("slot", response);
            if (write_has_invalid_number(body_object, "count")) return invalid_write_number_error("count", response);
            if (write_has_invalid_number(body_object, "to_bag")) return invalid_write_number_error("to_bag", response);
            if (write_has_invalid_number(body_object, "to_slot")) return invalid_write_number_error("to_slot", response);
            if (!write_has_member(body_object, "bag") || !write_has_member(body_object, "slot") ||
                !write_has_member(body_object, "count") || !write_has_member(body_object, "to_bag") ||
                !write_has_member(body_object, "to_slot")) {
                return response_error(response, 400,
                                      "bag/slot/count/to_bag/to_slot required (bag 0-10, 5=task bag excluded for extension)");
            }
            args.i0 = write_opt_int(body_object, "bag", -1);
            args.i1 = write_opt_int(body_object, "slot", -1);
            args.i2 = write_opt_int(body_object, "count", -1);
            args.i3 = write_opt_int(body_object, "to_bag", -1);
            args.i4 = write_opt_int(body_object, "to_slot", -1);
            if (args.i0 < 0 || args.i0 > 10 || args.i1 < 0 || args.i2 <= 0 || args.i3 < 0 ||
                args.i3 > 10 || args.i4 < -1) {
                return response_error(response, 400,
                                      "bag/slot/count/to_bag/to_slot required (bag 0-10, 5=task bag excluded for extension)");
            }
            break;
        case contracts::QolItemWriteKind::kAcceptDice:
        case contracts::QolItemWriteKind::kRejectDice:
            break;
        case contracts::QolItemWriteKind::kEquipItem:
            if (!write_path_int(request, "role", &path_value)) return response_error(response, 400, "bad request");
            args.i0 = path_value;
            if (write_has_invalid_number(body_object, "category")) return invalid_write_number_error("category", response);
            if (write_has_invalid_number(body_object, "bag")) return invalid_write_number_error("bag", response);
            if (write_has_invalid_number(body_object, "slot")) return invalid_write_number_error("slot", response);
            args.i5 = write_opt_int(body_object, "category", -1);
            args.i1 = write_opt_int(body_object, "bag", -1);
            args.i2 = write_opt_int(body_object, "slot", -1);
            if (args.i5 < 0 && (args.i1 < 0 || args.i2 < 0)) {
                return response_error(response, 400, "bag+slot or category required");
            }
            break;
        case contracts::QolItemWriteKind::kUnequipItem:
            if (!write_path_int(request, "role", &path_value)) return response_error(response, 400, "bad request");
            args.i0 = path_value;
            if (write_has_invalid_number(body_object, "slot")) return invalid_write_number_error("slot", response);
            args.i1 = write_opt_int(body_object, "slot", -1);
            if (args.i1 < 0) return response_error(response, 400, "slot required");
            break;
        case contracts::QolItemWriteKind::kPutJewel:
        case contracts::QolItemWriteKind::kEnchant:
            if (!write_path_int(request, "role", &path_value)) return response_error(response, 400, "bad request");
            args.i0 = path_value;
            if (write_has_invalid_number(body_object, "bag")) return invalid_write_number_error("bag", response);
            if (write_has_invalid_number(body_object, "slot")) return invalid_write_number_error("slot", response);
            if (write_has_invalid_number(body_object, "equip_slot")) return invalid_write_number_error("equip_slot", response);
            args.i1 = write_opt_int(body_object, "bag", -1);
            args.i2 = write_opt_int(body_object, "slot", -1);
            args.i3 = write_opt_int(body_object, "equip_slot", -1);
            if (args.i1 < 0 || args.i2 < 0 || args.i3 < 0) {
                return response_error(response, 400, "bag/slot/equip_slot required");
            }
            break;
        case contracts::QolItemWriteKind::kShopBuy:
            if (write_has_invalid_number(body_object, "slot")) return invalid_write_number_error("slot", response);
            args.i0 = write_opt_int(body_object, "slot", -1);
            if (args.i0 < 0) return response_error(response, 400, "slot required");
            break;
        case contracts::QolItemWriteKind::kAddItem:
            if (write_has_invalid_number(body_object, "category")) return invalid_write_number_error("category", response);
            if (write_has_invalid_number(body_object, "count")) return invalid_write_number_error("count", response);
            if (write_has_invalid_number(body_object, "socket")) return invalid_write_number_error("socket", response);
            if (write_has_invalid_number(body_object, "enhanceRemaining")) return invalid_write_number_error("enhanceRemaining", response);
            if (write_has_invalid_number(body_object, "rarity")) return invalid_write_number_error("rarity", response);
            args.i0 = write_opt_int(body_object, "category", -1);
            if (args.i0 < 0) return response_error(response, 400, "category required");
            args.i1 = write_opt_int(body_object, "count", 1);
            args.i2 = write_opt_int(body_object, "socket", 0);
            args.i3 = write_opt_int(body_object, "enhanceRemaining", 0);
            args.i4 = write_opt_int(body_object, "rarity", -1);
            break;
        case contracts::QolItemWriteKind::kSetMoney:
            if (write_has_invalid_number(body_object, "money")) return invalid_write_number_error("money", response);
            args.i64 = write_opt_long(body_object, "money", -1);
            if (args.i64 < 0) return response_error(response, 400, "money required");
            break;
    }
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
}  // namespace qol::features::core_item

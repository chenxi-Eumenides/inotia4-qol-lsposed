#include "plugins/core.item/runtime/core_item_internal.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include "plugins/core.item/bags/snapshot_store.h"
#include "plugins/core.item/http/json_lite.h"
#include "plugins/core.item/shop/buy.h"
#ifndef QOL_CORE_ITEM_SHOP_SEPARATE
#include "plugins/core.item/shop/buy.cpp"
#endif

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

struct WriteRef {
    int32_t space_id;
    int32_t bag;
    int32_t slot;
};

int32_t write_port_bag(const WriteRef& ref);

enum class WriteRefStatus {
    kOk,
    kInvalid,
    kTaskBag,
};

bool parse_optional_space(const JsonValue& object, const char* field, bool* present,
                          int32_t* out_space) {
    if (present == nullptr || out_space == nullptr) return false;
    const JsonValue* value = write_member_last(object, field);
    *present = value != nullptr;
    if (value == nullptr) return true;
    if (write_has_invalid_number(object, field)) return false;
    const int32_t space = write_opt_int(object, field, -1);
    if (space < 0) return false;
    *out_space = space;
    return true;
}

bool write_space_count(int32_t space_id, int32_t* out_count) {
    if (out_count == nullptr) return false;
    if (space_id >= contracts::kQolBagSpaceFirstRegistered) {
        // 优先用主线程已发布的快照；袋源回调不允许在 HTTP 解析线程调用。
        const auto snapshot = bags::inventory_snapshot_store().read();
        if (snapshot) {
            int32_t snapshot_count = 0;
            for (const bags::BagSnapshot& bag : snapshot->bags) {
                if (bag.space_id == space_id) ++snapshot_count;
            }
            if (snapshot_count > 0) {
                *out_count = snapshot_count;
                return true;
            }
        }
        const contracts::QolBagSource* source = find_bag_source(space_id);
        if (source == nullptr || source->count == nullptr) {
            *out_count = 0;
            return true;
        }
        const int32_t count = source->count();
        if (count < 0) return false;
        *out_count = count;
        return true;
    }
    if (out_count == nullptr || g_port == nullptr || g_port->read.bag_count == nullptr ||
        !contracts::qol_item_port_bag_space_size_valid(g_port)) {
        return false;
    }
    int32_t count = 0;
    const QolResult result = g_port->read.bag_count(space_id, &count);
    if (!contracts::succeeded(result)) {
        if (space_id != contracts::kQolBagSpaceOriginal &&
            (result.code == kQolErrUnsupported || result.code == kQolErrNotReady)) {
            *out_count = 0;
            return true;
        }
        return false;
    }
    if (count < 0) return false;
    *out_count = count;
    return true;
}

WriteRefStatus resolve_write_ref(int32_t requested_bag, bool has_space, int32_t requested_space,
                                 int32_t slot, bool allow_auto_slot, WriteRef* out) {
    if (out == nullptr || requested_bag < 0 || slot < (allow_auto_slot ? -1 : 0)) {
        return WriteRefStatus::kInvalid;
    }
    int32_t original_count = 0;
    int32_t c18_count = 0;
    if (!write_space_count(contracts::kQolBagSpaceOriginal, &original_count) ||
        !write_space_count(contracts::kQolBagSpaceVariant, &c18_count)) {
        return WriteRefStatus::kInvalid;
    }

    WriteRef ref{};
    if (has_space) {
        if (requested_space == contracts::kQolBagSpaceOriginal) {
            ref = {requested_space, requested_bag, slot};
        } else if (requested_space == contracts::kQolBagSpaceVariant ||
                   requested_space >= contracts::kQolBagSpaceFirstRegistered) {
            ref = {requested_space, requested_bag, slot};
        } else {
            return WriteRefStatus::kInvalid;
        }
    } else {
        int32_t remaining = requested_bag;
        if (remaining < original_count) {
            ref = {contracts::kQolBagSpaceOriginal, remaining, slot};
        } else {
            remaining -= original_count;
            if (remaining < c18_count) {
                ref = {contracts::kQolBagSpaceVariant, remaining, slot};
            } else {
                remaining -= c18_count;
                bool found = false;
                for (uint32_t index = 0; index < registered_bag_source_count(); ++index) {
                    const contracts::QolBagSource* source = registered_bag_source_at(index);
                    if (source == nullptr) continue;
                    int32_t source_count = 0;
                    if (!write_space_count(source->space_id, &source_count))
                        return WriteRefStatus::kInvalid;
                    if (remaining < source_count) {
                        ref = {source->space_id, remaining, slot};
                        found = true;
                        break;
                    }
                    remaining -= source_count;
                }
                if (!found) return WriteRefStatus::kInvalid;
            }
        }
    }

    // 注册源只做主线程动作前的索引透传；袋源回调与容量校验不能在 HTTP 线程执行。
    if (ref.space_id >= contracts::kQolBagSpaceFirstRegistered) {
        *out = ref;
        return WriteRefStatus::kOk;
    }
    int32_t count = 0;
    if (ref.space_id == contracts::kQolBagSpaceOriginal) count = original_count;
    else if (ref.space_id == contracts::kQolBagSpaceVariant) count = c18_count;
    if (ref.bag < 0 || ref.bag >= count) return WriteRefStatus::kInvalid;
    if (ref.space_id == contracts::kQolBagSpaceOriginal &&
        ref.bag == contracts::kQolTaskBagIndex) {
        return WriteRefStatus::kTaskBag;
    }
    const int32_t host_bag = write_port_bag(ref);
    if (host_bag < 0) return WriteRefStatus::kInvalid;
    const contracts::QolBagRef bag_ref{ref.space_id, ref.bag};
    contracts::QolBagInfo info{};
    const QolResult info_result =
        g_port != nullptr && g_port->read.bag_info != nullptr &&
                contracts::qol_item_port_bag_space_size_valid(g_port)
            ? g_port->read.bag_info(&bag_ref, &info)
            : contracts::error(kQolErrUnsupported, "bag info unavailable", 21);
    if (!contracts::succeeded(info_result) || info.capacity < 0 ||
        ((!allow_auto_slot || slot >= 0) && slot >= info.capacity)) {
        return WriteRefStatus::kInvalid;
    }
    *out = ref;
    return WriteRefStatus::kOk;
}

int32_t write_port_bag(const WriteRef& ref) {
    if (ref.space_id == contracts::kQolBagSpaceOriginal) return ref.bag;
    int32_t original_count = 0;
    int32_t c18_count = 0;
    if (!write_space_count(contracts::kQolBagSpaceOriginal, &original_count) ||
        !write_space_count(contracts::kQolBagSpaceVariant, &c18_count)) return -1;
    int64_t offset = original_count;
    if (ref.space_id == contracts::kQolBagSpaceVariant) return original_count + ref.bag;
    offset += c18_count;
    for (uint32_t index = 0; index < registered_bag_source_count(); ++index) {
        const contracts::QolBagSource* source = registered_bag_source_at(index);
        if (source == nullptr) continue;
        if (source->space_id == ref.space_id) {
            const int64_t flat = offset + ref.bag;
            return flat <= std::numeric_limits<int32_t>::max()
                       ? static_cast<int32_t>(flat)
                       : -1;
        }
        int32_t count = 0;
        if (!write_space_count(source->space_id, &count)) return -1;
        offset += count;
    }
    return -1;
}

QolResult write_ref_error(WriteRefStatus status, contracts::HttpRes* response,
                          const char* invalid_message) {
    if (status == WriteRefStatus::kTaskBag) {
        return response_error(response, 400, "task bag excluded");
    }
    return response_error(response, 400, invalid_message);
}

QolResult invalid_write_number_error(const char* key, contracts::HttpRes* response) {
    const std::string message = std::string("bad value for ") + key;
    return response_error(response, 400, message.c_str());
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
    {"/api/item/inventory/use_item", contracts::QolItemWriteKind::kUseItem, &item_write_command},
    {"/api/item/inventory/sell_item", contracts::QolItemWriteKind::kSellItem, &item_write_command},
    {"/api/item/inventory/discard_item", contracts::QolItemWriteKind::kDiscardItem, &item_write_command},
    {"/api/item/inventory/accept_dice", contracts::QolItemWriteKind::kAcceptDice, &item_write_command},
    {"/api/item/inventory/reject_dice", contracts::QolItemWriteKind::kRejectDice, &item_write_command},
    {"/api/item/inventory/{role}/equip_item", contracts::QolItemWriteKind::kEquipItem, &item_write_command},
    {"/api/item/inventory/{role}/unequip_item", contracts::QolItemWriteKind::kUnequipItem, &item_write_command},
    {"/api/item/inventory/{role}/put_jewel", contracts::QolItemWriteKind::kPutJewel, &item_write_command},
    {"/api/item/inventory/{role}/enchant", contracts::QolItemWriteKind::kEnchant, &item_write_command},
    {shop::kBuyRoute},
    {"/api/op/inventory/add", contracts::QolItemWriteKind::kAddItem, &item_write_command},
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
    int32_t resolved_from_space = -1;
    int32_t resolved_to_space = -1;
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
        {
            if (write_has_invalid_number(body_object, "bag")) return invalid_write_number_error("bag", response);
            if (write_has_invalid_number(body_object, "slot")) return invalid_write_number_error("slot", response);
            bool has_space = false;
            int32_t space = -1;
            if (!parse_optional_space(body_object, "space", &has_space, &space)) {
                return invalid_write_number_error("space", response);
            }
            args.i0 = write_opt_int(body_object, "bag", -1);
            args.i1 = write_opt_int(body_object, "slot", -1);
            if (args.i0 < 0 || args.i1 < 0) return response_error(response, 400, "bag/slot required");
            WriteRef ref{};
            const WriteRefStatus ref_status =
                resolve_write_ref(args.i0, has_space, space, args.i1, false, &ref);
            if (ref_status != WriteRefStatus::kOk) {
                return write_ref_error(ref_status, response, "bag/slot required");
            }
            args.i0 = write_port_bag(ref);
            break;
        }
        case contracts::QolItemWriteKind::kAcceptDice:
        case contracts::QolItemWriteKind::kRejectDice:
            break;
        case contracts::QolItemWriteKind::kEquipItem:
            if (!write_path_int(request, "role", &path_value)) return response_error(response, 400, "bad request");
            args.i0 = path_value;
            if (write_has_invalid_number(body_object, "category")) return invalid_write_number_error("category", response);
            if (write_has_invalid_number(body_object, "bag")) return invalid_write_number_error("bag", response);
            if (write_has_invalid_number(body_object, "slot")) return invalid_write_number_error("slot", response);
            {
                bool has_space = false;
                int32_t space = -1;
                if (!parse_optional_space(body_object, "space", &has_space, &space)) {
                    return invalid_write_number_error("space", response);
                }
                args.i5 = write_opt_int(body_object, "category", -1);
                args.i1 = write_opt_int(body_object, "bag", -1);
                args.i2 = write_opt_int(body_object, "slot", -1);
                if (args.i5 < 0 && (args.i1 < 0 || args.i2 < 0)) {
                    return response_error(response, 400, "bag+slot or category required");
                }
                if (args.i1 >= 0 && args.i2 >= 0) {
                    WriteRef ref{};
                    const WriteRefStatus ref_status =
                        resolve_write_ref(args.i1, has_space, space, args.i2, false, &ref);
                    if (ref_status != WriteRefStatus::kOk) {
                        return write_ref_error(ref_status, response,
                                               "bag+slot or category required");
                    }
                    args.i1 = write_port_bag(ref);
                }
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
            {
                bool has_space = false;
                int32_t space = -1;
                if (!parse_optional_space(body_object, "space", &has_space, &space)) {
                    return invalid_write_number_error("space", response);
                }
                args.i1 = write_opt_int(body_object, "bag", -1);
                args.i2 = write_opt_int(body_object, "slot", -1);
                args.i3 = write_opt_int(body_object, "equip_slot", -1);
                if (args.i1 < 0 || args.i2 < 0 || args.i3 < 0) {
                    return response_error(response, 400, "bag/slot/equip_slot required");
                }
                WriteRef ref{};
                const WriteRefStatus ref_status =
                    resolve_write_ref(args.i1, has_space, space, args.i2, false, &ref);
                if (ref_status != WriteRefStatus::kOk) {
                    return write_ref_error(ref_status, response,
                                           "bag/slot/equip_slot required");
                }
                args.i1 = write_port_bag(ref);
            }
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
    }
    if (g_sdk == nullptr || g_sdk->commands.post_typed == nullptr) {
        return response_error(response, 503, "command port unavailable");
    }
    contracts::QolCommandTicket ticket = 0;
    const ItemWritePayload payload{args, resolved_from_space, resolved_to_space};
    if (!contracts::succeeded(g_sdk->commands.post_typed(
            kItemWriteHandlerId, &payload, sizeof(payload), &ticket)) ||
        ticket == 0) {
        return response_error(response, 503, "command queue unavailable");
    }
    std::string ack = "{\"ok\":true,\"ticket\":";
    ack += std::to_string(ticket);
    ack += "}";
    return response_body(response, ack.data(), static_cast<uint32_t>(ack.size()));
}
}  // namespace qol::features::core_item

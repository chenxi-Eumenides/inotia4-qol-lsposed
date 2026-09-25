#include "plugin.h"

#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include "contracts/commands.h"
#include "contracts/game_ports/item_port.h"
#include "contracts/game_ports/item_dice_write_port.h"
#include "contracts/game_ports/item_money_write_port.h"
#include "contracts/game_ports/item_write_port.h"
#include "contracts/http.h"
#include "contracts/item_facts.h"
#include "contracts/item_count.h"
#include "json_lite.h"
#include "response.h"
#include "snapshot.h"

namespace qol::features::core_item {
namespace {

constexpr const char* kPluginId = "core.item";
constexpr const char* kPluginVersion = "0.1.0";
constexpr uint32_t kInventoryCapacity = 65536;
constexpr uint32_t kShopCapacity = 32768;
// 主线程快照节拍：库存/商店 JSON 构造不宜逐帧执行；15 帧（≈4Hz）对只读查询足够。
constexpr int32_t kSnapshotIntervalFrames = 15;
constexpr char kNotInGameBody[] = "{\"error\":\"not in game\"}";
constexpr char kSetMoneySuccess[] = "{\"ok\":true}";
constexpr char kSetMoneyNotInGame[] = "{\"ok\":false,\"error\":\"not in game\"}";
constexpr char kSetMoneySymbolMissing[] = "{\"ok\":false,\"error\":\"symbol not resolved\"}";
constexpr char kSetMoneyFailed[] = "{\"ok\":false,\"error\":\"set money failed\"}";
constexpr char kDiceRejectSuccess[] = "{\"ok\":true}";
constexpr char kDiceRejectNotInGame[] = "{\"ok\":false,\"error\":\"not in game\"}";
constexpr char kDiceRejectNoPending[] =
    "{\"ok\":false,\"error\":\"no dice result pending\"}";
constexpr char kDiceRejectFailed[] = "{\"ok\":false,\"error\":\"dice reject failed\"}";

enum class Query : uint8_t {
    kInventory,
    kMoney,
    kItems,
    kBagInfo,
    kBagSlot,
    kShopItems,
};

constexpr contracts::HttpRoute kRoutes[] = {
    {contracts::HttpMethod::kGet, "/api/item/inventory", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/money", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/items", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/bag/{bag}/info", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/bag/{bag}/{slot}", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/shop/items", kPluginId, 0, 1},
};

constexpr Query kQueries[] = {
    Query::kInventory, Query::kMoney,        Query::kItems,
    Query::kBagInfo,   Query::kBagSlot,      Query::kShopItems,
};

enum class SnapshotState : uint8_t { kUnavailable, kNotInGame, kReady };

struct ItemSnapshot {
    SnapshotState state = SnapshotState::kUnavailable;
    int64_t money = 0;
    uint32_t inventory_size = 0;
    char inventory[kInventoryCapacity]{};
    uint32_t shop_size = 0;
    char shop[kShopCapacity]{};
};

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolItemPort* g_port = nullptr;
// 写面私有端口（仅 core.item 可注入；ADR/阶段 5 写面迁移）。
const contracts::QolItemWritePort* g_item_write = nullptr;
const contracts::QolItemMoneyWritePort* g_item_money_write = nullptr;
const contracts::QolItemDiceWritePort* g_item_dice_write = nullptr;
// 写命令的票据结果处理器 id（全局 typed handler id 空间；core.item 前缀）。
constexpr uint32_t kItemWriteHandlerId = 0x49540001;
uint64_t g_frame_task = 0;
Snapshot<ItemSnapshot> g_snapshot;

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, "item facts not ready", 20);
}

QolResult category(const void* item, int32_t* out_category) {
    if (g_port == nullptr || g_port->read.category == nullptr) return not_ready();
    return g_port->read.category(item, out_category);
}

QolResult is_real_equip(const void* item, int32_t* out_value) {
    if (g_port == nullptr || g_port->read.is_real_equip == nullptr) return not_ready();
    return g_port->read.is_real_equip(item, out_value);
}

QolResult is_jewel_category(int32_t category_value, int32_t* out_is_jewel) {
    if (g_port == nullptr || g_port->read.is_jewel_category == nullptr) {
        return not_ready();
    }
    return g_port->read.is_jewel_category(category_value, out_is_jewel);
}

QolResult is_no_sell(int32_t category_value, int32_t* out_value) {
    if (g_port == nullptr || g_port->read.is_no_sell == nullptr) return not_ready();
    return g_port->read.is_no_sell(category_value, out_value);
}

QolResult sell_price(const void* item, int64_t* out_price) {
    if (g_port == nullptr || g_port->read.sell_price == nullptr) return not_ready();
    return g_port->read.sell_price(item, out_price);
}

QolResult cumulate_count(const void* item, int32_t* out_count) {
    if (g_port == nullptr || g_port->read.cumulate_count == nullptr) return not_ready();
    if (g_sdk != nullptr && g_sdk->capabilities.require != nullptr) {
        const void* iface = nullptr;
        const QolResult capability_result = g_sdk->capabilities.require(
            contracts::kStackLimitItemCountCapabilityId,
            QolVersionRange{contracts::kStackLimitItemCountVersion.major,
                            contracts::kStackLimitItemCountVersion.minor},
            &iface);
        if (contracts::succeeded(capability_result) && iface != nullptr) {
            const auto* override_api =
                static_cast<const contracts::StackLimitItemCountCapability*>(iface);
            if (override_api->enabled != nullptr && override_api->enabled() != 0 &&
                override_api->api != nullptr && override_api->api->canonical != nullptr) {
                uint32_t canonical = 0;
                const QolResult result = override_api->api->canonical(item, &canonical);
                if (!contracts::succeeded(result)) return result;
                *out_count = static_cast<int32_t>(canonical);
                return contracts::ok();
            }
        }
    }
    return g_port->read.cumulate_count(item, out_count);
}

QolResult item_ref_at(int32_t bag, int32_t slot, contracts::QolInventoryItemRef* out) {
    if (g_port == nullptr || g_port->read.item_ref_at == nullptr) return not_ready();
    return g_port->read.item_ref_at(bag, slot, out);
}

QolResult physical_item_at(int32_t bag, int32_t slot, void** out_item) {
    if (g_port == nullptr || g_port->read.physical_item_at == nullptr) {
        return not_ready();
    }
    return g_port->read.physical_item_at(bag, slot, out_item);
}

QolResult detail_context(int32_t* out_desc_type, int32_t* out_bag, void** out_control) {
    if (g_port == nullptr || g_port->read.detail_context == nullptr) {
        return not_ready();
    }
    return g_port->read.detail_context(out_desc_type, out_bag, out_control);
}

QolResult cursor_index(const void* control, int32_t* out_slot) {
    if (g_port == nullptr || g_port->read.cursor_index == nullptr) {
        return not_ready();
    }
    return g_port->read.cursor_index(control, out_slot);
}

constexpr contracts::ItemFactsApi kItemFactsApi{
    &category,
    &is_real_equip,
    &is_jewel_category,
    &is_no_sell,
    &sell_price,
    &cumulate_count,
    &item_ref_at,
    &physical_item_at,
    &detail_context,
    &cursor_index,
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kItemFactsCapabilityId, contracts::kItemFactsVersion, &kItemFactsApi},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_ITEM, {1, 0}},
    {contracts::QolPortId::QOL_PORT_ITEM_MONEY_WRITE,
     {contracts::kQolPortItemMoneyWriteVersion.major,
      contracts::kQolPortItemMoneyWriteVersion.minor}},
    {contracts::QolPortId::QOL_PORT_ITEM_DICE_WRITE,
     {contracts::kQolPortItemDiceWriteVersion.major,
      contracts::kQolPortItemDiceWriteVersion.minor}},
    {contracts::QolPortId::QOL_PORT_ITEM_WRITE,
     {contracts::kQolPortItemWriteVersion.major, contracts::kQolPortItemWriteVersion.minor}},
};

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kHttpRegistryCapabilityId, {contracts::kHttpRegistryVersion.major,
                                             contracts::kHttpRegistryVersion.minor}},
    {contracts::kHttpDispatchCapabilityId, {contracts::kHttpDispatchVersion.major,
                                             contracts::kHttpDispatchVersion.minor}},
};

const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kStackLimitItemCountCapabilityId,
     {contracts::kStackLimitItemCountVersion.major,
      contracts::kStackLimitItemCountVersion.minor}},
};

void log_message(const contracts::QolHostSdk* sdk, const char* message) {
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)sdk->log.write(1, 1, kPluginId, message, length);
}

bool copy_buffer(char* destination, uint32_t capacity, uint32_t* destination_size,
                 const char* source, uint32_t source_size) {
    if (destination == nullptr || destination_size == nullptr || source == nullptr ||
        source_size >= capacity) {
        return false;
    }
    std::memcpy(destination, source, source_size);
    destination[source_size] = '\0';
    *destination_size = source_size;
    return true;
}

void publish_not_in_game(ItemSnapshot& snapshot) {
    snapshot.state = SnapshotState::kNotInGame;
    (void)copy_buffer(snapshot.inventory, kInventoryCapacity, &snapshot.inventory_size,
                      kNotInGameBody, static_cast<uint32_t>(sizeof(kNotInGameBody) - 1));
    (void)copy_buffer(snapshot.shop, kShopCapacity, &snapshot.shop_size, kNotInGameBody,
                      static_cast<uint32_t>(sizeof(kNotInGameBody) - 1));
    g_snapshot.publish(snapshot);
}

void update_snapshot() {
    if (g_port == nullptr || g_port->read.inventory_json == nullptr ||
        g_port->read.shop_items_json == nullptr || g_port->read.money == nullptr ||
        g_port->read.in_world == nullptr) {
        return;
    }
    // 静态名称表未下发时不发布，避免输出空名称；HTTP 侧表现为 not ready。
    if (g_port->read.static_ready != nullptr) {
        int32_t ready = 0;
        if (!contracts::succeeded(g_port->read.static_ready(&ready)) || ready == 0) return;
    }

    ItemSnapshot next{};
    int32_t in_world = 0;
    if (!contracts::succeeded(g_port->read.in_world(&in_world))) return;
    if (in_world == 0) {
        publish_not_in_game(next);
        return;
    }

    char inventory[kInventoryCapacity]{};
    uint32_t inventory_size = 0;
    if (!contracts::succeeded(g_port->read.inventory_json(inventory, kInventoryCapacity,
                                                          &inventory_size))) {
        return;
    }
    if (std::strncmp(inventory, "{\"error\":", 9) == 0) {
        publish_not_in_game(next);
        return;
    }
    if (!copy_buffer(next.inventory, kInventoryCapacity, &next.inventory_size, inventory,
                     inventory_size)) {
        return;
    }

    char shop[kShopCapacity]{};
    uint32_t shop_size = 0;
    if (!contracts::succeeded(g_port->read.shop_items_json(shop, kShopCapacity, &shop_size))) {
        return;
    }
    if (!copy_buffer(next.shop, kShopCapacity, &next.shop_size, shop, shop_size)) return;

    if (!contracts::succeeded(g_port->read.money(&next.money))) return;
    next.state = SnapshotState::kReady;
    g_snapshot.publish(next);
}

int32_t frame_tick(int64_t, void*) {
    update_snapshot();
    return 1;
}

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
                        uint16_t status = 200) {
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
        const std::string value(param.value, param.value_len);
        char* end = nullptr;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (end == nullptr || *end != '\0') return false;
        *out = static_cast<int32_t>(parsed);
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

// ---- 写面（13 条声明式 POST；阶段 5 写面迁移）----------------------------------
// HTTP 线程只解析参数并投递票据；游戏写在主线程结果处理器中执行（ARCHITECTURE §7.1）。
// 写请求按 Android JSONObject.optInt/optLong 语义取值，不改变 item_facts JSON 工具行为。
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

bool find_item_slot(int32_t category, int32_t* bag_out, int32_t* slot_out) {
    if (g_port == nullptr || g_port->read.inventory_json == nullptr || bag_out == nullptr ||
        slot_out == nullptr) {
        return false;
    }
    constexpr uint32_t kInventoryJsonCapacity = 65536;
    char inventory[kInventoryJsonCapacity]{};
    uint32_t inventory_size = 0;
    if (!contracts::succeeded(g_port->read.inventory_json(
            inventory, kInventoryJsonCapacity, &inventory_size))) {
        return false;
    }
    JsonValue root{};
    if (!json_parse(inventory, inventory_size, &root)) return false;
    const JsonValue* bags = root.find("bags");
    if (bags == nullptr || bags->type != JsonValue::Type::kArray) return false;
    for (size_t b = 0; b < bags->items.size(); ++b) {
        const JsonValue& bag = bags->items[b];
        const JsonValue* items = bag.find("items");
        if (items == nullptr || items->type != JsonValue::Type::kArray) continue;
        for (const JsonValue& item : items->items) {
            if (write_opt_int(item, "category", -1) != category) continue;
            *bag_out = static_cast<int32_t>(b);
            *slot_out = write_opt_int(item, "slot", -1);
            return true;
        }
    }
    return false;
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
        uint32_t k = 0;
        bool negative = false;
        if (k < param.value_len && param.value[k] == '-') {
            negative = true;
            ++k;
        }
        if (k >= param.value_len) return false;
        int64_t value = 0;
        for (; k < param.value_len; ++k) {
            const char c = param.value[k];
            if (c < '0' || c > '9') return false;
            value = value * 10 + (c - '0');
        }
        *out = static_cast<int32_t>(negative ? -value : value);
        return true;
    }
    return false;
}

struct ItemWriteRoute {
    const char* path;
    contracts::QolItemWriteKind kind;
};

constexpr ItemWriteRoute kItemWriteRoutes[] = {
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

constexpr uint32_t kItemWriteRouteCount =
    static_cast<uint32_t>(sizeof(kItemWriteRoutes) / sizeof(kItemWriteRoutes[0]));

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
            args.i0 = write_opt_int(body_object, "bag", -1);
            args.i1 = write_opt_int(body_object, "slot", -1);
            if (args.i0 < 0 || args.i1 < 0) return response_error(response, 400, "bag/slot required");
            break;
        case contracts::QolItemWriteKind::kMoveItem:
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
            args.i1 = write_opt_int(body_object, "slot", -1);
            if (args.i1 < 0) return response_error(response, 400, "slot required");
            break;
        case contracts::QolItemWriteKind::kPutJewel:
        case contracts::QolItemWriteKind::kEnchant:
            if (!write_path_int(request, "role", &path_value)) return response_error(response, 400, "bad request");
            args.i0 = path_value;
            args.i1 = write_opt_int(body_object, "bag", -1);
            args.i2 = write_opt_int(body_object, "slot", -1);
            args.i3 = write_opt_int(body_object, "equip_slot", -1);
            if (args.i1 < 0 || args.i2 < 0 || args.i3 < 0) {
                return response_error(response, 400, "bag/slot/equip_slot required");
            }
            break;
        case contracts::QolItemWriteKind::kShopBuy:
            args.i0 = write_opt_int(body_object, "slot", -1);
            if (args.i0 < 0) return response_error(response, 400, "slot required");
            break;
        case contracts::QolItemWriteKind::kAddItem:
            args.i0 = write_opt_int(body_object, "category", -1);
            if (args.i0 < 0) return response_error(response, 400, "category required");
            args.i1 = write_opt_int(body_object, "count", 1);
            args.i2 = write_opt_int(body_object, "socket", 0);
            args.i3 = write_opt_int(body_object, "enhanceRemaining", 0);
            args.i4 = write_opt_int(body_object, "rarity", -1);
            break;
        case contracts::QolItemWriteKind::kSetMoney:
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

bool item_write_result(void*, const uint8_t* payload, uint32_t size, char* out, uint32_t capacity,
                       uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (payload == nullptr || size != sizeof(contracts::QolItemWriteArgs) || out == nullptr ||
        out_size == nullptr || g_item_write == nullptr || g_item_write->read.write == nullptr ||
        g_item_money_write == nullptr || g_item_money_write->set_money == nullptr ||
        g_item_dice_write == nullptr || g_item_dice_write->reject == nullptr) {
        return false;
    }
    contracts::QolItemWriteArgs args{};
    std::memcpy(&args, payload, size);
    if (args.kind == static_cast<int32_t>(contracts::QolItemWriteKind::kRejectDice)) {
        const QolResult operation = g_item_dice_write->reject();
        const char* body = kDiceRejectSuccess;
        uint32_t body_size = sizeof(kDiceRejectSuccess) - 1;
        if (!contracts::succeeded(operation)) {
            if (operation.code == kQolErrNotReady) {
                body = kDiceRejectNotInGame;
                body_size = sizeof(kDiceRejectNotInGame) - 1;
            } else if (operation.code == kQolErrNotFound) {
                body = kDiceRejectNoPending;
                body_size = sizeof(kDiceRejectNoPending) - 1;
            } else {
                body = kDiceRejectFailed;
                body_size = sizeof(kDiceRejectFailed) - 1;
            }
        }
        if (body_size > capacity) return false;
        std::memcpy(out, body, body_size);
        *out_size = body_size;
        return contracts::succeeded(operation);
    }
    if (args.kind == static_cast<int32_t>(contracts::QolItemWriteKind::kSetMoney)) {
        const QolResult operation = g_item_money_write->set_money(args.i64);
        const char* body = kSetMoneySuccess;
        uint32_t body_size = sizeof(kSetMoneySuccess) - 1;
        if (!contracts::succeeded(operation)) {
            if (operation.code == kQolErrNotReady) {
                body = kSetMoneyNotInGame;
                body_size = sizeof(kSetMoneyNotInGame) - 1;
            } else if (operation.code == kQolErrUnsupported) {
                body = kSetMoneySymbolMissing;
                body_size = sizeof(kSetMoneySymbolMissing) - 1;
            } else {
                body = kSetMoneyFailed;
                body_size = sizeof(kSetMoneyFailed) - 1;
            }
        }
        if (body_size > capacity) return false;
        std::memcpy(out, body, body_size);
        *out_size = body_size;
        return contracts::succeeded(operation);
    }
    if (args.kind == static_cast<int32_t>(contracts::QolItemWriteKind::kEquipItem) &&
        args.i5 >= 0) {
        if (!find_item_slot(args.i5, &args.i1, &args.i2)) {
            constexpr char kNotFound[] = "{\"ok\":false,\"error\":\"item not found\"}";
            if (sizeof(kNotFound) - 1 > capacity) return false;
            std::memcpy(out, kNotFound, sizeof(kNotFound) - 1);
            *out_size = static_cast<uint32_t>(sizeof(kNotFound) - 1);
            return false;
        }
    }
    const QolResult result = g_item_write->read.write(&args, out, capacity, out_size);
    return contracts::succeeded(result);
}

const contracts::HttpQueryHandlerDescriptor kHttpQueries[] = {
    {kRoutes[0].method, kRoutes[0].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[0])},
    {kRoutes[1].method, kRoutes[1].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[1])},
    {kRoutes[2].method, kRoutes[2].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[2])},
    {kRoutes[3].method, kRoutes[3].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[3])},
    {kRoutes[4].method, kRoutes[4].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[4])},
    {kRoutes[5].method, kRoutes[5].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[5])},
    {contracts::HttpMethod::kPost, kItemWriteRoutes[0].path, kPluginId, &item_write_command,
     const_cast<contracts::QolItemWriteKind*>(&kItemWriteRoutes[0].kind)},
    {contracts::HttpMethod::kPost, kItemWriteRoutes[1].path, kPluginId, &item_write_command,
     const_cast<contracts::QolItemWriteKind*>(&kItemWriteRoutes[1].kind)},
    {contracts::HttpMethod::kPost, kItemWriteRoutes[2].path, kPluginId, &item_write_command,
     const_cast<contracts::QolItemWriteKind*>(&kItemWriteRoutes[2].kind)},
    {contracts::HttpMethod::kPost, kItemWriteRoutes[3].path, kPluginId, &item_write_command,
     const_cast<contracts::QolItemWriteKind*>(&kItemWriteRoutes[3].kind)},
    {contracts::HttpMethod::kPost, kItemWriteRoutes[4].path, kPluginId, &item_write_command,
     const_cast<contracts::QolItemWriteKind*>(&kItemWriteRoutes[4].kind)},
    {contracts::HttpMethod::kPost, kItemWriteRoutes[5].path, kPluginId, &item_write_command,
     const_cast<contracts::QolItemWriteKind*>(&kItemWriteRoutes[5].kind)},
    {contracts::HttpMethod::kPost, kItemWriteRoutes[6].path, kPluginId, &item_write_command,
     const_cast<contracts::QolItemWriteKind*>(&kItemWriteRoutes[6].kind)},
    {contracts::HttpMethod::kPost, kItemWriteRoutes[7].path, kPluginId, &item_write_command,
     const_cast<contracts::QolItemWriteKind*>(&kItemWriteRoutes[7].kind)},
    {contracts::HttpMethod::kPost, kItemWriteRoutes[8].path, kPluginId, &item_write_command,
     const_cast<contracts::QolItemWriteKind*>(&kItemWriteRoutes[8].kind)},
    {contracts::HttpMethod::kPost, kItemWriteRoutes[9].path, kPluginId, &item_write_command,
     const_cast<contracts::QolItemWriteKind*>(&kItemWriteRoutes[9].kind)},
    {contracts::HttpMethod::kPost, kItemWriteRoutes[10].path, kPluginId, &item_write_command,
     const_cast<contracts::QolItemWriteKind*>(&kItemWriteRoutes[10].kind)},
    {contracts::HttpMethod::kPost, kItemWriteRoutes[11].path, kPluginId, &item_write_command,
     const_cast<contracts::QolItemWriteKind*>(&kItemWriteRoutes[11].kind)},
    {contracts::HttpMethod::kPost, kItemWriteRoutes[12].path, kPluginId, &item_write_command,
     const_cast<contracts::QolItemWriteKind*>(&kItemWriteRoutes[12].kind)},
};

void stop_frame_task(const contracts::QolHostSdk* sdk) {
    if (g_frame_task != 0 && sdk != nullptr && sdk->scheduler.remove != nullptr) {
        (void)sdk->scheduler.remove(g_frame_task);
    }
    g_frame_task = 0;
}

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_port = nullptr;
    g_item_write = nullptr;
    g_item_money_write = nullptr;
    g_item_dice_write = nullptr;
    g_frame_task = 0;
    ItemSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    if (ctx.struct_size < contracts::PLUGIN_CONTEXT_PORTS_STRUCT_SIZE ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || ctx.ports == nullptr ||
        ctx.port_count != 4 || ctx.ports[0] == nullptr || ctx.ports[1] == nullptr ||
        ctx.ports[2] == nullptr || ctx.ports[3] == nullptr) {
        g_sdk = nullptr;
        return 1;
    }
    g_port = static_cast<const contracts::QolItemPort*>(ctx.ports[0]);
    if (!contracts::satisfies(g_port->abi_version, {1, 0}) ||
        g_port->read.category == nullptr || g_port->read.item_ref_at == nullptr ||
        g_port->read.inventory_json == nullptr || g_port->read.shop_items_json == nullptr ||
        g_port->read.in_world == nullptr || g_port->read.money == nullptr ||
        g_port->read.static_text == nullptr || g_port->read.static_ready == nullptr) {
        g_port = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    g_item_money_write = static_cast<const contracts::QolItemMoneyWritePort*>(ctx.ports[1]);
    if (!contracts::satisfies(g_item_money_write->abi_version,
                              {contracts::kQolPortItemMoneyWriteVersion.major,
                               contracts::kQolPortItemMoneyWriteVersion.minor}) ||
        g_item_money_write->set_money == nullptr) {
        g_port = nullptr;
        g_item_money_write = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    g_item_dice_write = static_cast<const contracts::QolItemDiceWritePort*>(ctx.ports[2]);
    if (!contracts::satisfies(g_item_dice_write->abi_version,
                              {contracts::kQolPortItemDiceWriteVersion.major,
                               contracts::kQolPortItemDiceWriteVersion.minor}) ||
        g_item_dice_write->reject == nullptr) {
        g_port = nullptr;
        g_item_money_write = nullptr;
        g_item_dice_write = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    g_item_write = static_cast<const contracts::QolItemWritePort*>(ctx.ports[3]);
    if (!contracts::satisfies(g_item_write->abi_version, {1, 0}) ||
        g_item_write->read.write == nullptr) {
        g_port = nullptr;
        g_item_write = nullptr;
        g_item_money_write = nullptr;
        g_item_dice_write = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    // 结果处理器需要 host 尾部的票据结果面（QolHostSdk 尾部追加，按 abi 协商）。
    if (g_sdk->abi.size < sizeof(contracts::QolHostSdk) || g_sdk->abi.minor < 9 ||
        g_sdk->ticket_result.register_typed_result_handler == nullptr ||
        !contracts::succeeded(g_sdk->ticket_result.register_typed_result_handler(
            kItemWriteHandlerId, &item_write_result, nullptr))) {
        g_port = nullptr;
        g_item_write = nullptr;
        g_item_money_write = nullptr;
        g_item_dice_write = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    log_message(g_sdk, "[core.item] provider install");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    stop_frame_task(ctx.sdk);
    log_message(ctx.sdk, "[core.item] provider deactivate");
    if (g_sdk != nullptr && g_sdk->ticket_result.unregister_typed_result_handler != nullptr) {
        (void)g_sdk->ticket_result.unregister_typed_result_handler(kItemWriteHandlerId);
    }
    g_port = nullptr;
    g_item_write = nullptr;
    g_item_money_write = nullptr;
    g_item_dice_write = nullptr;
    g_sdk = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    ItemSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    if (g_frame_task == 0 && g_sdk != nullptr && g_sdk->scheduler.add != nullptr) {
        uint64_t task = 0;
        if (contracts::succeeded(g_sdk->scheduler.add(
                1, &frame_tick, nullptr, kSnapshotIntervalFrames, 0, &task))) {
            g_frame_task = task;
        } else {
            log_message(ctx.sdk, "[core.item] snapshot scheduler unavailable");
        }
    }
    log_message(ctx.sdk, "[core.item] provider enabled: capability + http queries");
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    stop_frame_task(ctx.sdk);
    ItemSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    log_message(ctx.sdk, "[core.item] provider disabled: capability + http queries withdrawn");
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_port != nullptr;
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    2,
    kRequires,
    static_cast<uint32_t>(sizeof(kOptionalRequires) / sizeof(kOptionalRequires[0])),
    kOptionalRequires,
    1,
    kProvides,
    0,
    nullptr,
    0,
    nullptr,
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,
    kPorts,
    4,
    kHttpQueries,
    static_cast<uint32_t>(sizeof(kHttpQueries) / sizeof(kHttpQueries[0])),
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::core_item

#include "plugin.h"

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "contracts/commands.h"
#include "contracts/character_state.h"
#include "contracts/game_ports/item_port.h"
#include "contracts/game_ports/item_dice_write_port.h"
#include "contracts/game_ports/item_money_write_port.h"
#include "contracts/game_ports/item_write_port.h"
#include "contracts/game_ports/item_inventory_state_write_port.h"
#include "contracts/http.h"
#include "contracts/inventory_extension.h"
#include "contracts/item_facts.h"
#include "contracts/item_state.h"
#include "json_lite.h"
#include "response.h"
#include "snapshot_sdk.h"

namespace qol::features::core_item {
namespace {

constexpr const char* kPluginId = "core.item";
constexpr const char* kPluginVersion = "0.1.0";
constexpr uint32_t kInventoryCapacity = contracts::kQolItemStateRawInventoryCapacity;
constexpr uint32_t kShopCapacity = 32768;
// Raw inventory buffer matches the owner projection contract's 256 KiB ceiling.
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
const contracts::QolItemMoneyWritePort* g_item_money_write = nullptr;
const contracts::QolItemDiceWritePort* g_item_dice_write = nullptr;
const contracts::QolItemInventoryStateWritePort* g_item_state_write = nullptr;
// 写命令的票据结果处理器 id（全局 typed handler id 空间；core.item 前缀）。
constexpr uint32_t kItemWriteHandlerId = 0x49540001;
uint64_t g_frame_task = 0;
Snapshot<ItemSnapshot> g_snapshot;

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, "item facts not ready", 20);
}

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult category(contracts::QolItemHandle item, int32_t* out_category) {
    if (g_port == nullptr || g_port->read.category == nullptr) return not_ready();
    return g_port->read.category(item, out_category);
}

QolResult is_real_equip(contracts::QolItemHandle item, int32_t* out_value) {
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

QolResult sell_price(contracts::QolItemHandle item, int64_t* out_price) {
    if (g_port == nullptr || g_port->read.sell_price == nullptr) return not_ready();
    return g_port->read.sell_price(item, out_price);
}

QolResult cumulate_count(contracts::QolItemHandle item, int32_t* out_count) {
    if (g_port == nullptr || g_port->read.cumulate_count == nullptr) return not_ready();
    return g_port->read.cumulate_count(item, out_count);
}

QolResult item_ref_at(int32_t bag, int32_t slot, contracts::QolInventoryItemRef* out) {
    if (g_port == nullptr || g_port->read.item_ref_at == nullptr) return not_ready();
    return g_port->read.item_ref_at(bag, slot, out);
}

QolResult physical_item_at(int32_t bag, int32_t slot, contracts::QolItemHandle* out_item) {
    if (g_port == nullptr || g_port->read.physical_item_at == nullptr) {
        return not_ready();
    }
    return g_port->read.physical_item_at(bag, slot, out_item);
}

QolResult item_view(contracts::QolItemHandle item, contracts::QolItemInventoryView* out_view) {
    if (g_port == nullptr || g_port->read.inventory_item_view == nullptr) return not_ready();
    return g_port->read.inventory_item_view(item, out_view);
}

QolResult effective_count(contracts::QolItemHandle item, int32_t* out_count) {
    if (g_port == nullptr || g_port->read.effective_cumulate_count == nullptr) return not_ready();
    return g_port->read.effective_cumulate_count(item, out_count);
}

QolResult count_limit(contracts::QolItemHandle item, uint32_t* out_limit) {
    if (g_port == nullptr || g_port->read.count_limit == nullptr) return not_ready();
    return g_port->read.count_limit(item, out_limit);
}

QolResult count_encoding(contracts::QolItemHandle item,
                         contracts::QolItemCountEncoding* out_encoding) {
    if (g_port == nullptr || g_port->read.count_encoding == nullptr) return not_ready();
    return g_port->read.count_encoding(item, out_encoding);
}

QolResult detail_item_context(contracts::QolItemDetailContext* out_context) {
    if (g_port == nullptr || g_port->read.detail_item_context == nullptr) return not_ready();
    return g_port->read.detail_item_context(out_context);
}

QolResult money(int64_t* out_money) {
    if (g_port == nullptr || g_port->read.money == nullptr) return not_ready();
    return g_port->read.money(out_money);
}

constexpr contracts::ItemFactsApi kItemFactsApi{
    sizeof(contracts::ItemFactsApi),
    &category,
    &is_real_equip,
    &is_jewel_category,
    &is_no_sell,
    &sell_price,
    &cumulate_count,
    &item_ref_at,
    &physical_item_at,
    &detail_item_context,
    &money,
    &item_view,
    &effective_count,
    &count_limit,
    &count_encoding,
};

// T2h-B2：item.state 发布面——唯一实现复用既有 serialize_inventory_json（禁止第二实现）。
QolResult write_item_state_json(int32_t kind, char* out, uint32_t capacity, uint32_t* out_size);
constexpr contracts::ItemStateApi kItemStateApi{&write_item_state_json};

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kItemFactsCapabilityId, contracts::kItemFactsVersion, &kItemFactsApi},
    {contracts::kItemStateCapabilityId, contracts::kItemStateVersion, &kItemStateApi},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_ITEM,
     {contracts::kQolPortItemVersion.major, contracts::kQolPortItemVersion.minor}},
    {contracts::QolPortId::QOL_PORT_ITEM_MONEY_WRITE,
     {contracts::kQolPortItemMoneyWriteVersion.major,
      contracts::kQolPortItemMoneyWriteVersion.minor}},
    {contracts::QolPortId::QOL_PORT_ITEM_DICE_WRITE,
     {contracts::kQolPortItemDiceWriteVersion.major,
      contracts::kQolPortItemDiceWriteVersion.minor}},
    {contracts::QolPortId::QOL_PORT_ITEM_INVENTORY_STATE_WRITE,
     {contracts::kQolPortItemInventoryStateWriteVersion.major,
      contracts::kQolPortItemInventoryStateWriteVersion.minor}},
};

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kHttpRegistryCapabilityId, {contracts::kHttpRegistryVersion.major,
                                             contracts::kHttpRegistryVersion.minor}},
    {contracts::kHttpDispatchCapabilityId, {contracts::kHttpDispatchVersion.major,
                                             contracts::kHttpDispatchVersion.minor}},
};

const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kCharacterStateCapabilityId,
     {contracts::kCharacterStateVersion.major, contracts::kCharacterStateVersion.minor}},
};

void log_message(const contracts::QolHostSdk* sdk, const char* message) {
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)sdk->log.write(1, 1, kPluginId, message, length);
}

bool g_snapshot_serialize_warned = false;

void log_snapshot_serialize_failure() {
    if (g_snapshot_serialize_warned) return;
    g_snapshot_serialize_warned = true;
    constexpr char kMessage[] = "[core.item] snapshot serialize failed";
    if (g_sdk == nullptr || g_sdk->log.write == nullptr) return;
    (void)g_sdk->log.write(2, 1, kPluginId, kMessage,
                           static_cast<uint32_t>(sizeof(kMessage) - 1));
}

uint32_t g_degrade_warn_ticks = 0;

void log_degrade_warn(const char* message) {
    if (message == nullptr) return;
    if (++g_degrade_warn_ticks % 300u != 1u) return;
    if (g_sdk == nullptr || g_sdk->log.write == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)g_sdk->log.write(2, 1, kPluginId, message, length);
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

bool copy_result_truncated(char* out, uint32_t capacity, uint32_t* out_size,
                           const std::string& body) {
    if (out == nullptr || out_size == nullptr) return false;
    const uint32_t copied = static_cast<uint32_t>(std::min<std::size_t>(body.size(), capacity));
    if (copied != 0) std::memcpy(out, body.data(), copied);
    *out_size = static_cast<uint32_t>(body.size());
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

const contracts::CharacterStateApi* character_state_capability() {
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const QolResult result = g_sdk->capabilities.require(
        contracts::kCharacterStateCapabilityId,
        QolVersionRange{contracts::kCharacterStateVersion.major,
                        contracts::kCharacterStateVersion.minor},
        &iface);
    if (!contracts::succeeded(result) || iface == nullptr) return nullptr;
    return static_cast<const contracts::CharacterStateApi*>(iface);
}

// 中立扩展 provider 访问：core.item 只经 Item Port 暴露的中立通道读取 provider，
// 不感知扩展袋能力、逻辑袋号或 provider 实现。每次现取，不缓存跨帧指针。
struct InventoryExtensionAccessResult {
    const contracts::QolInventoryExtensionProvider* provider = nullptr;
    bool available = false;
};

InventoryExtensionAccessResult inventory_extension() {
    InventoryExtensionAccessResult result;
    if (g_port == nullptr || g_port->read.inventory_extension_access == nullptr) return result;
    contracts::QolInventoryExtensionAccess access{};
    if (!contracts::succeeded(g_port->read.inventory_extension_access(&access))) return result;
    result.provider = access.provider;
    result.available = access.available != 0;
    return result;
}

bool provider_owns_bag(const InventoryExtensionAccessResult& access, int32_t bag) {
    return access.provider != nullptr && access.provider->owns_bag != nullptr &&
           access.provider->owns_bag(bag) != 0;
}

bool effective_item_count(contracts::QolItemHandle item, bool original_count_supported,
                          uint32_t* out_count,
                          bool* out_present) {
    if (item.slot == 0 || item.generation == 0 || out_count == nullptr || out_present == nullptr) {
        return false;
    }
    *out_present = false;
    if (g_port != nullptr && g_port->read.effective_cumulate_count != nullptr) {
        int32_t effective = 0;
        const QolResult result = g_port->read.effective_cumulate_count(item, &effective);
        if (contracts::succeeded(result)) {
            *out_count = static_cast<uint32_t>(effective);
            *out_present = true;
            return true;
        }
    }

    if (g_port != nullptr && g_port->read.cumulate_count != nullptr) {
        int32_t original_count = 0;
        const QolResult result = g_port->read.cumulate_count(item, &original_count);
        if (contracts::succeeded(result)) {
            *out_count = static_cast<uint32_t>(original_count);
            *out_present = true;
            return true;
        }
        if (original_count_supported) return false;
    } else if (original_count_supported) {
        return false;
    }
    return true;
}

void append_inventory_item(std::string* out, int32_t slot,
                           const contracts::QolItemInventoryView& item, bool count_present,
                           uint32_t count) {
    out->append("{\"slot\":");
    *out += std::to_string(slot);
    out->append(",\"type_flags\":");
    *out += std::to_string(item.type_flags);
    out->append(",\"raw_rarity\":");
    *out += std::to_string(item.raw_rarity);
    if ((item.fields & contracts::kQolItemInventoryHasCategory) != 0) {
        out->append(",\"category\":");
        *out += std::to_string(item.category);
    }
    if (count_present) {
        out->append(",\"count\":");
        *out += std::to_string(count);
    }
    out->append(item.equip != 0 ? ",\"equip\":true" : ",\"equip\":false");
    if ((item.fields & contracts::kQolItemInventoryHasRarity) != 0) {
        out->append(",\"rarity\":");
        *out += std::to_string(item.rarity);
    }
    if ((item.fields & contracts::kQolItemInventoryHasDamage) != 0) {
        out->append(",\"damage\":");
        *out += std::to_string(item.damage);
    }
    if ((item.fields & contracts::kQolItemInventoryHasDefense) != 0) {
        out->append(",\"defense\":");
        *out += std::to_string(item.defense);
    }
    out->append(",\"magic_rate\":");
    *out += std::to_string(item.magic_rate);
    if ((item.fields & contracts::kQolItemInventoryHasNeedLevel) != 0) {
        out->append(",\"need_level\":");
        *out += std::to_string(item.need_level);
    }
    out->append(",\"socket\":");
    *out += std::to_string(item.socket);
    out->append(",\"socket_filled\":");
    *out += std::to_string(item.socket_filled);
    out->append(",\"socket_total\":");
    *out += std::to_string(item.socket_total);
    out->append(",\"enchant\":");
    *out += std::to_string(item.enchant);
    out->append(item.chaos != 0 ? ",\"chaos\":true" : ",\"chaos\":false");
    out->append(",\"enchant_id\":");
    *out += std::to_string(item.enchant_id);
    out->append(",\"enchant_level\":");
    *out += std::to_string(item.enchant_level);
    out->append(",\"chaos_level\":");
    *out += std::to_string(item.chaos_level);
    out->append(",\"chaos_rate\":");
    *out += std::to_string(item.chaos_rate);

    out->append(",\"options\":[");
    for (uint32_t index = 0; index < item.option_count; ++index) {
        if (index != 0) out->push_back(',');
        *out += std::to_string(item.options[index].value);
    }
    out->append("],\"option_ids\":[");
    for (uint32_t index = 0; index < item.option_count; ++index) {
        if (index != 0) out->push_back(',');
        *out += std::to_string(item.options[index].id);
    }
    out->append("],\"option_types\":[");
    for (uint32_t index = 0; index < item.option_count; ++index) {
        if (index != 0) out->push_back(',');
        *out += std::to_string(item.options[index].type);
    }
    out->append("]}");
}

// 扩展段由 provider 贡献完整 JSON fragment（含前导逗号）；core.item 只追加，不重拼字段、
// 不感知袋号与数量视图（R-47b 真值表唯一实现点在 provider）。
QolResult append_extension_inventory_bags(std::string* out) {
    if (out == nullptr || g_port == nullptr) return contracts::ok();
    const InventoryExtensionAccessResult access = inventory_extension();
    if (!access.available || access.provider == nullptr ||
        access.provider->inventory_json_fragment == nullptr) {
        // provider 未注册/停用 → 省略扩展段（限速 WARN 保留可诊断性）；主快照照常。
        log_degrade_warn("extension inventory omitted: provider unavailable");
        return contracts::ok();
    }
    int32_t in_world = 0;
    if (g_port->read.in_world == nullptr ||
        !contracts::succeeded(g_port->read.in_world(&in_world)) || in_world == 0) {
        return contracts::ok();
    }
    thread_local std::vector<char> fragment(kInventoryCapacity);
    uint32_t fragment_size = 0;
    const QolResult result = access.provider->inventory_json_fragment(
        fragment.data(), static_cast<uint32_t>(fragment.size()), &fragment_size);
    if (!contracts::succeeded(result) || fragment_size == 0) {
        // provider 门禁关闭或不可用时贡献空片段：省略扩展段，主快照/写票据照常。
        log_degrade_warn("extension inventory omitted: fragment unavailable");
        return contracts::ok();
    }
    out->append(fragment.data(), fragment_size);
    return contracts::ok();
}

QolResult serialize_inventory_json(char* out, uint32_t capacity, uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr || g_port == nullptr ||
        g_port->read.in_world == nullptr || g_port->read.item_ref_at == nullptr ||
        g_port->read.inventory_item_view == nullptr) {
        return error_result(kQolErrInvalidArgument, "inventory serializer unavailable");
    }
    *out_size = 0;
    int32_t in_world = 0;
    if (!contracts::succeeded(g_port->read.in_world(&in_world))) return not_ready();
    if (in_world == 0) {
        constexpr char kNotInWorld[] = "{\"error\":\"not in game\"}";
        if (sizeof(kNotInWorld) - 1 > capacity) {
            return error_result(kQolErrInvalidArgument, "inventory buffer too small");
        }
        std::memcpy(out, kNotInWorld, sizeof(kNotInWorld) - 1);
        *out_size = sizeof(kNotInWorld) - 1;
        return contracts::ok();
    }

    std::string json = "{\"bags\":[";
    for (int32_t bag = 0; bag < 6; ++bag) {
        if (bag != 0) json.push_back(',');
        json += "{\"bag\":" + std::to_string(bag) + ",\"items\":[";
        int32_t filled = 0;
        for (int32_t slot = 0; slot < 16; ++slot) {
            contracts::QolInventoryItemRef item_ref{};
            const QolResult ref_result = g_port->read.item_ref_at(bag, slot, &item_ref);
            if (!contracts::succeeded(ref_result)) {
                if (ref_result.code == kQolErrNotFound) continue;
                return ref_result;
            }
            if (item_ref.kind != static_cast<int32_t>(contracts::QolInventoryItemKind::kOriginal) ||
                item_ref.item.slot == 0) {
                continue;
            }
            contracts::QolItemInventoryView view{};
            const QolResult view_result = g_port->read.inventory_item_view(item_ref.item,
                                                                            &view);
            if (!contracts::succeeded(view_result)) return view_result;
            if (view.option_count > contracts::kQolItemInventoryOptionCapacity) {
                return error_result(kQolErrInvalidArgument, "invalid item option count");
            }
            uint32_t count = 0;
            bool count_present = false;
            if (!effective_item_count(item_ref.item, view.count_supported != 0, &count,
                                      &count_present)) {
                return error_result(kQolErrNotReady, "item count unavailable");
            }
            if (filled != 0) json.push_back(',');
            append_inventory_item(&json, slot, view, count_present, count);
            ++filled;
        }
        json += "],\"capacity\":16,\"slot_count\":" + std::to_string(filled) + "}";
    }
    const QolResult extension_result = append_extension_inventory_bags(&json);
    if (!contracts::succeeded(extension_result)) return extension_result;
    json += "]}";
    if (json.size() > capacity) {
        return error_result(kQolErrInvalidArgument, "inventory buffer too small");
    }
    std::memcpy(out, json.data(), json.size());
    *out_size = static_cast<uint32_t>(json.size());
    return contracts::ok();
}

QolResult write_item_state_json(int32_t kind, char* out, uint32_t capacity, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (out == nullptr || out_size == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid item state output");
    }
    switch (static_cast<contracts::QolItemStateKind>(kind)) {
        case contracts::QolItemStateKind::kRawInventory:
            return serialize_inventory_json(out, capacity, out_size);
        case contracts::QolItemStateKind::kEnrichedInventory: {
            const auto write_error = [out, capacity, out_size](const char* error) {
                const std::string body = std::string("{\"ok\":false,\"error\":\"") + error +
                                         "\"}";
                if (body.size() > capacity) {
                    return error_result(kQolErrBlocked, "item state buffer too small");
                }
                std::memcpy(out, body.data(), body.size());
                *out_size = static_cast<uint32_t>(body.size());
                return contracts::ok();
            };
            static thread_local std::vector<char> raw_inventory(kInventoryCapacity);
            uint32_t raw_size = 0;
            const QolResult serialized = serialize_inventory_json(
                raw_inventory.data(), static_cast<uint32_t>(raw_inventory.size()), &raw_size);
            if (!contracts::succeeded(serialized)) return serialized;
            if (std::strncmp(raw_inventory.data(), "{\"error\":", 9) == 0) {
                if (raw_size > capacity) {
                    return error_result(kQolErrBlocked, "item state buffer too small");
                }
                std::memcpy(out, raw_inventory.data(), raw_size);
                *out_size = raw_size;
                return contracts::ok();
            }
            std::string body;
            if (!enrich_inventory(raw_inventory.data(), raw_size, g_port, &body)) {
                return write_error("internal error");
            }
            if (body.size() > capacity) {
                return error_result(kQolErrBlocked, "item state buffer too small");
            }
            std::memcpy(out, body.data(), body.size());
            *out_size = static_cast<uint32_t>(body.size());
            return contracts::ok();
        }
        default:
            return error_result(kQolErrInvalidArgument, "invalid item state kind");
    }
}

void update_snapshot() {
    if (g_port == nullptr || g_port->read.inventory_item_view == nullptr ||
        g_port->read.item_ref_at == nullptr ||
        g_port->read.shop_items_json == nullptr || g_port->read.money == nullptr ||
        g_port->read.in_world == nullptr) {
        log_snapshot_serialize_failure();
        return;
    }
    // 静态名称表未下发时不发布，避免输出空名称；HTTP 侧表现为 not ready。
    if (g_port->read.static_ready != nullptr) {
        int32_t ready = 0;
        if (!contracts::succeeded(g_port->read.static_ready(&ready))) {
            log_snapshot_serialize_failure();
            return;
        }
        if (ready == 0) return;
    }

    ItemSnapshot next{};
    int32_t in_world = 0;
    if (!contracts::succeeded(g_port->read.in_world(&in_world))) {
        log_snapshot_serialize_failure();
        return;
    }
    if (in_world == 0) {
        publish_not_in_game(next);
        return;
    }

    thread_local std::vector<char> inventory(kInventoryCapacity);
    std::fill(inventory.begin(), inventory.end(), '\0');
    uint32_t inventory_size = 0;
    if (!contracts::succeeded(serialize_inventory_json(inventory.data(), kInventoryCapacity,
                                                       &inventory_size))) {
        log_snapshot_serialize_failure();
        return;
    }
    if (std::strncmp(inventory.data(), "{\"error\":", 9) == 0) {
        publish_not_in_game(next);
        return;
    }
    if (!copy_buffer(next.inventory, kInventoryCapacity, &next.inventory_size, inventory.data(),
                     inventory_size)) {
        log_snapshot_serialize_failure();
        return;
    }

    char shop[kShopCapacity]{};
    uint32_t shop_size = 0;
    if (!contracts::succeeded(g_port->read.shop_items_json(shop, kShopCapacity, &shop_size))) {
        log_snapshot_serialize_failure();
        return;
    }
    if (!copy_buffer(next.shop, kShopCapacity, &next.shop_size, shop, shop_size)) {
        log_snapshot_serialize_failure();
        return;
    }

    if (!contracts::succeeded(g_port->read.money(&next.money))) {
        log_snapshot_serialize_failure();
        return;
    }
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

// 原版物理袋数量（0..5，含任务袋 5）；其余袋号由中立 provider 认领，本函数不感知其编号。
constexpr int32_t kOriginalInventoryBagCount = 6;

bool find_item_slot(int32_t category, int32_t* bag_out, int32_t* slot_out) {
    if (g_port == nullptr || g_port->read.item_ref_at == nullptr || bag_out == nullptr ||
        slot_out == nullptr) return false;
    for (int32_t bag = 0; bag < kOriginalInventoryBagCount; ++bag) {
        for (int32_t slot = 0; slot < 16; ++slot) {
            contracts::QolInventoryItemRef item{};
            const QolResult result = g_port->read.item_ref_at(bag, slot, &item);
            if (!contracts::succeeded(result) || item.category != category) continue;
            *bag_out = item.bag;
            *slot_out = item.slot;
            return true;
        }
    }
    const InventoryExtensionAccessResult access = inventory_extension();
    if (!access.available || access.provider == nullptr ||
        access.provider->for_each_item == nullptr) {
        return false;
    }
    struct Ctx {
        int32_t category;
        int32_t* bag;
        int32_t* slot;
        bool found;
    } ctx{category, bag_out, slot_out, false};
    (void)access.provider->for_each_item(
        [](const contracts::QolInventoryExtensionItemRef* ref, void* raw) -> int32_t {
            auto* p = static_cast<Ctx*>(raw);
            if (ref->category != p->category) return 0;
            *p->bag = ref->bag;
            *p->slot = ref->slot;
            p->found = true;
            return 1;
        },
        &ctx);
    return ctx.found;
}

constexpr int32_t kResultBagCount = 11;
constexpr int32_t kResultSlotsPerBag = 16;
using InventoryResultSnapshot =
    std::array<std::string, kResultBagCount * kResultSlotsPerBag>;

std::size_t inventory_result_index(int32_t bag, int32_t slot) {
    return static_cast<std::size_t>(bag * kResultSlotsPerBag + slot);
}

bool read_enriched_item(int32_t bag, int32_t slot, std::string* out,
                        const InventoryExtensionAccessResult& extension_access) {
    if (out == nullptr || g_port == nullptr || g_port->read.item_ref_at == nullptr ||
        g_port->read.inventory_item_view == nullptr) {
        return false;
    }
    contracts::QolInventoryItemRef item_ref{};
    contracts::QolInventoryExtensionItemRef extension_item_ref{};
    const bool provider_item = bag >= kOriginalInventoryBagCount;
    QolResult ref_result = contracts::error(kQolErrNotFound, "empty slot", 10);
    if (!provider_item) {
        ref_result = g_port->read.item_ref_at(bag, slot, &item_ref);
    } else {
        if (extension_access.available && extension_access.provider != nullptr &&
            extension_access.provider->item_ref_at != nullptr) {
            ref_result = extension_access.provider->item_ref_at(bag, slot, &extension_item_ref);
        }
    }
    if (!contracts::succeeded(ref_result)) {
        if (ref_result.code == kQolErrNotFound || ref_result.code == kQolErrNotReady ||
            ref_result.code == kQolErrUnsupported) {
            *out = "null";
            return true;
        }
        return false;
    }
    const contracts::QolItemHandle item_handle = provider_item ? extension_item_ref.item
                                                               : item_ref.item;
    if (item_handle.slot == 0 || item_handle.generation == 0) {
        *out = "null";
        return true;
    }
    contracts::QolItemInventoryView view{};
    if (!contracts::succeeded(g_port->read.inventory_item_view(item_handle, &view)) ||
        view.option_count > contracts::kQolItemInventoryOptionCapacity) {
        return false;
    }
    uint32_t count = 0;
    bool count_present = false;
    if (!effective_item_count(item_handle, view.count_supported != 0, &count,
                              &count_present)) {
        return false;
    }
    std::string raw;
    append_inventory_item(&raw, slot, view, count_present, count);
    JsonValue item{};
    if (!json_parse(raw.data(), static_cast<uint32_t>(raw.size()), &item) ||
        !enrich_item_value(item, g_port, out)) {
        return false;
    }
    return true;
}

bool capture_inventory_result(InventoryResultSnapshot* out) {
    if (out == nullptr) return false;
    const InventoryExtensionAccessResult extension_access = inventory_extension();
    for (int32_t bag = 0; bag < kResultBagCount; ++bag) {
        for (int32_t slot = 0; slot < kResultSlotsPerBag; ++slot) {
            if (!read_enriched_item(bag, slot, &(*out)[inventory_result_index(bag, slot)],
                                    extension_access)) {
                return false;
            }
        }
    }
    return true;
}

void append_result_slot(std::string& body, int32_t bag, int32_t slot,
                        const InventoryResultSnapshot& snapshot, bool* first) {
    if (!*first) body.push_back(',');
    *first = false;
    body += "{\"bag\":" + std::to_string(bag) + ",\"slot\":" +
            std::to_string(slot) + ",\"item\":" +
            snapshot[inventory_result_index(bag, slot)] + "}";
}

void append_inventory_changes(std::string& body, const InventoryResultSnapshot& before,
                              const InventoryResultSnapshot& after,
                              const std::array<bool, kResultBagCount * kResultSlotsPerBag>& forced,
                              bool* first) {
    for (int32_t bag = 0; bag < kResultBagCount; ++bag) {
        for (int32_t slot = 0; slot < kResultSlotsPerBag; ++slot) {
            const std::size_t index = inventory_result_index(bag, slot);
            if (!forced[index] && before[index] == after[index]) continue;
            append_result_slot(body, bag, slot, after, first);
        }
    }
}

void append_unforced_inventory_changes(
    std::string& body, const InventoryResultSnapshot& before,
    const InventoryResultSnapshot& after,
    const std::array<bool, kResultBagCount * kResultSlotsPerBag>& forced, bool* first) {
    for (int32_t bag = 0; bag < kResultBagCount; ++bag) {
        for (int32_t slot = 0; slot < kResultSlotsPerBag; ++slot) {
            const std::size_t index = inventory_result_index(bag, slot);
            if (forced[index] || before[index] == after[index]) continue;
            append_result_slot(body, bag, slot, after, first);
        }
    }
}

bool write_move_slots_result(const contracts::QolItemWriteArgs& args,
                             const InventoryResultSnapshot& before,
                             char* out, uint32_t capacity, uint32_t* out_size) {
    InventoryResultSnapshot after{};
    if (!capture_inventory_result(&after)) return false;
    std::array<bool, kResultBagCount * kResultSlotsPerBag> forced{};
    std::string body = "{\"ok\":true,\"slots\":[";
    bool first = true;
    if (args.i0 >= 0 && args.i0 < kResultBagCount && args.i1 >= 0 &&
        args.i1 < kResultSlotsPerBag) {
        forced[inventory_result_index(args.i0, args.i1)] = true;
        append_result_slot(body, args.i0, args.i1, after, &first);
    }
    if (args.i3 >= 0 && args.i3 < kResultBagCount && args.i4 >= 0 &&
        args.i4 < kResultSlotsPerBag &&
        (args.i0 != args.i3 || args.i1 != args.i4)) {
        forced[inventory_result_index(args.i3, args.i4)] = true;
        append_result_slot(body, args.i3, args.i4, after, &first);
    }
    append_unforced_inventory_changes(body, before, after, forced, &first);
    body += "]}";
    return copy_result_truncated(out, capacity, out_size, body);
}

bool read_character_state(contracts::QolCharacterStateKind kind, std::string* out) {
    if (out == nullptr) return false;
    const contracts::CharacterStateApi* state = character_state_capability();
    if (state == nullptr || state->write_state_json == nullptr) return false;
    std::vector<char> buffer(kind == contracts::QolCharacterStateKind::kParty ? 65536 : 4096);
    uint32_t size = 0;
    if (!contracts::succeeded(state->write_state_json(kind, buffer.data(),
                                                      static_cast<uint32_t>(buffer.size()),
                                                      &size)) ||
        size > buffer.size()) {
        return false;
    }
    out->assign(buffer.data(), size);
    return true;
}

const JsonValue* party_member(const JsonValue& party, int32_t role) {
    if (party.type == JsonValue::Type::kArray) {
        return role >= 0 && static_cast<std::size_t>(role) < party.items.size()
                   ? &party.items[static_cast<std::size_t>(role)]
                   : nullptr;
    }
    const JsonValue* roles = party.find("roles");
    if (roles == nullptr || roles->type != JsonValue::Type::kArray) return nullptr;
    for (const JsonValue& member : roles->items) {
        long long slot = -1;
        const JsonValue* slot_value = member.find("slot");
        if (slot_value != nullptr && slot_value->as_int(&slot) && slot == role) return &member;
    }
    return nullptr;
}

bool json_int(const JsonValue* object, const char* key, int32_t* out) {
    if (object == nullptr || out == nullptr) return false;
    const JsonValue* value = object->find(key);
    long long parsed = 0;
    if (value == nullptr || !value->as_int(&parsed) || parsed < INT32_MIN || parsed > INT32_MAX) {
        return false;
    }
    *out = static_cast<int32_t>(parsed);
    return true;
}

bool active_role(int32_t* out_role) {
    std::string player_json;
    JsonValue player{};
    if (!read_character_state(contracts::QolCharacterStateKind::kPlayer, &player_json) ||
        !json_parse(player_json.data(), static_cast<uint32_t>(player_json.size()), &player)) {
        return false;
    }
    return json_int(&player, "leader_slot", out_role);
}

bool read_active_health(int32_t* out_hp, int32_t* out_mp) {
    int32_t role = -1;
    std::string party_json;
    JsonValue party{};
    if (!active_role(&role) ||
        !read_character_state(contracts::QolCharacterStateKind::kParty, &party_json) ||
        !json_parse(party_json.data(), static_cast<uint32_t>(party_json.size()), &party)) {
        return false;
    }
    const JsonValue* member = party_member(party, role);
    return json_int(member, "hp", out_hp) && json_int(member, "mp", out_mp);
}

bool read_equipment_item(int32_t role, int32_t equip_slot, std::string* out) {
    if (out == nullptr || role < 0 || equip_slot < 0) return false;
    std::string party_json;
    JsonValue party{};
    if (!read_character_state(contracts::QolCharacterStateKind::kParty, &party_json) ||
        !json_parse(party_json.data(), static_cast<uint32_t>(party_json.size()), &party)) {
        return false;
    }
    const JsonValue* member = party_member(party, role);
    const JsonValue* equipment = member != nullptr ? member->find("equipment") : nullptr;
    if (equipment == nullptr || equipment->type != JsonValue::Type::kArray ||
        static_cast<std::size_t>(equip_slot) >= equipment->items.size()) {
        return false;
    }
    const JsonValue& item = equipment->items[static_cast<std::size_t>(equip_slot)];
    if (item.type == JsonValue::Type::kNull) {
        *out = "null";
        return true;
    }
    return enrich_item_value(item, g_port, out);
}

bool json_value_equal(const JsonValue& left, const JsonValue& right) {
    if (left.type != right.type) return false;
    switch (left.type) {
        case JsonValue::Type::kNull: return true;
        case JsonValue::Type::kBool: return left.boolean == right.boolean;
        case JsonValue::Type::kNumber: return left.integer == right.integer;
        case JsonValue::Type::kString: return left.string == right.string;
        case JsonValue::Type::kArray:
            if (left.items.size() != right.items.size()) return false;
            for (std::size_t i = 0; i < left.items.size(); ++i) {
                if (!json_value_equal(left.items[i], right.items[i])) return false;
            }
            return true;
        case JsonValue::Type::kObject:
            if (left.member_keys != right.member_keys ||
                left.member_values.size() != right.member_values.size()) {
                return false;
            }
            for (std::size_t i = 0; i < left.member_values.size(); ++i) {
                if (!json_value_equal(left.member_values[i], right.member_values[i])) return false;
            }
            return true;
    }
    return false;
}

int32_t changed_equipment_slot(int32_t role, const std::string& before_json,
                               const std::string& after_json) {
    JsonValue before{};
    JsonValue after{};
    if (!json_parse(before_json.data(), static_cast<uint32_t>(before_json.size()), &before) ||
        !json_parse(after_json.data(), static_cast<uint32_t>(after_json.size()), &after)) {
        return -1;
    }
    const JsonValue* before_member = party_member(before, role);
    const JsonValue* after_member = party_member(after, role);
    const JsonValue* before_equipment = before_member != nullptr
                                            ? before_member->find("equipment")
                                            : nullptr;
    const JsonValue* after_equipment = after_member != nullptr
                                           ? after_member->find("equipment")
                                           : nullptr;
    if (before_equipment == nullptr || after_equipment == nullptr ||
        before_equipment->type != JsonValue::Type::kArray ||
        after_equipment->type != JsonValue::Type::kArray) {
        return -1;
    }
    const std::size_t count = std::min(before_equipment->items.size(), after_equipment->items.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (!json_value_equal(before_equipment->items[i], after_equipment->items[i])) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
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

bool write_item_inventory_state_result(const contracts::QolItemWriteArgs& args, char* out,
                                       uint32_t capacity, uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr || g_item_state_write == nullptr) return false;
    *out_size = 0;
    contracts::QolItemInventoryStateWriteResult operation_data{};
    QolResult operation = contracts::error(kQolErrUnsupported, "item operation unavailable", 26);
    const auto kind = static_cast<contracts::QolItemWriteKind>(args.kind);
    InventoryResultSnapshot inventory_before{};
    const bool tracks_inventory =
        kind == contracts::QolItemWriteKind::kUseItem ||
        kind == contracts::QolItemWriteKind::kSellItem ||
        kind == contracts::QolItemWriteKind::kDiscardItem ||
        kind == contracts::QolItemWriteKind::kShopBuy ||
        kind == contracts::QolItemWriteKind::kMoveItem;
    if (tracks_inventory) (void)capture_inventory_result(&inventory_before);
    int32_t hp_before = 0;
    int32_t mp_before = 0;
    const bool health_before_available =
        kind == contracts::QolItemWriteKind::kUseItem &&
        read_active_health(&hp_before, &mp_before);
    switch (kind) {
        case contracts::QolItemWriteKind::kUseItem:
            operation = g_item_state_write->use_item(args.i0, args.i1, &operation_data);
            break;
        case contracts::QolItemWriteKind::kSellItem:
            operation = g_item_state_write->sell_item(args.i0, args.i1, &operation_data);
            break;
        case contracts::QolItemWriteKind::kDiscardItem:
            operation = g_item_state_write->discard_item(args.i0, args.i1, &operation_data);
            break;
        case contracts::QolItemWriteKind::kShopBuy:
            operation = g_item_state_write->shop_buy(args.i0, &operation_data);
            break;
        case contracts::QolItemWriteKind::kMoveItem:
            operation = g_item_state_write->move_item(args.i0, args.i1, args.i2, args.i3,
                                                       args.i4, &operation_data);
            break;
        case contracts::QolItemWriteKind::kAddItem: {
            operation = g_item_state_write->add_item(
                args.i0, args.i1, args.i2, args.i3, args.i4,
                nullptr, &operation_data);
            break;
        }
        default:
            return false;
    }

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

    if (kind == contracts::QolItemWriteKind::kAddItem) {
        constexpr char kAddItemSuccess[] = "{\"ok\":true}";
        return copy_buffer(out, capacity, out_size, kAddItemSuccess,
                           sizeof(kAddItemSuccess) - 1);
    }

    std::string body = "{\"ok\":true";
    if (kind == contracts::QolItemWriteKind::kUseItem &&
        (operation_data.flags & contracts::kQolItemInventoryStateHasGained) != 0) {
        if (operation_data.gained_count > contracts::kQolItemInventoryStateGainedCapacity) {
            constexpr char kBadGained[] = "{\"ok\":false,\"error\":\"invalid gained item count\"}";
            (void)copy_buffer(out, capacity, out_size, kBadGained, sizeof(kBadGained) - 1);
            return false;
        }
        body += ",\"gained\":[";
        for (uint32_t i = 0; i < operation_data.gained_count; ++i) {
            if (i != 0) body.push_back(',');
            const auto& item = operation_data.gained[i];
            body += "{\"bag\":" + std::to_string(item.bag) +
                    ",\"slot\":" + std::to_string(item.slot) +
                    ",\"category\":" + std::to_string(item.category) +
                    ",\"count\":" + std::to_string(item.count) + "}";
        }
        body.push_back(']');
    }
    if (kind == contracts::QolItemWriteKind::kSellItem) {
        body += ",\"price\":" + std::to_string(operation_data.value);
    }

    InventoryResultSnapshot inventory_after{};
    if (tracks_inventory && !capture_inventory_result(&inventory_after)) {
        constexpr char kInventoryUnavailable[] =
            "{\"ok\":false,\"error\":\"inventory state unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kInventoryUnavailable,
                          sizeof(kInventoryUnavailable) - 1);
        return false;
    }
    if (tracks_inventory) {
        body += ",\"slots\":[";
        std::array<bool, kResultBagCount * kResultSlotsPerBag> forced{};
        if (kind == contracts::QolItemWriteKind::kUseItem ||
            kind == contracts::QolItemWriteKind::kSellItem ||
            kind == contracts::QolItemWriteKind::kDiscardItem) {
            if (args.i0 >= 0 && args.i0 < kResultBagCount && args.i1 >= 0 &&
                args.i1 < kResultSlotsPerBag) {
                forced[inventory_result_index(args.i0, args.i1)] = true;
            }
        } else if (kind == contracts::QolItemWriteKind::kMoveItem) {
            if (args.i0 >= 0 && args.i0 < kResultBagCount && args.i1 >= 0 &&
                args.i1 < kResultSlotsPerBag) {
                forced[inventory_result_index(args.i0, args.i1)] = true;
            }
            if (args.i3 >= 0 && args.i3 < kResultBagCount && args.i4 >= 0 &&
                args.i4 < kResultSlotsPerBag) {
                forced[inventory_result_index(args.i3, args.i4)] = true;
            }
        }
        if (kind == contracts::QolItemWriteKind::kUseItem &&
            (operation_data.flags & contracts::kQolItemInventoryStateHasGained) != 0) {
            for (uint32_t i = 0; i < operation_data.gained_count; ++i) {
                const auto& gained = operation_data.gained[i];
                if (gained.bag >= 0 && gained.bag < kResultBagCount && gained.slot >= 0 &&
                    gained.slot < kResultSlotsPerBag) {
                    forced[inventory_result_index(gained.bag, gained.slot)] = true;
                }
            }
        }
        bool first_slot = true;
        if (kind == contracts::QolItemWriteKind::kMoveItem) {
            if (args.i0 >= 0 && args.i0 < kResultBagCount && args.i1 >= 0 &&
                args.i1 < kResultSlotsPerBag) {
                append_result_slot(body, args.i0, args.i1, inventory_after, &first_slot);
            }
            if (args.i3 >= 0 && args.i3 < kResultBagCount && args.i4 >= 0 &&
                args.i4 < kResultSlotsPerBag && (args.i0 != args.i3 || args.i1 != args.i4)) {
                append_result_slot(body, args.i3, args.i4, inventory_after, &first_slot);
            }
            append_unforced_inventory_changes(body, inventory_before, inventory_after, forced,
                                              &first_slot);
        } else {
            append_inventory_changes(body, inventory_before, inventory_after, forced, &first_slot);
        }
        body.push_back(']');
    }
    if (kind == contracts::QolItemWriteKind::kSellItem ||
        kind == contracts::QolItemWriteKind::kShopBuy) {
        int64_t money_after = 0;
        if (!contracts::succeeded(money(&money_after))) {
            constexpr char kMoneyUnavailable[] =
                "{\"ok\":false,\"error\":\"money unavailable\"}";
            (void)copy_buffer(out, capacity, out_size, kMoneyUnavailable,
                              sizeof(kMoneyUnavailable) - 1);
            return false;
        }
        if (kind == contracts::QolItemWriteKind::kShopBuy) {
            const std::size_t slots_key = body.find(",\"slots\":");
            body.insert(slots_key,
                        ",\"price\":" + std::to_string(operation_data.value) +
                            ",\"money\":" + std::to_string(money_after));
        } else {
            body += ",\"money\":" + std::to_string(money_after);
        }
    }
    if (kind == contracts::QolItemWriteKind::kUseItem && health_before_available) {
        int32_t hp_after = 0;
        int32_t mp_after = 0;
        if (read_active_health(&hp_after, &mp_after) &&
            (hp_before != hp_after || mp_before != mp_after)) {
            body += ",\"player\":{";
            bool first_health = true;
            if (hp_before != hp_after) {
                body += "\"hp\":" + std::to_string(hp_after);
                first_health = false;
            }
            if (mp_before != mp_after) {
                if (!first_health) body.push_back(',');
                body += "\"mp\":" + std::to_string(mp_after);
            }
            body.push_back('}');
        }
    }
    body.push_back('}');
    return copy_result_truncated(out, capacity, out_size, body);
}

bool write_item_party_state_result(const contracts::QolItemWriteArgs& args, char* out,
                                   uint32_t capacity, uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr) return false;
    *out_size = 0;
    const contracts::CharacterStateApi* state = character_state_capability();
    if (state == nullptr || state->write_state_json == nullptr) {
        constexpr char kStateUnavailable[] =
            "{\"ok\":false,\"error\":\"character state unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kStateUnavailable,
                          sizeof(kStateUnavailable) - 1);
        return false;
    }
    if (g_item_state_write == nullptr) {
        constexpr char kOperationUnavailable[] =
            "{\"ok\":false,\"error\":\"item operation unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kOperationUnavailable,
                          sizeof(kOperationUnavailable) - 1);
        return false;
    }

    // 扩展 provider 由 host 写端口按中立 facade 解析并转发；core.item 不持有其指针。
    contracts::QolItemInventoryStateWriteResult operation_data{};
    QolResult operation = contracts::error(kQolErrUnsupported, "item operation unavailable", 26);
    const auto kind = static_cast<contracts::QolItemWriteKind>(args.kind);
    InventoryResultSnapshot inventory_before{};
    const bool tracks_inventory = kind != contracts::QolItemWriteKind::kEnchant;
    if (tracks_inventory) (void)capture_inventory_result(&inventory_before);
    std::string party_before_json;
    if (kind == contracts::QolItemWriteKind::kEquipItem &&
        !read_character_state(contracts::QolCharacterStateKind::kParty, &party_before_json)) {
        constexpr char kStateUnavailable[] =
            "{\"ok\":false,\"error\":\"character state unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kStateUnavailable,
                          sizeof(kStateUnavailable) - 1);
        return false;
    }
    switch (kind) {
        case contracts::QolItemWriteKind::kEquipItem: {
            int32_t bag = args.i1;
            int32_t slot = args.i2;
            if (args.i5 >= 0 && !find_item_slot(args.i5, &bag, &slot)) {
                constexpr char kNotFound[] = "{\"ok\":false,\"error\":\"item not found\"}";
                (void)copy_buffer(out, capacity, out_size, kNotFound, sizeof(kNotFound) - 1);
                return false;
            }
            operation = g_item_state_write->equip_item(args.i0, bag, slot, nullptr,
                                                        &operation_data);
            break;
        }
        case contracts::QolItemWriteKind::kUnequipItem:
            operation = g_item_state_write->unequip_item(args.i0, args.i1, nullptr,
                                                          &operation_data);
            break;
        case contracts::QolItemWriteKind::kPutJewel:
            operation = g_item_state_write->put_jewel(args.i0, args.i1, args.i2, args.i3,
                                                       nullptr, &operation_data);
            break;
        case contracts::QolItemWriteKind::kEnchant:
            operation = g_item_state_write->enchant(args.i0, args.i1, args.i2, args.i3,
                                                     nullptr, &operation_data);
            break;
        default:
            return false;
    }
    if (!contracts::succeeded(operation)) {
        const char* message = operation.message != nullptr ? operation.message
                                                           : "item operation failed";
        const uint32_t message_size = operation.message != nullptr
                                          ? operation.message_len
                                          : static_cast<uint32_t>(std::strlen(message));
        const std::string body = std::string("{\"ok\":false,\"error\":\"") +
                                 std::string(message, message_size) + "\"}";
        (void)copy_buffer(out, capacity, out_size, body.data(),
                          static_cast<uint32_t>(body.size()));
        return false;
    }

    std::string party_after_json;
    int32_t equip_slot = kind == contracts::QolItemWriteKind::kUnequipItem ? args.i1 : args.i3;
    if (kind == contracts::QolItemWriteKind::kEquipItem) {
        if (!read_character_state(contracts::QolCharacterStateKind::kParty, &party_after_json)) {
            constexpr char kStateUnavailable[] =
                "{\"ok\":false,\"error\":\"character state unavailable\"}";
            (void)copy_buffer(out, capacity, out_size, kStateUnavailable,
                              sizeof(kStateUnavailable) - 1);
            return false;
        }
        equip_slot = changed_equipment_slot(args.i0, party_before_json, party_after_json);
    }
    std::string equipment_item = "null";
    if (kind == contracts::QolItemWriteKind::kUnequipItem) {
        equipment_item = "null";
    } else if (equip_slot < 0 || !read_equipment_item(args.i0, equip_slot, &equipment_item)) {
        constexpr char kStateUnavailable[] =
            "{\"ok\":false,\"error\":\"character state unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kStateUnavailable,
                          sizeof(kStateUnavailable) - 1);
        return false;
    }
    std::string body = "{\"ok\":true,\"equipment\":{\"role\":" +
                       std::to_string(args.i0) + ",\"slot\":" +
                       std::to_string(equip_slot) + ",\"item\":" + equipment_item + "}";
    InventoryResultSnapshot inventory_after{};
    if (tracks_inventory) {
        if (!capture_inventory_result(&inventory_after)) {
            constexpr char kInventoryUnavailable[] =
                "{\"ok\":false,\"error\":\"inventory state unavailable\"}";
            (void)copy_buffer(out, capacity, out_size, kInventoryUnavailable,
                              sizeof(kInventoryUnavailable) - 1);
            return false;
        }
        body += ",\"inventory\":[";
        std::array<bool, kResultBagCount * kResultSlotsPerBag> forced{};
        bool first_slot = true;
        append_inventory_changes(body, inventory_before, inventory_after, forced, &first_slot);
        body.push_back(']');
    }
    body.push_back('}');
    return copy_result_truncated(out, capacity, out_size, body);
}

bool write_item_dice_accept_result(const contracts::QolItemWriteArgs& args, char* out,
                                   uint32_t capacity, uint32_t* out_size) {
    (void)args;
    if (out == nullptr || out_size == nullptr) return false;
    *out_size = 0;
    // 状态能力缺失即 fail-closed，accept 不执行（写未发生）。
    const contracts::CharacterStateApi* state = character_state_capability();
    if (state == nullptr || state->write_state_json == nullptr) {
        constexpr char kStateUnavailable[] =
            "{\"ok\":false,\"error\":\"character state unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kStateUnavailable,
                          sizeof(kStateUnavailable) - 1);
        return false;
    }
    if (g_item_dice_write == nullptr || g_item_dice_write->accept == nullptr) {
        constexpr char kOperationUnavailable[] =
            "{\"ok\":false,\"error\":\"item operation unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kOperationUnavailable,
                          sizeof(kOperationUnavailable) - 1);
        return false;
    }
    int32_t role = -1;
    std::string party_before_json;
    JsonValue party_before{};
    if (!active_role(&role) ||
        !read_character_state(contracts::QolCharacterStateKind::kParty, &party_before_json) ||
        !json_parse(party_before_json.data(), static_cast<uint32_t>(party_before_json.size()),
                    &party_before)) {
        constexpr char kStateUnavailable[] =
            "{\"ok\":false,\"error\":\"character state unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kStateUnavailable,
                          sizeof(kStateUnavailable) - 1);
        return false;
    }
    contracts::QolItemDiceAcceptResult accept{};
    const QolResult operation = g_item_dice_write->accept(&accept);
    if (!contracts::succeeded(operation)) {
        const char* message = operation.message != nullptr ? operation.message
                                                           : "dice accept failed";
        const uint32_t message_size = operation.message != nullptr
                                          ? operation.message_len
                                          : static_cast<uint32_t>(std::strlen(message));
        const std::string body = std::string("{\"ok\":false,\"error\":\"") +
                                 std::string(message, message_size) + "\"}";
        (void)copy_buffer(out, capacity, out_size, body.data(),
                          static_cast<uint32_t>(body.size()));
        return false;
    }
    std::string party_after_json;
    JsonValue party_after{};
    if (!read_character_state(contracts::QolCharacterStateKind::kParty, &party_after_json) ||
        !json_parse(party_after_json.data(), static_cast<uint32_t>(party_after_json.size()),
                    &party_after)) {
        constexpr char kStateUnavailable[] =
            "{\"ok\":false,\"error\":\"character state unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kStateUnavailable,
                          sizeof(kStateUnavailable) - 1);
        return false;
    }
    const JsonValue* before_member = party_member(party_before, role);
    const JsonValue* after_member = party_member(party_after, role);
    const JsonValue* before_stats = before_member != nullptr ? before_member->find("main_stats")
                                                             : nullptr;
    const JsonValue* after_stats = after_member != nullptr ? after_member->find("main_stats")
                                                           : nullptr;
    int32_t status_point = 0;
    if (before_stats == nullptr || after_stats == nullptr ||
        before_stats->type != JsonValue::Type::kArray ||
        after_stats->type != JsonValue::Type::kArray ||
        !json_int(after_member, "status_point", &status_point)) {
        constexpr char kStateUnavailable[] =
            "{\"ok\":false,\"error\":\"character state unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kStateUnavailable,
                          sizeof(kStateUnavailable) - 1);
        return false;
    }
    std::string body = "{\"ok\":true,\"base\":[";
    for (uint32_t i = 0; i < contracts::kQolItemDiceStatCount; ++i) {
        if (i != 0) body.push_back(',');
        body += std::to_string(accept.base[i]);
    }
    body += "],\"applied\":[";
    for (uint32_t i = 0; i < contracts::kQolItemDiceStatCount; ++i) {
        if (i != 0) body.push_back(',');
        body += std::to_string(accept.applied[i]);
    }
    body += "],\"delta\":[";
    for (uint32_t i = 0; i < contracts::kQolItemDiceStatCount; ++i) {
        if (i != 0) body.push_back(',');
        body += std::to_string(accept.delta[i]);
    }
    body += "],\"player\":{\"main_stats\":[";
    bool first_stat = true;
    const std::size_t stat_count = std::min(before_stats->items.size(), after_stats->items.size());
    for (std::size_t i = 0; i < stat_count; ++i) {
        long long before_value = 0;
        long long after_value = 0;
        if (!before_stats->items[i].as_int(&before_value) ||
            !after_stats->items[i].as_int(&after_value) || before_value == after_value) {
            continue;
        }
        if (!first_stat) body.push_back(',');
        first_stat = false;
        body += "{\"attr\":" + std::to_string(i) + ",\"value\":" +
                std::to_string(after_value) + "}";
    }
    body += "],\"status_point\":" + std::to_string(status_point) + "}";
    body.push_back('}');
    return copy_result_truncated(out, capacity, out_size, body);
}

bool write_item_move_result(const contracts::QolItemWriteArgs& args, char* out,
                            uint32_t capacity, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    const InventoryExtensionAccessResult access = inventory_extension();
    const bool provider_requested =
        access.available &&
        (provider_owns_bag(access, args.i0) || provider_owns_bag(access, args.i3));
    if (provider_requested && access.provider != nullptr &&
        access.provider->move_item != nullptr) {
        InventoryResultSnapshot inventory_before{};
        (void)capture_inventory_result(&inventory_before);
        char error_body[4096]{};
        uint32_t move_result_size = 0;
        const int32_t move_status = access.provider->move_item(
            args.i0, args.i1, args.i3, args.i4, error_body, sizeof(error_body), &move_result_size);
        if (move_status != 0) {
            return write_move_slots_result(args, inventory_before, out, capacity, out_size);
        }
        if (move_result_size <= sizeof(error_body) &&
            move_result_size >= 11 &&
            std::string_view(error_body, move_result_size).compare(0, 11, "{\"ok\":false") == 0) {
            (void)copy_buffer(out, capacity, out_size, error_body, move_result_size);
            return false;
        }
        constexpr char kMoveFailed[] =
            "{\"ok\":false,\"error\":\"extension bag move failed\"}";
        (void)copy_buffer(out, capacity, out_size, kMoveFailed, sizeof(kMoveFailed) - 1);
        return false;
    }
    return write_item_inventory_state_result(args, out, capacity, out_size);
}

bool item_write_result(void*, const uint8_t* payload, uint32_t size, char* out, uint32_t capacity,
                       uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (payload == nullptr || size != sizeof(contracts::QolItemWriteArgs) || out == nullptr ||
        out_size == nullptr) {
        return false;
    }
    contracts::QolItemWriteArgs args{};
    std::memcpy(&args, payload, size);
    const auto kind = static_cast<contracts::QolItemWriteKind>(args.kind);
    if (kind == contracts::QolItemWriteKind::kEquipItem ||
        kind == contracts::QolItemWriteKind::kUnequipItem ||
        kind == contracts::QolItemWriteKind::kPutJewel ||
        kind == contracts::QolItemWriteKind::kEnchant) {
        return write_item_party_state_result(args, out, capacity, out_size);
    }
    if (kind == contracts::QolItemWriteKind::kAcceptDice) {
        return write_item_dice_accept_result(args, out, capacity, out_size);
    }
    if (kind == contracts::QolItemWriteKind::kMoveItem) {
        return write_item_move_result(args, out, capacity, out_size);
    }
    if (kind == contracts::QolItemWriteKind::kAddItem) {
        return write_item_inventory_state_result(args, out, capacity, out_size);
    }
    if (kind == contracts::QolItemWriteKind::kUseItem ||
        kind == contracts::QolItemWriteKind::kSellItem ||
        kind == contracts::QolItemWriteKind::kDiscardItem ||
        kind == contracts::QolItemWriteKind::kShopBuy) {
        return write_item_inventory_state_result(args, out, capacity, out_size);
    }
    if (g_item_money_write == nullptr || g_item_money_write->set_money == nullptr ||
        g_item_dice_write == nullptr || g_item_dice_write->reject == nullptr) {
        return false;
    }
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
        const uint32_t copied = std::min(body_size, capacity);
        if (copied != 0) std::memcpy(out, body, copied);
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
        const uint32_t copied = std::min(body_size, capacity);
        if (copied != 0) std::memcpy(out, body, copied);
        *out_size = body_size;
        return contracts::succeeded(operation);
    }
    return false;
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
    g_degrade_warn_ticks = 0;
    g_port = nullptr;
    g_item_money_write = nullptr;
    g_item_dice_write = nullptr;
    g_item_state_write = nullptr;
    g_frame_task = 0;
    if (ctx.struct_size < contracts::PLUGIN_CONTEXT_PORTS_STRUCT_SIZE ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || ctx.ports == nullptr ||
        ctx.port_count != 4 || ctx.ports[0] == nullptr || ctx.ports[1] == nullptr ||
        ctx.ports[2] == nullptr || ctx.ports[3] == nullptr) {
        g_sdk = nullptr;
        return 1;
    }
    g_port = static_cast<const contracts::QolItemPort*>(ctx.ports[0]);
    if (!contracts::satisfies(g_port->abi_version,
                              {contracts::kQolPortItemVersion.major,
                               contracts::kQolPortItemVersion.minor}) ||
        !contracts::qol_item_port_size_valid(g_port) ||
        !contracts::qol_item_port_count_encoding_size_valid(g_port) ||
        g_port->read.category == nullptr || g_port->read.cumulate_count == nullptr ||
        g_port->read.item_ref_at == nullptr ||
        g_port->read.inventory_item_view == nullptr || g_port->read.shop_items_json == nullptr ||
        g_port->read.in_world == nullptr || g_port->read.money == nullptr ||
        g_port->read.static_text == nullptr || g_port->read.static_ready == nullptr ||
        g_port->read.count_encoding == nullptr) {
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
        g_item_dice_write->reject == nullptr || g_item_dice_write->accept == nullptr) {
        g_port = nullptr;
        g_item_money_write = nullptr;
        g_item_dice_write = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    g_item_state_write =
        static_cast<const contracts::QolItemInventoryStateWritePort*>(ctx.ports[3]);
    if (!contracts::satisfies(g_item_state_write->abi_version,
                              {contracts::kQolPortItemInventoryStateWriteVersion.major,
                               contracts::kQolPortItemInventoryStateWriteVersion.minor}) ||
        g_item_state_write->use_item == nullptr || g_item_state_write->sell_item == nullptr ||
        g_item_state_write->discard_item == nullptr || g_item_state_write->shop_buy == nullptr ||
        g_item_state_write->move_item == nullptr || g_item_state_write->add_item == nullptr ||
        g_item_state_write->equip_item == nullptr || g_item_state_write->unequip_item == nullptr ||
        g_item_state_write->put_jewel == nullptr || g_item_state_write->enchant == nullptr) {
        g_port = nullptr;
        g_item_money_write = nullptr;
        g_item_dice_write = nullptr;
        g_item_state_write = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    if (!g_snapshot.initialize(g_sdk)) {
        g_port = nullptr;
        g_item_money_write = nullptr;
        g_item_dice_write = nullptr;
        g_item_state_write = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    ItemSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    // 结果处理器需要 host 尾部的票据结果面（QolHostSdk 尾部追加，按 abi 协商）。
    if (g_sdk->abi.size < sizeof(contracts::QolHostSdk) || g_sdk->abi.minor < 9 ||
        g_sdk->ticket_result.register_typed_result_handler == nullptr ||
        !contracts::succeeded(g_sdk->ticket_result.register_typed_result_handler(
            kItemWriteHandlerId, &item_write_result, nullptr))) {
        g_snapshot.destroy();
        g_port = nullptr;
        g_item_money_write = nullptr;
        g_item_dice_write = nullptr;
        g_item_state_write = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    log_message(g_sdk, "[core.item] provider install");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    stop_frame_task(ctx.sdk);
    g_snapshot.destroy();
    log_message(ctx.sdk, "[core.item] provider deactivate");
    if (g_sdk != nullptr && g_sdk->ticket_result.unregister_typed_result_handler != nullptr) {
        (void)g_sdk->ticket_result.unregister_typed_result_handler(kItemWriteHandlerId);
    }
    g_port = nullptr;
    g_item_money_write = nullptr;
    g_item_dice_write = nullptr;
    g_item_state_write = nullptr;
    g_sdk = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    if (!g_snapshot.initialize(g_sdk)) {
        log_message(ctx.sdk, "[core.item] snapshot unavailable");
        return;
    }
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
    g_snapshot.destroy();
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
    2,
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

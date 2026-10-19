// item_facts HTTP dispatch 接缝 host 测试：
// 覆盖 6 条 /api/item/* GET query handler 的注册、快照发布前的 not-ready、发布后的富化响应
// （字段顺序与迁移前 Kotlin restructureItem 一致）、非 world 错误体、owner 注销墓碑。
//
// 真实 registry/dispatch（host/http_registry.cpp、host/http_dispatch.cpp）参与链接，
// 游戏侧只以 fake QolItemPort 注入，避免依赖真机内存。

#include "contracts/game_ports/ports.h"
#include "contracts/game_ports/item_port.h"
#include "contracts/game_ports/item_dice_write_port.h"
#include "contracts/game_ports/item_money_write_port.h"
#include "contracts/game_ports/item_write_port.h"
#include "contracts/game_ports/item_inventory_state_write_port.h"
#include "contracts/extension_bag.h"
#include "contracts/inventory_extension.h"
#include "contracts/character_state.h"
#include "contracts/item_count.h"
#include "contracts/item_facts.h"
#include "contracts/item_state.h"
#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "contracts/result.h"
#include "bridge/native/item_write_result.h"
#include "core/native/inventory_trade.h"
#include "game/access/game_access.h"
#include "game/adapter_ports/inventory_extension_facade.h"
#include "game/adapter_ports/item_count_facade.h"
#include "game/ops/game_ops_common.h"
#include "game/read/item_class.h"
#include "game/state/game_state.h"
#include "game/access/inventory_legacy_read.h"
#include "host/http_dispatch.h"
#include "host/http_registry.h"
#include "http_endpoint_test_support.h"
#include "host/owner_cache_builder.h"
#include "infra/command_queue.h"
#include "plugins/core.item/plugin.h"
#include "test_snapshot_sdk.h"

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

using qol::contracts::QolItemPort;
using qol::contracts::QolItemPortReadApi;
using qol::contracts::QolItemWriteArgs;
using qol::contracts::QolItemStaticTextKind;

constexpr char kInventoryJson[] =
    "{\"bags\":[{\"bag\":0,\"items\":[{\"slot\":0,\"type_flags\":40,\"raw_rarity\":0,"
    "\"category\":10,\"count\":3,\"equip\":false,\"rarity\":0,\"damage\":0,\"defense\":0,"
    "\"magic_rate\":0,\"need_level\":0,\"socket\":0,\"socket_filled\":0,\"socket_total\":0,"
    "\"enchant\":0,\"chaos\":false,\"enchant_id\":0,\"enchant_level\":0,\"chaos_level\":0,"
    "\"chaos_rate\":100,\"options\":[],\"option_ids\":[],\"option_types\":[]}],"
    "\"capacity\":16,\"slot_count\":1},{\"bag\":1,\"items\":[],\"capacity\":16,\"slot_count\":0},"
    "{\"bag\":2,\"items\":[],\"capacity\":16,\"slot_count\":0},"
    "{\"bag\":3,\"items\":[],\"capacity\":16,\"slot_count\":0},"
    "{\"bag\":4,\"items\":[],\"capacity\":16,\"slot_count\":0},"
     "{\"bag\":5,\"items\":[],\"capacity\":16,\"slot_count\":0}]}";

constexpr char kShopJson[] =
    "{\"items\":[{\"slot\":0,\"category\":10,\"count\":5,\"price\":15}]}";

constexpr char kExpectedItem[] =
    "{\"slot\":0,\"name\":\"再生药水（小）\",\"category\":10,\"count\":3,"
    "\"item_type\":\"potion\",\"need_level\":0,\"rarity\":0,\"rarity_tier\":\"白\","
    "\"base\":{\"damage\":0,\"defense\":0,\"magic_rate\":0},\"bonus\":[],"
    "\"gem\":{\"total_slots\":0,\"slots\":[]},"
    "\"chaos\":{\"is_chaos\":false,\"level\":0,\"rate\":100},"
    "\"enchant\":{\"id\":0,\"level\":0,\"effect\":null}}";

std::string expected_item_with_count(uint32_t count) {
    std::string item(kExpectedItem);
    const std::string old_count = "\"count\":3,";
    const size_t position = item.find(old_count);
    if (position != std::string::npos) {
        item.replace(position, old_count.size(), "\"count\":" + std::to_string(count) + ",");
    }
    return item;
}

std::string expected_item_at_slot(uint32_t slot, uint32_t count, bool equip = false) {
    std::string item = expected_item_with_count(count);
    const std::string old_slot = "\"slot\":0,";
    const size_t position = item.find(old_slot);
    if (position != std::string::npos) {
        item.replace(position, old_slot.size(), "\"slot\":" + std::to_string(slot) + ",");
    }
    if (equip) {
        const std::string old_name = "\"name\":\"再生药水（小）\"";
        const size_t name_position = item.find(old_name);
        if (name_position != std::string::npos) {
            item.replace(name_position, old_name.size(), "\"name\":\"生锈的 再生药水（小）\"");
        }
    }
    return item;
}

std::string expected_slot_record(int32_t bag, int32_t slot, const std::string& item) {
    return "{\"bag\":" + std::to_string(bag) + ",\"slot\":" +
           std::to_string(slot) + ",\"item\":" + item + "}";
}

std::string expected_inventory_with_count(uint32_t count) {
    std::string inventory = std::string("{\"bags\":[{\"bag\":0,\"items\":[") +
        expected_item_with_count(count) + "],\"capacity\":16,\"slot_count\":1}";
    for (int32_t bag = 1; bag < 6; ++bag) {
        inventory += ",{\"bag\":" + std::to_string(bag) +
                     ",\"items\":[],\"capacity\":16,\"slot_count\":0}";
    }
    inventory += "]}";
    return inventory;
}

std::string expected_raw_inventory_with_count(uint32_t count) {
    std::string inventory(kInventoryJson);
    const std::string old_count = "\"count\":3,";
    const size_t position = inventory.find(old_count);
    if (position != std::string::npos) {
        inventory.replace(position, old_count.size(), "\"count\":" + std::to_string(count) + ",");
    }
    return inventory;
}

bool g_in_world = true;
int32_t g_static_ready = 1;
int32_t g_item_ref_result = kQolOk;
uint32_t g_canonical_count = 3;
uint32_t g_original_count = 3;
bool g_count_encoded = true;
bool g_effective_fail = false;
QolResult g_money_write_result = qol::contracts::ok();
int64_t g_money_write_value = 0;
QolResult g_dice_reject_result = qol::contracts::ok();
int g_dice_reject_calls = 0;
QolResult g_dice_accept_result = qol::contracts::ok();
int g_dice_accept_calls = 0;
int g_dice_accept_order = 0;
qol::contracts::QolItemDiceAcceptResult g_dice_accept_data{};
qol::contracts::QolTypedResultCommandHandler g_result_handler = nullptr;
uint32_t g_post_handler_id = 0;
uint8_t g_post_payload[qol::contracts::kTypedCommandPayloadCapacity]{};
uint32_t g_post_payload_size = 0;
int g_write_calls = 0;
qol::contracts::QolItemInventoryStateWriteResult g_state_write_data{};
QolResult g_state_write_result = qol::contracts::ok();
int g_state_write_calls = 0;
int g_state_write_kind = -1;
int32_t g_state_write_arg0 = -1;
int32_t g_state_write_arg1 = -1;
int32_t g_state_write_count = 2;
int32_t g_state_move_count = -1;
int32_t g_state_move_to_bag = -1;
int32_t g_state_move_to_slot = -1;
int32_t g_state_add_category = -1;
int32_t g_state_add_count = -1;
int32_t g_state_add_socket = -1;
int32_t g_state_add_enhance = -1;
int32_t g_state_add_rarity = -1;
bool g_inventory_slot_present[11][16] = {{true}};
uint32_t g_inventory_slot_count[11][16]{};
int32_t g_party_hp = 100;
int32_t g_party_mp = 50;
int32_t g_party_main_stats[5] = {10, 11, 13, 14, 15};
int32_t g_party_status_point = 7;
int32_t g_party_equipment_slot = -1;
bool g_use_changes_health = false;
const qol::contracts::QolItemCountApi* g_state_add_count_api = nullptr;
bool g_character_state_available = true;
int g_character_state_require_calls = 0;
int g_character_state_require_order = 0;
int g_character_state_write_calls = 0;
QolResult g_character_state_result = qol::contracts::ok();
int g_party_sequence = 0;
int g_party_write_order = 0;
int g_character_state_write_order = 0;
int g_extension_bag_require_calls = 0;
uint16_t g_extension_bag_required_minor = 0;
int g_party_write_calls = 0;
int32_t g_party_write_kind = -1;
int32_t g_party_write_args[4] = {-1, -1, -1, -1};
QolResult g_party_write_result = qol::contracts::ok();

QolResult unsupported() { return qol::contracts::error(kQolErrUnsupported, "stub", 4); }
bool g_override_available = false;
bool g_override_enabled = false;
QolResult override_canonical(const void*, uint32_t* out_count);
QolResult override_effective(const void*, uint32_t* out_count);
const qol::contracts::QolItemCountApi* override_item_count_api();
QolResult stub_category(qol::contracts::QolItemHandle, int32_t*) { return unsupported(); }
QolResult stub_is_real_equip(qol::contracts::QolItemHandle, int32_t*) { return unsupported(); }
QolResult stub_is_jewel(int32_t, int32_t*) { return unsupported(); }
QolResult stub_no_sell(int32_t, int32_t*) { return unsupported(); }
QolResult stub_sell_price(qol::contracts::QolItemHandle, int64_t*) { return unsupported(); }
std::size_t fake_slot_index(qol::contracts::QolItemHandle item) {
    return item.slot == 0 ? 176 : static_cast<std::size_t>(item.slot - 1u);
}
QolResult stub_cumulate(qol::contracts::QolItemHandle item, int32_t* out_count) {
    if (g_override_available && g_override_enabled) {
        uint32_t canonical = 0;
        const QolResult result = override_canonical(nullptr, &canonical);
        if (!qol::contracts::succeeded(result)) return result;
        *out_count = static_cast<int32_t>(canonical);
        return qol::contracts::ok();
    }
    const std::size_t index = fake_slot_index(item);
    *out_count = static_cast<int32_t>(index < 176 &&
                                             g_inventory_slot_count[index / 16][index % 16] != 0
                                         ? g_inventory_slot_count[index / 16][index % 16]
                                         : g_original_count);
    return qol::contracts::ok();
}
QolResult stub_effective_cumulate(qol::contracts::QolItemHandle item, int32_t* out_count) {
    if (out_count == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    }
    if (!g_override_available) {
        const std::size_t index = fake_slot_index(item);
        if (index < 176 && g_inventory_slot_count[index / 16][index % 16] != 0) {
            *out_count = static_cast<int32_t>(g_inventory_slot_count[index / 16][index % 16]);
            return qol::contracts::ok();
        }
        return qol::contracts::error(kQolErrNotReady, "not ready", 9);
    }
    uint32_t effective = 0;
    const QolResult result = override_effective(
        reinterpret_cast<const void*>(static_cast<uintptr_t>(item.slot)), &effective);
    if (!qol::contracts::succeeded(result)) return result;
    *out_count = static_cast<int32_t>(effective);
    return qol::contracts::ok();
}
QolResult stub_item_ref(int32_t bag, int32_t slot, qol::contracts::QolInventoryItemRef* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    if (g_item_ref_result != kQolOk) {
        return qol::contracts::error(g_item_ref_result, "item reference unavailable", 26);
    }
    if (bag < 0 || bag >= 11 || slot < 0 || slot >= 16 ||
        !g_inventory_slot_present[bag][slot]) {
        return qol::contracts::error(kQolErrNotFound, "empty", 5);
    }
    *out = qol::contracts::QolInventoryItemRef{
        static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kOriginal), bag, slot, 10,
        static_cast<int32_t>(g_inventory_slot_count[bag][slot] != 0
                                 ? g_inventory_slot_count[bag][slot]
                                 : g_original_count),
        {static_cast<uint32_t>(bag * 16 + slot + 1), 1}};
    return qol::contracts::ok();
}
QolResult fake_inventory_item_view(qol::contracts::QolItemHandle item,
                                   qol::contracts::QolItemInventoryView* out) {
    if (item.slot == 0 || item.generation == 0 || out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad item view", 13);
    }
    *out = {};
    out->fields = qol::contracts::kQolItemInventoryHasCategory |
                  qol::contracts::kQolItemInventoryHasRarity |
                  qol::contracts::kQolItemInventoryHasDamage |
                  qol::contracts::kQolItemInventoryHasDefense |
                  qol::contracts::kQolItemInventoryHasNeedLevel |
                  qol::contracts::kQolItemInventoryHasJewelValue |
                  qol::contracts::kQolItemInventoryHasJewelAttributeType;
    out->type_flags = 40;
    out->raw_rarity = 0;
    out->category = 10;
    out->count_supported = 1;
    out->effective_count = 4;
    out->equip = 0;
    out->rarity = 0;
    out->damage = 0;
    out->defense = 0;
    out->magic_rate = 0;
    out->need_level = 0;
    out->enchant_remaining = 0;
    out->jewel_value = 0;
    out->jewel_attribute_type = 0;
    out->chaos_rate = 100;
    return qol::contracts::ok();
}
QolResult stub_physical(int32_t, int32_t, qol::contracts::QolItemHandle*) { return unsupported(); }
QolResult stub_detail_item_context(qol::contracts::QolItemDetailContext*) {
    return unsupported();
}

QolResult fake_in_world(int32_t* out) {
    *out = g_in_world ? 1 : 0;
    return qol::contracts::ok();
}

QolResult fake_static_ready(int32_t* out) {
    *out = g_static_ready;
    return qol::contracts::ok();
}

std::unordered_map<int32_t, std::string>& text_table(int32_t kind) {
    static std::unordered_map<int32_t, std::string> tables[4];
    return tables[kind];
}

QolResult fake_static_text(int32_t kind, int32_t id, const char** out) {
    if (kind < 0 || kind >= 4 || out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad kind", 8);
    }
    const auto& table = text_table(kind);
    const auto it = table.find(id);
    if (it == table.end()) return qol::contracts::error(kQolErrNotFound, "missing", 7);
    *out = it->second.c_str();
    return qol::contracts::ok();
}

QolResult fake_inventory(char* out, uint32_t capacity, uint32_t* out_size) {
    const uint32_t size = static_cast<uint32_t>(sizeof(kInventoryJson) - 1);
    if (capacity <= size) return qol::contracts::error(kQolErrBlocked, "small", 5);
    std::memcpy(out, kInventoryJson, size);
    out[size] = '\0';
    *out_size = size;
    return qol::contracts::ok();
}

QolResult fake_shop(char* out, uint32_t capacity, uint32_t* out_size) {
    const uint32_t size = static_cast<uint32_t>(sizeof(kShopJson) - 1);
    if (capacity <= size) return qol::contracts::error(kQolErrBlocked, "small", 5);
    std::memcpy(out, kShopJson, size);
    out[size] = '\0';
    *out_size = size;
    return qol::contracts::ok();
}

QolResult fake_money(int64_t* out) {
    *out = 72503;
    return qol::contracts::ok();
}

QolResult fake_set_money(int64_t money) {
    g_money_write_value = money;
    return g_money_write_result;
}

QolResult fake_reject_dice() {
    ++g_dice_reject_calls;
    return g_dice_reject_result;
}

QolResult fake_accept_dice(qol::contracts::QolItemDiceAcceptResult* out) {
    ++g_dice_accept_calls;
    g_dice_accept_order = ++g_party_sequence;
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    *out = g_dice_accept_data;
    if (!qol::contracts::succeeded(g_dice_accept_result)) return g_dice_accept_result;
    for (std::size_t i = 0; i < 5; ++i) g_party_main_stats[i] = out->applied[i];
    return g_dice_accept_result;
}

QolResult fake_inventory_extension_access(qol::contracts::QolInventoryExtensionAccess* out);

QolResult fake_count_encoding(qol::contracts::QolItemHandle,
                              qol::contracts::QolItemCountEncoding* out_encoding) {
    if (out_encoding == nullptr) {
        constexpr char kMessage[] = "bad encoding output";
        return qol::contracts::error(kQolErrInvalidArgument, kMessage, sizeof(kMessage) - 1);
    }
    *out_encoding = g_count_encoded ? qol::contracts::QolItemCountEncoding::kEncoded
                                    : qol::contracts::QolItemCountEncoding::kNotEncoded;
    return qol::contracts::ok();
}

QolResult fake_item_count_limit(qol::contracts::QolItemHandle, uint32_t* out_limit) {
    if (out_limit == nullptr) {
        constexpr char kMessage[] = "bad limit output";
        return qol::contracts::error(kQolErrInvalidArgument, kMessage, sizeof(kMessage) - 1);
    }
    *out_limit = 99;
    return qol::contracts::ok();
}

const QolItemPortReadApi kFakeRead{
    &stub_category,   &stub_is_real_equip, &stub_is_jewel,   &stub_no_sell,
    &stub_sell_price, &stub_cumulate,      &stub_item_ref,   &stub_physical,
    &stub_detail_item_context, &fake_inventory, &fake_money,
    &fake_shop, &fake_in_world, &fake_static_text, &fake_static_ready,
    &fake_inventory_item_view, &stub_effective_cumulate, &fake_inventory_extension_access,
    &fake_item_count_limit, &fake_count_encoding,
};

const QolItemPort kFakePort{qol::contracts::kQolPortItemVersion, sizeof(QolItemPort), kFakeRead};
const qol::contracts::QolItemMoneyWritePort kFakeMoneyWritePort{
    qol::contracts::kQolPortItemMoneyWriteVersion, &fake_set_money};
const qol::contracts::QolItemDiceWritePort kFakeDiceWritePort{
    qol::contracts::kQolPortItemDiceWriteVersion, &fake_reject_dice, &fake_accept_dice};

QolResult fake_state_write(int32_t kind, int32_t arg0, int32_t arg1,
                           qol::contracts::QolItemInventoryStateWriteResult* out) {
    ++g_state_write_calls;
    g_state_write_kind = kind;
    g_state_write_arg0 = arg0;
    g_state_write_arg1 = arg1;
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    *out = g_state_write_data;
    if (!qol::contracts::succeeded(g_state_write_result)) return g_state_write_result;
    g_original_count = static_cast<uint32_t>(g_state_write_count);
    const auto op_kind = static_cast<qol::contracts::QolItemWriteKind>(kind);
    if (op_kind == qol::contracts::QolItemWriteKind::kAddItem) {
        // The OP add-item result remains the fixed compact success body.
    } else if (op_kind == qol::contracts::QolItemWriteKind::kSellItem ||
        op_kind == qol::contracts::QolItemWriteKind::kDiscardItem) {
        g_inventory_slot_present[arg0][arg1] = false;
    } else if (op_kind == qol::contracts::QolItemWriteKind::kShopBuy) {
        g_inventory_slot_present[0][3] = true;
        g_inventory_slot_count[0][3] = g_state_write_count;
    } else if (op_kind == qol::contracts::QolItemWriteKind::kMoveItem) {
        g_inventory_slot_present[arg0][arg1] = false;
        const int32_t target_slot = g_state_move_to_slot >= 0 ? g_state_move_to_slot : 2;
        if (g_state_move_to_bag >= 0 && g_state_move_to_bag < 11) {
            g_inventory_slot_present[g_state_move_to_bag][target_slot] = true;
            g_inventory_slot_count[g_state_move_to_bag][target_slot] = g_state_write_count;
        }
    } else {
        g_inventory_slot_present[arg0][arg1] = true;
        g_inventory_slot_count[arg0][arg1] = g_state_write_count;
    }
    if (op_kind == qol::contracts::QolItemWriteKind::kUseItem &&
        (g_state_write_data.flags & qol::contracts::kQolItemInventoryStateHasGained) != 0) {
        for (uint32_t i = 0; i < g_state_write_data.gained_count; ++i) {
            const auto& gained = g_state_write_data.gained[i];
            if (gained.bag >= 0 && gained.bag < 11 && gained.slot >= 0 && gained.slot < 16) {
                g_inventory_slot_present[gained.bag][gained.slot] = true;
                g_inventory_slot_count[gained.bag][gained.slot] = gained.count;
            }
        }
    }
    if (op_kind == qol::contracts::QolItemWriteKind::kUseItem && g_use_changes_health) {
        ++g_party_hp;
    }
    return qol::contracts::ok();
}

QolResult fake_state_use(int32_t bag, int32_t slot,
                         qol::contracts::QolItemInventoryStateWriteResult* out) {
    return fake_state_write(static_cast<int32_t>(qol::contracts::QolItemWriteKind::kUseItem),
                            bag, slot, out);
}

QolResult fake_state_sell(int32_t bag, int32_t slot,
                          qol::contracts::QolItemInventoryStateWriteResult* out) {
    return fake_state_write(static_cast<int32_t>(qol::contracts::QolItemWriteKind::kSellItem),
                            bag, slot, out);
}

QolResult fake_state_discard(int32_t bag, int32_t slot,
                             qol::contracts::QolItemInventoryStateWriteResult* out) {
    return fake_state_write(static_cast<int32_t>(qol::contracts::QolItemWriteKind::kDiscardItem),
                            bag, slot, out);
}

QolResult fake_state_shop_buy(int32_t slot,
                              qol::contracts::QolItemInventoryStateWriteResult* out) {
    return fake_state_write(static_cast<int32_t>(qol::contracts::QolItemWriteKind::kShopBuy),
                            slot, -1, out);
}

QolResult fake_state_move(int32_t bag, int32_t slot, int32_t count, int32_t to_bag,
                          int32_t to_slot,
                          qol::contracts::QolItemInventoryStateWriteResult* out) {
    if (bag == 5 || to_bag == 5) {
        return qol::contracts::error(kQolErrBlocked, "task bag excluded", 17);
    }
    g_state_move_count = count;
    g_state_move_to_bag = to_bag;
    g_state_move_to_slot = to_slot;
    return fake_state_write(static_cast<int32_t>(qol::contracts::QolItemWriteKind::kMoveItem),
                            bag, slot, out);
}

QolResult fake_state_add(int32_t category, int32_t count, int32_t socket_total,
                         int32_t enhance_remaining, int32_t rarity_group,
                         const qol::contracts::QolItemCountApi* item_count,
                         qol::contracts::QolItemInventoryStateWriteResult* out) {
    g_state_add_category = category;
    g_state_add_count = count;
    g_state_add_socket = socket_total;
    g_state_add_enhance = enhance_remaining;
    g_state_add_rarity = rarity_group;
    g_state_add_count_api = item_count != nullptr ? item_count : override_item_count_api();
    return fake_state_write(static_cast<int32_t>(qol::contracts::QolItemWriteKind::kAddItem),
                            category, count, out);
}

QolResult fake_party_write(int32_t kind, int32_t role, int32_t arg1, int32_t arg2,
                           int32_t arg3, qol::contracts::QolItemInventoryStateWriteResult* out) {
    ++g_party_write_calls;
    g_party_write_order = ++g_party_sequence;
    g_party_write_kind = kind;
    g_party_write_args[0] = role;
    g_party_write_args[1] = arg1;
    g_party_write_args[2] = arg2;
    g_party_write_args[3] = arg3;
    const auto write_kind = static_cast<qol::contracts::QolItemWriteKind>(kind);
    if (qol::contracts::succeeded(g_party_write_result)) {
        if (write_kind == qol::contracts::QolItemWriteKind::kEquipItem) {
            g_party_equipment_slot = 0;
        } else if (write_kind == qol::contracts::QolItemWriteKind::kUnequipItem) {
            g_party_equipment_slot = -1;
        } else if (write_kind == qol::contracts::QolItemWriteKind::kPutJewel ||
                   write_kind == qol::contracts::QolItemWriteKind::kEnchant) {
            g_party_equipment_slot = arg3;
        }
        if (write_kind == qol::contracts::QolItemWriteKind::kEquipItem && arg1 >= 0 &&
            arg1 < 11 && arg2 >= 0 && arg2 < 16) {
            g_inventory_slot_present[arg1][arg2] = false;
        } else if (write_kind == qol::contracts::QolItemWriteKind::kUnequipItem) {
            g_inventory_slot_present[0][4] = true;
            g_inventory_slot_count[0][4] = 3;
            g_inventory_slot_present[0][5] = true;
            g_inventory_slot_count[0][5] = 1;
        } else if (write_kind == qol::contracts::QolItemWriteKind::kPutJewel && arg1 >= 0 &&
                   arg1 < 11 && arg2 >= 0 && arg2 < 16) {
            g_inventory_slot_present[arg1][arg2] = false;
        }
    }
    if (out != nullptr) *out = {};
    return g_party_write_result;
}

QolResult fake_state_equip(int32_t role, int32_t bag, int32_t slot,
                           const qol::contracts::QolInventoryExtensionProvider*,
                           qol::contracts::QolItemInventoryStateWriteResult* out) {
    return fake_party_write(static_cast<int32_t>(qol::contracts::QolItemWriteKind::kEquipItem),
                            role, bag, slot, -1, out);
}

QolResult fake_state_unequip(int32_t role, int32_t slot,
                             const qol::contracts::QolInventoryExtensionProvider*,
                             qol::contracts::QolItemInventoryStateWriteResult* out) {
    return fake_party_write(
        static_cast<int32_t>(qol::contracts::QolItemWriteKind::kUnequipItem), role, slot, -1, -1,
        out);
}

QolResult fake_state_put_jewel(int32_t role, int32_t bag, int32_t slot, int32_t equip_slot,
                               const qol::contracts::QolInventoryExtensionProvider*,
                               qol::contracts::QolItemInventoryStateWriteResult* out) {
    return fake_party_write(static_cast<int32_t>(qol::contracts::QolItemWriteKind::kPutJewel),
                            role, bag, slot, equip_slot, out);
}

QolResult fake_state_enchant(int32_t role, int32_t bag, int32_t slot, int32_t equip_slot,
                             const qol::contracts::QolInventoryExtensionProvider*,
                             qol::contracts::QolItemInventoryStateWriteResult* out) {
    return fake_party_write(static_cast<int32_t>(qol::contracts::QolItemWriteKind::kEnchant),
                            role, bag, slot, equip_slot, out);
}

const qol::contracts::QolItemInventoryStateWritePort kFakeItemStateWritePort{
    qol::contracts::kQolPortItemInventoryStateWriteVersion,
    &fake_state_use,
    &fake_state_sell,
    &fake_state_discard,
    &fake_state_shop_buy,
    &fake_state_move,
    &fake_state_add,
    &fake_state_equip,
    &fake_state_unequip,
    &fake_state_put_jewel,
    &fake_state_enchant,
};

const void* const kPortArray[] = {
    &kFakePort, &kFakeMoneyWritePort, &kFakeDiceWritePort, &kFakeItemStateWritePort};

struct FakeScheduler {
    qol::contracts::QolFrameHandler handler = nullptr;
    void* ctx = nullptr;
    int32_t interval = 0;
};

FakeScheduler g_scheduler;

bool run_scheduler(uint64_t frame) {
    return g_scheduler.handler != nullptr && g_scheduler.handler(frame, g_scheduler.ctx) != 0;
}

int32_t g_last_log_level = -1;
char g_last_log_message[256] = {};
QolResult fake_log_write(int32_t level, int32_t, const char*, const char* message, uint32_t) {
    g_last_log_level = level;
    std::snprintf(g_last_log_message, sizeof(g_last_log_message), "%s",
                  message != nullptr ? message : "");
    return qol::contracts::ok();
}

// T2h-B0：golden fixture 期间让 EB capability 返回带 inventory 成员的金装 API。
bool g_golden_require_active = false;
const qol::contracts::ExtensionBagApi* golden_extension_bag_api();

QolResult override_canonical(const void*, uint32_t* out_count) {
    *out_count = g_canonical_count;
    return qol::contracts::ok();
}

QolResult override_effective(const void*, uint32_t* out_count) {
    if (g_effective_fail) {
        return qol::contracts::error(kQolErrNotReady, "not ready", 9);
    }
    if (out_count == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    }
    if (!g_count_encoded) {
        *out_count = 1;
    } else if (g_override_enabled) {
        *out_count = std::min(g_canonical_count, 999u);
    } else {
        *out_count = g_canonical_count % 128u;
    }
    return qol::contracts::ok();
}

int32_t override_enabled() { return g_override_enabled ? 1 : 0; }

const qol::contracts::QolItemCountApi kOverrideItemCountApi{
    &override_canonical, &override_effective, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr,
};
const qol::contracts::QolItemCountApi* override_item_count_api() {
    return g_override_available ? &kOverrideItemCountApi : nullptr;
}

bool g_extension_bag_enabled = true;
int g_extension_move_calls = 0;
std::string g_extension_move_body = "{\"ok\":true,\"state\":{}}";

bool fake_extension_enabled() { return g_extension_bag_enabled; }
bool fake_is_logical_bag(int32_t bag) { return bag >= 6 && bag <= 10; }
int32_t fake_extension_move(int32_t, int32_t, int32_t, int32_t, char* out, uint32_t capacity,
                            uint32_t* out_len) {
    ++g_extension_move_calls;
    if (out == nullptr || out_len == nullptr) return 0;
    const uint32_t copied = static_cast<uint32_t>(
        std::min<std::size_t>(g_extension_move_body.size(), capacity));
    if (copied != 0) std::memcpy(out, g_extension_move_body.data(), copied);
    *out_len = static_cast<uint32_t>(g_extension_move_body.size());
    return 1;
}

qol::contracts::ExtensionBagApi make_fake_extension_bag_api() {
    qol::contracts::ExtensionBagApi api{};
    api.enabled = &fake_extension_enabled;
    api.is_logical_bag = &fake_is_logical_bag;
    api.move_item = &fake_extension_move;
    return api;
}

const qol::contracts::ExtensionBagApi kFakeExtensionBagApi = make_fake_extension_bag_api();

// ---------------------------------------------------------------------------
// ITM-13：中立物品域 provider 测试替身。
// core.item 只经 Item Port 的 inventory_extension_access 读取 provider；本替身把既有 EB
// fake（owns_bag/move）与 legacy EB 段 JSON 桥接为中立语义，供三态 golden 复用。
// ---------------------------------------------------------------------------
bool g_provider_resident = false;
bool g_provider_available = false;
std::string g_provider_fragment;

int32_t fake_provider_owns_bag(int32_t bag) { return fake_is_logical_bag(bag) ? 1 : 0; }
QolResult fake_provider_item_ref_at(int32_t bag, int32_t slot,
                                    qol::contracts::QolInventoryExtensionItemRef* out) {
    if (out != nullptr) *out = {};
    if (out != nullptr && bag >= 6 && bag < 11 && slot >= 0 && slot < 16 &&
        g_inventory_slot_present[bag][slot]) {
        *out = qol::contracts::QolInventoryExtensionItemRef{
            static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider), bag, slot, 10,
            static_cast<int32_t>(g_inventory_slot_count[bag][slot] != 0
                                    ? g_inventory_slot_count[bag][slot]
                                    : g_original_count),
            {static_cast<uint32_t>(bag * 16 + slot + 1), 1}};
        return qol::contracts::ok();
    }
    return qol::contracts::error(kQolErrNotFound, "not found", 9);
}
QolResult fake_provider_for_each(qol::contracts::QolInventoryExtensionItemFn, void*) {
    return qol::contracts::ok();
}
QolResult fake_provider_bag_capacity(int32_t, int32_t* out_capacity) {
    if (out_capacity != nullptr) *out_capacity = 0;
    return qol::contracts::error(kQolErrNotFound, "not found", 9);
}
QolResult fake_provider_fragment(char* out, uint32_t capacity, uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    }
    *out_size = 0;
    if (g_provider_fragment.empty()) return qol::contracts::ok();
    if (g_provider_fragment.size() >= capacity) {
        return qol::contracts::error(kQolErrBlocked, "small", 5);
    }
    std::memcpy(out, g_provider_fragment.data(), g_provider_fragment.size());
    out[g_provider_fragment.size()] = '\0';
    *out_size = static_cast<uint32_t>(g_provider_fragment.size());
    return qol::contracts::ok();
}
int32_t fake_provider_move(int32_t from_bag, int32_t from_slot, int32_t to_bag, int32_t to_slot,
                           char* out, uint32_t capacity, uint32_t* out_size) {
    const int32_t result = fake_extension_move(from_bag, from_slot, to_bag, to_slot, out,
                                                capacity, out_size);
    if (result == 0 || g_extension_move_body.compare(0, 11, "{\"ok\":false") == 0) return 0;
    g_inventory_slot_present[from_bag][from_slot] = false;
    const int32_t target_slot = to_slot >= 0 ? to_slot : 0;
    g_inventory_slot_present[to_bag][target_slot] = true;
    g_inventory_slot_count[to_bag][target_slot] = 3;
    return 1;
}
QolResult fake_provider_sell_price(qol::contracts::QolItemHandle, int32_t, int32_t,
                                   int64_t* out_price) {
    if (out_price != nullptr) *out_price = 0;
    return qol::contracts::error(kQolErrNotReady, "not ready", 9);
}
QolResult fake_provider_sync_slot(int32_t, int32_t) { return qol::contracts::ok(); }
QolResult fake_provider_sync_bag() { return qol::contracts::ok(); }
int32_t fake_provider_module_view() { return 0; }
int32_t fake_provider_identify(qol::contracts::QolItemHandle, int32_t*, int32_t*) { return 0; }
int32_t fake_provider_remove(qol::contracts::QolItemHandle) { return 0; }
QolResult fake_provider_put_jewel(int32_t, int32_t, int32_t, int32_t) {
    return qol::contracts::error(kQolErrNotReady, "not ready", 9);
}

const qol::contracts::QolInventoryExtensionProvider kFakeProvider{
    &fake_provider_owns_bag,   &fake_provider_item_ref_at, &fake_provider_for_each,
    &fake_provider_bag_capacity, &fake_provider_fragment,  &fake_provider_move,
    &fake_provider_sell_price, &fake_provider_sync_slot,   &fake_provider_sync_bag,
    &fake_provider_module_view, &fake_provider_identify,   &fake_provider_remove,
    &fake_provider_put_jewel,  nullptr,
};

bool fake_provider_available_probe() { return g_provider_available; }

QolResult fake_inventory_extension_access(qol::contracts::QolInventoryExtensionAccess* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    *out = {};
    if (!g_provider_resident) return qol::contracts::error(kQolErrNotReady, "not ready", 9);
    out->provider = &kFakeProvider;
    out->resident = 1;
    out->available = g_provider_available ? 1 : 0;
    return qol::contracts::ok();
}

std::string fake_party_state_json() {
    std::string json = "[";
    for (int32_t role = 0; role < 3; ++role) {
        if (role != 0) json.push_back(',');
        json += "{\"hp\":" + std::to_string(g_party_hp) + ",\"mp\":" +
                std::to_string(g_party_mp) + ",\"main_stats\":[";
        for (std::size_t stat = 0; stat < 5; ++stat) {
            if (stat != 0) json.push_back(',');
            json += std::to_string(g_party_main_stats[stat]);
        }
        json += "],\"status_point\":" + std::to_string(g_party_status_point) +
                ",\"equipment\":[";
        for (int32_t slot = 0; slot < 10; ++slot) {
            if (slot != 0) json.push_back(',');
            if (role != 1 || slot != g_party_equipment_slot) {
                json += "null";
            } else {
                json += "{\"slot\":" + std::to_string(slot) +
                        ",\"type_flags\":40,\"raw_rarity\":0,\"category\":10,"
                        "\"count\":3,\"equip\":true,\"rarity\":0,\"damage\":0,"
                        "\"defense\":0,\"magic_rate\":0,\"need_level\":0,"
                        "\"socket\":0,\"socket_filled\":0,\"socket_total\":0,"
                        "\"enchant\":0,\"chaos\":false,\"enchant_id\":0,"
                        "\"enchant_level\":0,\"chaos_level\":0,\"chaos_rate\":100,"
                        "\"options\":[],\"option_ids\":[],\"option_types\":[]}";
            }
        }
        json += "]}";
    }
    json.push_back(']');
    return json;
}
constexpr char kPlayerStateJson[] = "{\"money\":72503,\"leader_slot\":0}";

QolResult fake_character_state_json(qol::contracts::QolCharacterStateKind kind, char* out,
                                    uint32_t capacity, uint32_t* out_size) {
    ++g_character_state_write_calls;
    g_character_state_write_order = ++g_party_sequence;
    if (!qol::contracts::succeeded(g_character_state_result)) return g_character_state_result;
    std::string dynamic_body;
    const char* body = nullptr;
    uint32_t body_size = 0;
    if (kind == qol::contracts::QolCharacterStateKind::kParty) {
        dynamic_body = fake_party_state_json();
        body = dynamic_body.data();
        body_size = static_cast<uint32_t>(dynamic_body.size());
    } else if (kind == qol::contracts::QolCharacterStateKind::kPlayer) {
        body = kPlayerStateJson;
        body_size = sizeof(kPlayerStateJson) - 1;
    } else {
        return qol::contracts::error(kQolErrInvalidArgument, "bad state request", 18);
    }
    if (out == nullptr || out_size == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad state request", 18);
    }
    if (body_size > capacity) {
        return qol::contracts::error(kQolErrBlocked, "state buffer too small", 22);
    }
    std::memcpy(out, body, body_size);
    *out_size = body_size;
    return qol::contracts::ok();
}

const qol::contracts::CharacterStateApi kFakeCharacterStateApi{&fake_character_state_json};

QolResult fake_capability_require(const char* id, QolVersionRange required,
                                  const void** out_iface) {
    if (id == nullptr || out_iface == nullptr) {
        return qol::contracts::error(kQolErrNotFound, "missing", 7);
    }
    if (std::strcmp(id, qol::contracts::kCharacterStateCapabilityId) == 0) {
        ++g_character_state_require_calls;
        g_character_state_require_order = ++g_party_sequence;
        if (!g_character_state_available || required.major != 1 || required.min_minor != 0) {
            return qol::contracts::error(kQolErrNotFound, "missing", 7);
        }
        *out_iface = &kFakeCharacterStateApi;
        return qol::contracts::ok();
    }
    if (std::strcmp(id, qol::contracts::kExtensionBagCapabilityId) == 0) {
        ++g_extension_bag_require_calls;
        g_extension_bag_required_minor = required.min_minor;
        if (g_golden_require_active) {
            *out_iface = golden_extension_bag_api();
            return qol::contracts::ok();
        }
        // ITM-13：core.item 不再经 capability 取扩展袋；该分支仅为兼容其它潜在消费者保留。
        *out_iface = &kFakeExtensionBagApi;
        return qol::contracts::ok();
    }
    return qol::contracts::error(kQolErrNotFound, "missing", 7);
}

QolResult fake_register_result(uint32_t, qol::contracts::QolTypedResultCommandHandler handler,
                               void*) {
    g_result_handler = handler;
    return qol::contracts::ok();
}

QolResult fake_unregister_result(uint32_t) {
    g_result_handler = nullptr;
    return qol::contracts::ok();
}

QolResult fake_post_typed(uint32_t handler_id, const void* payload, uint32_t size,
                          qol::contracts::QolCommandTicket* ticket) {
    if (payload == nullptr || size > sizeof(g_post_payload) || ticket == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad command", 11);
    }
    g_post_handler_id = handler_id;
    g_post_payload_size = size;
    std::memcpy(g_post_payload, payload, size);
    *ticket = 72;
    return qol::contracts::ok();
}

QolResult fake_scheduler_add(int32_t, qol::contracts::QolFrameHandler handler, void* ctx,
                             int32_t interval, int32_t, uint64_t* out_id) {
    g_scheduler.handler = handler;
    g_scheduler.ctx = ctx;
    g_scheduler.interval = interval;
    *out_id = 1;
    return qol::contracts::ok();
}

QolResult fake_scheduler_remove(uint64_t) {
    g_scheduler.handler = nullptr;
    g_scheduler.ctx = nullptr;
    return qol::contracts::ok();
}

qol::contracts::QolHostSdk make_sdk() {
    qol::contracts::QolHostSdk sdk{};
    sdk.abi = {sizeof(qol::contracts::QolHostSdk), qol::contracts::kHostSdkMajor,
               qol::contracts::kHostSdkMinor};
    sdk.log = {&fake_log_write, nullptr};
    sdk.capabilities.require = &fake_capability_require;
    sdk.scheduler = {&fake_scheduler_add, &fake_scheduler_remove};
    sdk.commands.post_typed = &fake_post_typed;
    sdk.ticket_result.register_typed_result_handler = &fake_register_result;
    sdk.ticket_result.unregister_typed_result_handler = &fake_unregister_result;
    test_snapshot_sdk::attach(&sdk);
    return sdk;
}

qol::contracts::PluginContext make_context(const qol::contracts::QolHostSdk* sdk) {
    qol::contracts::PluginContext ctx{};
    ctx.struct_size = sizeof(qol::contracts::PluginContext);
    ctx.sdk_major = qol::contracts::kHostSdkMajor;
    ctx.sdk_minor = qol::contracts::kHostSdkMinor;
    ctx.sdk = sdk;
    ctx.game = nullptr;
    ctx.plugin_id = "core.item";
    ctx.ports = kPortArray;
    ctx.port_count = 4;
    return ctx;
}

std::string dispatch_post(const char* path, const char* body,
                          const qol::contracts::HttpPathParam* params, uint32_t param_count,
                          uint16_t* status) {
    const std::string path_value(path);
    const uint32_t body_size = body == nullptr ? 0 : static_cast<uint32_t>(std::strlen(body));
    char storage[8192]{};
    qol::contracts::HttpReq request{
        qol::contracts::HttpMethod::kPost, path_value.c_str(),
        static_cast<uint32_t>(path_value.size()),
        reinterpret_cast<const uint8_t*>(body), body_size,
        nullptr, 0, params, param_count,
    };
    qol::contracts::HttpRes response{reinterpret_cast<uint8_t*>(storage), sizeof(storage), 0, 0};
    (void)qol::host::http_dispatch_api().dispatch(&request, &response);
    if (status != nullptr) *status = response.status;
    return std::string(storage, response.body_size);
}

struct ItemWriteResultContext {
    qol::contracts::QolTypedResultCommandHandler handler = nullptr;
    uint8_t payload[qol::contracts::kTypedCommandPayloadCapacity]{};
    uint32_t payload_size = 0;
};

bool run_item_write_result(void* opaque, char* out, uint32_t capacity, uint32_t* out_size) {
    auto* context = static_cast<ItemWriteResultContext*>(opaque);
    if (context == nullptr || context->handler == nullptr) return false;
    return context->handler(nullptr, context->payload, context->payload_size, out, capacity,
                             out_size);
}

void test_write_routes() {
    std::memset(g_inventory_slot_present, 0, sizeof(g_inventory_slot_present));
    std::memset(g_inventory_slot_count, 0, sizeof(g_inventory_slot_count));
    g_inventory_slot_present[0][0] = true;
    g_original_count = 3;
    g_inventory_slot_count[0][0] = 3;
    g_party_hp = 100;
    g_party_mp = 50;
    g_party_main_stats[0] = 10;
    g_party_main_stats[1] = 11;
    g_party_main_stats[2] = 13;
    g_party_main_stats[3] = 14;
    g_party_main_stats[4] = 15;
    g_party_status_point = 7;
    g_party_equipment_slot = -1;
    g_use_changes_health = false;
    g_override_available = false;
    g_override_enabled = false;
    const auto& descriptor = qol::features::core_item::plugin_descriptor();
    qol::host::http_dispatch_set_op_enabled(true);
    uint16_t status = 0;
    CHECK(dispatch_post("/api/item/inventory/use_item",
                        "{\"bag\":8,\"bag\":\"0\",\"slot\":0,\"slot\":\"1\"}", nullptr,
                        0, &status) == "{\"ok\":true,\"ticket\":72}");
    CHECK(status == 200 && g_post_payload_size > 0 && g_result_handler != nullptr);
    CHECK(descriptor.http_query_count == 0 && descriptor.http_queries == nullptr);
    CHECK(descriptor.http_endpoint_count == 19 && descriptor.http_endpoints != nullptr);
    CHECK(descriptor.http_route_metadata_count == 2 &&
          descriptor.http_route_metadata != nullptr);
    char result[8192]{};
    uint32_t result_size = 0;

    g_state_write_result = qol::contracts::ok();
    g_state_write_count = 2;
    g_state_write_data = {};
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":true,\"slots\":[" +
              expected_slot_record(0, 1, expected_item_at_slot(1, 2)) + "]}");
    CHECK(g_state_write_kind == static_cast<int32_t>(qol::contracts::QolItemWriteKind::kUseItem));
    CHECK(g_state_write_arg0 == 0 && g_state_write_arg1 == 1);
    CHECK(g_write_calls == 0);

    g_state_write_count = 4;
    g_state_write_data.value = 27;
    CHECK(dispatch_post("/api/item/inventory/sell_item", "{\"bag\":0,\"slot\":0}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":true,\"price\":27,\"slots\":[" +
              expected_slot_record(0, 0, "null") + "],\"money\":72503}");
    CHECK(g_state_write_kind == static_cast<int32_t>(qol::contracts::QolItemWriteKind::kSellItem));

    g_state_write_count = 5;
    CHECK(dispatch_post("/api/item/inventory/discard_item", "{\"bag\":0,\"slot\":0}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":true,\"slots\":[" + expected_slot_record(0, 0, "null") + "]}");
    CHECK(g_state_write_kind ==
          static_cast<int32_t>(qol::contracts::QolItemWriteKind::kDiscardItem));

    g_state_write_count = 6;
    g_state_write_data.value = 31;
    CHECK(dispatch_post("/api/item/shop/buy_item", "{\"slot\":2}", nullptr, 0, &status) ==
          "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":true,\"price\":31,\"money\":72503,\"slots\":[" +
              expected_slot_record(0, 3, expected_item_at_slot(3, 6)) + "]}");
    CHECK(g_state_write_kind == static_cast<int32_t>(qol::contracts::QolItemWriteKind::kShopBuy));

    g_state_write_count = 3;
    g_state_write_data = {};
    g_state_write_data.flags = qol::contracts::kQolItemInventoryStateHasGained;
    g_state_write_data.gained_count = 1;
    g_state_write_data.gained[0] = {4, 2, 10, 3};
    CHECK(dispatch_post("/api/item/inventory/use_item", "{\"bag\":0,\"slot\":0}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":true,\"gained\":[{\"bag\":4,\"slot\":2,\"category\":10,\"count\":3}],"
          "\"slots\":[" + expected_slot_record(0, 0, expected_item_at_slot(0, 3)) + "," +
              expected_slot_record(4, 2, expected_item_at_slot(2, 3)) + "]}");

    g_state_write_data = {};
    g_use_changes_health = true;
    CHECK(dispatch_post("/api/item/inventory/use_item", "{\"bag\":0,\"slot\":0}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":true,\"slots\":[" +
              expected_slot_record(0, 0, expected_item_at_slot(0, 3)) +
              "],\"player\":{\"hp\":101}}");

    g_use_changes_health = false;
    CHECK(dispatch_post("/api/item/inventory/use_item", "{\"bag\":0,\"slot\":0}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":true,\"slots\":[" +
              expected_slot_record(0, 0, expected_item_at_slot(0, 3)) + "]}");

    g_state_write_data = {};
    g_state_write_result = qol::contracts::error(kQolErrBlocked, "on cooldown", 11);
    CHECK(dispatch_post("/api/item/inventory/use_item", "{\"bag\":0,\"slot\":0}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    ItemWriteResultContext state_context{};
    state_context.handler = g_result_handler;
    state_context.payload_size = g_post_payload_size;
    std::memcpy(state_context.payload, g_post_payload, g_post_payload_size);
    qol::infra::command_queue_reset_for_test();
    const qol::infra::Command state_command{
        "core.item kUseItem", nullptr, &state_context, false, &run_item_write_result, 0};
    const qol::infra::CommandTicket state_ticket = qol::infra::post(state_command);
    CHECK(state_ticket != 0 && qol::infra::drain() == 1);
    CHECK(qol::infra::ticket_state(state_ticket) == qol::infra::CommandState::kFailed);
    char state_error[128]{};
    uint32_t state_error_size = 0;
    CHECK(qol::infra::read_result(state_ticket, state_error, sizeof(state_error), &state_error_size));
    CHECK(std::string(state_error, state_error_size) ==
          "{\"ok\":false,\"error\":\"on cooldown\"}");
    qol::infra::command_queue_reset_for_test();
    g_state_write_result = qol::contracts::ok();


    CHECK(dispatch_post("/api/item/inventory/reject_dice", nullptr, nullptr, 0, &status) ==
          "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) == "{\"ok\":true}");
    CHECK(g_dice_reject_calls == 1);

    g_dice_reject_result =
        qol::contracts::error(kQolErrNotFound, "no dice result pending", 22);
    CHECK(dispatch_post("/api/item/inventory/reject_dice", nullptr, nullptr, 0, &status) ==
          "{\"ok\":true,\"ticket\":72}");
    ItemWriteResultContext dice_context{};
    dice_context.handler = g_result_handler;
    dice_context.payload_size = g_post_payload_size;
    std::memcpy(dice_context.payload, g_post_payload, g_post_payload_size);
    qol::infra::command_queue_reset_for_test();
    const qol::infra::Command dice_command{
        "core.item kRejectDice", nullptr, &dice_context, false, &run_item_write_result, 0};
    const qol::infra::CommandTicket dice_ticket = qol::infra::post(dice_command);
    CHECK(dice_ticket != 0 && qol::infra::drain() == 1);
    CHECK(qol::infra::ticket_state(dice_ticket) == qol::infra::CommandState::kFailed);
    char dice_error[128]{};
    uint32_t dice_error_size = 0;
    CHECK(qol::infra::read_result(dice_ticket, dice_error, sizeof(dice_error), &dice_error_size));
    CHECK(std::string(dice_error, dice_error_size) ==
          "{\"ok\":false,\"error\":\"no dice result pending\"}");
    qol::infra::command_queue_reset_for_test();
    g_dice_reject_result = qol::contracts::ok();

    // accept_dice：保留业务数组，仅投影写前后变化的主属性与写后剩余点。
    g_character_state_available = true;
    g_character_state_require_calls = 0;
    g_character_state_write_calls = 0;
    g_dice_accept_calls = 0;
    g_party_sequence = 0;
    g_dice_accept_data = {};
    g_dice_accept_data.base[0] = 10;    g_dice_accept_data.applied[0] = 12;
    g_dice_accept_data.delta[0] = 2;
    g_dice_accept_data.base[1] = 11;    g_dice_accept_data.applied[1] = 11;
    g_dice_accept_data.delta[1] = 0;
    g_dice_accept_data.base[2] = 13;    g_dice_accept_data.applied[2] = 15;
    g_dice_accept_data.delta[2] = 2;
    g_dice_accept_data.base[3] = 14;    g_dice_accept_data.applied[3] = 14;
    g_dice_accept_data.delta[3] = 0;
    g_dice_accept_data.base[4] = 15;    g_dice_accept_data.applied[4] = 17;
    g_dice_accept_data.delta[4] = 2;
    CHECK(dispatch_post("/api/item/inventory/accept_dice", nullptr, nullptr, 0, &status) ==
          "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":true,\"base\":[10,11,13,14,15],\"applied\":[12,11,15,14,17],"
          "\"delta\":[2,0,2,0,2],\"player\":{\"main_stats\":["
          "{\"attr\":0,\"value\":12},{\"attr\":2,\"value\":15},"
          "{\"attr\":4,\"value\":17}],\"status_point\":7}}");
    CHECK(g_dice_accept_calls == 1);
    CHECK(g_character_state_require_calls >= 2 && g_character_state_write_calls >= 2);
    // 时序：状态投影读取覆盖 accept 操作前后。
    CHECK(g_dice_accept_order < g_character_state_write_order);

    // 状态能力缺失：accept 不执行（写未发生）。
    g_character_state_available = false;
    const int accept_calls_before_missing = g_dice_accept_calls;
    CHECK(dispatch_post("/api/item/inventory/accept_dice", nullptr, nullptr, 0, &status) ==
          "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(!g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                            &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":false,\"error\":\"character state unavailable\"}");
    CHECK(g_dice_accept_calls == accept_calls_before_missing);
    g_character_state_available = true;

    // accept 业务失败：透旧错误体 + ticket failed 可回取；不追加 state。
    g_dice_accept_result =
        qol::contracts::error(kQolErrNotFound, "no dice result pending", 22);
    const int accept_state_writes_before_failure = g_character_state_write_calls;
    CHECK(dispatch_post("/api/item/inventory/accept_dice", nullptr, nullptr, 0, &status) ==
          "{\"ok\":true,\"ticket\":72}");
    ItemWriteResultContext accept_context{};
    accept_context.handler = g_result_handler;
    accept_context.payload_size = g_post_payload_size;
    std::memcpy(accept_context.payload, g_post_payload, g_post_payload_size);
    qol::infra::command_queue_reset_for_test();
    const qol::infra::Command accept_command{
        "core.item kAcceptDice", nullptr, &accept_context, false, &run_item_write_result, 0};
    const qol::infra::CommandTicket accept_ticket = qol::infra::post(accept_command);
    CHECK(accept_ticket != 0 && qol::infra::drain() == 1);
    CHECK(qol::infra::ticket_state(accept_ticket) == qol::infra::CommandState::kFailed);
    char accept_error[128]{};
    uint32_t accept_error_size = 0;
    CHECK(qol::infra::read_result(accept_ticket, accept_error, sizeof(accept_error),
                                  &accept_error_size));
    CHECK(std::string(accept_error, accept_error_size) ==
          "{\"ok\":false,\"error\":\"no dice result pending\"}");
    CHECK(g_character_state_write_calls == accept_state_writes_before_failure + 2);
    qol::infra::command_queue_reset_for_test();
    g_dice_accept_result = qol::contracts::ok();

    // 普通袋移动：typed 低层写入后只回传源槽与目标槽终态。
    g_state_write_result = qol::contracts::ok();
    g_state_write_count = 7;
    g_state_write_data = {};
    const uint32_t payload_size_before_invalid = g_post_payload_size;
    const int state_calls_before_invalid = g_state_write_calls;
    const auto expect_invalid_move_value = [&](const char* body, const char* field) {
        const std::string error = std::string("{\"ok\":false,\"error\":\"bad value for ") +
                                  field + "\"}";
        CHECK(dispatch_post("/api/item/inventory/move_item", body, nullptr, 0, &status) == error);
        CHECK(status == 400);
        CHECK(g_post_payload_size == payload_size_before_invalid);
        CHECK(g_state_write_calls == state_calls_before_invalid);
    };
    expect_invalid_move_value(
        "{\"bag\":1e2,\"slot\":2,\"count\":4,\"to_bag\":0,\"to_slot\":3}",
        "bag");
    expect_invalid_move_value(
        "{\"bag\":2147483648,\"slot\":2,\"count\":4,\"to_bag\":0,\"to_slot\":3}",
        "bag");
    expect_invalid_move_value(
        "{\"bag\":1.5,\"slot\":2,\"count\":4,\"to_bag\":0,\"to_slot\":3}",
        "bag");
    expect_invalid_move_value(
        "{\"bag\":1234567890123456789012345678901234567890,\"slot\":2,\"count\":4,\"to_bag\":0,\"to_slot\":3}",
        "bag");
    expect_invalid_move_value(
        "{\"bag\":+1,\"slot\":2,\"count\":4,\"to_bag\":0,\"to_slot\":3}",
        "bag");
    CHECK(dispatch_post("/api/item/inventory/move_item",
                        "{\"bag\":0,\"slot\":2,\"count\":4,\"to_bag\":0}",
                        nullptr, 0, &status) ==
          "{\"ok\":false,\"error\":\"bag/slot/count/to_bag/to_slot required (bag 0-10, 5=task bag excluded for extension)\"}");
    CHECK(status == 400);
    CHECK(g_post_payload_size == payload_size_before_invalid);
    CHECK(g_state_write_calls == state_calls_before_invalid);
    CHECK(dispatch_post("/api/item/inventory/move_item",
                        "{\"bag\":0,\"slot\":2,\"count\":4,\"to_bag\":0,\"to_slot\":3}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    CHECK(status == 200 && g_post_payload_size > 0);
    CHECK(dispatch_post("/api/item/inventory/move_item",
                        "{\"bag\":\"1\",\"slot\":\"2\",\"count\":\"4\","
                        "\"to_bag\":\"0\",\"to_slot\":\"3\"}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":true,\"slots\":[" + expected_slot_record(1, 2, "null") + "," +
              expected_slot_record(0, 3, expected_item_at_slot(3, 7)) + "]}");
    CHECK(g_state_write_kind == static_cast<int32_t>(qol::contracts::QolItemWriteKind::kMoveItem));
    CHECK(g_state_write_arg0 == 1 && g_state_write_arg1 == 2);
    CHECK(g_state_move_count == 4 && g_state_move_to_bag == 0 && g_state_move_to_slot == 3);

    // 扩展袋移动：count=1 仍按整堆 provider 契约执行，再用中立库存读面组装槽位。
    g_provider_resident = true;
    g_provider_available = true;
    g_extension_bag_enabled = true;
    g_extension_move_calls = 0;
    g_extension_move_body = "{\"ok\":true,\"state\":{\"extension\":true}}";
    g_state_write_count = 1;
    CHECK(dispatch_post("/api/item/inventory/move_item",
                        "{\"bag\":\"6\",\"slot\":\"2\",\"count\":\"1\","
                        "\"to_bag\":\"7\",\"to_slot\":\"-1\"}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":true,\"slots\":[" + expected_slot_record(6, 2, "null") + "," +
              expected_slot_record(7, 0, expected_item_at_slot(0, 3)) + "]}");
    CHECK(g_extension_move_calls == 1);
    const int state_calls_before_missing = g_state_write_calls;
    g_extension_move_body = "{\"ok\":false,\"error\":\"move failed\"}";
    CHECK(!g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                            &result_size));
    CHECK(std::string(result, result_size) == "{\"ok\":false,\"error\":\"move failed\"}");
    CHECK(g_extension_move_calls == 2);
    CHECK(g_state_write_calls == state_calls_before_missing);
    g_extension_move_body = "{\"ok\":true,\"state\":{}}";

    // 能力缺失回退：扩展请求回落 typed 移动分支，票据仍只保留源/目标槽终态。
    g_provider_resident = false;
    g_provider_available = false;
    g_state_write_result = qol::contracts::ok();
    g_state_write_count = 2;
    g_state_write_data = {};
    CHECK(dispatch_post("/api/item/inventory/move_item",
                        "{\"bag\":\"6\",\"slot\":\"2\",\"count\":\"4\","
                        "\"to_bag\":\"7\",\"to_slot\":\"-1\"}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":true,\"slots\":[" + expected_slot_record(6, 2, "null") + "]}");
    CHECK(g_state_write_calls == state_calls_before_missing + 1);
    CHECK(g_state_move_count == 4 && g_state_move_to_bag == 7 && g_state_move_to_slot == -1);

    // 失败票据：owner 返回 false 并把原错误体写入结果缓冲，可经票据回取。
    g_state_write_result = qol::contracts::error(kQolErrBlocked, "move failed", 11);
    CHECK(dispatch_post("/api/item/inventory/move_item",
                        "{\"bag\":\"0\",\"slot\":\"2\",\"count\":\"4\","
                        "\"to_bag\":\"1\",\"to_slot\":\"3\"}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    ItemWriteResultContext move_context{};
    move_context.handler = g_result_handler;
    move_context.payload_size = g_post_payload_size;
    std::memcpy(move_context.payload, g_post_payload, g_post_payload_size);
    qol::infra::command_queue_reset_for_test();
    const qol::infra::Command move_command{
        "core.item kMoveItem", nullptr, &move_context, false, &run_item_write_result, 0};
    const qol::infra::CommandTicket move_ticket = qol::infra::post(move_command);
    CHECK(move_ticket != 0 && qol::infra::drain() == 1);
    CHECK(qol::infra::ticket_state(move_ticket) == qol::infra::CommandState::kFailed);
    char move_error[128]{};
    uint32_t move_error_size = 0;
    CHECK(qol::infra::read_result(move_ticket, move_error, sizeof(move_error), &move_error_size));
    CHECK(std::string(move_error, move_error_size) == "{\"ok\":false,\"error\":\"move failed\"}");
    qol::infra::command_queue_reset_for_test();
    g_state_write_result = qol::contracts::ok();
    g_provider_resident = false;
    g_provider_available = false;
    g_original_count = 3;

    const qol::contracts::HttpPathParam role{"role", 4, "1", 1};
    g_provider_resident = true;
    g_provider_available = true;
    g_extension_bag_require_calls = 0;
    g_character_state_require_calls = 0;
    g_character_state_write_calls = 0;
    g_party_write_calls = 0;
    g_party_sequence = 0;
    CHECK(dispatch_post("/api/item/inventory/1/equip_item", "{\"category\":10}", &role, 1,
                        &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":true,\"equipment\":{\"role\":1,\"slot\":0,\"item\":" +
              expected_item_at_slot(0, 3, true) + "},\"inventory\":[" +
              expected_slot_record(0, 0, "null") + "]}");
    CHECK(g_party_write_kind == static_cast<int32_t>(qol::contracts::QolItemWriteKind::kEquipItem));
    CHECK(g_party_write_args[0] == 1 && g_party_write_args[1] == 0 &&
          g_party_write_args[2] == 0 && g_party_write_args[3] == -1);
    CHECK(g_party_write_order < g_character_state_write_order);
    CHECK(g_character_state_require_calls >= 3 && g_character_state_write_calls >= 3);
    // ITM-13：core.item 不再 require 扩展袋能力（provider 经 Item Port 中立通道提供）。
    CHECK(g_extension_bag_require_calls == 0);

    const struct {
        const char* path;
        const char* body;
        qol::contracts::QolItemWriteKind kind;
        int32_t arg1;
        int32_t arg2;
        int32_t arg3;
    } party_write_cases[] = {
        {"/api/item/inventory/1/unequip_item", "{\"slot\":2}",
         qol::contracts::QolItemWriteKind::kUnequipItem, 2, -1, -1},
        {"/api/item/inventory/1/put_jewel", "{\"bag\":0,\"slot\":1,\"equip_slot\":2}",
         qol::contracts::QolItemWriteKind::kPutJewel, 0, 1, 2},
        {"/api/item/inventory/1/enchant", "{\"bag\":0,\"slot\":1,\"equip_slot\":2}",
         qol::contracts::QolItemWriteKind::kEnchant, 0, 1, 2},
    };
    g_party_equipment_slot = 2;
    for (const auto& test_case : party_write_cases) {
        const int operation_calls_before = g_party_write_calls;
        const int state_calls_before = g_character_state_write_calls;
        const int state_requires_before = g_character_state_require_calls;
        const int extension_requires_before = g_extension_bag_require_calls;
        (void)extension_requires_before;
        CHECK(dispatch_post(test_case.path, test_case.body, &role, 1, &status) ==
              "{\"ok\":true,\"ticket\":72}");
        result_size = 0;
        CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result,
                               sizeof(result), &result_size));
        if (test_case.kind == qol::contracts::QolItemWriteKind::kUnequipItem) {
            CHECK(std::string(result, result_size) ==
                  "{\"ok\":true,\"equipment\":{\"role\":1,\"slot\":2,\"item\":null},"
                  "\"inventory\":[" +
                      expected_slot_record(0, 4, expected_item_at_slot(4, 3)) + "," +
                      expected_slot_record(0, 5, expected_item_at_slot(5, 1)) + "]}");
        } else if (test_case.kind == qol::contracts::QolItemWriteKind::kPutJewel) {
            CHECK(std::string(result, result_size) ==
                  "{\"ok\":true,\"equipment\":{\"role\":1,\"slot\":2,\"item\":" +
                      expected_item_at_slot(2, 3, true) + "},\"inventory\":[" +
                      expected_slot_record(0, 1, "null") + "]}");
        } else {
            CHECK(std::string(result, result_size) ==
                  "{\"ok\":true,\"equipment\":{\"role\":1,\"slot\":2,\"item\":" +
                      expected_item_at_slot(2, 3, true) + "}}");
        }
        CHECK(g_party_write_calls == operation_calls_before + 1);
        CHECK(g_party_write_kind == static_cast<int32_t>(test_case.kind));
        CHECK(g_party_write_args[0] == 1 && g_party_write_args[1] == test_case.arg1 &&
              g_party_write_args[2] == test_case.arg2 && g_party_write_args[3] == test_case.arg3);
        const bool reads_equipment =
            test_case.kind != qol::contracts::QolItemWriteKind::kUnequipItem;
        CHECK(g_character_state_write_calls == state_calls_before + (reads_equipment ? 1 : 0));
        CHECK(g_character_state_require_calls == state_requires_before + (reads_equipment ? 2 : 1));
        // ITM-13：写路径不再 require 扩展袋能力。
        CHECK(g_extension_bag_require_calls == extension_requires_before);
        if (reads_equipment) CHECK(g_party_write_order < g_character_state_write_order);
    }

    g_character_state_available = false;
    const int party_writes_before_missing_state = g_party_write_calls;
    CHECK(dispatch_post("/api/item/inventory/1/unequip_item", "{\"slot\":2}", &role, 1,
                        &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(!g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                            &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":false,\"error\":\"character state unavailable\"}");
    CHECK(g_party_write_calls == party_writes_before_missing_state);
    g_character_state_available = true;

    g_party_write_result = qol::contracts::error(kQolErrBlocked, "cannot unequip", 14);
    CHECK(dispatch_post("/api/item/inventory/1/unequip_item", "{\"slot\":2}", &role, 1,
                        &status) == "{\"ok\":true,\"ticket\":72}");
    ItemWriteResultContext party_failure_context{};
    party_failure_context.handler = g_result_handler;
    party_failure_context.payload_size = g_post_payload_size;
    std::memcpy(party_failure_context.payload, g_post_payload, g_post_payload_size);
    qol::infra::command_queue_reset_for_test();
    const qol::infra::Command party_failure_command{
        "core.item kUnequipItem", nullptr, &party_failure_context, false, &run_item_write_result, 0};
    const qol::infra::CommandTicket party_failure_ticket = qol::infra::post(party_failure_command);
    CHECK(party_failure_ticket != 0 && qol::infra::drain() == 1);
    CHECK(qol::infra::ticket_state(party_failure_ticket) == qol::infra::CommandState::kFailed);
    char party_error[128]{};
    uint32_t party_error_size = 0;
    CHECK(qol::infra::read_result(party_failure_ticket, party_error, sizeof(party_error),
                                  &party_error_size));
    CHECK(std::string(party_error, party_error_size) ==
          "{\"ok\":false,\"error\":\"cannot unequip\"}");
    qol::infra::command_queue_reset_for_test();
    g_party_write_result = qol::contracts::ok();
    g_provider_resident = false;
    g_provider_available = false;

    CHECK(dispatch_post("/api/op/inventory/add",
                        "{\"category\":\" 12 \",\"count\":\"1e2\",\"socket\":\"3.7\","
                        "\"enhanceRemaining\":\"+3\",\"rarity\":\"NaN\"}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    g_override_available = true;
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) == "{\"ok\":true}");
    CHECK(g_state_add_category == 12 && g_state_add_count == 100 && g_state_add_socket == 3 &&
          g_state_add_enhance == 3 && g_state_add_rarity == 0);
    CHECK(g_state_add_count_api == &kOverrideItemCountApi);

    CHECK(dispatch_post("/api/op/inventory/add",
                        "{\"category\":\"Infinity\",\"count\":\"-Infinity\","
                        "\"socket\":\"NaN\",\"enhanceRemaining\":\"-3.7\","
                        "\"rarity\":\"1e2\"}", nullptr, 0, &status) ==
          "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(g_state_add_category == std::numeric_limits<int32_t>::max() &&
          g_state_add_count == std::numeric_limits<int32_t>::min() && g_state_add_socket == 0 &&
          g_state_add_enhance == -3 && g_state_add_rarity == 100);

    CHECK(dispatch_post("/api/op/inventory/add",
                        "{\"category\":\"1e100\",\"count\":\"-1e100\"}", nullptr,
                        0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(g_state_add_category == std::numeric_limits<int32_t>::max() &&
          g_state_add_count == std::numeric_limits<int32_t>::min());

    const char* invalid_integer_tokens[] = {
        "{\"category\":1e2}", "{\"category\":1.5}", "{\"category\":+1}",
        "{\"category\":1.0}", "{\"category\":2147483648}",
        "{\"category\":-2147483649}",
    };
    for (const char* invalid_body : invalid_integer_tokens) {
        CHECK(dispatch_post("/api/op/inventory/add", invalid_body, nullptr, 0, &status) ==
              "{\"ok\":false,\"error\":\"bad value for category\"}");
        CHECK(status == 400);
    }
    CHECK(dispatch_post("/api/op/inventory/add", "{\"category\":1,}", nullptr, 0,
                        &status) == "{\"ok\":false,\"error\":\"bad request\"}");
    CHECK(status == 400);
    CHECK(dispatch_post("/api/op/inventory/add", "{\"category\": 1}", nullptr, 0,
                        &status) == "{\"ok\":true,\"ticket\":72}");
    CHECK(status == 200);

    CHECK(dispatch_post("/api/op/inventory/add", "{\"category\":5}", nullptr, 0, &status) ==
          "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) == "{\"ok\":true}");
    CHECK(g_state_add_category == 5 && g_state_add_count == 1 && g_state_add_socket == 0 &&
          g_state_add_enhance == 0 && g_state_add_rarity == -1);

    g_state_write_result = qol::contracts::error(kQolErrBlocked, "inventory full", 14);
    CHECK(dispatch_post("/api/op/inventory/add", "{\"category\":5}", nullptr, 0, &status) ==
          "{\"ok\":true,\"ticket\":72}");
    ItemWriteResultContext add_context{};
    add_context.handler = g_result_handler;
    add_context.payload_size = g_post_payload_size;
    std::memcpy(add_context.payload, g_post_payload, sizeof(QolItemWriteArgs));
    qol::infra::command_queue_reset_for_test();
    const qol::infra::Command add_command{
        "core.item kAddItem", nullptr, &add_context, false, &run_item_write_result, 0};
    const qol::infra::CommandTicket add_ticket = qol::infra::post(add_command);
    CHECK(add_ticket != 0 && qol::infra::drain() == 1);
    CHECK(qol::infra::ticket_state(add_ticket) == qol::infra::CommandState::kFailed);
    char add_error[128]{};
    uint32_t add_error_size = 0;
    CHECK(qol::infra::read_result(add_ticket, add_error, sizeof(add_error), &add_error_size));
    CHECK(std::string(add_error, add_error_size) ==
          "{\"ok\":false,\"error\":\"inventory full\"}");
    qol::infra::command_queue_reset_for_test();
    g_state_write_result = qol::contracts::ok();

    const char* invalid_numbers[] = {"abc", "3.7.9", "1e", ""};
    for (const char* invalid : invalid_numbers) {
        const std::string invalid_body = std::string("{\"category\":\"") + invalid + "\"}";
        CHECK(dispatch_post("/api/op/inventory/add", invalid_body.c_str(), nullptr, 0, &status) ==
              "{\"ok\":false,\"error\":\"category required\"}");
        CHECK(status == 400);
    }

    CHECK(dispatch_post("/api/op/inventory/money", "{\"money\":\"4294967296\"}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    g_money_write_result = qol::contracts::ok();
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) == "{\"ok\":true}");
    CHECK(status == 200 && g_money_write_value == 4294967296LL && g_write_calls == 0);

    g_money_write_result = qol::contracts::error(kQolErrUnsupported, "symbol not resolved", 19);
    result_size = 0;
    CHECK(!g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                            &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":false,\"error\":\"symbol not resolved\"}");

    // The production command queue retains a failed owner's exact result for ticket readback.
    g_money_write_result = qol::contracts::error(kQolErrNotReady, "not in game", 11);
    ItemWriteResultContext command_context{};
    command_context.handler = g_result_handler;
    command_context.payload_size = g_post_payload_size;
    std::memcpy(command_context.payload, g_post_payload, g_post_payload_size);
    qol::infra::command_queue_reset_for_test();
    const qol::infra::Command command{
        "core.item kSetMoney", nullptr, &command_context, false, &run_item_write_result, 0};
    const qol::infra::CommandTicket failed_ticket = qol::infra::post(command);
    CHECK(failed_ticket != 0);
    CHECK(qol::infra::drain() == 1);
    CHECK(qol::infra::ticket_state(failed_ticket) == qol::infra::CommandState::kFailed);
    char failed_result[128]{};
    uint32_t failed_result_size = 0;
    CHECK(qol::infra::read_result(failed_ticket, failed_result, sizeof(failed_result),
                                  &failed_result_size));
    CHECK(std::string(failed_result, failed_result_size) ==
          "{\"ok\":false,\"error\":\"not in game\"}");
    qol::infra::command_queue_reset_for_test();
    g_money_write_result = qol::contracts::ok();

    CHECK(dispatch_post("/api/op/inventory/money", "{\"money\":\"Infinity\"}", nullptr, 0,
                        &status) == "{\"ok\":true,\"ticket\":72}");
    CHECK(g_post_payload_size == sizeof(QolItemWriteArgs));
    QolItemWriteArgs money_args{};
    std::memcpy(&money_args, g_post_payload, sizeof(money_args));
    CHECK(money_args.i64 == std::numeric_limits<int64_t>::max());
    CHECK(dispatch_post("/api/op/inventory/money", "{\"money\":\"1e100\"}", nullptr, 0,
                        &status) == "{\"ok\":true,\"ticket\":72}");
    std::memcpy(&money_args, g_post_payload, sizeof(money_args));
    CHECK(money_args.i64 == std::numeric_limits<int64_t>::max());
    CHECK(dispatch_post("/api/op/inventory/money", "{\"money\":\"NaN\"}", nullptr, 0,
                        &status) == "{\"ok\":true,\"ticket\":72}");
    std::memcpy(&money_args, g_post_payload, sizeof(money_args));
    CHECK(money_args.i64 == 0);
    CHECK(dispatch_post("/api/op/inventory/money", "{\"money\":\"-Infinity\"}", nullptr,
                        0, &status) == "{\"ok\":false,\"error\":\"money required\"}");
    CHECK(status == 400);

    CHECK(dispatch_post("/api/op/inventory/money", "{\"money\":-1}", nullptr, 0, &status) ==
          "{\"ok\":false,\"error\":\"money required\"}");
    CHECK(status == 400);

    const qol::contracts::HttpPathParam invalid_role{"role", 4, "99999999999999999999", 20};
    CHECK(dispatch_post("/api/item/inventory/{role}/equip_item", "{\"category\":10}",
                        &invalid_role, 1, &status) ==
          "{\"ok\":false,\"error\":\"bad request\"}");
    CHECK(status == 400);

    const int write_calls_before_invalid = g_write_calls;
    CHECK(dispatch_post("/api/op/inventory/add", "{\"category\":\"not-a-number\"}",
                        nullptr, 0, &status) ==
          "{\"ok\":false,\"error\":\"category required\"}");
    CHECK(status == 400 && g_write_calls == write_calls_before_invalid);
    std::memset(g_inventory_slot_present, 0, sizeof(g_inventory_slot_present));
    std::memset(g_inventory_slot_count, 0, sizeof(g_inventory_slot_count));
    g_inventory_slot_present[0][0] = true;
    g_original_count = 3;
    g_party_equipment_slot = -1;
    qol::host::http_dispatch_set_op_enabled(false);
}

std::string dispatch_get(const char* path, uint16_t* status = nullptr) {
    const std::string path_value(path);
    char storage[8192]{};
    qol::contracts::HttpReq request{
        qol::contracts::HttpMethod::kGet,
        path_value.c_str(),
        static_cast<uint32_t>(path_value.size()),
        nullptr,
        0,
        nullptr,
        0,
        nullptr,
        0,
    };
    qol::contracts::HttpRes response{reinterpret_cast<uint8_t*>(storage), sizeof(storage), 0, 0};
    (void)qol::host::http_dispatch_api().dispatch(&request, &response);
    if (status != nullptr) *status = response.status;
    return std::string(storage, response.body_size);
}

// 直接调用带路径参数的 handler：dispatch 的模板匹配由 test_http_dispatch.cpp 覆盖。
std::string dispatch_template(const char* path, const qol::contracts::HttpPathParam* params,
                              uint32_t param_count, uint16_t* status) {
    const std::string path_value(path);
    char storage[8192]{};
    qol::contracts::HttpReq request{
        qol::contracts::HttpMethod::kGet,
        path_value.c_str(),
        static_cast<uint32_t>(path_value.size()),
        nullptr,
        0,
        nullptr,
        0,
        params,
        param_count,
    };
    qol::contracts::HttpRes response{reinterpret_cast<uint8_t*>(storage), sizeof(storage), 0, 0};
    (void)qol::host::http_dispatch_api().dispatch(&request, &response);
    if (status != nullptr) *status = response.status;
    return std::string(storage, response.body_size);
}

void test_lifecycle_and_responses() {
    qol::host::http_dispatch_reset_for_test();
    qol::host::http_registry_reset_for_test();
    g_scheduler = FakeScheduler{};
    g_in_world = true;
    g_static_ready = 1;
    text_table(static_cast<int32_t>(QolItemStaticTextKind::kItemName))[10] = "再生药水（小）";
    text_table(static_cast<int32_t>(QolItemStaticTextKind::kItemType))[10] = "potion";
    const qol::contracts::QolHostSdk sdk = make_sdk();
    qol::contracts::PluginContext ctx = make_context(&sdk);
    const auto& descriptor = qol::features::core_item::plugin_descriptor();

    CHECK(descriptor.install(ctx) == 0);
    const auto* item_facts = static_cast<const qol::contracts::ItemFactsApi*>(
        descriptor.provides[0].iface);
    int64_t money = 0;
    CHECK(item_facts->money(&money).ok == kQolOk);
    CHECK(money == 72503);
    int32_t count = 0;
    CHECK(item_facts->cumulate_count({1, 1}, &count).ok == kQolOk);
    CHECK(count == 3);
    g_override_available = true;
    g_override_enabled = false;
    CHECK(item_facts->cumulate_count({1, 1}, &count).ok == kQolOk);
    CHECK(count == 3);
    g_canonical_count = 23;
    g_override_enabled = true;
    CHECK(item_facts->cumulate_count({1, 1}, &count).ok == kQolOk);
    CHECK(count == 23);
    g_override_available = false;
    g_override_enabled = false;
    g_canonical_count = 3;
    descriptor.on_enabled(ctx);
    CHECK(qol::contracts::succeeded(qol::tests::register_plugin_http(descriptor)));
    CHECK(g_scheduler.handler != nullptr);
    CHECK(g_scheduler.interval > 1);
    test_write_routes();

    uint16_t status = 0;
    (void)dispatch_get("/api/item/inventory", &status);
    CHECK(status == 503);

    CHECK(run_scheduler(100));

    const std::string expected_inventory = expected_inventory_with_count(3);
    CHECK(dispatch_get("/api/item/inventory", &status) == expected_inventory);
    CHECK(status == 200);

    // A physical ref/handle lookup failure must not publish a partial inventory with the slot
    // silently omitted. Retain the last complete snapshot until a full serialization succeeds.
    g_item_ref_result = kQolErrNotReady;
    CHECK(run_scheduler(101));
    CHECK(dispatch_get("/api/item/inventory", &status) == expected_inventory);
    CHECK(status == 200);
    g_item_ref_result = kQolOk;

    const struct CountViewCase {
        uint32_t canonical;
        uint32_t disabled_view;
        uint32_t enabled_view;
    } count_cases[] = {{99, 99, 99}, {100, 100, 100}, {999, 103, 999}, {1023, 127, 999}};
    g_override_available = true;
    g_count_encoded = true;
    for (const CountViewCase& test_case : count_cases) {
        g_canonical_count = test_case.canonical;
        g_override_enabled = false;
        CHECK(run_scheduler(200 + test_case.canonical));
        CHECK(dispatch_get("/api/item/inventory", &status) ==
              expected_inventory_with_count(test_case.disabled_view));
        CHECK(status == 200);
        g_override_enabled = true;
        CHECK(run_scheduler(1200 + test_case.canonical));
        CHECK(dispatch_get("/api/item/inventory", &status) ==
              expected_inventory_with_count(test_case.enabled_view));
        CHECK(status == 200);
        if (test_case.canonical == 1023) {
            int32_t canonical = 0;
            CHECK(item_facts->cumulate_count({1, 1}, &canonical).ok == kQolOk);
            CHECK(canonical == 1023);
        }
    }
    g_count_encoded = false;
    g_override_enabled = true;
    CHECK(run_scheduler(2300));
    CHECK(dispatch_get("/api/item/inventory", &status) == expected_inventory_with_count(1));
    CHECK(status == 200);

    // Capability loss and effective-count failure both fall back to the original getter.
    g_count_encoded = true;
    g_override_available = false;
    g_canonical_count = 900;
    g_original_count = 71;
    CHECK(run_scheduler(2400));
    CHECK(dispatch_get("/api/item/inventory", &status) == expected_inventory_with_count(71));
    CHECK(status == 200);
    g_override_available = true;
    g_override_enabled = false;
    g_effective_fail = true;
    g_original_count = 100;
    CHECK(run_scheduler(2500));
    CHECK(dispatch_get("/api/item/inventory", &status) == expected_inventory_with_count(100));
    CHECK(status == 200);
    g_effective_fail = false;
    g_override_available = false;
    g_override_enabled = false;
    g_canonical_count = 3;
    g_original_count = 3;
    CHECK(run_scheduler(2600));
    CHECK(dispatch_get("/api/item/inventory/money", &status) == "{\"money\":72503}");
    CHECK(status == 200);
    CHECK(dispatch_get("/api/item/inventory/items", &status) ==
          std::string("{\"items\":[") + kExpectedItem + "]}");
    CHECK(dispatch_get("/api/item/shop/items", &status) ==
          "{\"items\":[{\"slot\":0,\"category\":10,\"count\":5,\"price\":15,"
          "\"name\":\"再生药水（小）\"}]}");
    CHECK(status == 200);

    const qol::contracts::HttpPathParam bag_param{"bag", 3, "0", 1};
    const qol::contracts::HttpPathParam bag_slot_params[] = {{"bag", 3, "0", 1},
                                                             {"slot", 4, "0", 1}};
    CHECK(dispatch_template("/api/item/inventory/bag/0/info", &bag_param, 1, &status) ==
          "{\"bag\":0,\"capacity\":16,\"slot_count\":1}");
    CHECK(status == 200);
    CHECK(dispatch_template("/api/item/inventory/bag/0/0", bag_slot_params, 2, &status) ==
          kExpectedItem);
    CHECK(status == 200);
    const qol::contracts::HttpPathParam bag_slot_empty[] = {{"bag", 3, "0", 1},
                                                             {"slot", 4, "5", 1}};
    CHECK(dispatch_template("/api/item/inventory/bag/0/5", bag_slot_empty, 2, &status) == "null");
    CHECK(status == 200);
    const qol::contracts::HttpPathParam missing_bag{"bag", 3, "5", 1};
    CHECK(dispatch_template("/api/item/inventory/bag/5/info", &missing_bag, 1, &status) ==
          "{\"bag\":5,\"capacity\":16,\"slot_count\":0}");
    CHECK(status == 200);
    const qol::contracts::HttpPathParam oversized_bag{"bag", 3, "99999999999999999999", 20};
    CHECK(dispatch_template("/api/item/inventory/bag/{bag}/info", &oversized_bag, 1,
                            &status) == "{\"ok\":false,\"error\":\"bad request\"}");
    CHECK(status == 400);
    const qol::contracts::HttpPathParam negative_bag{"bag", 3, "-1", 2};
    CHECK(dispatch_template("/api/item/inventory/bag/{bag}/info", &negative_bag, 1,
                            &status) == "{\"ok\":false,\"error\":\"bad request\"}");
    CHECK(status == 400);
    const qol::contracts::HttpPathParam oversized_bag_slot[] = {
        {"bag", 3, "0", 1}, {"slot", 4, "99999999999999999999", 20}};
    CHECK(dispatch_template("/api/item/inventory/bag/{bag}/{slot}", oversized_bag_slot, 2,
                            &status) == "{\"ok\":false,\"error\":\"bad request\"}");
    CHECK(status == 400);

    // 非 world：发布与迁移前一致的 not in game 错误体（HTTP 200）。
    g_in_world = false;
    CHECK(run_scheduler(115));
    CHECK(dispatch_get("/api/item/inventory", &status) == "{\"error\":\"not in game\"}");
    CHECK(status == 200);
    g_in_world = true;
    CHECK(run_scheduler(130));
    CHECK(dispatch_get("/api/item/inventory/money") == "{\"money\":72503}");

    // owner 停用：handler 墓碑，请求返回明确停用错误。
    CHECK(qol::host::http_dispatch_api().unregister_owner("core.item").ok == kQolOk);
    descriptor.on_disabled(ctx);
    CHECK(g_scheduler.handler == nullptr);
    (void)dispatch_get("/api/item/inventory", &status);
    CHECK(status == 503);

    descriptor.deactivate(ctx);
}

void test_static_not_ready_holds_snapshot() {
    qol::host::http_dispatch_reset_for_test();
    qol::host::http_registry_reset_for_test();
    g_scheduler = FakeScheduler{};
    g_in_world = true;
    g_static_ready = 0;
    const qol::contracts::QolHostSdk sdk = make_sdk();
    qol::contracts::PluginContext ctx = make_context(&sdk);
    const auto& descriptor = qol::features::core_item::plugin_descriptor();
    CHECK(descriptor.install(ctx) == 0);
    descriptor.on_enabled(ctx);
    CHECK(qol::contracts::succeeded(qol::tests::register_plugin_http(descriptor)));
    // 静态名称表未下发：快照不发布，HTTP 侧保持 not ready。
    CHECK(run_scheduler(1));
    uint16_t status = 0;
    (void)dispatch_get("/api/item/inventory", &status);
    CHECK(status == 503);
    g_static_ready = 1;
    CHECK(run_scheduler(20));
    CHECK(dispatch_get("/api/item/inventory", &status).find("\"bags\"") != std::string::npos);
    CHECK(status == 200);
    descriptor.deactivate(ctx);
}

// T2e-0：bridge item 写信封 helper 的逐字节断言（对照 api/native 现状组装，冻结组装契约）。
void test_bridge_item_write_result_helper() {
    using qol::contracts::QolItemDiceAcceptResult;
    using qol::contracts::QolItemInventoryStateWriteResult;

    // 纯信封：成功 / 失败 / message 为空回退。
    CHECK(qol::bridge::item_write_result_json(qol::contracts::ok(), "item write failed") ==
          "{\"ok\":true}");
    CHECK(qol::bridge::item_write_result_json(
              qol::contracts::error(kQolErrBlocked, "inventory full", 14),
              "item write failed") == "{\"ok\":false,\"error\":\"inventory full\"}");
    CHECK(qol::bridge::item_write_result_json(
              qol::contracts::error(kQolErrBlocked, nullptr, 0), "item write failed") ==
          "{\"ok\":false,\"error\":\"item write failed\"}");
    CHECK(qol::bridge::item_error_json("not in game") ==
          "{\"ok\":false,\"error\":\"not in game\"}");

    // use_item：成功无 gained / 1 项 / 多项（字段序 bag,slot,category,count）。
    QolItemInventoryStateWriteResult use_out{};
    CHECK(qol::bridge::item_use_result_json(qol::contracts::ok(), use_out) == "{\"ok\":true}");
    use_out.flags = qol::contracts::kQolItemInventoryStateHasGained;
    use_out.gained_count = 1;
    use_out.gained[0] = {0, 3, 10, 2};
    CHECK(qol::bridge::item_use_result_json(qol::contracts::ok(), use_out) ==
          "{\"ok\":true,\"gained\":[{\"bag\":0,\"slot\":3,\"category\":10,\"count\":2}]}");
    use_out.gained_count = 2;
    use_out.gained[1] = {1, 4, 20, 5};
    CHECK(qol::bridge::item_use_result_json(qol::contracts::ok(), use_out) ==
          "{\"ok\":true,\"gained\":[{\"bag\":0,\"slot\":3,\"category\":10,\"count\":2},"
          "{\"bag\":1,\"slot\":4,\"category\":20,\"count\":5}]}");
    CHECK(qol::bridge::item_use_result_json(
              qol::contracts::error(kQolErrBlocked, "bag full", 8), QolItemInventoryStateWriteResult{}) ==
          "{\"ok\":false,\"error\":\"bag full\"}");
    CHECK(qol::bridge::item_use_result_json(
              qol::contracts::error(kQolErrBlocked, nullptr, 0), QolItemInventoryStateWriteResult{}) ==
          "{\"ok\":false,\"error\":\"item use failed\"}");

    // dice_accept：全 5 项 base/applied/delta（含负数 delta）。
    QolItemDiceAcceptResult dice{};
    dice.base[0] = 1; dice.base[1] = 2; dice.base[2] = 3; dice.base[3] = 4; dice.base[4] = 5;
    dice.applied[0] = 6; dice.applied[1] = 7; dice.applied[2] = 8; dice.applied[3] = 9;
    dice.applied[4] = 10;
    dice.delta[0] = 5; dice.delta[1] = 5; dice.delta[2] = 5; dice.delta[3] = 5; dice.delta[4] = -1;
    CHECK(qol::bridge::item_dice_accept_result_json(qol::contracts::ok(), dice) ==
          "{\"ok\":true,\"base\":[1,2,3,4,5],\"applied\":[6,7,8,9,10],\"delta\":[5,5,5,5,-1]}");
    CHECK(qol::bridge::item_dice_accept_result_json(
              qol::contracts::error(kQolErrBlocked, nullptr, 0), QolItemDiceAcceptResult{}) ==
          "{\"ok\":false,\"error\":\"dice accept failed\"}");
    CHECK(qol::bridge::item_dice_accept_result_json(
              qol::contracts::error(kQolErrBlocked, "no dice result pending", 21),
              QolItemDiceAcceptResult{}) ==
          "{\"ok\":false,\"error\":\"no dice result pending\"}");

    // shop_buy：value=12 → price 12；失败/空文案。
    QolItemInventoryStateWriteResult shop{};
    shop.value = 12;
    CHECK(qol::bridge::item_shop_buy_result_json(qol::contracts::ok(), shop) ==
          "{\"ok\":true,\"price\":12}");
    CHECK(qol::bridge::item_shop_buy_result_json(
              qol::contracts::error(kQolErrBlocked, nullptr, 0), shop) ==
          "{\"ok\":false,\"error\":\"shop buy failed\"}");

    // discard：kOk 与每个 Status 分支（含 kStale→slot empty、default→discard failed）。
    using inventory_trade::Status;
    CHECK(qol::bridge::item_discard_result_json(Status::kOk) == "{\"ok\":true}");
    CHECK(qol::bridge::item_discard_result_json(Status::kNotInWorld) ==
          "{\"ok\":false,\"error\":\"not in game\"}");
    CHECK(qol::bridge::item_discard_result_json(Status::kTaskBagExcluded) ==
          "{\"ok\":false,\"error\":\"task bag excluded\"}");
    CHECK(qol::bridge::item_discard_result_json(Status::kEmptySlot) ==
          "{\"ok\":false,\"error\":\"slot empty\"}");
    CHECK(qol::bridge::item_discard_result_json(Status::kStale) ==
          "{\"ok\":false,\"error\":\"slot empty\"}");
    CHECK(qol::bridge::item_discard_result_json(Status::kNoNativeItem) ==
          "{\"ok\":false,\"error\":\"extension item unavailable\"}");
    CHECK(qol::bridge::item_discard_result_json(Status::kSymbolsUnresolved) ==
          "{\"ok\":false,\"error\":\"symbol not resolved\"}");
    CHECK(qol::bridge::item_discard_result_json(Status::kRemoveFailed) ==
          "{\"ok\":false,\"error\":\"discard failed\"}");
    CHECK(qol::bridge::item_discard_result_json(Status::kNoSell) ==
          "{\"ok\":false,\"error\":\"discard failed\"}");
    CHECK(qol::bridge::item_discard_result_json(Status::kPriceInvalid) ==
          "{\"ok\":false,\"error\":\"discard failed\"}");
    CHECK(qol::bridge::item_discard_result_json(Status::kCreditFailed) ==
          "{\"ok\":false,\"error\":\"discard failed\"}");
    CHECK(qol::bridge::item_discard_result_json(static_cast<Status>(999)) ==
          "{\"ok\":false,\"error\":\"discard failed\"}");

    // sell：kOk price 与每个 Status 分支（含 default→sell failed）。
    auto sell = [](Status status, int64_t price) {
        inventory_trade::Result r{};
        r.status = status;
        r.price = price;
        return qol::bridge::item_sell_result_json(r);
    };
    CHECK(sell(Status::kOk, 7) == "{\"ok\":true,\"price\":7}");
    CHECK(sell(Status::kNotInWorld, 0) == "{\"ok\":false,\"error\":\"not in game\"}");
    CHECK(sell(Status::kTaskBagExcluded, 0) == "{\"ok\":false,\"error\":\"task bag excluded\"}");
    CHECK(sell(Status::kEmptySlot, 0) == "{\"ok\":false,\"error\":\"slot empty\"}");
    CHECK(sell(Status::kNoNativeItem, 0) ==
          "{\"ok\":false,\"error\":\"extension item unavailable\"}");
    CHECK(sell(Status::kStale, 0) == "{\"ok\":false,\"error\":\"slot empty\"}");
    CHECK(sell(Status::kSymbolsUnresolved, 0) ==
          "{\"ok\":false,\"error\":\"symbol not resolved\"}");
    CHECK(sell(Status::kNoSell, 0) == "{\"ok\":false,\"error\":\"item cannot be sold\"}");
    CHECK(sell(Status::kPriceInvalid, 0) == "{\"ok\":false,\"error\":\"invalid sell price\"}");
    CHECK(sell(Status::kCreditFailed, 0) ==
          "{\"ok\":false,\"error\":\"sell money update failed\"}");
    CHECK(sell(Status::kRemoveFailed, 0) == "{\"ok\":false,\"error\":\"sell failed\"}");
    CHECK(sell(static_cast<Status>(999), 0) == "{\"ok\":false,\"error\":\"sell failed\"}");
}

}  // namespace

// ============================================================================
// T2h-B0：owner `serialize_inventory_json` vs legacy `build_inventory_json`
// 逐字节 golden（fake 环境；纯 tests）。两侧读同一份 fake 袋槽/物品字节。
// 见 .tmp/item-char-writes/plans/T2h-read-side-plan.md §4.1。
// ============================================================================
uint8_t g_golden_bag_storage[6 * 0x80]{};
uint8_t g_golden_item_a[0x40]{};
uint8_t g_golden_item_b[0x40]{};
uint8_t g_golden_item_projected[0x40]{};
uint8_t g_golden_item_task[0x40]{};
int32_t g_golden_world = 1;
int32_t g_golden_raw = 71;
int32_t g_golden_eb_enabled = 0;
int32_t g_golden_eb_caps[5] = {16, 16, 12, 12, 12};

static int golden_bit(int v, int hi, int lo) { return (v >> lo) & ((1 << (hi - lo + 1)) - 1); }
static int golden_cumulate(void*) { return g_golden_raw; }
static int golden_rarity(void*) { return 3; }
static int golden_damage(void*) { return 4; }
static int golden_defense(void*) { return 5; }
static int golden_need_level(void*) { return 7; }

bool game_in_world() { return g_golden_world != 0; }
void* g_inven = nullptr;
GetBitFn fn_get_bit = &golden_bit;
GetCumulateCountFn fn_get_cumulate_count = &golden_cumulate;
GetRarityFn fn_get_rarity = &golden_rarity;
GetItemStatFn fn_get_damage = &golden_damage;
GetItemStatFn fn_get_defense = &golden_defense;
ItemGetAbilityLevelFn fn_item_get_ability_level = &golden_need_level;
// T2h-B3#2：item_class.cpp 参与链接后由它提供 item_is_equip/item_count_encoding/
// item_is_backpack/category_is_no_equip/debug_item_raw_json，本测试不再各自 fake。
uintptr_t g_base = 0;
void frame_cache_force_refresh() {}
// T2h-B3#2 golden fixture：默认 not-found（与既有断言一致）；置 mode 后返回合成 ref
//（default 槽 / 扩展袋槽），供 debug raw 逐字节对照。native_item 指向同一字节缓冲。
static uint8_t g_debug_raw_bytes[0x40]{};
static int g_debug_raw_mode = 0;  // 0=not found, 1=default, 2=extension
bool inventory_item_ref_at(int bag, int slot, InventoryItemRef* out) {
    if (out == nullptr || g_debug_raw_mode == 0) return false;
    if (g_debug_raw_mode == 1) {
        if (bag != 2 || slot != 5) return false;
        out->kind = InventoryItemKind::kOriginal;
        out->bag = 2;
        out->slot = 5;
        out->category = 12;
        out->count = 3;
    } else {
        if (bag != 6 || slot != 0) return false;
        out->kind = InventoryItemKind::kProvider;
        out->bag = 6;
        out->slot = 0;
        out->category = 10;
        out->count = 199;
    }
    out->native_item = g_debug_raw_bytes;
    return true;
}

namespace qol::game {
const qol::contracts::QolItemCountApi* item_count_api_if_resident() {
    return override_item_count_api();
}
}  // namespace qol::game

static void* golden_bag_item(int bag, int slot) {
    if (bag < 0 || bag >= 6 || slot < 0 || slot >= 16) return nullptr;
    return *reinterpret_cast<void**>(g_golden_bag_storage + bag * 0x80 + slot * 8);
}

static std::string golden_legacy_eb_json() {
    if (g_golden_eb_enabled == 0) return "";
    std::string out =
        ",{\"bag\":6,\"items\":[{\"slot\":0,\"category\":10,\"count\":199}],\"capacity\":16,"
        "\"slot_count\":1}";
    for (int bag = 7; bag <= 10; ++bag) {
        out += ",{\"bag\":" + std::to_string(bag) + ",\"items\":[],\"capacity\":" +
               std::to_string(g_golden_eb_caps[bag - 6]) + ",\"slot_count\":0}";
    }
    return out;
}
std::string extension_bag_inventory_bags_json() { return golden_legacy_eb_json(); }

static bool golden_eb_enabled() { return g_golden_eb_enabled != 0; }
static bool golden_eb_is_logical(int32_t bag) { return bag >= 6 && bag <= 10; }
static int32_t golden_eb_internal(int32_t) { return 6; }
static bool golden_eb_capacity(int32_t bag, int32_t* out) {
    if (bag < 6 || bag > 10 || out == nullptr) return false;
    *out = g_golden_eb_caps[bag - 6];
    return true;
}
static int32_t golden_eb_item_at(int32_t bag, int32_t slot,
                                 qol::contracts::QolItemHandle* out_item) {
    if (out_item == nullptr) return 0;
    *out_item = {};
    if (bag != 6 || (slot != 0 && slot != 4)) return 0;
    *out_item = {static_cast<uint32_t>(slot + 1), 1};
    return 1;
}
int32_t g_golden_bad_slot = 0;
int32_t g_golden_projected_canonical = 0;
static void golden_eb_for_each(qol::contracts::ExtensionBagItemFn fn, void* ctx) {
    if (fn == nullptr) return;
    const auto invoke = [fn, ctx](int32_t bag, int32_t slot, int32_t category, int32_t count) {
        const qol::contracts::QolInventoryExtensionItemRef ref{
            static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider), bag, slot,
            category, count, {static_cast<uint32_t>(slot + 1), 1}};
        return fn(&ref, ctx);
    };
    // R2：可选注入一个 count=0 的非法槽，验证 L1 单槽跳过而非整快照失败。
    if (g_golden_bad_slot != 0) (void)invoke(6, 1, 10, 0);
    // R3-①：可选注入 canonical>0 但 b 段为 0 的投影槽（R2-1 修复后 EB 应输出 canonical 视图）。
    if (g_golden_projected_canonical != 0) {
        (void)invoke(9, 4, 35, g_golden_projected_canonical);
    }
    (void)invoke(6, 0, 10, 199);
}
static int32_t golden_eb_move(int32_t, int32_t, int32_t, int32_t, char* out,
                              uint32_t capacity, uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr) return 0;
    const uint32_t copied = static_cast<uint32_t>(
        std::min<std::size_t>(g_extension_move_body.size(), capacity));
    if (copied != 0) std::memcpy(out, g_extension_move_body.data(), copied);
    *out_size = static_cast<uint32_t>(g_extension_move_body.size());
    return 1;
}
static qol::contracts::ExtensionBagApi make_golden_eb_api() {
    qol::contracts::ExtensionBagApi api{};
    api.enabled = &golden_eb_enabled;
    api.is_logical_bag = &golden_eb_is_logical;
    api.internal_index = &golden_eb_internal;
    api.for_each_logical_item = &golden_eb_for_each;
    api.item_at = &golden_eb_item_at;
    api.logical_bag_capacity = &golden_eb_capacity;
    api.move_item = &golden_eb_move;
    return api;
}
const qol::contracts::ExtensionBagApi kGoldenExtensionBagApi = make_golden_eb_api();
namespace {
const qol::contracts::ExtensionBagApi* golden_extension_bag_api() {
    return &kGoldenExtensionBagApi;
}
}  // namespace

static QolResult golden_unsupported() {
    return qol::contracts::error(kQolErrUnsupported, "stub", 4);
}
static QolResult golden_unsupported2(qol::contracts::QolItemHandle, int32_t*) {
    return golden_unsupported();
}
static QolResult golden_unsupported1(int32_t, int32_t*) { return golden_unsupported(); }
static QolResult golden_unsupported_price(qol::contracts::QolItemHandle, int64_t*) {
    return golden_unsupported();
}
static QolResult golden_unsupported_phys(int32_t, int32_t, qol::contracts::QolItemHandle*) {
    return golden_unsupported();
}
static QolResult golden_unsupported_detail_context(qol::contracts::QolItemDetailContext*) {
    return golden_unsupported();
}

struct GoldenHandleEntry {
    uint32_t slot = 0;
    const void* item = nullptr;
};
GoldenHandleEntry g_golden_handle_entries[16]{};
uint32_t g_golden_next_handle_slot = 1000;

static qol::contracts::QolItemHandle golden_handle_for_item(const void* item) {
    for (const GoldenHandleEntry& entry : g_golden_handle_entries) {
        if (entry.slot != 0 && entry.item == item) return {entry.slot, 1};
    }
    for (GoldenHandleEntry& entry : g_golden_handle_entries) {
        if (entry.slot != 0) continue;
        entry = {g_golden_next_handle_slot++, item};
        return {entry.slot, 1};
    }
    return {};
}

static const void* golden_item_for_handle(qol::contracts::QolItemHandle handle) {
    for (const GoldenHandleEntry& entry : g_golden_handle_entries) {
        if (entry.slot == handle.slot && handle.generation == 1) return entry.item;
    }
    return nullptr;
}

static QolResult golden_category(qol::contracts::QolItemHandle item, int32_t* out) {
    if (item.slot == 0 || item.generation == 0 || out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    }
    const void* native_item = golden_item_for_handle(item);
    if (native_item == nullptr) return qol::contracts::error(kQolErrNotFound, "stale", 5);
    const auto* item_bytes = static_cast<const uint8_t*>(native_item);
    *out = golden_bit(
        *reinterpret_cast<const uint16_t*>(item_bytes + I_TYPE), 15, 6);
    return qol::contracts::ok();
}
static QolResult golden_cumulate_count(qol::contracts::QolItemHandle, int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    *out = g_golden_raw;
    return qol::contracts::ok();
}
static QolResult golden_item_ref_at(int32_t bag, int32_t slot,
                                    qol::contracts::QolInventoryItemRef* out) {
    if (out == nullptr || bag < 0 || bag > 5 || slot < 0 || slot >= 16) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    }
    void* item = golden_bag_item(bag, slot);
    if (item == nullptr) return qol::contracts::error(kQolErrNotFound, "empty", 5);
    *out = qol::contracts::QolInventoryItemRef{
        static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kOriginal), bag, slot, 0, 0,
        golden_handle_for_item(item)};
    return qol::contracts::ok();
}
static QolResult golden_inventory_json(char*, uint32_t, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    return qol::contracts::ok();
}
static QolResult golden_money(int64_t* out) {
    if (out != nullptr) *out = 0;
    return qol::contracts::ok();
}
static QolResult golden_shop(char* out, uint32_t capacity, uint32_t* out_size) {
    constexpr char kShop[] = "{\"items\":[]}";
    const uint32_t size = sizeof(kShop) - 1;
    if (out == nullptr || out_size == nullptr || capacity <= size) {
        return qol::contracts::error(kQolErrBlocked, "small", 5);
    }
    std::memcpy(out, kShop, size);
    out[size] = '\0';
    *out_size = size;
    return qol::contracts::ok();
}
static QolResult golden_in_world(int32_t* out) {
    if (out != nullptr) *out = g_golden_world;
    return qol::contracts::ok();
}
static QolResult golden_static_text(int32_t, int32_t, const char**) {
    return qol::contracts::error(kQolErrNotFound, "x", 1);
}
static QolResult golden_static_ready(int32_t* out) {
    if (out != nullptr) *out = 1;
    return qol::contracts::ok();
}
static QolResult golden_view(qol::contracts::QolItemHandle handle,
                             qol::contracts::QolItemInventoryView* out) {
    if (handle.slot == 0 || handle.generation == 0 || out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    }
    if (handle.slot < 177u) {
        return fake_inventory_item_view(handle, out);
    }
    const void* item = golden_item_for_handle(handle);
    if (item == nullptr) return qol::contracts::error(kQolErrNotFound, "stale", 5);
    const auto* bytes = static_cast<const uint8_t*>(item);
    *out = {};
    const uint16_t flags = *reinterpret_cast<const uint16_t*>(bytes + I_TYPE);
    out->fields = qol::contracts::kQolItemInventoryHasCategory |
                  qol::contracts::kQolItemInventoryHasRarity |
                  qol::contracts::kQolItemInventoryHasDamage |
                  qol::contracts::kQolItemInventoryHasDefense |
                  qol::contracts::kQolItemInventoryHasNeedLevel;
    out->type_flags = flags;
    out->raw_rarity = (flags >> 2) & 0x0F;
    out->category = golden_bit(flags, 15, 6);
    out->count_supported = 1;
    out->equip = item_is_equip(const_cast<void*>(item)) ? 1 : 0;
    out->rarity = golden_rarity(nullptr);
    out->damage = golden_damage(nullptr);
    out->defense = golden_defense(nullptr);
    out->magic_rate = bytes[I_MAGIC_RATE];
    out->need_level = golden_need_level(nullptr);
    out->socket = bytes[I_SOCKET];
    out->socket_filled = out->socket & 0x0F;
    out->socket_total = (out->socket >> 4) & 0x0F;
    out->enchant = *reinterpret_cast<const uint16_t*>(bytes + I_ENCHANT);
    out->chaos = (out->enchant & 1) != 0 ? 1 : 0;
    out->enchant_id = (out->enchant >> 11) & 0x1F;
    out->enchant_level = (out->enchant >> 6) & 0x1F;
    const uint32_t count = *reinterpret_cast<const uint32_t*>(bytes + I_COUNT);
    out->chaos_level = static_cast<int32_t>(count & 0xFF);
    out->chaos_rate = static_cast<int32_t>((count >> 8) & 0xFF);
    return qol::contracts::ok();
}

const qol::contracts::QolItemPortReadApi kGoldenRead{
    &golden_category,          &golden_unsupported2,      &golden_unsupported1,
    &golden_unsupported1,      &golden_unsupported_price, &golden_cumulate_count,
    &golden_item_ref_at,       &golden_unsupported_phys,  &golden_unsupported_detail_context,
    &golden_inventory_json, &golden_money,
    &golden_shop, &golden_in_world, &golden_static_text,
    &golden_static_ready, &golden_view, &stub_effective_cumulate,
    &fake_inventory_extension_access, &fake_item_count_limit, &fake_count_encoding,
};
const qol::contracts::QolItemPort kGoldenPort{qol::contracts::kQolPortItemVersion,
                                               sizeof(qol::contracts::QolItemPort), kGoldenRead};
const void* const kGoldenPortArray[] = {
    &kGoldenPort, &kFakeMoneyWritePort, &kFakeDiceWritePort, &kFakeItemStateWritePort};

static void golden_fill_item(uint8_t* bytes, uint16_t flags, uint32_t raw_count, uint8_t magic,
                             uint8_t socket, uint16_t enchant) {
    *reinterpret_cast<uint16_t*>(bytes + I_TYPE) = flags;
    *reinterpret_cast<uint32_t*>(bytes + I_COUNT) = raw_count;
    bytes[I_MAGIC_RATE] = magic;
    bytes[I_SOCKET] = socket;
    *reinterpret_cast<uint16_t*>(bytes + I_ENCHANT) = enchant;
    *reinterpret_cast<uint8_t**>(bytes + I_OPTION_LIST) = nullptr;
}

// legacy `build_inventory_json` 的逐字节复刻（T2h-0 手法）。api/native 读面退役后作为
// inventory 的独立 state 对照源；同时用于 T2h-B0 的 frozen==legacy 等值断言。
std::string frozen_inventory_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    constexpr int BAG_COUNT = 6;
    constexpr size_t BAG_STRIDE = 0x80;
    std::string s = "{\"bags\":[";
    for (int b = 0; b < BAG_COUNT; ++b) {
        if (b > 0) s += ",";
        s += "{\"bag\":" + std::to_string(b) + ",\"items\":[";
        int filled = 0;
        if (g_inven != nullptr) {
            uint8_t* bag_slots = reinterpret_cast<uint8_t*>(g_inven) + b * BAG_STRIDE;
            for (int j = 0; j < 16; ++j) {
                void* item = *reinterpret_cast<void**>(bag_slots + j * 8);
                if (item == nullptr) continue;
                uint16_t flags =
                    *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
                if (filled > 0) s += ",";
                s += "{\"slot\":" + std::to_string(j);
                s += ",\"type_flags\":" + std::to_string(flags);
                s += ",\"raw_rarity\":" + std::to_string((flags >> 2) & 0x0F);
                if (fn_get_bit != nullptr) {
                    s += ",\"category\":" + std::to_string(fn_get_bit(flags, 15, 6));
                }
                const qol::contracts::QolItemCountApi* count_api =
                    qol::game::item_count_api_if_resident();
                if (fn_get_cumulate_count != nullptr || count_api != nullptr) {
                    int32_t effective =
                        fn_get_cumulate_count != nullptr ? fn_get_cumulate_count(item) : 0;
                    if (count_api != nullptr && count_api->effective != nullptr) {
                        uint32_t view = 0;
                        const QolResult result = count_api->effective(item, &view);
                        if (qol::contracts::succeeded(result)) {
                            effective = static_cast<int32_t>(view);
                        }
                    }
                    s += ",\"count\":" + std::to_string(effective);
                }
                s += std::string(",\"equip\":") + (item_is_equip(item) ? "true" : "false");
                if (fn_get_rarity != nullptr) {
                    s += ",\"rarity\":" + std::to_string(fn_get_rarity(item));
                }
                append_item_attrs(s, item);
                s += "}";
                ++filled;
            }
        }
        s += "],\"capacity\":16,\"slot_count\":" + std::to_string(filled) + "}";
    }
    s += extension_bag_inventory_bags_json();
    s += "]}";
    return s;
}

void test_inventory_owner_vs_legacy_golden() {
    std::memset(g_golden_bag_storage, 0, sizeof(g_golden_bag_storage));
    std::memset(g_golden_item_a, 0, sizeof(g_golden_item_a));
    std::memset(g_golden_item_b, 0, sizeof(g_golden_item_b));
    std::memset(g_golden_item_projected, 0, sizeof(g_golden_item_projected));
    std::memset(g_golden_item_task, 0, sizeof(g_golden_item_task));
    g_override_available = false;
    g_override_enabled = false;
    g_count_encoded = true;
    g_canonical_count = 0;
    golden_fill_item(g_golden_item_a, static_cast<uint16_t>(10 << 6), 71, 6, 0x21, 0);
    golden_fill_item(g_golden_item_b, static_cast<uint16_t>(12 << 6), 1, 0, 0, 1);
    golden_fill_item(g_golden_item_task, static_cast<uint16_t>(10 << 6), 2, 0, 0, 0);
    *reinterpret_cast<void**>(g_golden_bag_storage + 0 * 0x80 + 0 * 8) = g_golden_item_a;
    *reinterpret_cast<void**>(g_golden_bag_storage + 1 * 0x80 + 2 * 8) = g_golden_item_b;
    *reinterpret_cast<void**>(g_golden_bag_storage + 5 * 0x80 + 15 * 8) = g_golden_item_task;
    g_inven = g_golden_bag_storage;
    g_golden_world = 1;
    g_in_world = true;

    const auto& descriptor = qol::features::core_item::plugin_descriptor();
    qol::host::http_dispatch_reset_for_test();
    qol::host::http_dispatch_set_op_enabled(true);
    qol::contracts::QolHostSdk sdk = make_sdk();
    qol::contracts::PluginContext ctx{};
    ctx.struct_size = sizeof(qol::contracts::PluginContext);
    ctx.sdk_major = qol::contracts::kHostSdkMajor;
    ctx.sdk_minor = qol::contracts::kHostSdkMinor;
    ctx.sdk = &sdk;
    ctx.plugin_id = "core.item";
    ctx.ports = kGoldenPortArray;
    ctx.port_count = 4;
    CHECK(descriptor.install(ctx) == 0);
    descriptor.on_enabled(ctx);
    CHECK(qol::contracts::succeeded(qol::tests::register_plugin_http(descriptor)));
    CHECK(g_scheduler.handler != nullptr);
    g_golden_require_active = true;

    // T2h-B2 B2c（W7）：经 item.state capability 取 owner raw 投影（唯一实现 = serialize_inventory_json）。
    const qol::contracts::ItemStateApi* item_state_api = nullptr;
    for (uint32_t i = 0; i < descriptor.provides_count; ++i) {
        if (std::strcmp(descriptor.provides[i].id, qol::contracts::kItemStateCapabilityId) == 0) {
            item_state_api =
                static_cast<const qol::contracts::ItemStateApi*>(descriptor.provides[i].iface);
        }
    }
    CHECK(item_state_api != nullptr && item_state_api->write_state_json != nullptr);

    struct Fixture {
        const char* name;
        int32_t raw;
        bool override_on;
        uint32_t canonical;
        bool eb;
    };
    const Fixture fixtures[] = {
        {"default", 3, false, 3, false},
        {"stack_on", 71, true, 199, false},
        {"stack_off", 71, false, 71, false},
        {"extension_bag", 71, true, 199, true},
    };
    std::vector<char> owner_raw(qol::contracts::kQolItemStateRawInventoryCapacity);
    for (const auto& fixture : fixtures) {
        g_golden_raw = fixture.raw;
        g_override_available = fixture.override_on;
        g_override_enabled = fixture.override_on;
        g_canonical_count = fixture.canonical;
        g_golden_eb_enabled = fixture.eb ? 1 : 0;
        g_provider_resident = true;
        g_provider_available = true;
        g_provider_fragment = extension_bag_inventory_bags_json();
        const std::string legacy = build_inventory_json();
        const std::string frozen = frozen_inventory_json();
        // T2h-B0 golden（可证部分）：frozen 复刻 == legacy 实现，逐字节。
        CHECK(frozen == legacy);
        // T2h-B2 W7：owner raw（经 item.state）== frozen == legacy，逐字节（4 fixture）。
        uint32_t raw_size = 0;
        CHECK(item_state_api->write_state_json(
                  static_cast<int32_t>(qol::contracts::QolItemStateKind::kRawInventory),
                  owner_raw.data(), static_cast<uint32_t>(owner_raw.size()), &raw_size).ok ==
              kQolOk);
        CHECK(std::string(owner_raw.data(), raw_size) == frozen);
        CHECK(std::string(owner_raw.data(), raw_size).find(
                  "\"bag\":5,\"items\":[{\"slot\":15") != std::string::npos);
        CHECK(run_scheduler(1));
        uint16_t status = 0;
        const std::string owner_http = dispatch_get("/api/item/inventory", &status);
        std::vector<char> enriched_inventory(qol::contracts::kQolItemStateRawInventoryCapacity);
        uint32_t enriched_size = 0;
        CHECK(item_state_api->write_state_json(
                  static_cast<int32_t>(qol::contracts::QolItemStateKind::kEnrichedInventory),
                  enriched_inventory.data(), static_cast<uint32_t>(enriched_inventory.size()),
                  &enriched_size).ok == kQolOk);
        CHECK(std::string(enriched_inventory.data(), enriched_size) == owner_http);
        // 探针（非断言）：owner 公开面（core.item HTTP）输出为**富化** schema，与 legacy
        // raw builder 天然不同；raw owner serializer（plugin.cpp:490）未经任何公开接口暴露，
        // 故 B0 的 owner-raw==legacy 不可达 → B2 前置不通过。差异清单见执行记录。
        if (owner_http == legacy) {
            std::printf("T2h-B0 NOTE [%s] owner HTTP == legacy（意外）\n", fixture.name);
        }
    }
    {
        uint16_t status = 0;
        const std::string items = dispatch_get("/api/item/inventory/items", &status);
        CHECK(status == 200);
        CHECK(items.find("\"slot\":15") != std::string::npos);
        const qol::contracts::HttpPathParam task_bag{"bag", 3, "5", 1};
        CHECK(dispatch_template("/api/item/inventory/bag/5/info", &task_bag, 1, &status) ==
              "{\"bag\":5,\"capacity\":16,\"slot_count\":1}");
        const qol::contracts::HttpPathParam task_slot[] = {{"bag", 3, "5", 1},
                                                            {"slot", 4, "15", 2}};
        const std::string task_item =
            dispatch_template("/api/item/inventory/bag/5/15", task_slot, 2, &status);
        CHECK(status == 200 && task_item.find("\"slot\":15") != std::string::npos);
        const qol::contracts::HttpPathParam task_slot_out_of_range[] = {
            {"bag", 3, "5", 1}, {"slot", 4, "16", 2}};
        (void)dispatch_template("/api/item/inventory/bag/5/16", task_slot_out_of_range, 2,
                                &status);
        CHECK(status == 400 || status == 404);
        CHECK(dispatch_post("/api/item/inventory/move_item",
                            "{\"bag\":\"5\",\"slot\":\"15\",\"count\":\"1\","
                            "\"to_bag\":\"0\",\"to_slot\":\"0\"}",
                            nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
        ItemWriteResultContext context{};
        context.handler = g_result_handler;
        context.payload_size = g_post_payload_size;
        std::memcpy(context.payload, g_post_payload, g_post_payload_size);
        char result[128]{};
        uint32_t result_size = 0;
        CHECK(!run_item_write_result(&context, result, sizeof(result), &result_size));
        CHECK(std::string(result, result_size) ==
              "{\"ok\":false,\"error\":\"task bag excluded\"}");
    }
    // T2h-B2：not-in-world owner raw == legacy；capability 缺席（停用/未注册形态）→ fallback legacy。
    g_golden_world = 0;
    g_in_world = false;
    uint32_t nw_size = 0;
    CHECK(item_state_api->write_state_json(
              static_cast<int32_t>(qol::contracts::QolItemStateKind::kRawInventory),
              owner_raw.data(), static_cast<uint32_t>(owner_raw.size()), &nw_size).ok == kQolOk);
    CHECK(std::string(owner_raw.data(), nw_size) == frozen_inventory_json());
    CHECK(qol::host::build_item_state_json_via_owner(nullptr, 0, 4096, &build_inventory_json) ==
          build_inventory_json());
    g_golden_world = 1;
    g_in_world = true;
    g_golden_require_active = false;
    descriptor.deactivate(ctx);
}

// ---------------------------------------------------------------------------
// R2：数据面降级（单槽跳过 / EB 段省略 / 写票据不牵连 / 文案防回归）
// ---------------------------------------------------------------------------

struct GoldenHarness {
    const qol::contracts::ItemStateApi* item_state = nullptr;
    qol::contracts::PluginContext ctx{};
};

// 复用 T2h-B0 golden 夹具安装 core.item 并暴露 item.state 面（新用例共用）。
bool install_core_item_golden(GoldenHarness* out) {
    std::memset(g_golden_bag_storage, 0, sizeof(g_golden_bag_storage));
    std::memset(g_golden_item_a, 0, sizeof(g_golden_item_a));
    std::memset(g_golden_item_b, 0, sizeof(g_golden_item_b));
    std::memset(g_golden_item_task, 0, sizeof(g_golden_item_task));
    golden_fill_item(g_golden_item_a, static_cast<uint16_t>(10 << 6), 71, 6, 0x21, 0);
    golden_fill_item(g_golden_item_b, static_cast<uint16_t>(12 << 6), 1, 0, 0, 1);
    golden_fill_item(g_golden_item_task, static_cast<uint16_t>(10 << 6), 2, 0, 0, 0);
    *reinterpret_cast<void**>(g_golden_bag_storage + 0 * 0x80 + 0 * 8) = g_golden_item_a;
    *reinterpret_cast<void**>(g_golden_bag_storage + 1 * 0x80 + 2 * 8) = g_golden_item_b;
    *reinterpret_cast<void**>(g_golden_bag_storage + 5 * 0x80 + 15 * 8) = g_golden_item_task;
    g_inven = g_golden_bag_storage;
    g_golden_world = 1;
    g_in_world = true;
    g_golden_bad_slot = 0;
    g_golden_projected_canonical = 0;
    g_last_log_level = -1;
    g_last_log_message[0] = '\0';
    qol::host::http_dispatch_reset_for_test();
    qol::host::http_dispatch_set_op_enabled(true);
    const auto& descriptor = qol::features::core_item::plugin_descriptor();
    // 静态存储：插件持有 ctx.sdk 指针，须在 helper 返回后仍有效。
    static qol::contracts::QolHostSdk sdk;
    sdk = make_sdk();
    qol::contracts::PluginContext ctx{};
    ctx.struct_size = sizeof(qol::contracts::PluginContext);
    ctx.sdk_major = qol::contracts::kHostSdkMajor;
    ctx.sdk_minor = qol::contracts::kHostSdkMinor;
    ctx.sdk = &sdk;
    ctx.plugin_id = "core.item";
    ctx.ports = kGoldenPortArray;
    ctx.port_count = 4;
    if (descriptor.install(ctx) != 0) return false;
    descriptor.on_enabled(ctx);
    if (!qol::contracts::succeeded(qol::tests::register_plugin_http(descriptor))) {
        return false;
    }
    g_golden_require_active = true;
    // ITM-13：默认注入可用的中立 provider；具体 fragment 由各用例设置。
    g_provider_resident = true;
    g_provider_available = true;
    g_provider_fragment.clear();
    if (out != nullptr) {
        out->item_state = nullptr;
        out->ctx = ctx;
        for (uint32_t i = 0; i < descriptor.provides_count; ++i) {
            if (std::strcmp(descriptor.provides[i].id,
                            qol::contracts::kItemStateCapabilityId) == 0) {
                out->item_state =
                    static_cast<const qol::contracts::ItemStateApi*>(descriptor.provides[i].iface);
            }
        }
        return out->item_state != nullptr && out->item_state->write_state_json != nullptr;
    }
    return true;
}

std::string owner_raw_inventory() {
    std::vector<char> buffer(qol::contracts::kQolItemStateRawInventoryCapacity);
    uint32_t size = 0;
    const auto& descriptor = qol::features::core_item::plugin_descriptor();
    const qol::contracts::ItemStateApi* api = nullptr;
    for (uint32_t i = 0; i < descriptor.provides_count; ++i) {
        if (std::strcmp(descriptor.provides[i].id, qol::contracts::kItemStateCapabilityId) == 0) {
            api = static_cast<const qol::contracts::ItemStateApi*>(descriptor.provides[i].iface);
        }
    }
    if (api == nullptr ||
        api->write_state_json(static_cast<int32_t>(qol::contracts::QolItemStateKind::kRawInventory),
                              buffer.data(), static_cast<uint32_t>(buffer.size()), &size)
                .ok != kQolOk) {
        return "<serialize failed>";
    }
    return std::string(buffer.data(), size);
}

// ITM-13：扩展段由 provider 唯一实现（含槽位跳过与 R-47b 计数视图）；core.item 只逐字节
// 追加 provider fragment，不重拼字段、不二次覆盖 count、不丢弃或新增槽位。
void test_extension_degrade_bad_slot_skipped() {
    CHECK(install_core_item_golden(nullptr));
    g_golden_eb_enabled = 1;
    g_provider_fragment = extension_bag_inventory_bags_json();
    const std::string json = owner_raw_inventory();
    CHECK(json != "<install failed>" && json != "<serialize failed>");
    CHECK(json.find("\"bag\":0") != std::string::npos);              // 主袋照常
    CHECK(json.find("\"bag\":6") != std::string::npos);              // provider 段原样追加
    CHECK(json.find(g_provider_fragment) != std::string::npos);      // 逐字节追加
    CHECK(json.find("invalid extension inventory") == std::string::npos);
    g_provider_fragment.clear();
    g_golden_require_active = false;
}

// ITM-13：provider fragment 内的计数视图（canonical/关闭态 b）由 provider 决定；
// core.item 不得二次覆盖为 0 或重算。
void test_extension_degrade_projected_canonical_kept() {
    CHECK(install_core_item_golden(nullptr));
    g_golden_eb_enabled = 1;
    // provider 提供含非零 canonical 视图的投影槽；core.item 必须逐字节保留。
    g_provider_fragment =
        ",{\"bag\":9,\"items\":[{\"slot\":4,\"category\":35,\"count\":640}],"
        "\"capacity\":16,\"slot_count\":1}";
    const std::string json = owner_raw_inventory();
    CHECK(json != "<install failed>" && json != "<serialize failed>");
    CHECK(json.find("{\"slot\":4,\"category\":35,\"count\":640}") != std::string::npos);
    CHECK(json.find("\"bag\":9") != std::string::npos);
    CHECK(json.find(g_provider_fragment) != std::string::npos);
    g_provider_fragment =
        ",{\"bag\":9,\"items\":[{\"slot\":4,\"category\":35,\"count\":95}],"
        "\"capacity\":16,\"slot_count\":1}";
    const std::string json95 = owner_raw_inventory();
    CHECK(json95.find("{\"slot\":4,\"category\":35,\"count\":95}") != std::string::npos);
    CHECK(json95.find("\"bag\":9") != std::string::npos);
    g_provider_fragment.clear();
    g_golden_require_active = false;
}

// R2-2 用例 4：L2 EB capability 不可取 → 省略 EB 段、主快照成功、WARN、无 error。
void test_extension_degrade_omits_section() {
    CHECK(install_core_item_golden(nullptr));
    g_golden_require_active = false;
    g_provider_resident = false;
    g_provider_available = false;
    const std::string json = owner_raw_inventory();
    CHECK(json != "<install failed>" && json != "<serialize failed>");
    CHECK(json.find("\"bag\":0") != std::string::npos);
    CHECK(json.find("\"bag\":6") == std::string::npos);          // EB 段整体省略
    CHECK(json.find("\"error\"") == std::string::npos);
    CHECK(json.find("invalid extension inventory") == std::string::npos);
    CHECK(g_last_log_level == 2);
    CHECK(std::strstr(g_last_log_message, "extension inventory omitted") != nullptr);
    g_provider_resident = false;
    g_provider_available = false;
    g_golden_require_active = false;
}

// R2-2 用例 3：原版袋写票据不因 EB 段降级而 failed，结果只包含移动槽位。
void test_extension_degrade_write_ticket_done() {
    CHECK(install_core_item_golden(nullptr));
    g_golden_eb_enabled = 1;
    g_golden_bad_slot = 1;
    g_provider_fragment = extension_bag_inventory_bags_json();
    g_state_write_result = qol::contracts::ok();
    g_state_write_count = 4;
    g_state_write_data = {};
    uint16_t status = 0;
    CHECK(dispatch_post("/api/item/inventory/move_item",
                        "{\"bag\":\"0\",\"slot\":\"0\",\"count\":\"2\","
                        "\"to_bag\":\"1\",\"to_slot\":\"0\"}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    ItemWriteResultContext context{};
    context.handler = g_result_handler;
    context.payload_size = g_post_payload_size;
    std::memcpy(context.payload, g_post_payload, g_post_payload_size);
    qol::infra::command_queue_reset_for_test();
    const qol::infra::Command command{"core.item kMoveItem", nullptr, &context, false,
                                      &run_item_write_result, 0};
    const qol::infra::CommandTicket ticket = qol::infra::post(command);
    CHECK(ticket != 0 && qol::infra::drain() == 1);
    CHECK(qol::infra::ticket_state(ticket) == qol::infra::CommandState::kDone);
    char result[8192]{};
    uint32_t result_size = 0;
    CHECK(qol::infra::read_result(ticket, result, sizeof(result), &result_size));
    const std::string body(result, result_size);
    CHECK(body.find("{\"ok\":true,\"slots\":[") == 0);
    CHECK(body.find("\"state\"") == std::string::npos);
    CHECK(body.find("\"bags\"") == std::string::npos);
    CHECK(body.find("\"bag\":0,\"slot\":0") != std::string::npos);
    CHECK(body.find("\"bag\":1,\"slot\":0") != std::string::npos);
    CHECK(body.find("invalid extension inventory") == std::string::npos);
    g_golden_bad_slot = 0;
    qol::infra::command_queue_reset_for_test();
    g_golden_require_active = false;
}

void test_extension_move_result_capacity_and_overflow() {
    CHECK(install_core_item_golden(nullptr));
    g_golden_eb_enabled = 1;
    g_provider_resident = true;
    g_provider_available = true;
    const auto run_case = [](std::size_t body_size) {
        const std::string prefix = "{\"ok\":true,\"message\":\"";
        const std::string embedded = "\\\"ok\\\":false";
        const std::string suffix = "\"}";
        g_extension_move_body = prefix;
        g_extension_move_body += embedded;
        g_extension_move_body.append(body_size - prefix.size() - embedded.size() - suffix.size(), 'a');
        g_extension_move_body += suffix;
        uint16_t status = 0;
        CHECK(dispatch_post("/api/item/inventory/move_item",
                            "{\"bag\":\"6\",\"slot\":\"0\",\"count\":\"1\","
                            "\"to_bag\":\"6\",\"to_slot\":\"1\"}",
                            nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
        ItemWriteResultContext context{};
        context.handler = g_result_handler;
        context.payload_size = g_post_payload_size;
        std::memcpy(context.payload, g_post_payload, g_post_payload_size);
        qol::infra::command_queue_reset_for_test();
        const qol::infra::Command command{"core.item EB move overflow", nullptr, &context, false,
                                          &run_item_write_result, 0};
        const qol::infra::CommandTicket ticket = qol::infra::post(command);
        CHECK(ticket != 0 && qol::infra::drain() == 1);
        CHECK(qol::infra::ticket_state(ticket) == qol::infra::CommandState::kDone);
        CHECK(!qol::infra::ticket_result_overflowed(ticket));
        std::vector<char> result(qol::infra::kMaxResultBytes);
        uint32_t size = 0;
        CHECK(qol::infra::read_result(ticket, result.data(), result.size(), &size));
        constexpr std::string_view expected =
            "{\"ok\":true,\"slots\":[{\"bag\":6,\"slot\":0,\"item\":null},"
            "{\"bag\":6,\"slot\":1,\"item\":{\"slot\":1,\"name\":null,"
            "\"category\":10,\"count\":199,\"item_type\":\"consumable\","
            "\"need_level\":0,\"rarity\":0,\"rarity_tier\":\"白\","
            "\"base\":{\"damage\":0,\"defense\":0,\"magic_rate\":0},"
            "\"bonus\":[],\"gem\":{\"total_slots\":0,\"slots\":[]},"
            "\"chaos\":{\"is_chaos\":false,\"level\":0,\"rate\":100},"
            "\"enchant\":{\"id\":0,\"level\":0,\"effect\":null}}}]}";
        CHECK(size == expected.size());
        CHECK(std::string_view(result.data(), size) == expected);
        CHECK(std::string_view(result.data(), size).find("state") == std::string_view::npos);
        CHECK(size < body_size);
        qol::infra::command_queue_reset_for_test();
    };
    run_case(5000);
    run_case(20000);
    g_provider_resident = false;
    g_provider_available = false;
    g_golden_eb_enabled = 0;
    g_golden_require_active = false;
}

// T2h-B3#2：debug raw 整函数从 api/native/game_inventory_read.inc 搬至
// game/read/item_class.cpp。以下为搬迁前实现的逐字节复刻（before oracle），
// 与 game/read 新落点 debug_item_raw_json() 在 fake 环境对照。
namespace {

std::string frozen_dbg_hex(unsigned long value, int digits) {
    static const char kHexDigits[] = "0123456789abcdef";
    std::string s = "0x";
    for (int i = digits - 1; i >= 0; --i) {
        s += kHexDigits[(value >> (i * 4)) & 0xFu];
    }
    return s;
}

std::string frozen_debug_item_raw_json(int32_t bag, int32_t slot) {
    if (!game_in_world()) return op_err("not in world");
    InventoryItemRef ref{};
    if (!inventory_item_ref_at(bag, slot, &ref) || ref.native_item == nullptr) {
        return op_err("item not found");
    }
    uint8_t* it = reinterpret_cast<uint8_t*>(ref.native_item);
    const uint16_t type_raw = *reinterpret_cast<uint16_t*>(it + I_TYPE);
    const uint32_t count_raw = *reinterpret_cast<uint32_t*>(it + I_COUNT);
    const uint8_t socket_raw = it[I_SOCKET];
    const uint16_t enchant_raw = *reinterpret_cast<uint16_t*>(it + I_ENCHANT);
    std::string s = "{\"ok\":true";
    s += ",\"bag\":" + std::to_string(ref.bag);
    s += ",\"slot\":" + std::to_string(ref.slot);
    s += ",\"kind\":" +
         std::string(ref.kind == InventoryItemKind::kProvider ? "\"extension\"" : "\"original\"");
    s += ",\"category\":" + std::to_string(ref.category);
    s += ",\"count\":" + std::to_string(ref.count);
    s += ",\"type_raw\":\"" + frozen_dbg_hex(type_raw, 4) + "\"";
    s += ",\"raw_grade\":" + std::to_string((type_raw >> 2) & 0x0F);
    s += ",\"count_raw\":\"" + frozen_dbg_hex(count_raw, 8) + "\"";
    s += ",\"socket_raw\":\"" + frozen_dbg_hex(socket_raw, 2) + "\"";
    s += ",\"socket_total\":" + std::to_string((socket_raw >> 4) & 0x0F);
    s += ",\"socket_filled\":" + std::to_string(socket_raw & 0x0F);
    s += ",\"enchant_raw\":\"" + frozen_dbg_hex(enchant_raw, 4) + "\"";
    s += ",\"enchant_count\":" + std::to_string((enchant_raw >> 6) & 0x1F);
    s += ",\"enhance_low4\":" + std::to_string((enchant_raw >> 2) & 0x0F);
    s += ",\"enchant_id\":" + std::to_string((enchant_raw >> 11) & 0x1F);
    s += ",\"chaos\":" + std::to_string(enchant_raw & 1u);
    s += ",\"note\":\"raw fields for bit-level verification\"}";
    return s;
}

}  // namespace

void test_debug_item_raw_golden() {
    g_golden_world = 1;
    // fixture 1：default 槽（kind=original，投影 bag/slot/category/count）。
    g_debug_raw_mode = 1;
    std::memset(g_debug_raw_bytes, 0, sizeof(g_debug_raw_bytes));
    golden_fill_item(g_debug_raw_bytes, static_cast<uint16_t>((12 << 6) | (2 << 2)), 0x12345678u,
                     0x11, 0x21, 0x7C4u);
    const std::string def_after = debug_item_raw_json(2, 5);
    CHECK(def_after == frozen_debug_item_raw_json(2, 5));
    CHECK(def_after.find("\"kind\":\"original\"") != std::string::npos);
    // fixture 2：扩展袋槽（kind=extension）。
    g_debug_raw_mode = 2;
    std::memset(g_debug_raw_bytes, 0, sizeof(g_debug_raw_bytes));
    golden_fill_item(g_debug_raw_bytes, static_cast<uint16_t>((10 << 6) | (3 << 2)), 0x000000C7u,
                     0x22, 0x40, 0x0C1u);
    const std::string ext_after = debug_item_raw_json(6, 0);
    CHECK(ext_after == frozen_debug_item_raw_json(6, 0));
    CHECK(ext_after.find("\"kind\":\"extension\"") != std::string::npos);
    // 错误路径：item not found（world 内但 ref 不可用）。
    g_debug_raw_mode = 0;
    CHECK(debug_item_raw_json(2, 5) == frozen_debug_item_raw_json(2, 5));
    CHECK(debug_item_raw_json(2, 5) == "{\"ok\":false,\"error\":\"item not found\"}");
    // 错误路径：not in world。
    g_golden_world = 0;
    CHECK(debug_item_raw_json(2, 5) == frozen_debug_item_raw_json(2, 5));
    CHECK(debug_item_raw_json(2, 5) == "{\"ok\":false,\"error\":\"not in world\"}");
    g_golden_world = 1;
    g_debug_raw_mode = 0;
}

int main() {
    // legacy 读路径经 game 中立 facade 取 provider fragment；与 item port 替身共用同一 fake 表。
    qol::game::inventory_extension_bind_host();
    if (!qol::game::inventory_extension_set_provider(&kFakeProvider,
                                                    &fake_provider_available_probe)) {
        std::printf("FAIL %s:%d: fake provider registration\n", __FILE__, __LINE__);
        return 1;
    }
    test_lifecycle_and_responses();
    test_static_not_ready_holds_snapshot();
    test_bridge_item_write_result_helper();
    test_inventory_owner_vs_legacy_golden();
    test_extension_degrade_bad_slot_skipped();
    test_extension_degrade_projected_canonical_kept();
    test_extension_degrade_omits_section();
    test_extension_degrade_write_ticket_done();
    test_extension_move_result_capacity_and_overflow();
    test_debug_item_raw_golden();
    std::printf("item_facts_dispatch: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

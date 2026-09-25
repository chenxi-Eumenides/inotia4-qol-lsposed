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
#include "contracts/item_count.h"
#include "contracts/item_facts.h"
#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "contracts/result.h"
#include "host/http_dispatch.h"
#include "host/http_registry.h"
#include "infra/command_queue.h"
#include "plugins/core.item/plugin.h"

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>

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
using qol::contracts::QolItemWritePort;
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
uint32_t g_canonical_count = 3;
uint32_t g_original_count = 3;
bool g_count_encoded = true;
bool g_effective_fail = false;
QolResult g_money_write_result = qol::contracts::ok();
int64_t g_money_write_value = 0;
QolResult g_dice_reject_result = qol::contracts::ok();
int g_dice_reject_calls = 0;
qol::contracts::QolTypedResultCommandHandler g_result_handler = nullptr;
uint32_t g_post_handler_id = 0;
uint8_t g_post_payload[qol::contracts::kTypedCommandPayloadCapacity]{};
uint32_t g_post_payload_size = 0;
QolItemWriteArgs g_last_write{};
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

QolResult unsupported() { return qol::contracts::error(kQolErrUnsupported, "stub", 4); }
QolResult stub_category(const void*, int32_t*) { return unsupported(); }
QolResult stub_is_real_equip(const void*, int32_t*) { return unsupported(); }
QolResult stub_is_jewel(int32_t, int32_t*) { return unsupported(); }
QolResult stub_no_sell(int32_t, int32_t*) { return unsupported(); }
QolResult stub_sell_price(const void*, int64_t*) { return unsupported(); }
QolResult stub_cumulate(const void*, int32_t* out_count) {
    *out_count = static_cast<int32_t>(g_original_count);
    return qol::contracts::ok();
}
QolResult stub_item_ref(int32_t bag, int32_t slot, qol::contracts::QolInventoryItemRef* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    if (bag != 0 || slot != 0) return qol::contracts::error(kQolErrNotFound, "empty", 5);
    *out = qol::contracts::QolInventoryItemRef{
        static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kOriginal), 0, 0, 10,
        static_cast<int32_t>(g_canonical_count), reinterpret_cast<void*>(1)};
    return qol::contracts::ok();
}
QolResult fake_inventory_item_view(const void* item,
                                   qol::contracts::QolItemInventoryView* out) {
    if (item == nullptr || out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad item view", 13);
    }
    *out = {};
    out->fields = qol::contracts::kQolItemInventoryHasCategory |
                  qol::contracts::kQolItemInventoryHasRarity |
                  qol::contracts::kQolItemInventoryHasDamage |
                  qol::contracts::kQolItemInventoryHasDefense |
                  qol::contracts::kQolItemInventoryHasNeedLevel;
    out->type_flags = 40;
    out->raw_rarity = 0;
    out->category = 10;
    out->count_supported = 1;
    out->equip = 0;
    out->rarity = 0;
    out->damage = 0;
    out->defense = 0;
    out->magic_rate = 0;
    out->need_level = 0;
    out->chaos_rate = 100;
    return qol::contracts::ok();
}
QolResult stub_physical(int32_t, int32_t, void**) { return unsupported(); }
QolResult stub_detail(int32_t*, int32_t*, void**) { return unsupported(); }
QolResult stub_cursor(const void*, int32_t*) { return unsupported(); }

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

const QolItemPortReadApi kFakeRead{
    &stub_category,   &stub_is_real_equip, &stub_is_jewel,   &stub_no_sell,
    &stub_sell_price, &stub_cumulate,      &stub_item_ref,   &stub_physical,
    &stub_detail,     &stub_cursor,        &fake_inventory,  &fake_money,
    &fake_shop,       &fake_in_world,      &fake_static_text, &fake_static_ready,
    &fake_inventory_item_view,
};

const QolItemPort kFakePort{qol::contracts::kQolPortItemVersion, kFakeRead};
const qol::contracts::QolItemMoneyWritePort kFakeMoneyWritePort{
    qol::contracts::kQolPortItemMoneyWriteVersion, &fake_set_money};
const qol::contracts::QolItemDiceWritePort kFakeDiceWritePort{
    qol::contracts::kQolPortItemDiceWriteVersion, &fake_reject_dice};

QolResult fake_item_write(const QolItemWriteArgs* args, char* out, uint32_t capacity,
                          uint32_t* out_size) {
    if (args == nullptr || out == nullptr || out_size == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad write", 9);
    }
    g_last_write = *args;
    ++g_write_calls;
    constexpr char kResult[] = "{\"ok\":true}";
    if (capacity < sizeof(kResult) - 1) {
        return qol::contracts::error(kQolErrBlocked, "small", 5);
    }
    std::memcpy(out, kResult, sizeof(kResult) - 1);
    *out_size = sizeof(kResult) - 1;
    return qol::contracts::ok();
}

const QolItemWritePort kFakeWritePort{qol::contracts::kQolPortItemWriteVersion,
                                      {&fake_item_write}};

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
    g_state_move_count = count;
    g_state_move_to_bag = to_bag;
    g_state_move_to_slot = to_slot;
    return fake_state_write(static_cast<int32_t>(qol::contracts::QolItemWriteKind::kMoveItem),
                            bag, slot, out);
}

const qol::contracts::QolItemInventoryStateWritePort kFakeItemStateWritePort{
    qol::contracts::kQolPortItemInventoryStateWriteVersion,
    &fake_state_use,
    &fake_state_sell,
    &fake_state_discard,
    &fake_state_shop_buy,
    &fake_state_move,
};

const void* const kPortArray[] = {
    &kFakePort, &kFakeMoneyWritePort, &kFakeDiceWritePort, &kFakeWritePort,
    &kFakeItemStateWritePort};

struct FakeScheduler {
    qol::contracts::QolFrameHandler handler = nullptr;
    void* ctx = nullptr;
    int32_t interval = 0;
};

FakeScheduler g_scheduler;

QolResult fake_log_write(int32_t, int32_t, const char*, const char*, uint32_t) {
    return qol::contracts::ok();
}

bool g_override_available = false;
bool g_override_enabled = false;

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
const qol::contracts::StackLimitItemCountCapability kOverrideCapability{
    &kOverrideItemCountApi, &override_enabled};

bool g_extension_bag_available = false;
bool g_extension_bag_enabled = true;
int g_extension_move_calls = 0;
std::string g_extension_move_body = "{\"ok\":true,\"state\":{}}";

bool fake_extension_enabled() { return g_extension_bag_enabled; }
bool fake_is_logical_bag(int32_t bag) { return bag >= 6 && bag <= 10; }
int32_t fake_extension_move(int32_t, int32_t, int32_t, int32_t, char* out, uint32_t capacity,
                            uint32_t* out_len) {
    ++g_extension_move_calls;
    if (out == nullptr || out_len == nullptr) return 0;
    if (g_extension_move_body.size() > capacity) return 0;
    std::memcpy(out, g_extension_move_body.data(), g_extension_move_body.size());
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

QolResult fake_capability_require(const char* id, QolVersionRange, const void** out_iface) {
    if (id == nullptr || out_iface == nullptr) {
        return qol::contracts::error(kQolErrNotFound, "missing", 7);
    }
    if (std::strcmp(id, qol::contracts::kExtensionBagCapabilityId) == 0) {
        if (!g_extension_bag_available) {
            return qol::contracts::error(kQolErrNotFound, "missing", 7);
        }
        *out_iface = &kFakeExtensionBagApi;
        return qol::contracts::ok();
    }
    if (std::strcmp(id, qol::contracts::kStackLimitItemCountCapabilityId) != 0 ||
        !g_override_available) {
        return qol::contracts::error(kQolErrNotFound, "missing", 7);
    }
    *out_iface = &kOverrideCapability;
    return qol::contracts::ok();
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
    sdk.http = qol::host::http_registry_api();
    sdk.http_dispatch = qol::host::http_dispatch_api();
    sdk.commands.post_typed = &fake_post_typed;
    sdk.ticket_result.register_typed_result_handler = &fake_register_result;
    sdk.ticket_result.unregister_typed_result_handler = &fake_unregister_result;
    return sdk;
}

const qol::contracts::HttpRoute kRoutes[] = {
    {qol::contracts::HttpMethod::kGet, "/api/item/inventory", "core.item", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/item/inventory/money", "core.item", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/item/inventory/items", "core.item", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/item/inventory/bag/{bag}/info", "core.item", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/item/inventory/bag/{bag}/{slot}", "core.item", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/item/shop/items", "core.item", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/item/inventory/use_item", "core.item", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/item/inventory/sell_item", "core.item", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/item/inventory/move_item", "core.item", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/item/inventory/discard_item", "core.item", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/item/inventory/accept_dice", "core.item", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/item/inventory/reject_dice", "core.item", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/item/inventory/{role}/equip_item", "core.item", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/item/inventory/{role}/unequip_item", "core.item", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/item/inventory/{role}/put_jewel", "core.item", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/item/inventory/{role}/enchant", "core.item", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/item/shop/buy_item", "core.item", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/op/inventory/add", "core.item", 1, 1},
    {qol::contracts::HttpMethod::kPost, "/api/op/inventory/money", "core.item", 1, 1},
};

qol::contracts::PluginContext make_context(const qol::contracts::QolHostSdk* sdk) {
    qol::contracts::PluginContext ctx{};
    ctx.struct_size = sizeof(qol::contracts::PluginContext);
    ctx.sdk_major = qol::contracts::kHostSdkMajor;
    ctx.sdk_minor = qol::contracts::kHostSdkMinor;
    ctx.sdk = sdk;
    ctx.game = nullptr;
    ctx.plugin_id = "core.item";
    ctx.ports = kPortArray;
    ctx.port_count = 5;
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
    const auto& descriptor = qol::features::core_item::plugin_descriptor();
    qol::host::http_dispatch_set_op_enabled(true);
    uint16_t status = 0;
    CHECK(dispatch_post("/api/item/inventory/use_item",
                        "{\"bag\":8,\"bag\":\"0\",\"slot\":0,\"slot\":\"1\"}", nullptr,
                        0, &status) == "{\"ok\":true,\"ticket\":72}");
    CHECK(status == 200 && g_post_payload_size > 0 && g_result_handler != nullptr);
    CHECK(descriptor.http_query_count == 19);
    char result[8192]{};
    uint32_t result_size = 0;

    g_state_write_result = qol::contracts::ok();
    g_state_write_count = 2;
    g_state_write_data = {};
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":true,\"state\":" + expected_raw_inventory_with_count(2) + "}");
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
          "{\"ok\":true,\"price\":27,\"state\":" + expected_raw_inventory_with_count(4) + "}");
    CHECK(g_state_write_kind == static_cast<int32_t>(qol::contracts::QolItemWriteKind::kSellItem));

    g_state_write_count = 5;
    CHECK(dispatch_post("/api/item/inventory/discard_item", "{\"bag\":0,\"slot\":0}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":true,\"state\":" + expected_raw_inventory_with_count(5) + "}");
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
          "{\"ok\":true,\"price\":31,\"state\":" + expected_raw_inventory_with_count(6) + "}");
    CHECK(g_state_write_kind == static_cast<int32_t>(qol::contracts::QolItemWriteKind::kShopBuy));

    g_state_write_count = 3;
    g_state_write_data = {};
    g_state_write_data.flags = qol::contracts::kQolItemInventoryStateHasGained;
    g_state_write_data.gained_count = 1;
    g_state_write_data.gained[0] = {4, 2, 75, 3};
    CHECK(dispatch_post("/api/item/inventory/use_item", "{\"bag\":0,\"slot\":0}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":true,\"gained\":[{\"bag\":4,\"slot\":2,\"category\":75,\"count\":3}],\"state\":" +
              expected_raw_inventory_with_count(3) + "}");

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

    // 普通袋移动：typed 低层 + #10 owner serializer 即时写后 state。
    g_state_write_result = qol::contracts::ok();
    g_state_write_count = 7;
    g_state_write_data = {};
    CHECK(dispatch_post("/api/item/inventory/move_item",
                        "{\"bag\":\"0\",\"slot\":\"2\",\"count\":\"4\","
                        "\"to_bag\":\"1\",\"to_slot\":\"3\"}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) ==
          "{\"ok\":true,\"state\":" + expected_raw_inventory_with_count(7) + "}");
    CHECK(g_state_write_kind == static_cast<int32_t>(qol::contracts::QolItemWriteKind::kMoveItem));
    CHECK(g_state_write_arg0 == 0 && g_state_write_arg1 == 2);
    CHECK(g_state_move_count == 4 && g_state_move_to_bag == 1 && g_state_move_to_slot == 3);

    // 扩展袋移动：owner 经 capability 取 v2 move_item，纯 JSON 输出、不附 state。
    g_extension_bag_available = true;
    g_extension_bag_enabled = true;
    g_extension_move_calls = 0;
    g_extension_move_body = "{\"ok\":true,\"state\":{\"extension\":true}}";
    g_state_write_count = 1;
    CHECK(dispatch_post("/api/item/inventory/move_item",
                        "{\"bag\":\"6\",\"slot\":\"2\",\"count\":\"4\","
                        "\"to_bag\":\"7\",\"to_slot\":\"-1\"}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) == "{\"ok\":true,\"state\":{\"extension\":true}}");
    CHECK(g_extension_move_calls == 1);
    const int state_calls_before_missing = g_state_write_calls;
    g_extension_move_body = "{\"ok\":false,\"error\":\"move failed\"}";
    CHECK(!g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                            &result_size));
    CHECK(std::string(result, result_size) == "{\"ok\":false,\"error\":\"move failed\"}");
    CHECK(g_extension_move_calls == 2);
    CHECK(g_state_write_calls == state_calls_before_missing);
    g_extension_move_body = "{\"ok\":true,\"state\":{}}";

    // 能力缺失回退：扩展请求回落 typed 普通移动分支（旧 data_op_move_item + state 等价）。
    g_extension_bag_available = false;
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
          "{\"ok\":true,\"state\":" + expected_raw_inventory_with_count(2) + "}");
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
    g_extension_bag_available = false;
    g_original_count = 3;

    const qol::contracts::HttpPathParam role{"role", 4, "1", 1};
    CHECK(dispatch_post("/api/item/inventory/1/equip_item", "{\"category\":10}", &role, 1,
                        &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(g_last_write.kind == static_cast<int32_t>(qol::contracts::QolItemWriteKind::kEquipItem));
    CHECK(g_last_write.i0 == 1 && g_last_write.i1 == 0 && g_last_write.i2 == 0);

    CHECK(dispatch_post("/api/op/inventory/add",
                        "{\"category\":\" 12 \",\"count\":\"1e2\",\"socket\":\"3.7\","
                        "\"enhanceRemaining\":\"+3\",\"rarity\":\"NaN\"}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(g_last_write.kind == static_cast<int32_t>(qol::contracts::QolItemWriteKind::kAddItem));
    CHECK(g_last_write.i0 == 12 && g_last_write.i1 == 100 && g_last_write.i2 == 3 &&
          g_last_write.i3 == 3 && g_last_write.i4 == 0);

    CHECK(dispatch_post("/api/op/inventory/add",
                        "{\"category\":\"Infinity\",\"count\":\"-Infinity\","
                        "\"socket\":\"NaN\",\"enhanceRemaining\":\"-3.7\","
                        "\"rarity\":\"1e2\"}", nullptr, 0, &status) ==
          "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(g_last_write.i0 == std::numeric_limits<int32_t>::max() &&
          g_last_write.i1 == std::numeric_limits<int32_t>::min() && g_last_write.i2 == 0 &&
          g_last_write.i3 == -3 && g_last_write.i4 == 100);

    CHECK(dispatch_post("/api/op/inventory/add",
                        "{\"category\":\"1e100\",\"count\":\"-1e100\"}", nullptr,
                        0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(g_last_write.i0 == std::numeric_limits<int32_t>::max() &&
          g_last_write.i1 == std::numeric_limits<int32_t>::min());

    CHECK(dispatch_post("/api/op/inventory/add",
                        "{\"category\":3.7,\"count\":3.7,\"socket\":1.7,"
                        "\"enhanceRemaining\":2.7,\"rarity\":4.7}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(g_last_write.i0 == 3 && g_last_write.i1 == 3 && g_last_write.i2 == 1 &&
          g_last_write.i3 == 2 && g_last_write.i4 == 4);

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
    CHECK(status == 200 && g_money_write_value == 4294967296LL && g_write_calls == 5);

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

    const int write_calls_before_invalid = g_write_calls;
    CHECK(dispatch_post("/api/op/inventory/add", "{\"category\":\"not-a-number\"}",
                        nullptr, 0, &status) ==
          "{\"ok\":false,\"error\":\"category required\"}");
    CHECK(status == 400 && g_write_calls == write_calls_before_invalid);
    qol::host::http_dispatch_set_op_enabled(false);
}

void register_frozen_routes() {
    for (const auto& route : kRoutes) {
        CHECK(qol::host::http_registry_api().register_route(&route).ok == kQolOk);
    }
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
    register_frozen_routes();

    const qol::contracts::QolHostSdk sdk = make_sdk();
    qol::contracts::PluginContext ctx = make_context(&sdk);
    const auto& descriptor = qol::features::core_item::plugin_descriptor();

    CHECK(descriptor.install(ctx) == 0);
    const auto* item_facts = static_cast<const qol::contracts::ItemFactsApi*>(
        descriptor.provides[0].iface);
    int32_t count = 0;
    CHECK(item_facts->cumulate_count(reinterpret_cast<void*>(1), &count).ok == kQolOk);
    CHECK(count == 3);
    g_override_available = true;
    g_override_enabled = false;
    CHECK(item_facts->cumulate_count(reinterpret_cast<void*>(1), &count).ok == kQolOk);
    CHECK(count == 3);
    g_canonical_count = 23;
    g_override_enabled = true;
    CHECK(item_facts->cumulate_count(reinterpret_cast<void*>(1), &count).ok == kQolOk);
    CHECK(count == 23);
    g_override_available = false;
    g_override_enabled = false;
    g_canonical_count = 3;
    descriptor.on_enabled(ctx);
    CHECK(qol::host::http_dispatch_register_queries(descriptor.http_queries,
                                                     descriptor.http_query_count).ok == kQolOk);
    CHECK(g_scheduler.handler != nullptr);
    CHECK(g_scheduler.interval > 1);
    test_write_routes();

    uint16_t status = 0;
    (void)dispatch_get("/api/item/inventory", &status);
    CHECK(status == 503);

    CHECK(g_scheduler.handler(100, g_scheduler.ctx) != 0);

    const std::string expected_inventory = expected_inventory_with_count(3);
    CHECK(dispatch_get("/api/item/inventory", &status) == expected_inventory);
    CHECK(status == 200);

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
        CHECK(g_scheduler.handler(200 + test_case.canonical, g_scheduler.ctx) != 0);
        CHECK(dispatch_get("/api/item/inventory", &status) ==
              expected_inventory_with_count(test_case.disabled_view));
        CHECK(status == 200);
        g_override_enabled = true;
        CHECK(g_scheduler.handler(1200 + test_case.canonical, g_scheduler.ctx) != 0);
        CHECK(dispatch_get("/api/item/inventory", &status) ==
              expected_inventory_with_count(test_case.enabled_view));
        CHECK(status == 200);
        if (test_case.canonical == 1023) {
            int32_t canonical = 0;
            CHECK(item_facts->cumulate_count(reinterpret_cast<void*>(1), &canonical).ok == kQolOk);
            CHECK(canonical == 1023);
        }
    }
    g_count_encoded = false;
    g_override_enabled = true;
    CHECK(g_scheduler.handler(2300, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/item/inventory", &status) == expected_inventory_with_count(1));
    CHECK(status == 200);

    // Capability loss and effective-count failure both fall back to the original getter.
    g_count_encoded = true;
    g_override_available = false;
    g_canonical_count = 900;
    g_original_count = 71;
    CHECK(g_scheduler.handler(2400, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/item/inventory", &status) == expected_inventory_with_count(71));
    CHECK(status == 200);
    g_override_available = true;
    g_effective_fail = true;
    g_original_count = 100;
    CHECK(g_scheduler.handler(2500, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/item/inventory", &status) == expected_inventory_with_count(100));
    CHECK(status == 200);
    g_effective_fail = false;
    g_override_available = false;
    g_override_enabled = false;
    g_canonical_count = 3;
    g_original_count = 3;
    CHECK(g_scheduler.handler(2600, g_scheduler.ctx) != 0);
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

    // 非 world：发布与迁移前一致的 not in game 错误体（HTTP 200）。
    g_in_world = false;
    CHECK(g_scheduler.handler(115, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/item/inventory", &status) == "{\"error\":\"not in game\"}");
    CHECK(status == 200);
    g_in_world = true;
    CHECK(g_scheduler.handler(130, g_scheduler.ctx) != 0);
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
    register_frozen_routes();

    const qol::contracts::QolHostSdk sdk = make_sdk();
    qol::contracts::PluginContext ctx = make_context(&sdk);
    const auto& descriptor = qol::features::core_item::plugin_descriptor();
    CHECK(descriptor.install(ctx) == 0);
    descriptor.on_enabled(ctx);
    CHECK(qol::host::http_dispatch_register_queries(descriptor.http_queries,
                                                     descriptor.http_query_count).ok == kQolOk);
    // 静态名称表未下发：快照不发布，HTTP 侧保持 not ready。
    CHECK(g_scheduler.handler(1, g_scheduler.ctx) != 0);
    uint16_t status = 0;
    (void)dispatch_get("/api/item/inventory", &status);
    CHECK(status == 503);
    g_static_ready = 1;
    CHECK(g_scheduler.handler(20, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/item/inventory", &status).find("\"bags\"") != std::string::npos);
    CHECK(status == 200);
    descriptor.deactivate(ctx);
}

}  // namespace

int main() {
    test_lifecycle_and_responses();
    test_static_not_ready_holds_snapshot();
    std::printf("item_facts_dispatch: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

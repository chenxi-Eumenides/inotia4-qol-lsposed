// item_facts HTTP dispatch 接缝 host 测试：
// 覆盖 6 条 /api/item/* GET query handler 的注册、快照发布前的 not-ready、发布后的富化响应
// （字段顺序与迁移前 Kotlin restructureItem 一致）、非 world 错误体、owner 注销墓碑。
//
// 真实 registry/dispatch（host/http_registry.cpp、host/http_dispatch.cpp）参与链接，
// 游戏侧只以 fake QolItemPort 注入，避免依赖真机内存。

#include "contracts/game_ports/ports.h"
#include "contracts/game_ports/item_port.h"
#include "contracts/game_ports/item_write_port.h"
#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "contracts/result.h"
#include "host/http_dispatch.h"
#include "host/http_registry.h"
#include "plugins/core.item/plugin.h"

#include <cstdint>
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
    "\"capacity\":16,\"slot_count\":1}]}";

constexpr char kShopJson[] =
    "{\"items\":[{\"slot\":0,\"category\":10,\"count\":5,\"price\":15}]}";

constexpr char kExpectedItem[] =
    "{\"slot\":0,\"name\":\"再生药水（小）\",\"category\":10,\"count\":3,"
    "\"item_type\":\"potion\",\"need_level\":0,\"rarity\":0,\"rarity_tier\":\"白\","
    "\"base\":{\"damage\":0,\"defense\":0,\"magic_rate\":0},\"bonus\":[],"
    "\"gem\":{\"total_slots\":0,\"slots\":[]},"
    "\"chaos\":{\"is_chaos\":false,\"level\":0,\"rate\":100},"
    "\"enchant\":{\"id\":0,\"level\":0,\"effect\":null}}";

bool g_in_world = true;
int32_t g_static_ready = 1;
qol::contracts::QolTypedResultCommandHandler g_result_handler = nullptr;
uint32_t g_post_handler_id = 0;
uint8_t g_post_payload[qol::contracts::kTypedCommandPayloadCapacity]{};
uint32_t g_post_payload_size = 0;
QolItemWriteArgs g_last_write{};
int g_write_calls = 0;

QolResult unsupported() { return qol::contracts::error(kQolErrUnsupported, "stub", 4); }
QolResult stub_category(const void*, int32_t*) { return unsupported(); }
QolResult stub_is_real_equip(const void*, int32_t*) { return unsupported(); }
QolResult stub_is_jewel(int32_t, int32_t*) { return unsupported(); }
QolResult stub_no_sell(int32_t, int32_t*) { return unsupported(); }
QolResult stub_sell_price(const void*, int64_t*) { return unsupported(); }
QolResult stub_cumulate(const void*, int32_t*) { return unsupported(); }
QolResult stub_item_ref(int32_t, int32_t, qol::contracts::QolInventoryItemRef*) {
    return unsupported();
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

const QolItemPortReadApi kFakeRead{
    &stub_category,   &stub_is_real_equip, &stub_is_jewel,   &stub_no_sell,
    &stub_sell_price, &stub_cumulate,      &stub_item_ref,   &stub_physical,
    &stub_detail,     &stub_cursor,        &fake_inventory,  &fake_money,
    &fake_shop,       &fake_in_world,      &fake_static_text, &fake_static_ready,
};

const QolItemPort kFakePort{qol::contracts::kQolPortItemVersion, kFakeRead};

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

const void* const kPortArray[] = {&kFakePort, &kFakeWritePort};

struct FakeScheduler {
    qol::contracts::QolFrameHandler handler = nullptr;
    void* ctx = nullptr;
    int32_t interval = 0;
};

FakeScheduler g_scheduler;

QolResult fake_log_write(int32_t, int32_t, const char*, const char*, uint32_t) {
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
    ctx.port_count = 2;
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

void test_write_routes() {
    const auto& descriptor = qol::features::core_item::plugin_descriptor();
    qol::host::http_dispatch_set_op_enabled(true);
    uint16_t status = 0;
    CHECK(dispatch_post("/api/item/inventory/use_item",
                        "{\"bag\":8,\"bag\":\"0\",\"slot\":0,\"slot\":\"1\"}", nullptr,
                        0, &status) == "{\"ok\":true,\"ticket\":72}");
    CHECK(status == 200 && g_post_payload_size > 0 && g_result_handler != nullptr);
    CHECK(descriptor.http_query_count == 19);
    char result[256]{};
    uint32_t result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(std::string(result, result_size) == "{\"ok\":true}");
    CHECK(g_last_write.kind == static_cast<int32_t>(qol::contracts::QolItemWriteKind::kUseItem));
    CHECK(g_last_write.i0 == 0 && g_last_write.i1 == 1);

    CHECK(dispatch_post("/api/item/inventory/move_item",
                        "{\"bag\":\"6\",\"slot\":\"2\",\"count\":\"4\","
                        "\"to_bag\":\"7\",\"to_slot\":\"-1\"}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":72}");
    result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(g_last_write.kind == static_cast<int32_t>(qol::contracts::QolItemWriteKind::kMoveItem));
    CHECK(g_last_write.i0 == 6 && g_last_write.i1 == 2 && g_last_write.i2 == 4 &&
          g_last_write.i3 == 7 && g_last_write.i4 == -1);

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
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result, sizeof(result),
                           &result_size));
    CHECK(status == 200 && g_last_write.i64 == 4294967296LL && g_write_calls == 8);

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

    const std::string expected_inventory =
        std::string("{\"bags\":[{\"bag\":0,\"items\":[") + kExpectedItem +
        "],\"capacity\":16,\"slot_count\":1}]}";
    CHECK(dispatch_get("/api/item/inventory", &status) == expected_inventory);
    CHECK(status == 200);
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
    (void)dispatch_template("/api/item/inventory/bag/5/info", &missing_bag, 1, &status);
    CHECK(status == 404);

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

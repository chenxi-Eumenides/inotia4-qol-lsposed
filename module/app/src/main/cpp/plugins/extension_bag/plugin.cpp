#include "plugins/extension_bag/plugin.h"

#include <cstdint>
#include <cstring>

#include "contracts/events.h"
#include "contracts/craft_facts.h"
#include "contracts/http.h"
#include "contracts/inventory_refresh.h"
#include "contracts/world_facts.h"
#include "contracts/item_facts.h"
#include "contracts/save_facts.h"
#include "plugins/extension_bag/extension_bag_bridge.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/move_merge.h"

#include <cstdlib>
#include <string>

// ===========================================================================
// 扩展背包插件：29 条声明式资源的 owner + extension_bag v2.0 provider + move_merge 门禁
// ===========================================================================
//
// 资源面（a3b-1/a3b-2）：入口 hook / PopupState 占用 / desc 槽命名空间 / move_merge 指针槽
// 与 C1–C21 call patch 全部声明式化；wrapper 业务体逐字留在 plugins/extension_bag
// （D2c-2b-2 后 feature/patch 已删除），本文件只做声明与门禁接线。
//
// 能力面（a3c）：provides `extension_bag` v2.0（contracts/extension_bag.h 的 ExtensionBagApi），
// iface 由 extension_bag_api_table() 提供（EB TU 逐条转发既有 port）。宿主在 enabled 时注册、
// revoking 时注销（§3.4.4 既有路径）；本文件不新增 JNI / 资源。
//
// move_merge 门禁（D2c-2b-2）：nativeSetMoveMergeEnabled → kCommandSetConfigBool
// ("moveMergeEnabled") → 本插件订阅 kEventConfigChanged 应用 set_move_merge_enabled
// （extension source protection 联动由 set_virtual_bag_enabled 逐字保持）。
//
// M1/M2 校正（D2a-3b-1）：ADR-0007 表中 M1/M2 记为 required × failClosed；
// 按晚就绪语义校正为 required × failSoft —— 装配期失败时资源保持 free、插件
// degraded（error=资源 id），gate 开且 free 时宿主每逻辑帧重试到成功；语义等价
// 旧的 300ms 线程重试，且状态可观测。ADR-0007 与 ledger 已同步该一条校正。

namespace qol::features::extension_bag {
namespace {

constexpr const char* kPluginId = "extension_bag";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kMoveMergeGateKey = "moveMergeEnabled";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::QolExtensionBagWritePort* g_write_port = nullptr;
const contracts::WorldFactsApi* g_world_facts = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;
const contracts::SaveFactsApi* g_save_facts = nullptr;
const contracts::CraftFactsApi* g_craft_facts = nullptr;

// moveMergeEnabled 配置变更订阅句柄；deactivate 时成对注销（install 幂等重入）。
uint64_t g_config_subscription = 0;

void on_config_changed(void* ctx, const void* payload) {
    (void)ctx;
    if (payload == nullptr) return;
    const auto* event = static_cast<const qol::contracts::ConfigChangedEvent*>(payload);
    if (event->key == nullptr || std::strcmp(event->key, kMoveMergeGateKey) != 0) return;
    set_move_merge_enabled(event->int_value != 0);
}

// 资源表在 extension_bag_claims() 首次调用时构造（函数内静态，无跨 TU 静态初始化顺序问题）。
uint32_t g_resource_count = 0;
const qol::contracts::ResourceClaim* g_resources = extension_bag_claims(&g_resource_count);

// 能力提供声明：extension_bag v2.2，iface = EB TU 的 ExtensionBagApi 静态表。
// 注册失败（iface 空/容量满/重复）由宿主只记录不阻断（plugin_registry 既有口径）。
const qol::contracts::CapabilityProvisionRef kProvides[] = {
    {
        qol::contracts::kExtensionBagCapabilityId,
        {static_cast<uint16_t>(qol::contracts::kExtensionBagVersionMajor),
         static_cast<uint16_t>(qol::contracts::kExtensionBagVersionMinor)},
        extension_bag_api_table(),
    },
};

const qol::contracts::CapabilityRequirement kRequires[] = {
    {
        qol::contracts::kInventoryRefreshCapabilityId,
        {qol::contracts::kInventoryRefreshVersionMajor,
         qol::contracts::kInventoryRefreshVersionMinor},
    },
    {
        qol::contracts::kWorldFactsCapabilityId,
        {qol::contracts::kWorldFactsVersionMajor, qol::contracts::kWorldFactsVersionMinor},
    },
    {
        qol::contracts::kItemFactsCapabilityId,
        {qol::contracts::kItemFactsVersionMajor, qol::contracts::kItemFactsVersionMinor},
    },
    {
        qol::contracts::kSaveFactsCapabilityId,
        {qol::contracts::kSaveFactsVersionMajor, qol::contracts::kSaveFactsVersionMinor},
    },
    {
        qol::contracts::kCraftFactsCapabilityId,
        {qol::contracts::kCraftFactsVersionMajor, qol::contracts::kCraftFactsVersionMinor},
    },
};

const qol::contracts::QolPortRequirement kPorts[] = {
    {qol::contracts::QolPortId::QOL_PORT_EXTENSION_BAG_WRITE,
     {qol::contracts::kQolPortExtensionBagWriteVersion.major,
      qol::contracts::kQolPortExtensionBagWriteVersion.minor}},
    {qol::contracts::QolPortId::QOL_PORT_CHARACTER_EXTRAS,
     {qol::contracts::kQolPortCharacterExtrasVersion.major,
      qol::contracts::kQolPortCharacterExtrasVersion.minor}},
    {qol::contracts::QolPortId::QOL_PORT_INVENTORY_EXTRAS,
     {qol::contracts::kQolPortInventoryExtrasVersion.major,
      qol::contracts::kQolPortInventoryExtrasVersion.minor}},
};

// ---- HTTP command handlers（/api/debug/extension_bag/* POST，owner=extension_bag）----
// 自注册模式（core.api lifecycle 先例）：plugin_install 成功尾部逐条 register_command，
// 停用由宿主 unregister_http_queries 摘除置墓碑（503 http owner inactive）。
// body 校验逐字对齐迁移前 Kotlin DebugController 的 ApiException 文本与状态码。

enum class EbCommand : uint8_t {
    kEnterView,
    kExitView,
    kUnequip,
    kSelectBag,
    kClickItem,
    kEquip,
    kItem,
};

struct EbCommandRoute {
    const char* path;
    EbCommand kind;
};

constexpr EbCommandRoute kEbCommandRoutes[] = {
    {"/api/debug/extension_bag/enter_view", EbCommand::kEnterView},
    {"/api/debug/extension_bag/exit_view", EbCommand::kExitView},
    {"/api/debug/extension_bag/unequip", EbCommand::kUnequip},
    {"/api/debug/extension_bag/select_bag", EbCommand::kSelectBag},
    {"/api/debug/extension_bag/click_item", EbCommand::kClickItem},
    {"/api/debug/extension_bag/equip", EbCommand::kEquip},
    {"/api/debug/extension_bag/item", EbCommand::kItem},
};

// 从 JSON 对象文本提取整数字段。客户端 body 可能带空白：冒号后跳过空白再读
// 可选负号与数字；键不存在或值非整数标量返回 false（调用方按 Kotlin optInt 缺省 -1 处理）。
// debug 端点 body 只含数字字段，不做字符串值内键样子串的误匹配防护（与 core.api
// json_field 同级取舍）。
bool extract_int_field(const char* body, const char* key, int* out) {
    const std::string pattern = std::string("\"") + key + "\":";
    const char* p = std::strstr(body, pattern.c_str());
    if (p == nullptr) return false;
    p += pattern.size();
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    char* end = nullptr;
    const long value = std::strtol(p, &end, 10);
    if (end == p) return false;
    *out = static_cast<int>(value);
    return true;
}

QolResult eb_command_error(contracts::HttpRes* response, uint16_t status, const char* message) {
    const std::string body =
        std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
    if (response == nullptr || response->body == nullptr ||
        response->body_capacity < body.size()) {
        return contracts::error(kQolErrInternal, "http response buffer too small", 31);
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = status;
    return contracts::error(status == 400 ? kQolErrInvalidArgument : kQolErrInternal, message,
                            static_cast<uint32_t>(std::strlen(message)));
}

QolResult eb_command_body(contracts::HttpRes* response, const std::string& body) {
    if (response == nullptr || response->body == nullptr ||
        response->body_capacity < body.size()) {
        return eb_command_error(response, 500, "http response buffer too small");
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = 200;
    return contracts::ok();
}

// bag 合法域 [6,10] 与迁移前 Kotlin DebugController 一致；未提供同样拒绝。
bool eb_bag_valid(const char* body, int* out_bag, contracts::HttpRes* response) {
    int bag = -1;
    if (!extract_int_field(body, "bag", &bag)) bag = -1;
    if (bag < 6 || bag > 10) {
        eb_command_error(response, 400, "bag required (6-10)");
        return false;
    }
    *out_bag = bag;
    return true;
}

// 游戏写路由的命令化：HTTP 线程只校验并投递票据，DB 写在主线程结果处理器中执行
// （ARCHITECTURE.md §7.1/§8.1）。结果处理器把迁移前的原响应 JSON 写入票据缓冲。
constexpr uint32_t kEbResultHandlerId = 0x45420001;  // 全局 typed handler id 空间，EB 前缀

struct EbCommandPayload {
    int32_t kind = 0;
    int32_t a = -1;
    int32_t b = -1;
    int32_t c = -1;
    int32_t d = -1;
};

bool eb_result_handler(void*, const uint8_t* payload, uint32_t size, char* out, uint32_t capacity,
                       uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (payload == nullptr || size != sizeof(EbCommandPayload) || out == nullptr ||
        out_size == nullptr) {
        return false;
    }
    EbCommandPayload p{};
    std::memcpy(&p, payload, size);
    std::string body;
    switch (static_cast<EbCommand>(p.kind)) {
        case EbCommand::kEnterView:
            body = data_op_extension_bag_enter_view(p.a);
            break;
        case EbCommand::kExitView:
            body = data_op_extension_bag_exit_view();
            break;
        case EbCommand::kUnequip:
            body = data_op_extension_bag_unequip(p.a);
            break;
        case EbCommand::kSelectBag:
            body = data_op_extension_bag_select_bag(p.a);
            break;
        case EbCommand::kClickItem:
            body = data_op_extension_bag_click_item(p.a, p.b);
            break;
        case EbCommand::kEquip:
            body = data_virtual_bag_test_equip(p.a, p.b);
            break;
        case EbCommand::kItem:
            body = data_virtual_bag_test_item(p.a, p.b, p.c, p.d);
            break;
        default:
            return false;
    }
    if (body.size() > capacity) return false;
    std::memcpy(out, body.data(), body.size());
    *out_size = static_cast<uint32_t>(body.size());
    // 业务失败（原响应体含 "ok":false）→ 票据置 failed，结果仍携带原错误体。
    return body.find("\"ok\":false") == std::string::npos;
}

QolResult eb_command_handler(const contracts::HttpReq* request, contracts::HttpRes* response,
                             void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return eb_command_error(response, 400, "bad request");
    }
    const EbCommand kind = *static_cast<const EbCommand*>(ctx);
    // HttpReq.body 无 null 结尾契约：拷贝到 std::string（自带结尾）再做字段提取。
    const std::string body_text(request->body != nullptr
                                    ? reinterpret_cast<const char*>(request->body)
                                    : "",
                                request->body_len);
    const char* body = body_text.c_str();
    EbCommandPayload payload{};
    payload.kind = static_cast<int32_t>(kind);
    int bag = -1;
    int slot = -1;
    switch (kind) {
        case EbCommand::kEnterView:
            if (!eb_bag_valid(body, &bag, response)) return contracts::ok();
            payload.a = bag;
            break;
        case EbCommand::kExitView:
            break;
        case EbCommand::kUnequip:
            if (!eb_bag_valid(body, &bag, response)) return contracts::ok();
            payload.a = bag;
            break;
        case EbCommand::kSelectBag:
            if (!eb_bag_valid(body, &bag, response)) return contracts::ok();
            payload.a = bag;
            break;
        case EbCommand::kClickItem: {
            if (!eb_bag_valid(body, &bag, response)) return contracts::ok();
            if (!extract_int_field(body, "slot", &slot)) slot = -1;
            if (slot < 0 || slot > 15) {
                return eb_command_error(response, 400, "slot required (0-15)");
            }
            payload.a = bag;
            payload.b = slot;
            break;
        }
        case EbCommand::kEquip: {
            int index = -1;
            int bag_type = -1;
            (void)extract_int_field(body, "index", &index);
            (void)extract_int_field(body, "bagType", &bag_type);
            payload.a = index;
            payload.b = bag_type;
            break;
        }
        case EbCommand::kItem: {
            int index = -1;
            int item_slot = -1;
            int category = -1;
            int count = -1;
            (void)extract_int_field(body, "index", &index);
            (void)extract_int_field(body, "slot", &item_slot);
            (void)extract_int_field(body, "category", &category);
            (void)extract_int_field(body, "count", &count);
            payload.a = index;
            payload.b = item_slot;
            payload.c = category;
            payload.d = count;
            break;
        }
        default:
            return eb_command_error(response, 500, "unknown extension bag command");
    }
    // 游戏写只能在主线程 owner command 执行（ARCHITECTURE.md §7.1）：HTTP 线程只投递票据。
    if (g_sdk == nullptr || g_sdk->commands.post_typed == nullptr) {
        return eb_command_error(response, 503, "command port unavailable");
    }
    contracts::QolCommandTicket ticket = 0;
    if (!contracts::succeeded(g_sdk->commands.post_typed(kEbResultHandlerId, &payload,
                                                        sizeof(payload), &ticket)) ||
        ticket == 0) {
        return eb_command_error(response, 503, "command queue unavailable");
    }
    return eb_command_body(response, std::string("{\"ok\":true,\"ticket\":") +
                                        std::to_string(ticket) + "}");
}

bool register_extension_bag_commands(const qol::contracts::QolHostSdk* sdk) {
    if (sdk == nullptr || sdk->http_dispatch.register_command == nullptr) return false;
    // 结果处理器需要 host 尾部的票据结果面（QolHostSdk 尾部追加，按 abi 协商）。
    if (sdk->abi.size < sizeof(qol::contracts::QolHostSdk) || sdk->abi.minor < 8 ||
        sdk->ticket_result.register_typed_result_handler == nullptr) {
        return false;
    }
    if (!contracts::succeeded(sdk->ticket_result.register_typed_result_handler(
            kEbResultHandlerId, &eb_result_handler, nullptr))) {
        return false;
    }
    for (const EbCommandRoute& route : kEbCommandRoutes) {
        const QolResult result = sdk->http_dispatch.register_command(
            contracts::HttpMethod::kPost, route.path, kPluginId, &eb_command_handler,
            const_cast<EbCommand*>(&route.kind));
        if (!contracts::succeeded(result)) {
            if (sdk->ticket_result.unregister_typed_result_handler != nullptr) {
                (void)sdk->ticket_result.unregister_typed_result_handler(kEbResultHandlerId);
            }
            if (sdk->http_dispatch.unregister_owner != nullptr) {
                (void)sdk->http_dispatch.unregister_owner(kPluginId);
            }
            return false;
        }
    }
    return true;
}

int plugin_install(qol::contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
    g_world_facts = nullptr;
    g_item_facts = nullptr;
    g_save_facts = nullptr;
    g_craft_facts = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
    if (ctx.struct_size < sizeof(qol::contracts::PluginContext) ||
        ctx.sdk_major != qol::contracts::kHostSdkMajor ||
        ctx.sdk == nullptr || ctx.game == nullptr ||
        ctx.sdk->config.get_bool == nullptr || ctx.sdk->events.subscribe == nullptr ||
        ctx.sdk->events.unsubscribe == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        return 1;
    }
    if (ctx.ports == nullptr || ctx.port_count != 3 || ctx.ports[0] == nullptr ||
        ctx.ports[1] == nullptr || ctx.ports[2] == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        return 1;
    }
    g_write_port = static_cast<const qol::contracts::QolExtensionBagWritePort*>(ctx.ports[0]);
    extension_bag_bind_character_extras(
        static_cast<const qol::contracts::QolCharacterExtrasPort*>(ctx.ports[1]));
    extension_bag_bind_inventory_extras(
        static_cast<const qol::contracts::QolInventoryExtrasPort*>(ctx.ports[2]));
    const void* world_facts_iface = nullptr;
    if (ctx.sdk->capabilities.require == nullptr ||
        !qol::contracts::succeeded(ctx.sdk->capabilities.require(
            qol::contracts::kWorldFactsCapabilityId,
            {qol::contracts::kWorldFactsVersionMajor, qol::contracts::kWorldFactsVersionMinor},
            &world_facts_iface)) ||
        world_facts_iface == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        return 1;
    }
    g_world_facts = static_cast<const qol::contracts::WorldFactsApi*>(world_facts_iface);
    const void* item_facts_iface = nullptr;
    if (!qol::contracts::succeeded(ctx.sdk->capabilities.require(
            qol::contracts::kItemFactsCapabilityId,
            {qol::contracts::kItemFactsVersionMajor, qol::contracts::kItemFactsVersionMinor},
            &item_facts_iface)) ||
        item_facts_iface == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        g_world_facts = nullptr;
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        return 1;
    }
    g_item_facts = static_cast<const qol::contracts::ItemFactsApi*>(item_facts_iface);
    const void* save_facts_iface = nullptr;
    if (!qol::contracts::succeeded(ctx.sdk->capabilities.require(
            qol::contracts::kSaveFactsCapabilityId,
            {qol::contracts::kSaveFactsVersionMajor, qol::contracts::kSaveFactsVersionMinor},
            &save_facts_iface)) ||
        save_facts_iface == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        g_world_facts = nullptr;
        g_item_facts = nullptr;
        g_save_facts = nullptr;
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        return 1;
    }
    g_save_facts = static_cast<const qol::contracts::SaveFactsApi*>(save_facts_iface);
    const void* craft_facts_iface = nullptr;
    if (!qol::contracts::succeeded(ctx.sdk->capabilities.require(
            qol::contracts::kCraftFactsCapabilityId,
            {qol::contracts::kCraftFactsVersionMajor, qol::contracts::kCraftFactsVersionMinor},
            &craft_facts_iface)) ||
        craft_facts_iface == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        g_world_facts = nullptr;
        g_item_facts = nullptr;
        g_save_facts = nullptr;
        g_craft_facts = nullptr;
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        return 1;
    }
    g_craft_facts = static_cast<const qol::contracts::CraftFactsApi*>(craft_facts_iface);
    extension_bag_bind_item_facts(g_item_facts);
    extension_bag_bind_craft_facts(g_craft_facts);
    if (!extension_bag_bind_host(g_sdk, g_game, g_write_port)) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        return 1;
    }
    extension_bag_bind_port_sdk(g_sdk);
    // 自身初始化：资源由宿主声明式安装；gate 键 extensionBagEnabled 由
    // nativeSetExtensionBagEnabled 经命令端口写入。moveMergeEnabled 读初始值并订阅
    // 配置变更（inactive→resolving 幂等重入：已订阅不重复订阅）。
    int32_t move_merge_enabled = 0;
    (void)g_sdk->config.get_bool(kMoveMergeGateKey, 0, &move_merge_enabled);
    set_move_merge_enabled(move_merge_enabled != 0);
    if (g_config_subscription == 0) {
        (void)g_sdk->events.subscribe(qol::contracts::kEventConfigChanged, &on_config_changed,
                                      nullptr, &g_config_subscription);
    }
    // HTTP command 注册在全部绑定就绪后执行；失败即 install 失败（宿主不启用、资源不装）。
    if (!register_extension_bag_commands(g_sdk)) {
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
        return 1;
    }
    return 0;
}

void plugin_deactivate(qol::contracts::PluginContext& ctx) {
    // 自注册的 command handler 不在 descriptor（http_query_count=0），宿主的
    // unregister_http_queries 会早退；deactivate 必须显式摘除，否则 activate 的
    // 重注册走 duplicate 回滚会把整组槽置墓碑。幂等：宿主摘除后再调返回 not found。
    if (g_sdk != nullptr && g_sdk->http_dispatch.unregister_owner != nullptr) {
        (void)g_sdk->http_dispatch.unregister_owner(kPluginId);
    }
    // 结果处理器同样需要显式注销（同一自注册理由）。
    if (g_sdk != nullptr && g_sdk->ticket_result.unregister_typed_result_handler != nullptr) {
        (void)g_sdk->ticket_result.unregister_typed_result_handler(kEbResultHandlerId);
    }
    (void)ctx;
    // 幂等：退订后置 0，便于再次 install 重新订阅；move_merge 请求标志保持（与旧
    // 独立 JNI 开关语义一致，deactivate 不重置），资源归还包括 PopupState 回写与
    // X1 槽还原（宿主引擎执行）；本回调只关业务启用标志。
    if (g_config_subscription != 0 && g_sdk != nullptr &&
        g_sdk->events.unsubscribe != nullptr) {
        (void)g_sdk->events.unsubscribe(g_config_subscription);
        g_config_subscription = 0;
    }
    extension_bag_set_enabled(false);
    g_world_facts = nullptr;
    g_item_facts = nullptr;
    g_save_facts = nullptr;
    g_craft_facts = nullptr;
    extension_bag_bind_item_facts(nullptr);
    extension_bag_bind_craft_facts(nullptr);
    g_game = nullptr;
    g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
    g_sdk = nullptr;
}

// 启停仅切门禁，资源安装/还原由宿主完成（§3.3）。
void plugin_on_enabled(qol::contracts::PluginContext& ctx) {
    (void)ctx;
    extension_bag_set_enabled(true);
}

void plugin_on_disabled(qol::contracts::PluginContext& ctx) {
    (void)ctx;
    extension_bag_set_enabled(false);
}

void plugin_fill_status(qol::contracts::PluginStatusOut& out) {
    out.enabled = qol::features::extension_bag::extension_bag_enabled();
    out.error = nullptr;
}

int32_t plugin_on_resource_ready(qol::contracts::PluginContext& ctx, const char* resource_id,
                                 void* backup) {
    (void)ctx;
    // 受限回调（§3.3）：只缓存入口 backup（常数时间），无资源操作。
    return extension_bag_bind_backup(resource_id, backup);
}

const qol::contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    4, kRequires,  // requires=inventory_refresh + world.facts + item.facts + save.facts
    0, nullptr,   // optional_requires
    1, kProvides,  // provides（extension_bag v2.0）
    g_resource_count, g_resources,  // resources（29 条）
    0, nullptr,   // jni（JNI 入口由 bridge/native 保留，名/签名冻结）
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &plugin_on_resource_ready,
    kPorts,
    3,
};

}  // namespace

const qol::contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

const qol::contracts::WorldFactsApi* world_facts() {
    return g_world_facts;
}

const qol::contracts::SaveFactsApi* save_facts() { return g_save_facts; }

}  // namespace qol::features::extension_bag

#pragma once

#include <cstdint>
#include <string>

#include "contracts/http.h"
#include "contracts/extension_bag.h"
#include "contracts/item_facts.h"
#include "contracts/item_state.h"
#include "contracts/plugin.h"
#include "contracts/game_ports/item_write_port.h"
#include "plugins/core.item/usecases/sell_item.h"
#include "plugins/core.item/runtime/context.h"

namespace qol::features::core_item {

namespace inventory_hooks = ::qol::features::inventory_hooks;

struct InventoryExtensionAccessResult {
    const contracts::QolInventoryExtensionProvider* provider = nullptr;
    bool available = false;
};

QolResult error_result(int32_t code, const char* message);
QolResult not_ready();
bool copy_buffer(char* destination, uint32_t capacity, uint32_t* destination_size,
                 const char* source, uint32_t source_size);
bool effective_item_count(contracts::QolItemHandle item, bool original_count_supported,
                          uint32_t* out_count, bool* out_present);
bool inventory_option_counts_valid(const contracts::QolItemInventoryView& item);
void append_inventory_item(std::string* out, int32_t slot,
                           const contracts::QolItemInventoryView& item, bool count_present,
                           uint32_t count);
InventoryExtensionAccessResult inventory_extension();

extern const contracts::ItemFactsApi kItemFactsApi;
extern const contracts::ItemStateApi kItemStateApi;
const contracts::ItemFactsApi* item_facts_api();
QolResult money(int64_t* out_money);
QolResult write_item_state_json(int32_t kind, char* out, uint32_t capacity, uint32_t* out_size);
int32_t frame_tick(int64_t, void*);

QolResult response_error(contracts::HttpRes* response, uint16_t status, const char* message);
QolResult response_body(contracts::HttpRes* response, const char* body, uint32_t size,
                        uint16_t status = 200);

enum class Query : uint8_t {
    kInventory,
    kMoney,
    kItems,
    kBagInfo,
    kBagSlot,
    kShopItems,
};

extern Query kQueries[];
QolResult query_handler(const contracts::HttpReq* request, contracts::HttpRes* response, void* ctx);

struct ItemWriteRoute {
    const char* path;
    contracts::QolItemWriteKind kind;
};

inline constexpr uint32_t kItemWriteRouteCount = 13;
extern ItemWriteRoute kItemWriteRoutes[kItemWriteRouteCount];
QolResult item_write_command(const contracts::HttpReq* request, contracts::HttpRes* response,
                             void* ctx);
bool item_write_result(void*, const uint8_t* payload, uint32_t size, char* out,
                       uint32_t capacity, uint32_t* out_size);

const contracts::ResourceClaim* combined_resources(uint32_t* out_count);
void initialize_http_endpoints();
extern contracts::HttpEndpointDescriptor kHttpEndpoints[];
extern const uint32_t kHttpEndpointCount;
extern const contracts::HttpRouteMetadataDescriptor kHttpRouteMetadata[];
extern const uint32_t kHttpRouteMetadataCount;
extern const contracts::FeatureDefinition kFeatures[];

void stop_frame_task(const contracts::QolHostSdk* sdk);
extern uint32_t g_degrade_warn_ticks;
extern const uint32_t kItemWriteHandlerId;
int plugin_install(contracts::PluginContext& ctx);
void plugin_deactivate(contracts::PluginContext& ctx);
void plugin_on_enabled(contracts::PluginContext& ctx);
void plugin_on_disabled(contracts::PluginContext& ctx);
void plugin_fill_status(contracts::PluginStatusOut& out);
int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup);

}  // namespace qol::features::core_item

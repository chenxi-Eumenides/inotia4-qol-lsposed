#include "plugins/core.item/runtime/core_item_internal.h"

#include <cstring>

#include "contracts/commands.h"
#include "plugins/core.item/bags/snapshot_store.h"
#include "plugins/core.item/bags/native_bag_limit.h"
#include "plugins/core.item/hooks/inventory_hooks.h"
#include "plugins/core.item/shop/snapshot.h"
#include "plugins/core.item/usecases/sell_item.h"

namespace qol::features::core_item {
namespace {

constexpr int32_t kSnapshotFallbackIntervalFrames = 60;
uint64_t g_frame_task = 0;

void log_message(const contracts::QolHostSdk* sdk, const char* message) {
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)sdk->log.write(1, 1, kPluginId, message, length);
}

// 物理扫描上界 = 原版袋数 + C18 探测到的追加袋数，最终夹到 item port 布局容量。
void apply_native_bag_limit() {
    if (g_port == nullptr || g_port->read.bag_count == nullptr ||
        !contracts::qol_item_port_bag_space_size_valid(g_port)) {
        return;
    }
    int32_t limit = contracts::kQolOriginalBagCount;
    int32_t original = 0;
    if (contracts::succeeded(
            g_port->read.bag_count(contracts::kQolBagSpaceOriginal, &original)) &&
        original > 0) {
        limit = original;
    }
    int32_t c18 = 0;
    if (contracts::succeeded(g_port->read.bag_count(contracts::kQolBagSpaceVariant, &c18)) &&
        c18 > 0) {
        limit += c18;
    }
    bags::set_native_bag_limit(limit);
}

// Inventory claims；merge ptrslot 与 5 条装备页 UI hook 已迁往 extension_bag。
constexpr uint32_t kInventoryResourceCount = 11;
constexpr uint32_t kMaxResources = 32;
static_assert(kInventoryResourceCount <= kMaxResources,
              "core.item resource table capacity");
contracts::ResourceClaim g_resources[kMaxResources]{};

}  // namespace

extern const uint32_t kItemWriteHandlerId = 0x49540001;

const contracts::ResourceClaim* combined_resources(uint32_t* out_count) {
    uint32_t inventory_count = 0;
    const contracts::ResourceClaim* inventory =
        inventory_hooks::claims(&inventory_count);
    uint32_t count = 0;
    const auto append = [&](const contracts::ResourceClaim* claims, uint32_t size) {
        if ((claims == nullptr && size != 0) || size > kMaxResources - count) return false;
        for (uint32_t index = 0; index < size; ++index) {
            g_resources[count++] = claims[index];
        }
        return true;
    };
    // 资源数量固定；溢出属编程错误。返回空表让描述符不发布不完整集合。
    if (!append(inventory, inventory_count)) {
        if (out_count != nullptr) *out_count = 0;
        return nullptr;
    }
    if (out_count != nullptr) *out_count = count;
    return g_resources;
}

void stop_frame_task(const contracts::QolHostSdk* sdk) {
    if (g_frame_task != 0 && sdk != nullptr && sdk->scheduler.remove != nullptr) {
        (void)sdk->scheduler.remove(g_frame_task);
    }
    g_frame_task = 0;
}

int plugin_install(contracts::PluginContext& ctx) {
    reset_ports();
    reset_bag_sources();
    bags::inventory_snapshot_store().clear();
    g_sdk = ctx.sdk;
    g_frame_task = 0;
    if (ctx.struct_size < contracts::PLUGIN_CONTEXT_PORTS_STRUCT_SIZE ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || ctx.ports == nullptr ||
        ctx.port_count != 5 || ctx.ports[0] == nullptr || ctx.ports[1] == nullptr ||
        ctx.ports[2] == nullptr || ctx.ports[3] == nullptr || ctx.ports[4] == nullptr) {
        reset_ports();
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
        reset_ports();
        return 1;
    }
    g_item_money_write = static_cast<const contracts::QolItemMoneyWritePort*>(ctx.ports[1]);
    if (!contracts::satisfies(g_item_money_write->abi_version,
                              {contracts::kQolPortItemMoneyWriteVersion.major,
                               contracts::kQolPortItemMoneyWriteVersion.minor}) ||
        g_item_money_write->set_money == nullptr) {
        reset_ports();
        return 1;
    }
    g_item_dice_write = static_cast<const contracts::QolItemDiceWritePort*>(ctx.ports[2]);
    if (!contracts::satisfies(g_item_dice_write->abi_version,
                              {contracts::kQolPortItemDiceWriteVersion.major,
                               contracts::kQolPortItemDiceWriteVersion.minor}) ||
        g_item_dice_write->reject == nullptr || g_item_dice_write->accept == nullptr) {
        reset_ports();
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
        reset_ports();
        return 1;
    }
    // The v1.2 transfer callbacks and Item Port serialize_item are optional; transfer reports
    // Unsupported when a host does not provide them.
    // QOL_PORT_MOVE_MERGE 已迁往 extension_bag；character_extras 顺位前移到 ports[4]。
    g_character_extras_port =
        static_cast<const contracts::QolCharacterExtrasPort*>(ctx.ports[4]);
    if (!contracts::satisfies(g_character_extras_port->abi_version,
                              {contracts::kQolPortCharacterExtrasVersion.major,
                               contracts::kQolPortCharacterExtrasVersion.minor}) ||
        !contracts::qol_character_extras_read_api_size_valid(
            &g_character_extras_port->read) ||
        g_character_extras_port->read.can_apply_stuff_items == nullptr) {
        reset_ports();
        return 1;
    }
    if (!g_snapshot.initialize(g_sdk)) {
        reset_ports();
        return 1;
    }
    bind_bag_source_sdk(g_sdk);
    if (!shop::initialize_snapshot(g_sdk)) {
        g_snapshot.destroy();
        reset_ports();
        return 1;
    }
    ItemSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    // 结果处理器需要 host 尾部的票据结果面（QolHostSdk 尾部追加，按 abi 协商）。
    if (g_sdk->abi.size < sizeof(contracts::QolHostSdk) || g_sdk->abi.minor < 9 ||
        g_sdk->ticket_result.register_typed_result_handler == nullptr ||
        !contracts::succeeded(g_sdk->ticket_result.register_typed_result_handler(
            kItemWriteHandlerId, &item_write_result, nullptr))) {
        shop::destroy_snapshot();
        g_snapshot.destroy();
        reset_ports();
        return 1;
    }
    if (!inventory_hooks::attach(ctx.sdk, ctx.game, &kItemFactsApi, g_port)) {
        if (g_sdk->ticket_result.unregister_typed_result_handler != nullptr) {
            (void)g_sdk->ticket_result.unregister_typed_result_handler(kItemWriteHandlerId);
        }
        shop::destroy_snapshot();
        g_snapshot.destroy();
        reset_ports();
        return 1;
    }
    apply_native_bag_limit();
    log_message(g_sdk, "[core.item] provider install");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    inventory_hooks::detach();
    stop_frame_task(ctx.sdk);
    shop::destroy_snapshot();
    g_snapshot.destroy();
    bags::inventory_snapshot_store().clear();
    reset_bag_sources();
    log_message(ctx.sdk, "[core.item] provider deactivate");
    if (g_sdk != nullptr && g_sdk->ticket_result.unregister_typed_result_handler != nullptr) {
        (void)g_sdk->ticket_result.unregister_typed_result_handler(kItemWriteHandlerId);
    }
    reset_ports();
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    apply_native_bag_limit();
    if (!inventory_hooks::attach(ctx.sdk, ctx.game, &kItemFactsApi, g_port)) {
        log_message(ctx.sdk, "[core.item] inventory hooks attach unavailable");
        return;
    }
    if (!g_snapshot.initialize(g_sdk)) {
        inventory_hooks::detach();
        log_message(ctx.sdk, "[core.item] snapshot unavailable");
        return;
    }
    if (!shop::initialize_snapshot(g_sdk)) {
        g_snapshot.destroy();
        inventory_hooks::detach();
        log_message(ctx.sdk, "[core.item] snapshot unavailable");
        return;
    }
    ItemSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    bags::inventory_snapshot_store().clear();
    if (g_frame_task == 0 && g_sdk != nullptr && g_sdk->scheduler.add != nullptr) {
        uint64_t task = 0;
        if (contracts::succeeded(g_sdk->scheduler.add(
                1, &frame_tick, nullptr, kSnapshotFallbackIntervalFrames, 0, &task))) {
            g_frame_task = task;
        } else {
            log_message(ctx.sdk, "[core.item] snapshot scheduler unavailable");
        }
    }
    log_message(ctx.sdk, "[core.item] provider enabled: capability + http queries");
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    stop_frame_task(ctx.sdk);
    shop::destroy_snapshot();
    g_snapshot.destroy();
    bags::inventory_snapshot_store().clear();
    log_message(ctx.sdk, "[core.item] provider disabled: capability + http queries withdrawn");
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_port != nullptr;
    out.error = nullptr;
}

// Feature callback does not replace the existing lifecycle or business controls.
// move_merge.drag_merge 已迁往 extension_bag。
extern const contracts::FeatureDefinition kFeatures[] = {
    {"core.item.provider", 1, contracts::FeatureCategory::kHidden,
     contracts::FeatureType::kToggle, "物品领域服务", "",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id,
                          void* backup) {
    (void)ctx;
    return inventory_hooks::bind_backup(resource_id, backup);
}

}  // namespace qol::features::core_item

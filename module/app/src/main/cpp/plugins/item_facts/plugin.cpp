#include "plugin.h"

#include "contracts/item_facts.h"

namespace qol::features::item_facts {
namespace {

constexpr const char* kPluginId = "item_facts";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolGameHost* g_game = nullptr;

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, "item facts not ready", 20);
}

QolResult category(const void* item, int32_t* out_category) {
    if (g_game == nullptr || g_game->read.item.category == nullptr) return not_ready();
    return g_game->read.item.category(item, out_category);
}

QolResult is_real_equip(const void* item, int32_t* out_value) {
    if (g_game == nullptr || g_game->read_only.item.is_real_equip == nullptr) return not_ready();
    return g_game->read_only.item.is_real_equip(item, out_value);
}

QolResult is_jewel_category(int32_t category_value, int32_t* out_is_jewel) {
    if (g_game == nullptr || g_game->read_only.item.is_jewel_category == nullptr) {
        return not_ready();
    }
    return g_game->read_only.item.is_jewel_category(category_value, out_is_jewel);
}

QolResult is_no_sell(int32_t category_value, int32_t* out_value) {
    if (g_game == nullptr || g_game->read_only.item.is_no_sell == nullptr) return not_ready();
    return g_game->read_only.item.is_no_sell(category_value, out_value);
}

QolResult sell_price(const void* item, int64_t* out_price) {
    if (g_game == nullptr || g_game->read_only.item.sell_price == nullptr) return not_ready();
    return g_game->read_only.item.sell_price(item, out_price);
}

QolResult cumulate_count(const void* item, int32_t* out_count) {
    if (g_game == nullptr || g_game->read_only.item.cumulate_count == nullptr) return not_ready();
    return g_game->read_only.item.cumulate_count(item, out_count);
}

QolResult item_ref_at(int32_t bag, int32_t slot, contracts::QolInventoryItemRef* out) {
    if (g_game == nullptr || g_game->read_only.inventory.item_ref_at == nullptr) return not_ready();
    return g_game->read_only.inventory.item_ref_at(bag, slot, out);
}

QolResult physical_item_at(int32_t bag, int32_t slot, void** out_item) {
    if (g_game == nullptr || g_game->read_only.inventory.physical_item_at == nullptr) {
        return not_ready();
    }
    return g_game->read_only.inventory.physical_item_at(bag, slot, out_item);
}

QolResult detail_context(int32_t* out_desc_type, int32_t* out_bag, void** out_control) {
    if (g_game == nullptr || g_game->read_only.inventory.detail_context == nullptr) {
        return not_ready();
    }
    return g_game->read_only.inventory.detail_context(out_desc_type, out_bag, out_control);
}

QolResult cursor_index(const void* control, int32_t* out_slot) {
    if (g_game == nullptr || g_game->read_only.inventory.cursor_index == nullptr) {
        return not_ready();
    }
    return g_game->read_only.inventory.cursor_index(control, out_slot);
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

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || ctx.sdk == nullptr || ctx.game == nullptr) {
        g_game = nullptr;
        return 1;
    }
    g_game = ctx.game;
    if (ctx.sdk->log.write != nullptr) {
        constexpr const char kMessage[] = "[item_facts] provider install";
        (void)ctx.sdk->log.write(1, 1, kPluginId, kMessage, sizeof(kMessage) - 1);
    }
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    if (ctx.sdk != nullptr && ctx.sdk->log.write != nullptr) {
        constexpr const char kMessage[] = "[item_facts] provider deactivate";
        (void)ctx.sdk->log.write(1, 1, kPluginId, kMessage, sizeof(kMessage) - 1);
    }
    g_game = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    if (ctx.sdk != nullptr && ctx.sdk->log.write != nullptr) {
        constexpr const char kMessage[] = "[item_facts] provider enabled: capability published";
        (void)ctx.sdk->log.write(1, 1, kPluginId, kMessage, sizeof(kMessage) - 1);
    }
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    if (ctx.sdk != nullptr && ctx.sdk->log.write != nullptr) {
        constexpr const char kMessage[] = "[item_facts] provider disabled: capability withdrawn";
        (void)ctx.sdk->log.write(1, 1, kPluginId, kMessage, sizeof(kMessage) - 1);
    }
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_game != nullptr;
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0,
    nullptr,
    0,
    nullptr,
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
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::item_facts

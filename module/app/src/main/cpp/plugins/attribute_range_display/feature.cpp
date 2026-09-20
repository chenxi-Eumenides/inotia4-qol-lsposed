#include "feature.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "../../contracts/attribute_quality.h"
#include "../../contracts/ui.h"

namespace qol::features::attribute_range_display {
namespace {

using AddOptionFn = void (*)(void* builder, int type, int option_index, int value);
using MakeItemFn = uintptr_t (*)(void* item, void* character, void* arg2);

std::atomic<bool> g_active{false};
const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
void* g_backup_add_option = nullptr;
void* g_backup_make_item = nullptr;

thread_local void* t_current_item = nullptr;

const contracts::AttributeQualityApi* quality_api() {
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const QolResult result = g_sdk->capabilities.require(
        contracts::kAttributeQualityCapabilityId,
        {contracts::kAttributeQualityVersionMajor, contracts::kAttributeQualityVersionMinor},
        &iface);
    if (result.ok != kQolOk || iface == nullptr) return nullptr;
    return static_cast<const contracts::AttributeQualityApi*>(iface);
}

char tier_color(contracts::AttributeQualityTier tier) {
    switch (tier) {
        case contracts::AttributeQualityTier::Grey:
            return 'G';
        case contracts::AttributeQualityTier::White:
            return 'W';
        case contracts::AttributeQualityTier::Green:
            return 'C';
        case contracts::AttributeQualityTier::Blue:
            return 'L';
        case contracts::AttributeQualityTier::Purple:
            return 'V';
        case contracts::AttributeQualityTier::Gold:
            return 'Y';
    }
    return 'G';
}

}  // namespace

void set_context(const contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_game = ctx.game;
}

void set_active(bool active_value) {
    g_active.store(active_value, std::memory_order_release);
}

bool active() { return g_active.load(std::memory_order_acquire); }

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    set_context(ctx);
    if (resource_id == nullptr) return 0;
    if (std::strcmp(resource_id, kResourceIdAddOption) == 0) {
        g_backup_add_option = backup;
    } else if (std::strcmp(resource_id, kResourceIdMakeItem) == 0) {
        g_backup_make_item = backup;
    }
    return 0;
}

void uid_desc_add_option_wrapper(void* builder, int type, int option_index, int value) {
    const auto backup = reinterpret_cast<AddOptionFn>(g_backup_add_option);
    if (backup != nullptr) backup(builder, type, option_index, value);
    if (!active() || builder == nullptr || t_current_item == nullptr) return;

    const auto api = quality_api();
    if (g_game == nullptr || g_game->read.item.ability_level == nullptr || g_sdk == nullptr ||
        g_sdk->ui == nullptr || g_sdk->ui->insert_inline_color == nullptr || api == nullptr ||
        api->probe_option == nullptr || api->classify == nullptr) {
        return;
    }

    contracts::AttributeQualityResult quality{};
    int32_t level = 0;
    if (g_game->read.item.ability_level(t_current_item, &level).ok != kQolOk) return;
    if (api->probe_option(t_current_item, option_index, level, 1, value, &quality).ok != kQolOk) {
        return;
    }
    (void)g_sdk->ui->insert_inline_color(builder, 4096,
                                         static_cast<uint8_t>(tier_color(quality.tier)));
}

uintptr_t uid_desc_make_item_wrapper(void* item, void* character, void* arg2) {
    const auto backup = reinterpret_cast<MakeItemFn>(g_backup_make_item);
    if (!active()) return backup == nullptr ? 0 : backup(item, character, arg2);
    void* previous = t_current_item;
    t_current_item = item;
    const uintptr_t result = backup == nullptr ? 0 : backup(item, character, arg2);
    t_current_item = previous;
    return result;
}

}  // namespace qol::features::attribute_range_display

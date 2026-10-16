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
    const auto* api = static_cast<const contracts::AttributeQualityApi*>(iface);
    return contracts::attribute_quality_api_size_valid(api) ? api : nullptr;
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
}

void set_active(bool active_value) {
    g_active.store(active_value, std::memory_order_release);
}

bool active() { return g_active.load(std::memory_order_acquire); }

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    set_context(ctx);
    if (resource_id == nullptr) return 0;
    if (std::strcmp(resource_id, kResourceIdAddOption) == 0) {
        if (backup == nullptr) {
            g_backup_add_option = nullptr;
            set_active(false);
            return 1;
        }
        g_backup_add_option = backup;
    } else if (std::strcmp(resource_id, kResourceIdMakeItem) == 0) {
        if (backup == nullptr) {
            g_backup_make_item = nullptr;
            set_active(false);
            return 1;
        }
        g_backup_make_item = backup;
    }
    return 0;
}

void uid_desc_add_option_wrapper(void* builder, int type, int option_id, int value) {
    const auto backup = reinterpret_cast<AddOptionFn>(g_backup_add_option);
    if (backup == nullptr) return;
    if (!active()) {
        backup(builder, type, option_id, value);
        return;
    }

    const contracts::QolUiAdapter* ui = g_sdk == nullptr ? nullptr : g_sdk->ui;
    const bool span_api_available = builder != nullptr && ui != nullptr &&
        ui->minor >= contracts::kUiAdapterMinor && ui->ui_desc_text_cursor != nullptr &&
        ui->insert_inline_color_span != nullptr;
    uint32_t before_offset = 0;
    const bool before_ready = span_api_available &&
        ui->ui_desc_text_cursor(builder, &before_offset).ok == kQolOk;

    backup(builder, type, option_id, value);
    if (!before_ready) return;

    uint32_t after_offset = 0;
    if (ui->ui_desc_text_cursor(builder, &after_offset).ok != kQolOk) return;
    if (t_current_item == nullptr) return;

    const auto api = quality_api();
    if (g_sdk == nullptr || g_sdk->item_handle.register_item == nullptr || api == nullptr) return;

    contracts::QolItemHandle handle{};
    if (g_sdk->item_handle.register_item(t_current_item, contracts::kQolItemHandleOwnerGame,
                                        &handle) != contracts::kQolItemHandleStatusOk ||
        handle.slot == 0 || handle.generation == 0) {
        return;
    }

    contracts::AttributeQualityOptions options{};
    contracts::AttributeQualityItem option_context{};
    if (api->query_option_context(handle, option_id, value, &options, &option_context).ok !=
            kQolOk ||
        option_context.kind != contracts::AttributeKind::OptionContext ||
        option_context.status != kQolOk ||
        option_context.quality.level < 0 ||
        option_context.quality.level > contracts::kAttributeQualityHighestLevel) {
        return;
    }

    (void)ui->insert_inline_color_span(
        builder, before_offset, after_offset,
        static_cast<uint8_t>(tier_color(static_cast<contracts::AttributeQualityTier>(
            option_context.quality.level))));
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

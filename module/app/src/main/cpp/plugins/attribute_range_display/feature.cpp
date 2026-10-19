#include "feature.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../../contracts/attribute_quality.h"
#include "../../contracts/item_facts.h"
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

struct ItemViewCache {
    void* item = nullptr;
    bool loaded = false;
    bool valid = false;
    contracts::QolItemHandle handle{};
    contracts::QolItemInventoryView view{};
};
thread_local ItemViewCache t_item_view_cache;

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

const contracts::ItemFactsApi* item_facts_api() {
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const QolResult result = g_sdk->capabilities.require(
        contracts::kItemFactsCapabilityId,
        {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor},
        &iface);
    if (result.ok != kQolOk || iface == nullptr) return nullptr;
    const auto* api = static_cast<const contracts::ItemFactsApi*>(iface);
    return contracts::item_facts_api_size_valid(api) && api->view != nullptr ? api : nullptr;
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

bool load_current_item_view(contracts::QolItemInventoryView* out,
                            contracts::QolItemHandle* out_handle) {
    if (out == nullptr || out_handle == nullptr) return false;
    const contracts::ItemFactsApi* api = item_facts_api();
    if (api == nullptr || g_sdk == nullptr || g_sdk->item_handle.lookup_item == nullptr) {
        return false;
    }

    contracts::QolItemHandle handle{};
    uint32_t owner = contracts::kQolItemHandleOwnerAny;
    int32_t status = g_sdk->item_handle.lookup_item(t_current_item, &handle, &owner);
    if (status == contracts::kQolItemHandleStatusNotFound) {
        if (g_sdk->item_handle.register_item == nullptr) return false;
        status = g_sdk->item_handle.register_item(
            t_current_item, contracts::kQolItemHandleOwnerGame, &handle);
    }
    if (status != contracts::kQolItemHandleStatusOk || handle.slot == 0 ||
        handle.generation == 0) {
        return false;
    }
    (void)owner;

    if (api->view(handle, out).ok != kQolOk) return false;
    constexpr uint32_t kRequiredFields = contracts::kQolItemInventoryHasOptionCompleteness |
                                        contracts::kQolItemInventoryHasNativeOptions |
                                        contracts::kQolItemInventoryHasJewelOptions;
    if ((out->fields & kRequiredFields) != kRequiredFields) return false;
    if ((out->fields & contracts::kQolItemInventoryOptionsTruncated) != 0) return false;
    if (out->native_option_count > contracts::kQolItemInventoryNativeOptionCapacity ||
        out->jewel_option_count > contracts::kQolItemInventoryJewelOptionCapacity) {
        return false;
    }
    // 非装备物品的详情行不参与原生词条染色。
    if (out->equip == 0) return false;
    *out_handle = handle;
    return true;
}

// 每次 UIDesc_MakeItem 周期最多读取一次 view；同一 item 的多条 AddOption 行复用缓存。
bool current_item_view(const contracts::QolItemInventoryView** out_view,
                       contracts::QolItemHandle* out_handle) {
    if (out_view == nullptr || out_handle == nullptr || t_current_item == nullptr) return false;
    if (t_item_view_cache.item == t_current_item && t_item_view_cache.loaded) {
        if (!t_item_view_cache.valid) return false;
        *out_view = &t_item_view_cache.view;
        *out_handle = t_item_view_cache.handle;
        return true;
    }

    t_item_view_cache = {};
    t_item_view_cache.item = t_current_item;
    t_item_view_cache.loaded = true;
    if (!load_current_item_view(&t_item_view_cache.view, &t_item_view_cache.handle)) {
        return false;
    }
    t_item_view_cache.valid = true;
    *out_view = &t_item_view_cache.view;
    *out_handle = t_item_view_cache.handle;
    return true;
}

// AddOption wrapper 只有 (option_id, value)；用 item.facts 的 native/jewel 分类列表确认
// 当前行确实来自 native_options。view 错误、缺完整性、截断、无匹配、或同 id/value 同时
// 命中宝石词条时都不染色。重复原生项仍属于确定的原生来源。view 不暴露未知 option type，
// 因此未知来源无法单独判定；它只可能在与原生条目同 id/value 时被误认为原生。
bool line_is_native_option(const contracts::QolItemInventoryView& view, int32_t option_id,
                           int32_t display_value) {
    uint32_t native_matches = 0;
    uint32_t jewel_matches = 0;
    for (uint32_t index = 0; index < view.native_option_count; ++index) {
        const contracts::QolItemInventoryOptionView& option = view.native_options[index];
        if (option.type != static_cast<int32_t>(
                                contracts::QolItemInventoryOptionType::kNative)) {
            return false;
        }
        if (option.id == option_id && option.value == display_value) ++native_matches;
    }
    for (uint32_t index = 0; index < view.jewel_option_count; ++index) {
        const contracts::QolItemInventoryOptionView& option = view.jewel_options[index];
        if (option.type != static_cast<int32_t>(
                                contracts::QolItemInventoryOptionType::kJewel)) {
            return false;
        }
        if (option.id == option_id && option.value == display_value) ++jewel_matches;
    }
    return native_matches != 0 && jewel_matches == 0;
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
    constexpr std::size_t required_ui_size =
        offsetof(contracts::QolUiAdapter, insert_inline_color_span) +
        sizeof(((contracts::QolUiAdapter*)nullptr)->insert_inline_color_span);
    const bool span_api_available = builder != nullptr && ui != nullptr &&
        ui->major == contracts::kUiAdapterMajor && ui->size >= required_ui_size &&
        ui->ui_desc_text_cursor != nullptr &&
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
    if (api == nullptr) return;

    const contracts::QolItemInventoryView* view = nullptr;
    contracts::QolItemHandle handle{};
    if (!current_item_view(&view, &handle) || view == nullptr) return;
    if (!line_is_native_option(*view, option_id, value)) return;

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
    // 嵌套 MakeItem 必须恢复外层 item 与已缓存的 view；缓存只在本次调用周期内有效。
    const ItemViewCache previous_cache = t_item_view_cache;
    void* previous_item = t_current_item;
    t_current_item = item;
    t_item_view_cache = {};
    const uintptr_t result = backup == nullptr ? 0 : backup(item, character, arg2);
    t_item_view_cache = previous_cache;
    t_current_item = previous_item;
    return result;
}

}  // namespace qol::features::attribute_range_display

#include "feature.h"

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>

#include "rules.h"

namespace qol::features::attribute_quality {
namespace {

using MathGetRandomFn = int (*)(int min, int max);

std::atomic<bool> g_active{false};
const contracts::QolGameHost* g_game = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;
void* g_backup_math_get_random = nullptr;

thread_local bool t_capture_active = false;
thread_local bool t_capture_armed = false;
thread_local int t_capture_min = 0;
thread_local int t_capture_max = 0;

constexpr int kNoBackupSentinel = INT_MIN;

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

int32_t error_status(const QolResult& result) {
    return result.code != kQolOk ? result.code : result.ok;
}

QolResult not_ready() { return error_result(kQolErrNotReady, "not ready"); }

QolResult invalid_argument() {
    return error_result(kQolErrInvalidArgument, "invalid argument");
}

QolResult normalize_options(const contracts::AttributeQualityOptions* options,
                            contracts::AttributeQualityOptions* out) {
    if (out == nullptr) return invalid_argument();
    *out = {};
    if (options == nullptr) return contracts::ok();
    if (options->allow_outside_range > 1 || options->reserved[0] != 0 ||
        options->reserved[1] != 0 || options->reserved[2] != 0) {
        return error_result(kQolErrInvalidArgument, "invalid quality options");
    }
    *out = *options;
    return contracts::ok();
}

class CaptureScope {
public:
    CaptureScope() {
        t_capture_active = true;
        t_capture_armed = true;
        t_capture_min = 0;
        t_capture_max = 0;
    }

    ~CaptureScope() {
        t_capture_armed = false;
        t_capture_active = false;
        t_capture_min = 0;
        t_capture_max = 0;
    }

    CaptureScope(const CaptureScope&) = delete;
    CaptureScope& operator=(const CaptureScope&) = delete;
};

template <typename Call>
QolResult capture_range(Call&& call, int32_t* out_min, int32_t* out_max) {
    if (out_min == nullptr || out_max == nullptr) return invalid_argument();
    if (t_capture_active) return error_result(kQolErrBusy, "attribute capture busy");
    if (!active() || g_game == nullptr || g_backup_math_get_random == nullptr) return not_ready();

    CaptureScope scope;
    const QolResult host_result = call();
    if (!contracts::succeeded(host_result)) return host_result;
    if (t_capture_armed) {
        return error_result(kQolErrNotReady, "attribute range not captured");
    }
    *out_min = static_cast<int32_t>(t_capture_min);
    *out_max = static_cast<int32_t>(t_capture_max);
    return contracts::ok();
}

QolResult capture_jewel_range(contracts::QolItemHandle item, int32_t attribute_id,
                              int32_t* out_min, int32_t* out_max) {
    if (g_game == nullptr || g_game->read.attr_range.jewel_value == nullptr) return not_ready();
    int32_t value = 0;
    return capture_range(
        [&]() { return g_game->read.attr_range.jewel_value(attribute_id, item, &value); },
        out_min, out_max);
}

QolResult capture_option_range(contracts::QolItemHandle item, int32_t option_id,
                               int32_t ability_level, int32_t flag,
                               int32_t* out_min, int32_t* out_max) {
    if (g_game == nullptr || g_game->read.attr_range.option_value == nullptr) return not_ready();
    int32_t value = 0;
    return capture_range(
        [&]() {
            return g_game->read.attr_range.option_value(option_id, ability_level, flag, item,
                                                       &value);
        },
        out_min, out_max);
}

QolResult apply_fixed_option_range(int32_t category, int32_t option_id,
                                   int32_t captured_min, int32_t captured_max,
                                   int32_t* out_min, int32_t* out_max) {
    if (g_game == nullptr || out_min == nullptr || out_max == nullptr ||
        g_game->read.attr_range.uses_fixed_option_table == nullptr ||
        g_game->read.attr_range.fixed_option_percent_pair == nullptr) {
        return not_ready();
    }
    int32_t uses_table = 0;
    const QolResult uses_result =
        g_game->read.attr_range.uses_fixed_option_table(category, &uses_table);
    if (!contracts::succeeded(uses_result)) return uses_result;
    if (uses_table == 0) {
        *out_min = captured_min;
        *out_max = captured_max;
        return contracts::ok();
    }
    if (uses_table != 1) {
        return error_result(kQolErrInvalidArgument, "invalid fixed option table result");
    }

    int32_t p1 = 0;
    int32_t p2 = 0;
    const QolResult pair_result =
        g_game->read.attr_range.fixed_option_percent_pair(category, option_id, &p1, &p2);
    if (!contracts::succeeded(pair_result)) return pair_result;
    return ::attribute_quality::fixed_option_range(captured_max, p1, p2, out_min, out_max);
}

struct ItemFactsContext {
    int32_t category = 0;
    int32_t is_jewel = 0;
    int32_t is_real_equip = 0;
    contracts::QolItemInventoryView view{};
};

QolResult read_item_context(contracts::QolItemHandle item, ItemFactsContext* out) {
    if (out == nullptr) return invalid_argument();
    *out = {};
    if (g_item_facts == nullptr || g_item_facts->category == nullptr ||
        g_item_facts->is_jewel_category == nullptr || g_item_facts->is_real_equip == nullptr ||
        g_item_facts->view == nullptr) {
        return not_ready();
    }

    QolResult result = g_item_facts->category(item, &out->category);
    if (!contracts::succeeded(result)) return result;
    result = g_item_facts->is_jewel_category(out->category, &out->is_jewel);
    if (!contracts::succeeded(result)) return result;
    result = g_item_facts->is_real_equip(item, &out->is_real_equip);
    if (!contracts::succeeded(result)) return result;
    return g_item_facts->view(item, &out->view);
}

void set_item_error(contracts::AttributeQualityItem* item, const QolResult& result) {
    item->status = error_status(result);
}

QolResult fill_jewel_item(contracts::QolItemHandle handle,
                          const contracts::AttributeQualityOptions* options,
                          contracts::AttributeQualityItem* out) {
    int32_t minimum = 0;
    int32_t maximum = 0;
    QolResult result = capture_jewel_range(handle, out->attribute_id, &minimum, &maximum);
    if (!contracts::succeeded(result)) {
        set_item_error(out, result);
        return result;
    }
    out->range = {minimum, maximum};
    result = ::attribute_quality::evaluate_quality(out->value, out->range, options,
                                                   &out->quality);
    if (!contracts::succeeded(result)) {
        set_item_error(out, result);
        return result;
    }
    out->status = kQolOk;
    return contracts::ok();
}

QolResult fill_option_item(contracts::QolItemHandle handle, int32_t category,
                           int32_t ability_level, int32_t flag,
                           const contracts::AttributeQualityOptions* options,
                           contracts::AttributeQualityItem* out) {
    int32_t captured_min = 0;
    int32_t captured_max = 0;
    QolResult result = capture_option_range(handle, out->attribute_id, ability_level, flag,
                                            &captured_min, &captured_max);
    if (!contracts::succeeded(result)) {
        set_item_error(out, result);
        return result;
    }
    out->range = {captured_min, captured_max};

    int32_t minimum = captured_min;
    int32_t maximum = captured_max;
    result = apply_fixed_option_range(category, out->attribute_id, captured_min, captured_max,
                                      &minimum, &maximum);
    if (!contracts::succeeded(result)) {
        set_item_error(out, result);
        return result;
    }
    out->range = {minimum, maximum};
    result = ::attribute_quality::evaluate_quality(out->value, out->range, options,
                                                   &out->quality);
    if (!contracts::succeeded(result)) {
        set_item_error(out, result);
        return result;
    }
    out->status = kQolOk;
    return contracts::ok();
}

QolResult ensure_query_ready() {
    if (!active() || g_game == nullptr || g_item_facts == nullptr ||
        g_backup_math_get_random == nullptr) {
        return not_ready();
    }
    return contracts::ok();
}

QolResult set_context_error(contracts::AttributeQualityItem* out, const QolResult& result) {
    out->status = error_status(result);
    return result;
}

}  // namespace

void set_context(const contracts::PluginContext& ctx) { g_game = ctx.game; }

void set_item_facts(const contracts::ItemFactsApi* facts) { g_item_facts = facts; }

void set_active(bool active) { g_active.store(active, std::memory_order_release); }

bool active() { return g_active.load(std::memory_order_acquire); }

void clear_runtime() {
    set_active(false);
    g_item_facts = nullptr;
    g_backup_math_get_random = nullptr;
    g_game = nullptr;
}

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    set_context(ctx);
    if (resource_id == nullptr || std::strcmp(resource_id, kResourceIdMathGetRandom) != 0) return 0;
    if (backup == nullptr) {
        g_backup_math_get_random = nullptr;
        set_active(false);
        return 1;
    }
    g_backup_math_get_random = backup;
    return 0;
}

QolResult api_query_item(contracts::QolItemHandle item,
                         const contracts::AttributeQualityOptions* options,
                         contracts::AttributeQualityItemResult* out) {
    if (out == nullptr) return invalid_argument();
    *out = {};
    contracts::AttributeQualityOptions normalized_options{};
    QolResult result = normalize_options(options, &normalized_options);
    if (!contracts::succeeded(result)) return result;
    result = ensure_query_ready();
    if (!contracts::succeeded(result)) return result;

    ItemFactsContext context{};
    result = read_item_context(item, &context);
    if (!contracts::succeeded(result)) return result;
    if (context.is_jewel == 0 && context.is_real_equip == 0) {
        return error_result(kQolErrUnsupported, "item is not an equipment item");
    }
    if ((context.view.fields & contracts::kQolItemInventoryHasOptionCompleteness) == 0) {
        return error_result(kQolErrNotReady, "option completeness unavailable");
    }

    const bool complete =
        (context.view.fields & contracts::kQolItemInventoryOptionsTruncated) == 0;
    if (context.is_jewel != 0) {
        constexpr uint32_t kJewelFields = contracts::kQolItemInventoryHasJewelValue |
                                          contracts::kQolItemInventoryHasJewelAttributeType;
        if ((context.view.fields & kJewelFields) != kJewelFields) {
            return error_result(kQolErrNotReady, "jewel facts unavailable");
        }
        out->count = 1;
        out->complete = complete ? 1 : 0;
        contracts::AttributeQualityItem& jewel = out->attrs[0];
        jewel.kind = contracts::AttributeKind::Jewel;
        jewel.ordinal = 0;
        jewel.attribute_id = context.view.jewel_attribute_type;
        jewel.option_type = -1;
        jewel.value = context.view.jewel_value;
        (void)fill_jewel_item(item, &normalized_options, &jewel);
        return contracts::ok();
    }
    if ((context.view.fields & contracts::kQolItemInventoryHasNeedLevel) == 0) {
        return error_result(kQolErrNotReady, "ability level unavailable");
    }
    if (context.view.option_count > contracts::kQolItemInventoryOptionCapacity) {
        return error_result(kQolErrInvalidArgument, "invalid option count");
    }

    out->count = context.view.option_count;
    out->complete = complete ? 1 : 0;
    for (uint32_t ordinal = 0; ordinal < context.view.option_count; ++ordinal) {
        const contracts::QolItemInventoryOptionView& stored = context.view.options[ordinal];
        contracts::AttributeQualityItem& entry = out->attrs[ordinal];
        entry.kind = contracts::AttributeKind::StoredOption;
        entry.ordinal = ordinal;
        entry.attribute_id = stored.id;
        entry.option_type = stored.type;
        entry.value = stored.value;
        if (stored.type != 0) {
            entry.status = kQolErrUnsupported;
            continue;
        }
        (void)fill_option_item(item, context.category, context.view.need_level, 1,
                               &normalized_options, &entry);
    }
    return contracts::ok();
}

QolResult api_query_option_context(contracts::QolItemHandle item, int32_t option_id,
                                   int32_t display_value,
                                   const contracts::AttributeQualityOptions* options,
                                   contracts::AttributeQualityItem* out) {
    if (out == nullptr) return invalid_argument();
    *out = {};
    contracts::AttributeQualityOptions normalized_options{};
    QolResult result = normalize_options(options, &normalized_options);
    if (!contracts::succeeded(result)) return set_context_error(out, result);
    if (option_id < 0) return set_context_error(out, invalid_argument());
    result = ensure_query_ready();
    if (!contracts::succeeded(result)) return set_context_error(out, result);

    ItemFactsContext context{};
    result = read_item_context(item, &context);
    if (!contracts::succeeded(result)) return set_context_error(out, result);
    if (context.is_real_equip == 0) {
        return set_context_error(out,
                                 error_result(kQolErrUnsupported, "item is not an equipment item"));
    }
    if ((context.view.fields & contracts::kQolItemInventoryHasNeedLevel) == 0) {
        return set_context_error(out, error_result(kQolErrNotReady, "ability level unavailable"));
    }

    out->kind = contracts::AttributeKind::OptionContext;
    out->ordinal = UINT32_MAX;
    out->attribute_id = option_id;
    out->option_type = -1;
    out->value = display_value;
    int32_t captured_min = 0;
    int32_t captured_max = 0;
    result = capture_option_range(item, option_id, context.view.need_level, 1,
                                  &captured_min, &captured_max);
    if (!contracts::succeeded(result)) return set_context_error(out, result);
    out->range = {captured_min, captured_max};

    int32_t minimum = captured_min;
    int32_t maximum = captured_max;
    result = apply_fixed_option_range(context.category, option_id, captured_min, captured_max,
                                      &minimum, &maximum);
    if (!contracts::succeeded(result)) return set_context_error(out, result);
    out->range = {minimum, maximum};
    result = ::attribute_quality::evaluate_quality(display_value, out->range,
                                                   &normalized_options, &out->quality);
    if (!contracts::succeeded(result)) return set_context_error(out, result);
    out->status = kQolOk;
    return contracts::ok();
}

QolResult api_evaluate(int32_t value, contracts::AttributeRange range,
                       const contracts::AttributeQualityOptions* options,
                       contracts::AttributeQuality* out) {
    return ::attribute_quality::evaluate_quality(value, range, options, out);
}

int math_get_random_wrapper(int min, int max) {
    if (g_backup_math_get_random == nullptr) return kNoBackupSentinel;
    if (t_capture_active && t_capture_armed) {
        t_capture_armed = false;
        t_capture_min = min;
        t_capture_max = max;
        return max;
    }
    return reinterpret_cast<MathGetRandomFn>(g_backup_math_get_random)(min, max);
}

}  // namespace qol::features::attribute_quality

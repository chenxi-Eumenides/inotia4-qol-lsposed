#include "plugins/attribute_quality/feature.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "plugins/attribute_quality/rules.h"

namespace qol::features::attribute_quality {
namespace {

using MathGetRandomFn = int (*)(int min, int max);

std::atomic<bool> g_active{false};
const contracts::QolGameHost* g_game = nullptr;
void* g_backup_math_get_random = nullptr;

thread_local bool t_capture_armed = false;
thread_local int t_capture_min = 0;
thread_local int t_capture_max = 0;

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

template <typename Call>
bool capture_range(Call&& call, int* out_min, int* out_max) {
    if (!active() || g_backup_math_get_random == nullptr || out_min == nullptr || out_max == nullptr) {
        return false;
    }
    t_capture_armed = true;
    t_capture_min = 0;
    t_capture_max = 0;
    const bool probe_ok = call() == kQolOk;
    const bool captured = !t_capture_armed;
    t_capture_armed = false;
    if (!probe_ok || !captured) return false;
    *out_min = t_capture_min;
    *out_max = t_capture_max;
    return true;
}

bool probe_jewel_bounds(const void* item, int32_t attribute_type, int* out_min, int* out_max) {
    if (g_game == nullptr || g_game->read.attr_range.jewel_value == nullptr || item == nullptr ||
        attribute_type < 0) {
        return false;
    }
    int value = 0;
    return capture_range(
        [&]() {
            return g_game->read.attr_range.jewel_value(attribute_type, item, &value).ok;
        },
        out_min, out_max);
}

bool probe_option_range(const void* item, int32_t option, int32_t level, int32_t flag,
                        int* out_min, int* out_max) {
    if (g_game == nullptr || g_game->read.attr_range.option_value == nullptr || item == nullptr ||
        option < 0 || level < 0 || flag < 0) {
        return false;
    }
    int value = 0;
    if (!capture_range(
            [&]() {
                return g_game->read.attr_range.option_value(option, level, flag, item, &value).ok;
            },
            out_min, out_max)) {
        return false;
    }
    if (g_game->read.item.category == nullptr ||
        g_game->read.attr_range.uses_fixed_option_table == nullptr ||
        g_game->read.attr_range.fixed_option_percent_pair == nullptr) {
        return true;
    }
    int32_t category = 0;
    int32_t uses_table = 0;
    if (g_game->read.item.category(item, &category).ok != kQolOk ||
        g_game->read.attr_range.uses_fixed_option_table(category, &uses_table).ok != kQolOk ||
        uses_table == 0) {
        return true;
    }
    int32_t p1 = 0;
    int32_t p2 = 0;
    if (g_game->read.attr_range.fixed_option_percent_pair(category, option, &p1, &p2).ok ==
        kQolOk) {
        ::attribute_quality::fixed_option_range(*out_max, p1, p2, out_min, out_max);
    }
    return true;
}

void fill_result(int value, int minimum, int maximum,
                 contracts::AttributeQualityResult* out) {
    out->value = value;
    out->min = minimum;
    out->max = maximum;
    out->percentile = ::attribute_quality::percentile(value, minimum, maximum);
    out->tier = ::attribute_quality::classify(value, minimum, maximum);
    out->valid = 1;
    out->reserved = 0;
}

}  // namespace

void set_context(const contracts::PluginContext& ctx) { g_game = ctx.game; }

void set_active(bool active) {
    g_active.store(active, std::memory_order_release);
}

bool active() {
    return g_active.load(std::memory_order_acquire);
}

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    set_context(ctx);
    if (resource_id == nullptr) return 0;
    if (std::strcmp(resource_id, kResourceIdMathGetRandom) == 0) {
        g_backup_math_get_random = backup;
        return 0;
    }
    return 0;
}

QolResult api_probe_jewel(const void* item, int32_t attribute_type, int32_t value,
                          contracts::AttributeQualityResult* out) {
    if (item == nullptr || out == nullptr || attribute_type < 0) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    int minimum = 0;
    int maximum = 0;
    if (!probe_jewel_bounds(item, attribute_type, &minimum, &maximum)) {
        return error_result(kQolErrNotReady, "attribute range unavailable");
    }
    fill_result(value, minimum, maximum, out);
    return contracts::ok();
}

QolResult api_probe_option(const void* item, int32_t option, int32_t level, int32_t flag,
                           int32_t value,
                           contracts::AttributeQualityResult* out) {
    if (item == nullptr || out == nullptr || option < 0 || level < 0 || flag < 0) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    int minimum = 0;
    int maximum = 0;
    if (!probe_option_range(item, option, level, flag, &minimum, &maximum)) {
        return error_result(kQolErrNotReady, "attribute range unavailable");
    }
    fill_result(value, minimum, maximum, out);
    return contracts::ok();
}

QolResult api_classify(int32_t value, int32_t minimum, int32_t maximum,
                       contracts::AttributeQualityTier* out_tier) {
    if (out_tier == nullptr) return error_result(kQolErrInvalidArgument, "null output");
    *out_tier = ::attribute_quality::classify(value, minimum, maximum);
    return contracts::ok();
}

QolResult api_percentile(int32_t value, int32_t minimum, int32_t maximum, int32_t* out_percentile) {
    if (out_percentile == nullptr) return error_result(kQolErrInvalidArgument, "null output");
    *out_percentile = ::attribute_quality::percentile(value, minimum, maximum);
    return contracts::ok();
}

int math_get_random_wrapper(int min, int max) {
    if (t_capture_armed) {
        t_capture_armed = false;
        t_capture_min = min;
        t_capture_max = max;
        return max;
    }
    if (g_backup_math_get_random == nullptr) return min;
    return reinterpret_cast<MathGetRandomFn>(g_backup_math_get_random)(min, max);
}

}  // namespace qol::features::attribute_quality

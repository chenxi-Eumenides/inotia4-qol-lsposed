#include "features/attribute_range/feature.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "core/native/qol_log.h"
#include "domains/attribute_range/rules.h"
#include "game/access/attr_range_ui.h"

namespace qol::features::attribute_range {
namespace {

// 原函数 ABI（hook 目标签名；不引入符号头）。
using AddOptionFn = void (*)(void* builder, int type, int option_index, int value);
using MakeItemFn = uintptr_t (*)(void* item, void* character, void* arg2);

std::atomic<bool> g_active{false};

// MakeItem 入口缓存当前物品，供 AddOption 计算范围（AddOption 自身不带物品指针）。
thread_local void* t_current_item = nullptr;

// 真机验收用的有界诊断日志：每次进程最多 16 条，避免详情重绘刷屏。
std::atomic<int> g_verify_log_budget{16};

// 返回目标颜色码；0 表示不改写（非目标类型/无法取范围）。out_lo/out_hi 为动态范围。
char decide_color_code(int type, int option_index, int value, int* out_lo, int* out_hi) {
    void* item = t_current_item;
    if (item == nullptr) return 0;
    int lo = 0;
    int hi = 0;
    const int category = qol::game::attr_item_category(item);
    if (type == 0) {
        // 装备词缀：flag=1（标准生成路径）。probe 返回的区间上界即公式 base。
        const int level = qol::game::attr_ability_level(item);
        if (!qol::game::attr_probe_option_range(option_index, level, 1, item, &lo, &hi)) return 0;
        int p1 = 0;
        int p2 = 0;
        if (qol::game::attr_item_uses_fixed_option_table(category) &&
            qol::game::attr_fixed_option_percent_pair(category, option_index, &p1, &p2)) {
            // 固定词缀表路径：范围以同一 base 为基准按记录百分比偏移。
            attr_range::fixed_option_range(hi, p1, p2, &lo, &hi);
        }
    } else if (type == 1) {
        // 仅处理「独立宝石详情」：当前物品自身必须是宝石（category ∈ 宝石类别）。
        if (!qol::game::attr_is_jewel_category(category)) return 0;
        if (!qol::game::attr_probe_jewel_range(option_index, item, &lo, &hi)) return 0;
    } else {
        return 0;
    }
    *out_lo = lo;
    *out_hi = hi;
    return attr_range::color_code(attr_range::classify(value, lo, hi));
}

}  // namespace

void set_active(bool active) {
    g_active.store(active, std::memory_order_release);
}

bool active() {
    return g_active.load(std::memory_order_acquire);
}

int math_get_random_wrapper(int min, int max) {
    return qol::game::attr_math_get_random(min, max);
}

void uid_desc_add_option_wrapper(void* builder, int type, int option_index, int value) {
    char* before = (builder != nullptr) ? *reinterpret_cast<char**>(builder) : nullptr;
    const auto backup = reinterpret_cast<AddOptionFn>(qol::game::attr_backup_add_option());
    if (backup != nullptr) backup(builder, type, option_index, value);
    if (before == nullptr || builder == nullptr) return;
    char* after = *reinterpret_cast<char**>(builder);
    if (after <= before) return;

    int lo = 0;
    int hi = 0;
    const char code = decide_color_code(type, option_index, value, &lo, &hi);
    if (code == 0) return;

    // 游戏内联色约定为 "$<码>文本$B"；插入 "$B$<码>" 并右移数值段（见 game 层实现）。
    if (!qol::game::attr_insert_value_color(before, after, code)) return;
    *reinterpret_cast<char**>(builder) = after + 4;

    if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
        QOL_LOG_DEBUG(QolDomain::kAttrRange,
                      "color type=%d oi=%d value=%d range=[%d,%d] code=%c line=%.32s",
                      type, option_index, value, lo, hi, code, before);
    }
}

uintptr_t uid_desc_make_item_wrapper(void* item, void* character, void* arg2) {
    void* previous = t_current_item;
    t_current_item = item;
    uintptr_t result = 0;
    const auto backup = reinterpret_cast<MakeItemFn>(qol::game::attr_backup_make_item());
    if (backup != nullptr) result = backup(item, character, arg2);
    t_current_item = previous;
    return result;
}

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    (void)ctx;
    if (resource_id == nullptr) return 0;
    if (std::strcmp(resource_id, kResourceIdMathGetRandom) == 0) {
        qol::game::attr_set_math_backup(backup);
        return 0;
    }
    if (std::strcmp(resource_id, kResourceIdAddOption) == 0) {
        qol::game::attr_set_add_option_backup(backup);
        return 0;
    }
    if (std::strcmp(resource_id, kResourceIdMakeItem) == 0) {
        qol::game::attr_set_make_item_backup(backup);
        return 0;
    }
    QOL_LOG_WARN(QolDomain::kAttrRange, "unknown resource id: %s", resource_id);
    return 0;
}

bool api_probe_jewel_range(int type, void* item, int* out_min, int* out_max) {
    // 撤销期（§3.4.4）拒绝新调用；hook 未就绪时探测会真实消耗 RNG，故同样拒绝。
    if (!g_active.load(std::memory_order_acquire)) return false;
    if (!qol::game::attr_math_ready()) return false;
    return qol::game::attr_probe_jewel_range(type, item, out_min, out_max);
}

}  // namespace qol::features::attribute_range

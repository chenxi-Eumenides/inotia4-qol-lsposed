#include "game_ui_attr_range.h"

#include "attribute_range.h"

#include "core/native/qol_log.h"
#include "feature/patch/native_inventory_hook.h"
#include "game_access.h"
#include "game_symbols.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace {

// LSPosed native hook 写入的原始函数指针。
MathGetRandomFn g_backup_math_get_random = nullptr;
UiDescAddOptionFn g_backup_add_option = nullptr;
UiDescMakeItemFn g_backup_make_item = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_attempted{false};

// 同一线程内：详情渲染（主线程）依次经过 MakeItem → GetOptionValue → MATH_GetRandom
// → AddOption。capture 只在 probe 调用的同步窗口内置位，MATH_GetRandom wrapper 命中后
// 立即清除并返回上界（不调用原函数，因此不消耗游戏 RNG 状态）。
thread_local bool t_capture_armed = false;
thread_local int t_capture_min = 0;
thread_local int t_capture_max = 0;

// MakeItem 入口缓存当前物品，供 AddOption 计算范围（AddOption 自身不带物品指针）。
thread_local void* t_current_item = nullptr;

// 真机验收用的有界诊断日志：每次进程最多 16 条，避免详情重绘刷屏。
std::atomic<int> g_verify_log_budget{16};

int math_get_random_wrapper(int min, int max) {
    if (t_capture_armed) {
        t_capture_armed = false;
        t_capture_min = min;
        t_capture_max = max;
        return max;
    }
    if (g_backup_math_get_random == nullptr) return min;
    return g_backup_math_get_random(min, max);
}

// 调 ITEMSYSTEM_GetOptionValue 一次，借助 MATH_GetRandom wrapper 截获其掷值区间。
bool probe_option_range(int option_index, int level, int flag, void* item, int* out_min, int* out_max) {
    if (fn_item_system_get_option_value == nullptr || item == nullptr || option_index < 0) return false;
    t_capture_armed = true;
    t_capture_min = 0;
    t_capture_max = 0;
    (void)fn_item_system_get_option_value(option_index, level, flag, item);
    const bool captured = !t_capture_armed;
    t_capture_armed = false;
    if (!captured) return false;   // 退化区间 [1,1]（如低等级基础属性）也算有效
    *out_min = t_capture_min;
    *out_max = t_capture_max;
    return true;
}

// 独立宝石：range = GetJewelOptionValue(type, gemItem) 的掷值区间 [X, 2X]。
bool probe_jewel_range(int type, void* item, int* out_min, int* out_max) {
    if (fn_item_system_get_jewel_option_value == nullptr || item == nullptr || type < 0) return false;
    t_capture_armed = true;
    t_capture_min = 0;
    t_capture_max = 0;
    (void)fn_item_system_get_jewel_option_value(type, item);
    const bool captured = !t_capture_armed;
    t_capture_armed = false;
    if (!captured) return false;
    *out_min = t_capture_min;
    *out_max = t_capture_max;
    return true;
}

// 返回目标颜色码；0 表示不改写（非目标类型/无法取范围）。out_lo/out_hi 为动态范围。
char decide_color_code(int type, int option_index, int value, int* out_lo, int* out_hi) {
    void* item = t_current_item;
    if (item == nullptr) return 0;
    int lo = 0;
    int hi = 0;
    if (type == 0) {
        // 装备随机词缀：flag=1（标准生成路径；特例物品区间外 → classify 判 Gold）。
        const int level = (fn_item_get_ability_level != nullptr) ? fn_item_get_ability_level(item) : 0;
        if (!probe_option_range(option_index, level, 1, item, &lo, &hi)) return 0;
    } else if (type == 1) {
        // 仅处理「独立宝石详情」：当前物品自身必须是宝石（category ∈ 宝石类别）。
        // 装备上已镶嵌的宝石节点（type=1）不处理——镶嵌时类别/等级已丢弃，无法求范围（设计文档 E7）。
        if (fn_is_jewel == nullptr) return 0;
        const uint16_t type_flags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
        const int category = (type_flags >> 6) & 0x3ff;
        if (!fn_is_jewel(category)) return 0;
        if (!probe_jewel_range(option_index, item, &lo, &hi)) return 0;
    } else {
        return 0;
    }
    *out_lo = lo;
    *out_hi = hi;
    return attr_range::color_code(attr_range::classify(value, lo, hi));
}

void uid_desc_add_option_wrapper(void* builder, int type, int option_index, int value) {
    char* before = (builder != nullptr) ? *reinterpret_cast<char**>(builder) : nullptr;
    if (g_backup_add_option != nullptr) g_backup_add_option(builder, type, option_index, value);
    if (before == nullptr || builder == nullptr) return;
    char* after = *reinterpret_cast<char**>(builder);
    if (after <= before) return;

    int lo = 0;
    int hi = 0;
    const char code = decide_color_code(type, option_index, value, &lo, &hi);
    if (code == 0) return;

    // 行格式： "$<原色><标签>: <数值>$B"。只染数值：在数值前插入 "$<目标色>"，
    // 标签保持原色，行尾已有 "$B" 收束。插入需右移数值段 2 字节。
    char* insert_at = nullptr;
    for (char* p = before; p < after; ++p) {
        if (*p == ':') {
            insert_at = p + 1;
            while (insert_at < after && *insert_at == ' ') ++insert_at;
            break;
        }
    }
    if (insert_at == nullptr) {
        for (char* p = before; p < after; ++p) {
            if (*p >= '0' && *p <= '9') {
                insert_at = p;
                break;
            }
        }
    }
    if (insert_at == nullptr) return;
    if (insert_at > before && (insert_at[-1] == '-' || insert_at[-1] == '+')) --insert_at;

    // 游戏内联色约定为 "$<码>文本$B"（段内着色、$B 收束）；直接插入 "$<码>" 会让
    // 前一段（标签）回落到默认色。故先插入 "$B" 关闭标签段，再插入 "$<目标色>" 开值段。
    const uintptr_t buffer_end = g_base + G_UIDESC_TEXT_BUF_VMA + G_UIDESC_TEXT_BUF_SIZE;
    if (reinterpret_cast<uintptr_t>(after) + 4 > buffer_end) return;

    std::memmove(insert_at + 4, insert_at, static_cast<size_t>(after - insert_at));
    insert_at[0] = '$';
    insert_at[1] = 'B';
    insert_at[2] = '$';
    insert_at[3] = code;
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
    if (g_backup_make_item != nullptr) result = g_backup_make_item(item, character, arg2);
    t_current_item = previous;
    return result;
}

bool install_one(NativeHookFunType hook, uintptr_t target, void* replacement, void** backup, const char* name) {
    const int rc = hook(reinterpret_cast<void*>(target), replacement, backup);
    if (rc != 0 || backup == nullptr || *backup == nullptr) {
        QOL_LOG_ERROR(QolDomain::kAttrRange, "%s hook failed rc=%d", name, rc);
        return false;
    }
    return true;
}

}  // namespace

void attr_range_ui_install_if_ready() {
    if (g_installed.load(std::memory_order_acquire)) return;
    if (!bridge_ready()) return;
    NativeHookFunType hook = native_hook_func();
    if (hook == nullptr) return;

    bool expected = false;
    if (!g_attempted.compare_exchange_strong(expected, true)) return;

    const uintptr_t math_get_random = g_base + fn_resolve("F_MATH_GET_RANDOM_VMA", F_MATH_GET_RANDOM_VMA);
    const uintptr_t add_option = g_base + fn_resolve("F_UIDESC_ADD_OPTION_VMA", F_UIDESC_ADD_OPTION_VMA);
    const uintptr_t make_item = g_base + fn_resolve("F_UIDESC_MAKE_ITEM_VMA", F_UIDESC_MAKE_ITEM_VMA);

    if (!install_one(hook, math_get_random, reinterpret_cast<void*>(&math_get_random_wrapper),
                     reinterpret_cast<void**>(&g_backup_math_get_random), "MATH_GetRandom") ||
        !install_one(hook, add_option, reinterpret_cast<void*>(&uid_desc_add_option_wrapper),
                     reinterpret_cast<void**>(&g_backup_add_option), "UIDesc_AddOption") ||
        !install_one(hook, make_item, reinterpret_cast<void*>(&uid_desc_make_item_wrapper),
                     reinterpret_cast<void**>(&g_backup_make_item), "UIDesc_MakeItem")) {
        return;
    }

    g_installed.store(true, std::memory_order_release);
    QOL_LOG_INFO(QolDomain::kAttrRange, "attribute range hooks installed");
}

bool attr_range_ready() {
    return g_installed.load(std::memory_order_acquire);
}

bool attr_range_probe_jewel_range(int type, void* item, int* out_min, int* out_max) {
    // 未安装 hook 时不得调用游戏函数：否则 MATH_GetRandom 会真实消耗 RNG。
    if (!g_installed.load(std::memory_order_acquire)) return false;
    if (out_min == nullptr || out_max == nullptr) return false;
    return probe_jewel_range(type, item, out_min, out_max);
}

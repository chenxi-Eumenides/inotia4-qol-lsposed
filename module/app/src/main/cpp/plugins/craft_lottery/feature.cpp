#include "feature.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace qol::features::craft_lottery {
namespace {

constexpr uint32_t kPoolCapacity = 64;

std::atomic<bool> g_gate{false};
std::atomic<void*> g_backup_process_recipe{nullptr};
std::atomic<bool> g_degraded_logged{false};

using ProcessRecipeFn = int (*)(uint32_t);

void log_write(int32_t level, const char* format, va_list args) {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->log.write == nullptr || format == nullptr) return;
    char message[contracts::kMessageCap + 1]{};
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    if (length < 0) return;
    const uint32_t message_length = static_cast<uint32_t>(
        length < static_cast<int>(contracts::kMessageCap) ? length : contracts::kMessageCap);
    (void)sdk->log.write(level, 16, "plugins/craft_lottery/feature.cpp", message, message_length);
}

void log_info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log_write(1, format, args);
    va_end(args);
}

void log_warn(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log_write(2, format, args);
    va_end(args);
}

void note_degraded_once(const char* operation) {
    if (g_degraded_logged.exchange(true, std::memory_order_relaxed)) return;
    log_warn("degraded craft lottery operation=%s", operation);
}

bool port_ready(const contracts::QolCraftLotteryWritePort* port) {
    return port != nullptr && port->pool_read != nullptr && port->recipe_learned != nullptr &&
           port->decide_recipe_pure != nullptr && port->add_recipe_book != nullptr &&
           port->get_random != nullptr && port->show_recipe_popup != nullptr;
}

}  // namespace

int process_recipe_wrapper(uint32_t use_type) {
    const auto* sdk = host_sdk();
    const contracts::InFlightGuard guard(
        sdk == nullptr ? nullptr : &sdk->in_flight, {kResourceIdProcessRecipe});
    const ProcessRecipeFn backup = reinterpret_cast<ProcessRecipeFn>(
        g_backup_process_recipe.load(std::memory_order_acquire));
    if (backup == nullptr) return 0;

    const auto* port = craft_lottery_write_port();
    if (!gate_enabled() || !port_ready(port)) return backup(use_type);

    uint16_t pool[kPoolCapacity]{};
    uint32_t pool_count = 0;
    const QolResult pool_result = port->pool_read(use_type, pool, kPoolCapacity, &pool_count);
    if (!contracts::succeeded(pool_result) || pool_count > kPoolCapacity) {
        note_degraded_once("pool_read");
        return backup(use_type);
    }
    if (pool_count == 0) return backup(use_type);

    uint16_t unlearned[kPoolCapacity]{};
    uint32_t unlearned_count = 0;
    uint32_t learned_before = 0;
    for (uint32_t i = 0; i < pool_count; ++i) {
        bool learned = false;
        if (!contracts::succeeded(port->recipe_learned(pool[i], &learned))) {
            return backup(use_type);
        }
        if (learned) {
            ++learned_before;
        } else {
            unlearned[unlearned_count++] = pool[i];
        }
    }

    int32_t recipe_idx = -1;
    bool selected_unlearned = false;
    if (unlearned_count == 0) {
        if (!contracts::succeeded(port->decide_recipe_pure(use_type, &recipe_idx))) {
            return backup(use_type);
        }
    } else {
        int32_t random_index = -1;
        if (!contracts::succeeded(
                port->get_random(static_cast<int32_t>(unlearned_count - 1), &random_index)) ||
            random_index < 0 || static_cast<uint32_t>(random_index) >= unlearned_count) {
            return backup(use_type);
        }
        recipe_idx = unlearned[static_cast<uint32_t>(random_index)];
        selected_unlearned = true;
    }
    if (recipe_idx < 0 || recipe_idx > UINT16_MAX) return backup(use_type);

    bool belongs_to_pool = false;
    for (uint32_t i = 0; i < pool_count; ++i) {
        if (pool[i] == static_cast<uint16_t>(recipe_idx)) {
            belongs_to_pool = true;
            break;
        }
    }
    if (!belongs_to_pool) return backup(use_type);

    const uint16_t selected_recipe = static_cast<uint16_t>(recipe_idx);
    if (!contracts::succeeded(port->add_recipe_book(selected_recipe))) {
        return backup(use_type);
    }

    const uint32_t learned_after = learned_before + (selected_unlearned ? 1U : 0U);
    char extra[64]{};
    const int extra_length = std::snprintf(extra, sizeof(extra), "已获得：%u/%u",
                                           learned_after, pool_count);
    if (extra_length < 0 || extra_length >= static_cast<int>(sizeof(extra))) {
        note_degraded_once("popup_text_format");
        return 1;
    }
    // 追加行=「&N{套装名}已获得：x/y」：套装名由 port 读 ITEMDATABASE 补入，「已获得：x/y」为 UTF-8 中文常量，
    // &N 是游戏内换行标记（show_recipe_popup 负责拼接）。
    if (!contracts::succeeded(port->show_recipe_popup(selected_recipe, use_type, extra))) {
        note_degraded_once("show_recipe_popup");
    }
    return 1;
}

void set_gate(bool enabled) {
    g_gate.store(enabled, std::memory_order_release);
    log_info("enabled=%d", enabled);
}

bool gate_enabled() { return g_gate.load(std::memory_order_acquire); }

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    (void)ctx;
    if (resource_id == nullptr) return 0;
    if (std::strcmp(resource_id, kResourceIdProcessRecipe) == 0) {
        g_backup_process_recipe.store(backup, std::memory_order_release);
        return 0;
    }
    log_warn("unknown resource id: %s", resource_id);
    return 0;
}

}  // namespace qol::features::craft_lottery

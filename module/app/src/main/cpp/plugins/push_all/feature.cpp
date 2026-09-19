#include "feature.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "rules.h"

namespace qol::features::push_all {
namespace {

std::atomic<bool> g_gate{false};
// 首次进入 wrapper 只记一条 info：作为真机「重定向确实路由到模块」的证据。
std::atomic<bool> g_filter_seen{false};

void log_info(const char* format, ...) {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->log.write == nullptr || format == nullptr) return;
    char message[contracts::kMessageCap + 1]{};
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length < 0) return;
    const uint32_t message_length = static_cast<uint32_t>(
        length < static_cast<int>(contracts::kMessageCap) ? length : contracts::kMessageCap);
    (void)sdk->log.write(1, 0, "plugins/push_all/feature.cpp", message, message_length);
}

}  // namespace

void set_gate(bool enabled) {
    g_gate.store(enabled, std::memory_order_release);
    log_info("push_all enabled=%d", enabled);
}

bool gate_enabled() {
    return g_gate.load(std::memory_order_acquire);
}

int push_all_party_filter(void* ch) {
    // 符号未解析/空指针时判不可推：直接解引用空函数指针会在 CHAR_Move 调用链内崩溃。
    if (ch == nullptr) {
        return -1;
    }
    if (!g_filter_seen.exchange(true)) {
        log_info("push_all party filter active");
    }
    const auto* game = game_host();
    if (game == nullptr || game->read.character.type == nullptr ||
        game->read.character.is_active_player_group == nullptr ||
        game->read.character.party_index == nullptr) {
        return -1;
    }
    int32_t type = -1;
    int32_t active_group = 0;
    int32_t party_index = -1;
    if (game->read.character.type(ch, &type).ok != kQolOk ||
        game->read.character.is_active_player_group(ch, &active_group).ok != kQolOk ||
        game->read.character.party_index(ch, &party_index).ok != kQolOk) {
        return -1;
    }
    // 门禁关闭时按原版语义透传（资源应已还原，此为竞态窗口内的 fail-safe）。
    if (!g_gate.load(std::memory_order_relaxed)) {
        return party_index;
    }
    return party_decision(static_cast<uint8_t>(type), active_group != 0, party_index);
}

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    (void)ctx;
    (void)resource_id;
    (void)backup;
    // 调用点 patch 无原函数入口需缓存；本插件无失败路径。
    return 0;
}

}  // namespace qol::features::push_all

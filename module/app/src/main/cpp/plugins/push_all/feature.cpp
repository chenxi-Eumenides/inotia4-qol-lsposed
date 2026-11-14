#include "feature.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "contracts/log_domain.h"
#include "rules.h"

namespace qol::features::push_all {
namespace {

std::atomic<bool> g_gate{false};
const contracts::CharacterFactsApi* g_character_facts = nullptr;
// debug 开启时首次进入 wrapper 记一条诊断：确认重定向实际到达模块。
std::atomic<bool> g_filter_seen{false};
std::atomic<bool> g_filter_reject_logged{false};

void log_message(int32_t level, const char* format, ...) {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->log.write == nullptr || format == nullptr) return;
    if (level == 0 &&
        (sdk->log.debug_enabled == nullptr || sdk->log.debug_enabled() == 0)) return;
    char message[contracts::kMessageCap + 1]{};
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length < 0) return;
    const uint32_t message_length = static_cast<uint32_t>(
        length < static_cast<int>(contracts::kMessageCap) ? length : contracts::kMessageCap);
    (void)sdk->log.write(level, static_cast<int32_t>(QolDomain::kPlatform), "plugins/push_all/feature.cpp", message, message_length);
}

void log_filter_reject_once(const char* reason) {
    if (g_filter_reject_logged.exchange(true, std::memory_order_acq_rel)) return;
    log_message(2, "reject op=push_filter reason=%s", reason);
}

}  // namespace

void set_gate(bool enabled) {
    g_gate.store(enabled, std::memory_order_release);
}

bool gate_enabled() {
    return g_gate.load(std::memory_order_acquire);
}

int push_all_party_filter(void* ch) {
    // 符号未解析/空指针时判不可推：直接解引用空函数指针会在 CHAR_Move 调用链内崩溃。
    if (ch == nullptr) {
        return -1;
    }
    const auto* sdk = host_sdk();
    if (sdk != nullptr && sdk->log.debug_enabled != nullptr && sdk->log.debug_enabled() != 0 &&
        !g_filter_seen.exchange(true, std::memory_order_acq_rel)) {
        log_message(0, "dispatch hook=push_party_filter result=first_hit");
    }
    const auto* game = game_host();
    if (game == nullptr || g_character_facts == nullptr || g_character_facts->type == nullptr ||
        g_character_facts->is_active_player_group == nullptr ||
        character_extras() == nullptr || character_extras()->read.party_index == nullptr) {
        log_filter_reject_once("character_facts_unavailable");
        return -1;
    }
    int32_t type = -1;
    int32_t active_group = 0;
    int32_t party_index = -1;
    if (g_character_facts->type(ch, &type).ok != kQolOk ||
        g_character_facts->is_active_player_group(ch, &active_group).ok != kQolOk ||
        character_extras()->read.party_index(ch, &party_index).ok != kQolOk) {
        log_filter_reject_once("character_facts_read_failed");
        return -1;
    }
    // 门禁关闭时按原版语义透传（资源应已还原，此为竞态窗口内的 fail-safe）。
    if (!g_gate.load(std::memory_order_relaxed)) {
        return party_index;
    }
    return party_decision(static_cast<uint8_t>(type), active_group != 0, party_index);
}

void set_character_facts(const contracts::CharacterFactsApi* facts) {
    g_character_facts = facts;
}

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    (void)ctx;
    (void)resource_id;
    (void)backup;
    // 调用点 patch 无原函数入口需缓存；本插件无失败路径。
    return 0;
}

}  // namespace qol::features::push_all

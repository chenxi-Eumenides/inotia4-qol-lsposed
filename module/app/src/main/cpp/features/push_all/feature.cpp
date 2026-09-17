#include "features/push_all/feature.h"

#include <atomic>
#include <cstdint>

#include "core/native/qol_log.h"
#include "features/push_all/rules.h"
#include "game/access/char.h"

namespace qol::features::push_all {
namespace {

std::atomic<bool> g_gate{false};
// 首次进入 wrapper 只记一条 info：作为真机「重定向确实路由到模块」的证据。
std::atomic<bool> g_filter_seen{false};

}  // namespace

void set_gate(bool enabled) {
    g_gate.store(enabled, std::memory_order_release);
    QOL_LOG_INFO(QolDomain::kPlatform, "push_all enabled=%d", enabled);
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
        QOL_LOG_INFO(QolDomain::kPlatform, "push_all party filter active");
    }
    const int type = game::char_type(ch);
    const bool active_group = game::char_is_active_player_group(ch);
    const int party_index = game::char_party_index(ch);
    // 门禁关闭时按原版语义透传（资源应已还原，此为竞态窗口内的 fail-safe）。
    if (!g_gate.load(std::memory_order_relaxed)) {
        return party_index;
    }
    return party_decision(static_cast<uint8_t>(type), active_group, party_index);
}

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    (void)ctx;
    (void)resource_id;
    (void)backup;
    // 调用点 patch 无原函数入口需缓存；本插件无失败路径。
    return 0;
}

}  // namespace qol::features::push_all

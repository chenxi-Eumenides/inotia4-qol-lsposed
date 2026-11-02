#include "status_json.h"

#include "config.h"
#include "config_codec.h"

#include <cstdio>
#include <string>

namespace {

const char* json_bool(bool value) { return value ? "true" : "false"; }

}  // namespace

std::string autosell_status_json() {
    const autosell::StatusSnapshot snapshot = autosell::autosell_status_snapshot();
    const autosell::Config& cfg = snapshot.config;

    // special 保留为按新分档配置派生的 v2 兼容字段。
    const std::string special = autosell_special_to_array(cfg);
    char buf[768];
    std::snprintf(buf, sizeof(buf),
                  "{\"enabled\":%s,\"rarity\":%d,\"enhance\":%d,\"socket\":%d,"
                  "\"gemTier\":%d,\"gemRange\":%d,\"backpackTier\":%d,"
                  "\"badgeTier\":%d,\"diceTier\":%d,\"special\":%s,"
                  "\"sold\":%lld,\"failed\":%lld,\"lastScanFrame\":%lld,"
                  "\"slot\":%d,\"loadedSlot\":%d,\"persisted\":%s,\"hostInstalled\":%s}",
                  json_bool(cfg.enabled), cfg.rarity, cfg.enhance, cfg.socket, cfg.gem_tier,
                   cfg.gem_range, cfg.backpack_tier, cfg.badge_tier, cfg.dice_tier,
                   special.c_str(),
                  static_cast<long long>(snapshot.sold), static_cast<long long>(snapshot.failed),
                  static_cast<long long>(snapshot.last_scan_frame), snapshot.slot,
                  snapshot.loaded_slot, json_bool(snapshot.persisted),
                  json_bool(snapshot.host_installed));
    return std::string(buf);
}

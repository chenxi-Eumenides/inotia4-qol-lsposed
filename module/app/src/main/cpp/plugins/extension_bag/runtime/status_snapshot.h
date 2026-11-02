#pragma once

#include "plugins/extension_bag/model/virtual_bag_state.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace qol::extension_bag {

struct StatusSnapshot {
    virtual_bag::State bag{};
    std::array<std::array<uint32_t, virtual_bag::kSlotCount>,
               virtual_bag::kMaxExtensionBags> effective_counts{};
    bool enabled = false;
    bool injected = false;
    bool save_panel_hook_installed = false;
    bool extension_tab_button = false;
    bool inventory_frame_active = false;
    virtual_bag::RecoveryAction recovery_action = virtual_bag::RecoveryAction::kNone;
    std::array<int32_t, 5> original_capacities{{-1, -1, -1, -1, -1}};
    uint64_t revision = 0;
};

using StatusSnapshotPtr = std::shared_ptr<const StatusSnapshot>;

// 主线程在扩展状态提交点发布；查询线程只读取最近一次已发布的不可变快照。
void publish_status_snapshot(const StatusSnapshot& snapshot);
void publish_extension_bag_status_snapshot();
StatusSnapshotPtr load_status_snapshot();
std::string status_snapshot_json(const StatusSnapshot& snapshot);

}  // namespace qol::extension_bag

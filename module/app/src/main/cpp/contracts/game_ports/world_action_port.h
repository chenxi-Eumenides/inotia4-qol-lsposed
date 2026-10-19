#pragma once

#include <type_traits>

#include "../game_host.h"
#include "../version.h"

namespace qol::contracts {

// world_teleport 私有端口：世界动作面（确认框/扣费/切图/弹窗/横幅）。
// 把原先经聚合 QolGameHost::world_action 的调用收口为私有依赖，仅 world_teleport 可声明。
struct QolWorldActionPort {
    QolVersion abi_version;
    QolWorldActionApi actions;
};

static_assert(std::is_standard_layout<QolWorldActionPort>::value,
              "QolWorldActionPort must remain standard layout");
static_assert(std::is_trivially_copyable<QolWorldActionPort>::value,
              "QolWorldActionPort must remain trivially copyable");

}  // namespace qol::contracts

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
    // v1.1 尾追加：传送落点选点——中点锚，阻挡则先 X 正/负、再 Y 正/负（步长 ≤32）找
    // 最近非阻挡瓦片（与 c18 进图选点同口径，静态地图数据）。无该图静态数据返回错误，
    // 调用方回退 (0,0) 字面落点。
    QolResult (*pick_landing)(int32_t map_id, int32_t* out_x, int32_t* out_y);
};

static_assert(std::is_standard_layout<QolWorldActionPort>::value,
              "QolWorldActionPort must remain standard layout");
static_assert(std::is_trivially_copyable<QolWorldActionPort>::value,
              "QolWorldActionPort must remain trivially copyable");

}  // namespace qol::contracts

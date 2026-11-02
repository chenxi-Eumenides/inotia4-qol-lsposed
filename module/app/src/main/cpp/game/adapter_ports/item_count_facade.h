#pragma once

#include "contracts/item_count.h"

namespace qol::game {

// 999 扩展模式策略：由 Host 生命周期按 stack_limit 资源组整组 live/revoke 更新，插件不
// 参与写。kOff=请求明确关闭；kNotReady=请求开启但资源组/适配尚未落地；kApplied=已应用。
enum class ItemCountExtendedMode : uint8_t {
    kOff = 0,
    kNotReady = 1,
    kApplied = 2,
};

// 内部模式更新：替换旧的 active probe。仅 Host 内部调用。
void item_count_set_extended_mode(ItemCountExtendedMode mode);
ItemCountExtendedMode item_count_extended_mode();

// 999 已应用（mode==kApplied）。item_port 以该判据区分 canonical 覆盖面与原版 getter。
bool item_count_active();

// Host 装配的常驻数量 facade；实现自带 S2 codec，不再转发插件 provider。
const contracts::QolItemCountApi& item_count_api();
const contracts::QolItemCountApi* item_count_api_if_resident();

}  // namespace qol::game

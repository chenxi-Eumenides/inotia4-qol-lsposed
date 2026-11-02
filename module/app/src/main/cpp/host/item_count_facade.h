#pragma once

#include "contracts/game_host.h"
#include "game/adapter_ports/item_count_facade.h"

namespace qol::host {

// 数量扩展模式策略写权仅 Host 内部：由生命周期在 stack_limit 资源组整组 live/revoke 时
// 更新，替换旧 active probe。nullptr provider 语义随插件 provider 退役一并移除。
void host_item_count_set_extended_mode(qol::game::ItemCountExtendedMode mode);

// 返回宿主稳定持有的 facade 表，供 QolGameHost 注入。
const contracts::QolItemCountApi& host_item_count_api();

}  // namespace qol::host

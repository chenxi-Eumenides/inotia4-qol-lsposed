#pragma once

#include "contracts/game_host.h"

namespace qol::host {

// provider 的生命周期由宿主装配与所有者插件保证；nullptr 表示注销。
void host_item_count_set_provider(const contracts::QolItemCountApi* api);
void host_item_count_set_active_probe(bool (*probe)());

// 返回宿主稳定持有的 facade 表，供 QolGameHost 注入。
const contracts::QolItemCountApi& host_item_count_api();

}  // namespace qol::host

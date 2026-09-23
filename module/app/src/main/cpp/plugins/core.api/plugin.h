#pragma once

#include "contracts/http.h"
#include "contracts/plugin.h"

namespace qol::features::core_api {

const contracts::PluginDescriptor& plugin_descriptor();

// POST /api/op/features/lifecycle 的 owner command handler（生产经 http.dispatch 注册；
// host 测试直接调用以验证成功/失败/未知动作三类响应逐字节形状）。
QolResult lifecycle_command(const contracts::HttpReq* request, contracts::HttpRes* response,
                            void* ctx);

}  // namespace qol::features::core_api

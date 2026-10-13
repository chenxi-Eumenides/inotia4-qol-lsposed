#pragma once

#include "contracts/http.h"
#include "contracts/plugin.h"

namespace qol::features::core_api {

const contracts::PluginDescriptor& plugin_descriptor();

// POST /api/op/features/lifecycle 的响应兼容测试辅助；生产端点由 Host 常驻控制设施处理。
QolResult lifecycle_command(const contracts::HttpReq* request, contracts::HttpRes* response,
                            void* ctx);

}  // namespace qol::features::core_api

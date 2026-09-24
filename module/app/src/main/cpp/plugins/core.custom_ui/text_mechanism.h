#pragma once

#include <cstdint>

#include "../../contracts/host_sdk.h"
#include "../../contracts/text_layer.h"

// 文本层机制（ADR 0001 D3：机制归工具插件）——原先整块位于 plugins/text_layer，
// 现并入 core.custom_ui；内容（角色面板标签、合成页文案）由各内容插件经保留 ID 的
// `text_layer` 能力注册，机制本身不再内置任何文案表。
//
// 共享资源 `hook:MEMORYTEXT_GetText`（全游戏唯一文本读取热点，232 调用点）由本插件
// 独占挂载；消费者一律经能力 subscribe（LSPosed 同址二次挂载会失败）。
// 分派顺序：wrapper 先调原函数取原始文本 → sdk->hooks.run_text 分发 before/original/after。
//
// 线程纪律：wrapper 在游戏文本读取路径上执行；active 开关与能力注册由主线程生命周期驱动。

namespace qol::features::core_ui {

// 资源 id：plugin.cpp 的 ResourceClaim 与 wrapper 的分派键共用。
inline constexpr const char* kResourceIdGetText = "hook:MEMORYTEXT_GetText";

// 注入宿主 SDK（install）；解绑传 nullptr。
void text_mechanism_bind(const contracts::QolHostSdk* sdk);
const contracts::QolHostSdk* text_mechanism_sdk();

// 业务门禁：true = 分派生效（能力查询有效）；false = 撤销期/停用，wrapper 透传原函数。
void text_set_active(bool active);
bool text_active();

// 能力接口实现（contracts::TextLayerApi 的两个函数）。
uint64_t text_api_subscribe(int32_t phase, int32_t kind,
                            const char* (*fn)(uint32_t, const char*, void*), void* ctx);
void text_api_unsubscribe(uint64_t id);

// 共享资源 wrapper：由 plugin_descriptor 的 ResourceClaim.handler 声明，宿主安装。
const char* memorytext_get_text_wrapper(uint16_t text_id);

// 宿主在资源安装成功后交还 GetText 原函数入口（backup）。
void text_on_get_text_ready(void* backup);

}  // namespace qol::features::core_ui

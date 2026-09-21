#pragma once

// 帧源：两条常驻 core 资源（渲染开始前 / 逻辑帧开始前锚点）。
//
// 取代旧 infra/frame/frame_host 的专用安装器：锚点现在作为 patch 层的声明式
// kCallPatch 资源安装，wrapper 在游戏主线程派发 infra::scheduler 与 legacy
// infra/frame/frame_task（未迁移消费者），再复刻原调用。

namespace qol::host {

// 幂等安装两条锚点资源（owner="core"，常驻无门禁）；未就绪/失败 fail-closed 返回 false
// 且不改写字节，可后续重试。安装成功后 wrapper 生效。
bool frame_source_install();

// 两条锚点是否都已安装（transition_dispatch 据此 fail-fast）。
bool frame_source_installed();

}  // namespace qol::host

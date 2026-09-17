#pragma once

// 兼容声明（P4 A2a）：帧锚点安装已迁往 host/frame_source，本文件只保留
// transition_dispatch 仍在用的安装状态查询。
//
// 为什么保留：transition_dispatch.cpp 是 legacy frame_task 的存量消费者，本阶段不得改动，
// 它 include 本头并调用 frame_host_anchors_installed()。实现由 host/frame_source.cpp 提供。
// 待 transition_dispatch 迁移（改 scheduler + frame_source_installed）后删除本文件。

// 两条帧锚点是否都已安装（现由 host::frame_source_installed() 提供）。
bool frame_host_anchors_installed();

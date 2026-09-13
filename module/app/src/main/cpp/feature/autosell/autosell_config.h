#pragma once

#include <cstdint>
#include <string>

#include "feature/autosell/autosell_rules.h"

// 自动出售运行时配置/状态存储（阶段 B）。
//
// 线程模型：JVM 线程写配置（nativeSetAutoSellConfig）、主线程读（autosell_tick）；
// 配置读写与计数共享内部互斥/原子，跨线程安全。本层不触碰游戏内存。
//
// 「启用」由 false -> true 跳变时自动请求立即清扫一次（autosell_request_immediate_run），
// 由主线程 tick 消费。

// 覆盖运行时配置快照；跳变到 enabled 时请求立即清扫。
void autosell_set_runtime_config(const autosell::Config& config);

// 当前配置快照（拷贝）。
autosell::Config autosell_get_runtime_config();

// 请求下一次 tick 跳过 60 帧节流立即执行一次。
void autosell_request_immediate_run();

// 消费「立即执行」请求；返回是否曾请求（消费后清零）。
bool autosell_consume_immediate_run();

// 记录一次扫描结果：frame = 本次扫描帧号；sold/failed 为本次增量计数。
void autosell_note_scan(int64_t frame, int sold, int failed);

// 记录主线程逐帧宿主是否安装成功（M-10：宿主安装失败/未安装可经状态 JSON 查）。
void autosell_set_host_installed(bool installed);

// 记录按存档 sidecar 的加载/持久化状态（slot/loadedSlot/persisted 可经状态 JSON 查）。
void autosell_set_store_status(int slot, int loaded_slot, bool persisted);

// 状态 JSON：配置快照 + 已售/失败累计计数 + 上次扫描帧号 + 宿主安装状态 + 存档 sidecar 状态。
std::string autosell_status_json();

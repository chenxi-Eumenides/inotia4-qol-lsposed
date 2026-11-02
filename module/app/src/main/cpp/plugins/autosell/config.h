#pragma once

#include <cstdint>

#include "rules.h"

// 自动出售运行时配置/状态存储（阶段 B）。
//
// 线程模型：JVM 线程写配置（nativeSetAutoSellConfig）、主线程读（autosell_tick）；
// 配置读写与计数共享内部互斥/原子，跨线程安全。本层不触碰游戏内存。

// 覆盖运行时配置快照。
void autosell_set_runtime_config(const autosell::Config& config);

// 当前配置快照（拷贝）。
autosell::Config autosell_get_runtime_config();

// 记录一次扫描结果：frame = 本次扫描帧号；sold/failed 为本次增量计数。
void autosell_note_scan(int64_t frame, int sold, int failed);

// 记录主线程逐帧宿主是否安装成功（M-10：宿主安装失败/未安装可经状态 JSON 查）。
void autosell_set_host_installed(bool installed);

// 记录按存档 sidecar 的加载/持久化状态（slot/loadedSlot/persisted 可经状态 JSON 查）。
void autosell_set_store_status(int slot, int loaded_slot, bool persisted);

namespace autosell {

struct StatusSnapshot {
    Config config;
    int64_t sold;
    int64_t failed;
    int64_t last_scan_frame;
    bool host_installed;
    int slot;
    int loaded_slot;
    bool persisted;
};

StatusSnapshot autosell_status_snapshot();

}  // namespace autosell

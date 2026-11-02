#pragma once

#include <cstdint>

#include "../../contracts/slot_data.h"
#include "../../contracts/autosell.h"

// 自动出售按存档配置持久化（sidecar section `autosell` v2，UTF-8 JSON）。
// section IO 由 save.slot_data API 执行；不触碰扩展背包状态。

// 注册/注销 `autosell` slot-data participant；注册者传入真实宿主 owner token。
bool autosell_store_register(qol::contracts::QolOwnerToken owner,
                             const qol::contracts::QolSlotDataApi* api);
void autosell_store_unregister();

// Panel and JNI edits share one revision and retry path.
bool autosell_store_commit_config(const autosell::Config& config);
bool autosell_store_session_matches(int slot);
uint64_t autosell_store_session_epoch(int slot);
void autosell_store_reject_session();
void autosell_store_flush(int64_t frame);

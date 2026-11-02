#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "contracts/game_host.h"
#include "plugins/save_backup/bundle.h"

// save_backup port：游戏存档加解密/读取端口的窄包装。
// 只负责把 contracts 函数指针转成语义调用；线程契约由调用方保证（写/读槽仅主线程）。

namespace save_backup {
namespace port {

bool save_api_ready();
bool encrypt_available();
const char* hub_key();
// SAVE_LoadData 解密指定槽；成功时 plain 为完整明文（调用方负责后续解析）。
bool load_plain(int slot, std::vector<uint8_t>& plain);
// 明文块 0 槽字节改写目标槽后 ENCRYPT_Process2(mode=0) 加密；成功时 cipher 为 len+3 密文。
bool encrypt_plain_to_slot(int slot, const std::vector<uint8_t>& plain,
                           std::vector<uint8_t>& cipher);
void free_plain(void* data);
void mark_stale();
// ENCRYPT_Process2 原语（wh4 容器解密/加密回调也走这里）。
int encrypt_process(void* data, int len, int mode, const char* key);
const char* warehouse_state_name(qol::contracts::QolTargetFeatureState state);
bool warehouse_blob_usable_on_this_device(const uint8_t* data, size_t len, const char* key);
save_backup::WarehouseReencryptStatus warehouse_reencrypt_on_device(
    const save_backup::WarehouseFile& file, int slot, const char* key,
    std::vector<uint8_t>& out_cipher);

}  // namespace port
}  // namespace save_backup

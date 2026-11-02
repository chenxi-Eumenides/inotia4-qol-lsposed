#pragma once

#include <string>

#include "contracts/game_ports/character_write_ops_port.h"
#include "contracts/result.h"

namespace qol::bridge {

// JNI 旧 data_op_* 信封的逐字等价：成功 {"ok":true}；失败 {"ok":false,"error":"<op.error_message>"}，
// 空文案回退 "character write failed"。不调用 frame_cache_force_refresh——typed 原语成功路径
// 已刷新恰一次，helper 只拼信封。
inline std::string character_write_result_json(const QolResult& result,
                                               const contracts::QolCharacterWriteOpResult& op) {
    if (contracts::succeeded(result)) return "{\"ok\":true}";
    return std::string("{\"ok\":false,\"error\":\"") +
           (op.error_message_len != 0 ? op.error_message : "character write failed") + "\"}";
}

}  // namespace qol::bridge

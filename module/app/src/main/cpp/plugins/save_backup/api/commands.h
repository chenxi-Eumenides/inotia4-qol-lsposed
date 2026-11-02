#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// save_backup L6 HTTP/typed-command 契约：命令 id、payload 与票据 ACK。
// 与 `plugin.cpp` 的 handler 共享，host 测试直接覆盖这些纯函数。

namespace save_backup::api {

enum : uint32_t {
    kCmdExport = 0x53420101,
    kCmdImport = 0x53420102,
    kCmdDelete = 0x53420103,
};

struct ExportPayload {
    int32_t slot = -1;
};

struct ImportPayload {
    int32_t slot = -1;
    char checksum[13] = {};
};

struct DeletePayload {
    char checksum[13] = {};
};

inline std::string ticket_json(uint64_t ticket) {
    char body[64] = {};
    const int size = std::snprintf(body, sizeof(body), "{\"ok\":true,\"ticket\":%llu}",
                                   static_cast<unsigned long long>(ticket));
    return size > 0 ? std::string(body, static_cast<size_t>(size)) : std::string();
}

inline bool copy_result(const std::string& result, char* out, uint32_t capacity,
                        uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr || result.size() > capacity) return false;
    std::memcpy(out, result.data(), result.size());
    *out_size = static_cast<uint32_t>(result.size());
    return true;
}

}  // namespace save_backup::api

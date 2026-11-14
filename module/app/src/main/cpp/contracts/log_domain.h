#pragma once

#include <cstdint>

// 日志域枚举：唯一来源 contracts/qol_log_domains.inc。
// 插件通过 QolLogApi.write 的 int32_t domain 参数使用这些值；枚举顺序是日志 token 契约的一部分，
// 只追加、不插入、不重排。
enum class QolDomain : uint8_t {
#define QOL_LOG_DOMAIN(name, token) name,
#include "qol_log_domains.inc"
#undef QOL_LOG_DOMAIN
    kDomainCount,
};

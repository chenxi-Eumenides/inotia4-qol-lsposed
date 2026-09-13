#pragma once

#include "core/native/qol_log.h"

// 统一日志系统 P2/P3：扩展背包所有日志经统一 native 日志系统单写者输出。
// 保留 VIRTBAG_LOG 同名宏（355 处调用点无需改动）；级别沿用迁移前语义（原
// extension_bag_log 恒 INFO）。需要更高/更低级别时使用对应变体。
#define VIRTBAG_LOG(...) QOL_LOG_INFO(QolDomain::kExtensionBag, __VA_ARGS__)
#define VIRTBAG_LOG_WARN(...) QOL_LOG_WARN(QolDomain::kExtensionBag, __VA_ARGS__)
#define VIRTBAG_LOG_ERROR(...) QOL_LOG_ERROR(QolDomain::kExtensionBag, __VA_ARGS__)
#define VIRTBAG_LOG_DEBUG(...) QOL_LOG_DEBUG(QolDomain::kExtensionBag, __VA_ARGS__)

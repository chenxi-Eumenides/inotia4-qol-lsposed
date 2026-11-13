#pragma once
#include "infra/log/qol_log.h"

// item_lab 目录已在 contracts/qol_log_domain_dirs.inc 映射到 kItemLab；
// 使用 AUTO 宏在编译期推导域，调用点不出现域号。
#define ITEM_LAB_INFO(...) QOL_LOG_INFO_AUTO(__VA_ARGS__)
#define ITEM_LAB_WARN(...) QOL_LOG_WARN_AUTO(__VA_ARGS__)

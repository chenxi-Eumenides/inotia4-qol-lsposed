#pragma once

#include <jni.h>

#include <string>

#include "game/access/game_access.h"
#include "core/native/game_cache.h"
#include "game_dialog.h"
#include "game_inventory.h"
#include "api/native/game_save.h"
#include "plugins/save_backup/feature.h"
#include "game_shop.h"
#include "game/state/game_state.h"
#include "game_system.h"
#include "game_world.h"
#include "game/read/game_tiles.h"
#include "game/access/hive_iap.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/core.settings/panels/settings.h"
#include "plugins/save_backup/ui/panel.h"

#include "infra/log/qol_log.h"
#include "contracts/http.h"

// 统一日志：op_result 与 bridge 层沿用 MOVE_LOG 名称，接入 kApi 域。
#define MOVE_LOG(...) QOL_LOG_INFO(QolDomain::kApi, __VA_ARGS__)

template <typename T>
inline std::string str_of(T v) {
    return std::to_string(static_cast<long long>(v));
}

inline std::string str_of(const std::string& v) {
    return v;
}

inline jstring op_result(JNIEnv* env, const char* op, const std::string& argstr, const std::string& result) {
    MOVE_LOG("[%s] args={%s} -> %s", op, argstr.c_str(), result.c_str());
    return env->NewStringUTF(result.c_str());
}

#if QOL_KERNEL_ONLY
inline jstring kernel_only_result(JNIEnv* env) {
    return env->NewStringUTF("{\"ok\":false,\"error\":\"kernel-only\"}");
}
#endif

#pragma once

#include <jni.h>

#include <string>

#include "game_access.h"
#include "game_cache.h"
#include "game_character.h"
#include "game_dialog.h"
#include "game_inventory.h"
#include "game_party.h"
#include "game_quest.h"
#include "api/native/game_save.h"
#include "feature/save_backup/save_backup.h"
#include "game_shop.h"
#include "game_state.h"
#include "game_system.h"
#include "api/native/game_ui.h"
#include "game_world.h"
#include "game_tiles.h"
#include "feature/patch/game_patch.h"
#include "feature/ui/game_ui_exp.h"
#include "feature/ui/game_ui_settings.h"
#include "feature/ui/game_ui_savebackup.h"
#include "feature/ui/game_ui_gemcraft.h"
#include "feature/extension_bag/game_ui_virtbag.h"

#include "core/native/qol_log.h"

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

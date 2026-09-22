#include "gamebridge_internal.h"

#include "host/assembly.h"
#include "host/http_dispatch.h"

#include <cstring>
#include <mutex>

namespace {

std::string init_report_with_assembly() {
    std::string report = data_init_report();
    if (report.empty() || report.back() != '}') return report;

    const qol::host::AssemblyReport assembly = qol::host::assembly_report();
    report.pop_back();
    report += ",\"assembly\":{";
    report += "\"registered_plugins\":" + std::to_string(assembly.registered_plugins);
    report += ",\"registration_failures\":" + std::to_string(assembly.registration_failures);
    report += ",\"install_failures\":" + std::to_string(assembly.install_failures);
    report += ",\"kernel_only\":" + std::string(assembly.kernel_only ? "true" : "false");
    report += ",\"initialized\":" + std::string(assembly.initialized ? "true" : "false");
    report += "}}";
    return report;
}

}  // namespace

QolResult bridge_http_game_frame_query(const qol::contracts::HttpReq*,
                                       qol::contracts::HttpRes* response, void*) {
    if (response == nullptr || response->body == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid http response", 22);
    }
    const std::string body = "{\"frame\":" + std::to_string(data_frame_count()) + "}";
    if (response->body_capacity < body.size()) {
        return qol::contracts::error(kQolErrBlocked, "http response buffer too small", 31);
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = 200;
    return qol::contracts::ok();
}

QolResult bridge_register_game_frame_handler() {
    static std::mutex mutex;
    static bool registered = false;
    std::lock_guard<std::mutex> lock(mutex);
    if (registered) return qol::contracts::ok();
    const QolResult result = qol::host::http_dispatch_register_builtin_query(
        qol::contracts::HttpMethod::kGet, "/api/system/game_frame", "api.system",
        &bridge_http_game_frame_query, nullptr);
    if (qol::contracts::succeeded(result) || result.code == kQolErrConflict) registered = true;
    return result;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetFrameCount(JNIEnv*, jclass) {
    return static_cast<jlong>(data_frame_count());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetActiveQuest(JNIEnv*, jclass) {
    return data_active_quest();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeQuestList(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_quest_list_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeQuestCompleted(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_quest_completed_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeQuestActive(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_quest_active_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeCurrentSaveSlot(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_current_save_slot_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetInitReport(JNIEnv* env, jclass) {
    return env->NewStringUTF(init_report_with_assembly().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetPlayerJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_player_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetPartyJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_party_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetInventoryJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_inventory_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetMapJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_map_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetTilesJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(build_tiles_json().c_str());
}

// P0#瓦片矩阵：Kotlin 读取 assets maps/tiles.json 后传入，native 解析缓存
extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetTilesData(JNIEnv* env, jclass, jstring json) {
    const char* j = json != nullptr ? env->GetStringUTFChars(json, nullptr) : nullptr;
    if (j == nullptr) return JNI_FALSE;
    set_static_tiles(std::string(j));
    env->ReleaseStringUTFChars(json, j);
    return static_tiles_ready() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetUnitsJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_units_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetEnemiesJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_enemies_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetInteractivesJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_interactives_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetGamestateJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_gamestate_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetDebugUiJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_debug_ui_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetSnapshotJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_snapshot_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetSkillsJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_skills_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetMercenariesJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_mercenaries_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetDropsJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_drops_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetPathJson(JNIEnv* env, jclass, jint tx, jint ty) {
    return env->NewStringUTF(data_path_json(static_cast<int>(tx), static_cast<int>(ty)).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeDistanceJson(JNIEnv* env, jclass, jint tx, jint ty) {
    return env->NewStringUTF(data_distance_json(static_cast<int32_t>(tx), static_cast<int32_t>(ty)).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeDebugPathJson(JNIEnv* env, jclass, jint tx, jint ty) {
    return env->NewStringUTF(data_debug_path_json(static_cast<int32_t>(tx), static_cast<int32_t>(ty)).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeDebugItemRawJson(JNIEnv* env, jclass, jint bag, jint slot) {
    return env->NewStringUTF(data_debug_item_raw_json(static_cast<int32_t>(bag), static_cast<int32_t>(slot)).c_str());
}

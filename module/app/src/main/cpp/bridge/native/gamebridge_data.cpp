#include "gamebridge_internal.h"

#include "game/access/world.h"
#include "host/adapters.h"
#include "host/assembly.h"
#include "host/legacy_json_builders.h"
#include "host/module_health.h"
#include "game/adapter_ports/quest_port.h"
#include "game/access/save.h"
#include "game/read/item_class.h"
#include "contracts/module_health.h"
#include "infra/capabilities.h"
#include "bridge/native/module_health_read.h"

#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <mutex>
#include <string>
#include <unordered_map>

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

std::string module_health_json(const qol::contracts::QolModuleHealthSnapshot& snapshot) {
    return "{\"revision\":" + std::to_string(snapshot.revision) +
           ",\"known_mask\":" + std::to_string(snapshot.known_mask) +
           ",\"failed_mask\":" + std::to_string(snapshot.failed_mask) +
           ",\"main_menu_seen\":" +
           std::string(snapshot.main_menu_seen != 0 ? "true" : "false") + "}";
}

struct ModuleHealthCallbackContext : std::enable_shared_from_this<ModuleHealthCallbackContext> {
    JavaVM* vm = nullptr;
    jclass callbacks = nullptr;
    jmethodID method = nullptr;

    ~ModuleHealthCallbackContext() {
        if (vm == nullptr || callbacks == nullptr) return;
        JNIEnv* env = nullptr;
        bool attached = false;
        if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
            if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
                QOL_LOG_ERROR(QolDomain::kCore,
                              "module health callback could not attach to release class ref");
                return;
            }
            attached = true;
        }
        env->DeleteGlobalRef(callbacks);
        if (attached) vm->DetachCurrentThread();
    }
};

std::mutex g_module_health_callbacks_mutex;
std::unordered_map<uint64_t, std::shared_ptr<ModuleHealthCallbackContext>>
    g_module_health_callbacks;

void clear_java_exception(JNIEnv* env, const char* operation) {
    if (env == nullptr || !env->ExceptionCheck()) return;
    QOL_LOG_ERROR(QolDomain::kCore, "module health callback JNI failure during %s", operation);
    env->ExceptionDescribe();
    env->ExceptionClear();
}

void deliver_module_health(void* raw_context,
                           const qol::contracts::QolModuleHealthSnapshot& snapshot) {
    if (raw_context == nullptr) return;
    auto* const raw = static_cast<ModuleHealthCallbackContext*>(raw_context);
    std::shared_ptr<ModuleHealthCallbackContext> context;
    try {
        context = raw->shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        return;
    }

    JNIEnv* env = nullptr;
    bool attached = false;
    if (context->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (context->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            QOL_LOG_ERROR(QolDomain::kCore, "module health callback thread attach failed");
            return;
        }
        attached = true;
    }

    std::string json_payload;
    try {
        json_payload = module_health_json(snapshot);
    } catch (const std::bad_alloc&) {
        QOL_LOG_ERROR(QolDomain::kCore, "module health callback JSON allocation failed");
        if (attached) context->vm->DetachCurrentThread();
        return;
    }
    jstring json = env->NewStringUTF(json_payload.c_str());
    if (json != nullptr) {
        env->CallStaticVoidMethod(context->callbacks, context->method, json);
        env->DeleteLocalRef(json);
        clear_java_exception(env, "onModuleHealthChanged");
    } else {
        clear_java_exception(env, "NewStringUTF");
    }
    if (attached) context->vm->DetachCurrentThread();
}

constexpr char kModuleHealthUnavailable[] = "{\"ok\":false,\"error\":\"plugin unavailable\"}";

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetFrameCount(JNIEnv*, jclass) {
    return static_cast<jlong>(data_frame_count());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetActiveQuest(JNIEnv*, jclass) {
    int32_t active_quest_id = -1;
    const auto& quest = qol::game::quest_port();
    if (quest.read.active_quest_id != nullptr) {
        (void)quest.read.active_quest_id(&active_quest_id);
    }
    return active_quest_id;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeQuestList(JNIEnv* env, jclass) {
    return env->NewStringUTF(qol::host::legacy_data_quest_list_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeQuestCompleted(JNIEnv* env, jclass) {
    return env->NewStringUTF(qol::host::legacy_data_quest_completed_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeQuestActive(JNIEnv* env, jclass) {
    return env->NewStringUTF(qol::host::legacy_data_quest_active_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeCurrentSaveSlot(JNIEnv* env, jclass) {
    return env->NewStringUTF(qol::game::current_save_slot_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetInitReport(JNIEnv* env, jclass) {
    return env->NewStringUTF(init_report_with_assembly().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeReadModuleHealth(JNIEnv* env, jclass) {
    qol::contracts::QolModuleHealthSnapshot snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    const void* const iface = qol::infra::require_capability(
        qol::contracts::kModuleHealthCapabilityId,
        QolVersionRange{qol::contracts::kModuleHealthCapabilityVersion.major,
                        qol::contracts::kModuleHealthCapabilityVersion.minor});
    const QolResult result = qol::bridge::read_module_health_snapshot(
        iface, &qol::host::g_host_sdk, &snapshot);
    if (!qol::contracts::succeeded(result))
        return env->NewStringUTF(kModuleHealthUnavailable);
    return env->NewStringUTF(module_health_json(snapshot).c_str());
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_inotia4_qol_NativeBridge_nativeModuleHealthSubscribe(JNIEnv* env, jclass,
                                                               jclass callbacks) {
    if (env == nullptr || callbacks == nullptr) return 0;
    std::shared_ptr<ModuleHealthCallbackContext> context;
    try {
        context = std::make_shared<ModuleHealthCallbackContext>();
    } catch (const std::bad_alloc&) {
        return 0;
    }
    if (env->GetJavaVM(&context->vm) != JNI_OK) return 0;
    context->method = env->GetStaticMethodID(
        callbacks, "onModuleHealthChanged", "(Ljava/lang/String;)V");
    if (context->method == nullptr) {
        clear_java_exception(env, "GetStaticMethodID");
        return 0;
    }
    context->callbacks = static_cast<jclass>(env->NewGlobalRef(callbacks));
    if (context->callbacks == nullptr) {
        clear_java_exception(env, "NewGlobalRef");
        return 0;
    }

    uint64_t subscription_id = 0;
    const QolResult subscribed = qol::host::module_health_subscribe(
        &deliver_module_health, context.get(), &subscription_id);
    if (!qol::contracts::succeeded(subscribed) || subscription_id == 0 ||
        subscription_id > static_cast<uint64_t>(std::numeric_limits<jlong>::max())) {
        if (subscription_id != 0) (void)qol::host::module_health_unsubscribe(subscription_id);
        return 0;
    }

    bool inserted = false;
    try {
        std::lock_guard<std::mutex> lock(g_module_health_callbacks_mutex);
        inserted = g_module_health_callbacks.emplace(subscription_id, context).second;
    } catch (const std::bad_alloc&) {
        (void)qol::host::module_health_unsubscribe(subscription_id);
        return 0;
    }
    if (!inserted) {
        (void)qol::host::module_health_unsubscribe(subscription_id);
        return 0;
    }
    return static_cast<jlong>(subscription_id);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeModuleHealthUnsubscribe(JNIEnv*, jclass,
                                                                 jlong subscription_id) {
    if (subscription_id <= 0) return JNI_FALSE;
    const uint64_t id = static_cast<uint64_t>(subscription_id);
    {
        std::lock_guard<std::mutex> lock(g_module_health_callbacks_mutex);
        if (g_module_health_callbacks.find(id) == g_module_health_callbacks.end())
            return JNI_FALSE;
    }
    const QolResult result = qol::host::module_health_unsubscribe(id);
    if (!qol::contracts::succeeded(result)) return JNI_FALSE;
    {
        std::lock_guard<std::mutex> lock(g_module_health_callbacks_mutex);
        g_module_health_callbacks.erase(id);
    }
    return JNI_TRUE;
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
    return env->NewStringUTF(qol::game::build_tiles_json().c_str());
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
    return env->NewStringUTF(qol::host::legacy_data_debug_ui_json().c_str());
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
    return env->NewStringUTF(qol::game::data_path_json(static_cast<int>(tx), static_cast<int>(ty)).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeDistanceJson(JNIEnv* env, jclass, jint tx, jint ty) {
    return env->NewStringUTF(qol::game::data_distance_json(static_cast<int32_t>(tx), static_cast<int32_t>(ty)).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeDebugPathJson(JNIEnv* env, jclass, jint tx, jint ty) {
    return env->NewStringUTF(qol::game::data_debug_path_json(static_cast<int32_t>(tx), static_cast<int32_t>(ty)).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeDebugItemRawJson(JNIEnv* env, jclass, jint bag, jint slot) {
#if QOL_KERNEL_ONLY
    return kernel_only_result(env);
#else
    return env->NewStringUTF(debug_item_raw_json(static_cast<int32_t>(bag), static_cast<int32_t>(slot)).c_str());
#endif
}

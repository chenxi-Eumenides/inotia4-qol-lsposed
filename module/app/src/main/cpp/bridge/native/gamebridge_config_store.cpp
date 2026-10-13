// Path-only config bootstrap. Kotlin supplies the external files directory before
// NativeBridge init; assembly performs Store init/seed once all descriptors are registered.
#include "host/config_platform_bridge.h"
#include "host/features/production.h"
#include "host/features/receipt_formatter.h"

#include <jni.h>

#include <string>

namespace {
bool java_string(JNIEnv* env, jstring value, std::string* out) {
    if (env == nullptr || value == nullptr || out == nullptr) return false;
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) return false;
    out->assign(chars);
    env->ReleaseStringUTFChars(value, chars);
    return true;
}

jstring receipt_or_error(JNIEnv* env, const qol::contracts::FeatureReceipt& receipt) {
    const std::string json = qol::host::feature_receipt_json(receipt);
    return env->NewStringUTF(json.c_str());
}
}  // namespace

extern "C" JNIEXPORT jint JNICALL
Java_com_inotia4_qol_NativeBridge_nativeConfigStoreBootstrap(JNIEnv* env, jclass, jstring directory) {
    if (directory == nullptr) return -1;
    const char* path = env->GetStringUTFChars(directory, nullptr);
    if (path == nullptr) return -1;
    const jint status = qol::host::config_store_bootstrap(path);
    env->ReleaseStringUTFChars(directory, path);
    return status;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativePluginConfigRequestIfRevision(
    JNIEnv* env, jclass, jstring plugin_id, jstring key, jstring value_json,
    jlong expected_revision) {
    if (env == nullptr) return nullptr;
    qol::contracts::FeatureReceipt receipt{
        0, 0, qol::contracts::FeatureAdmission::kRejected,
        qol::contracts::FeaturePersistence::kFailed,
        qol::contracts::FeatureActual::kNotSubmitted, kQolErrInvalidArgument};
    if (expected_revision < 0) return receipt_or_error(env, receipt);
    std::string plugin;
    std::string config_key;
    std::string value;
    if (!java_string(env, plugin_id, &plugin) || !java_string(env, key, &config_key) ||
        !java_string(env, value_json, &value)) return receipt_or_error(env, receipt);
    receipt = {};
    const auto result = qol::host::production_feature_controller().request_set_config_if_revision(
        plugin.c_str(), config_key.c_str(), value.data(), static_cast<uint32_t>(value.size()),
        static_cast<uint64_t>(expected_revision), &receipt);
    if (!qol::contracts::succeeded(result) && receipt.error_code == 0)
        receipt.error_code = result.code;
    return receipt_or_error(env, receipt);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativePluginConfigReceipt(JNIEnv* env, jclass,
                                                            jlong request_id) {
    if (env == nullptr || request_id <= 0) return env != nullptr
        ? env->NewStringUTF("{\"ok\":false,\"error_code\":1}") : nullptr;
    qol::contracts::FeatureReceipt receipt{};
    const auto result = qol::host::production_feature_controller().receipt(
        static_cast<uint64_t>(request_id), &receipt);
    if (!qol::contracts::succeeded(result)) {
        const std::string error = "{\"ok\":false,\"error_code\":" +
            std::to_string(result.code) + "}";
        return env->NewStringUTF(error.c_str());
    }
    return receipt_or_error(env, receipt);
}

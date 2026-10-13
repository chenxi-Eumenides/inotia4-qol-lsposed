#include "gamebridge_internal.h"

#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "host/http_dispatch.h"
#include "host/http_endpoint_binder.h"
#include "host/features/production.h"
#include "infra/config.h"

namespace {

std::string read_string(JNIEnv* env, jstring value, bool required, bool* ok) {
    if (value == nullptr) {
        if (required) *ok = false;
        return {};
    }
    const char* text = env->GetStringUTFChars(value, nullptr);
    if (text == nullptr) {
        *ok = false;
        return {};
    }
    const std::string result(text);
    env->ReleaseStringUTFChars(value, text);
    return result;
}

void append_json_string(std::string& out, const char* data, uint32_t size) {
    for (uint32_t i = 0; i < size; ++i) {
        const unsigned char ch = static_cast<unsigned char>(data[i]);
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[ch >> 4];
                    out += kHex[ch & 0x0f];
                } else {
                    out += static_cast<char>(ch);
                }
                break;
        }
    }
}

std::string dispatch_result_json(const qol::contracts::HttpRes& response) {
    std::string result = "{\"status\":" + std::to_string(response.status) + ",\"body\":\"";
    if (response.body_size > 0 && response.body != nullptr) {
        result.reserve(result.size() + response.body_size + 64);
        append_json_string(result, reinterpret_cast<const char*>(response.body), response.body_size);
    }
    result += "\"}";
    return result;
}

}  // namespace

// Frozen compatibility config setters. Global config writes must use core.settings' native
// single-key owner-token path; these exports cannot mutate configuration.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetOpEnabled(JNIEnv*, jclass, jboolean) {
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeConfigSetString(JNIEnv* env, jclass, jstring key,
                                                       jstring value) {
    (void)env;
    (void)key;
    (void)value;
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeConfigSetInt(JNIEnv* env, jclass, jstring key, jint) {
    (void)env;
    (void)key;
    return JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeHttpDispatch(JNIEnv* env, jclass, jint method,
                                                     jstring path, jstring query, jstring body) {
    bool input_ok = true;
    const std::string path_value = read_string(env, path, true, &input_ok);
    const std::string query_value = read_string(env, query, false, &input_ok);
    const std::string body_value = read_string(env, body, false, &input_ok);
    constexpr uint32_t kMaxResponseCapacity = 32 * 1024 * 1024;
    if (!input_ok) {
        std::vector<uint8_t> output(128);
        qol::contracts::HttpRes response{output.data(), static_cast<uint32_t>(output.size()), 0, 0};
        response.status = 400;
        const std::string message = "{\"ok\":false,\"error\":\"invalid input\"}";
        std::memcpy(output.data(), message.data(), message.size());
        response.body_size = static_cast<uint32_t>(message.size());
        return env->NewStringUTF(dispatch_result_json(response).c_str());
    }
    const qol::contracts::HttpReq request{
        static_cast<qol::contracts::HttpMethod>(method),
        path_value.c_str(),
        static_cast<uint32_t>(path_value.size()),
        reinterpret_cast<const uint8_t*>(body_value.data()),
        static_cast<uint32_t>(body_value.size()),
        query_value.c_str(),
        static_cast<uint32_t>(query_value.size()),
        nullptr,
        0,
    };
    std::vector<uint8_t> output;
    uint16_t response_status = 0;
    uint32_t response_size = 0;
    const auto dispatched = qol::host::http_dispatch_dynamic(
        &request, &output, kMaxResponseCapacity, &response_status, &response_size);
    if (!qol::contracts::succeeded(dispatched) && response_size == 0) {
        return env->NewStringUTF("{\"status\":500,\"body\":\"{\\\"ok\\\":false,\\\"error\\\":\\\"oom\\\"}\"}");
    }
    qol::contracts::HttpRes response{output.data(), static_cast<uint32_t>(output.size()),
                                     response_size, response_status};
    (void)dispatched;
    try {
        const std::string result = dispatch_result_json(response);
        jstring output_string = env->NewStringUTF(result.c_str());
        if (output_string != nullptr) return output_string;
    } catch (const std::bad_alloc&) {
        return env->NewStringUTF("{\"status\":500,\"body\":\"{\\\"ok\\\":false,\\\"error\\\":\\\"oom\\\"}\"}");
    }
    return env->NewStringUTF("{\"status\":500,\"body\":\"{\\\"ok\\\":false,\\\"error\\\":\\\"oom\\\"}\"}");
}

extern "C" JNIEXPORT jint JNICALL
Java_com_inotia4_qol_NativeBridge_nativeHttpRouteEnter(JNIEnv* env, jclass, jint method,
                                                       jstring path, jlongArray lease_out) {
    if (lease_out == nullptr || env->GetArrayLength(lease_out) < 1) return kQolErrInvalidArgument;
    const jlong empty_lease = 0;
    env->SetLongArrayRegion(lease_out, 0, 1, &empty_lease);
    if (env->ExceptionCheck()) return kQolErrInvalidArgument;
    if (method != static_cast<jint>(qol::contracts::HttpMethod::kGet) &&
        method != static_cast<jint>(qol::contracts::HttpMethod::kPost))
        return kQolErrInvalidArgument;
    bool input_ok = true;
    const std::string path_value = read_string(env, path, true, &input_ok);
    if (!input_ok || path_value.empty()) return kQolErrInvalidArgument;

    uint64_t lease = 0;
    const QolResult result = qol::host::http_endpoint_binder_route_enter(
        static_cast<qol::contracts::HttpMethod>(method), path_value.c_str(),
        static_cast<uint32_t>(path_value.size()), &qol::host::security_op_enabled, &lease);
    if (!qol::contracts::succeeded(result)) return result.code;
    const jlong value = static_cast<jlong>(lease);
    env->SetLongArrayRegion(lease_out, 0, 1, &value);
    if (env->ExceptionCheck()) {
        if (lease != 0) (void)qol::host::http_endpoint_binder_route_leave(lease);
        return kQolErrInvalidArgument;
    }
    return kQolOk;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_inotia4_qol_NativeBridge_nativeHttpRouteLeave(JNIEnv*, jclass, jlong lease) {
    if (lease == 0) return kQolOk;
    if (lease < 0) return kQolErrInvalidArgument;
    const QolResult result = qol::host::http_endpoint_binder_route_leave(
        static_cast<uint64_t>(lease));
    return result.code;
}

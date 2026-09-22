#include "gamebridge_internal.h"

#include <array>
#include <cstring>
#include <string>

#include "host/http_dispatch.h"

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

void append_json_string(std::string& out, const std::string& value) {
    for (const unsigned char ch : value) {
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
    const std::string body(reinterpret_cast<const char*>(response.body), response.body_size);
    std::string result = "{\"status\":" + std::to_string(response.status) + ",\"body\":\"";
    append_json_string(result, body);
    result += "\"}";
    return result;
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeHttpDispatch(JNIEnv* env, jclass, jint method,
                                                     jstring path, jstring query, jstring body) {
    bool input_ok = true;
    const std::string path_value = read_string(env, path, true, &input_ok);
    const std::string query_value = read_string(env, query, false, &input_ok);
    const std::string body_value = read_string(env, body, false, &input_ok);
    std::array<uint8_t, 65536> output{};
    qol::contracts::HttpRes response{output.data(), static_cast<uint32_t>(output.size()), 0, 0};
    if (!input_ok) {
        response.status = 400;
        const std::string message = "{\"ok\":false,\"error\":\"invalid input\"}";
        std::memcpy(output.data(), message.data(), message.size());
        response.body_size = static_cast<uint32_t>(message.size());
        return env->NewStringUTF(dispatch_result_json(response).c_str());
    }
    (void)bridge_register_game_frame_handler();
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
    (void)qol::host::http_dispatch_api().dispatch(&request, &response);
    const std::string result = dispatch_result_json(response);
    jstring output_string = env->NewStringUTF(result.c_str());
    if (output_string != nullptr) return output_string;
    return env->NewStringUTF("{\"status\":500,\"body\":\"{\\\"ok\\\":false,\\\"error\\\":\\\"oom\\\"}\"}");
}

// HTTP owner 静态 enrichment 下发：Kotlin StaticData 在启动期构造名称表/片段表，native 侧
// 存入对应 adapter port 后由 owner 快照拼接。薄层只做字符串转换，不解析业务内容。
#include "gamebridge_internal.h"

#include "game/adapter_ports/item_port.h"
#include "game/adapter_ports/quest_port.h"

namespace {

std::string read_string(JNIEnv* env, jstring value) {
    if (value == nullptr) return {};
    const char* text = env->GetStringUTFChars(value, nullptr);
    if (text == nullptr) return {};
    const std::string result(text);
    env->ReleaseStringUTFChars(value, text);
    return result;
}

}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetItemStaticData(JNIEnv* env, jclass, jstring payload) {
    const std::string text = read_string(env, payload);
    if (text.empty()) return;
    qol::game::item_port_load_static_text(text.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetQuestStaticData(JNIEnv* env, jclass, jstring payload) {
    const std::string text = read_string(env, payload);
    if (text.empty()) return;
    qol::game::quest_port_load_static_fragments(text.c_str());
}

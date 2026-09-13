// gamebridge_log.cpp —— 统一日志系统 JNI 薄层。
//
// 仅做 JNI 参数转换；格式化与发射逻辑在 core/native/qol_log.*。
// Kotlin 侧方法名已冻结：nativeQolLogInit / nativeQolLogWrite / nativeQolLogSetDebugEnabled。

#include <jni.h>

#include "core/native/qol_log.h"

namespace {

// UTF 字符串 RAII：构造时获取，析构时释放；null 参数不获取。
class UtfChars {
public:
    UtfChars(JNIEnv* env, jstring str) : env_(env), str_(str) {
        if (str_ != nullptr) chars_ = env_->GetStringUTFChars(str_, nullptr);
    }
    ~UtfChars() {
        if (chars_ != nullptr) env_->ReleaseStringUTFChars(str_, chars_);
    }
    UtfChars(const UtfChars&) = delete;
    UtfChars& operator=(const UtfChars&) = delete;

    const char* get() const { return chars_; }

private:
    JNIEnv* env_;
    jstring str_;
    const char* chars_ = nullptr;
};

}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_inotia4_qol_NativeBridge_nativeQolLogInit(JNIEnv*, jclass) {
    qol_log_init();
}

extern "C" JNIEXPORT void JNICALL
Java_com_inotia4_qol_NativeBridge_nativeQolLogWrite(
    JNIEnv* env, jclass, jint level, jstring domain, jstring src, jstring msg) {
    if (domain == nullptr || src == nullptr || msg == nullptr) return;
    UtfChars domain_chars(env, domain);
    UtfChars src_chars(env, src);
    UtfChars msg_chars(env, msg);
    if (domain_chars.get() == nullptr || src_chars.get() == nullptr ||
        msg_chars.get() == nullptr) {
        return;
    }
    qol_log_write_kotlin(static_cast<QolLogLevel>(level), qol_domain_from_token(domain_chars.get()),
                         src_chars.get(), msg_chars.get());
}

extern "C" JNIEXPORT void JNICALL
Java_com_inotia4_qol_NativeBridge_nativeQolLogSetDebugEnabled(JNIEnv*, jclass, jboolean enabled) {
    qol_log_set_debug_enabled(enabled == JNI_TRUE);
}

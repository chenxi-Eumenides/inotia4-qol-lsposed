// gamebridge_store.cpp —— module sidecar 容器 JNI 薄层（infra/store）。
//
// 只做参数/返回值转换：jstring/jbyteArray ↔ std::string/std::vector<uint8_t>，
// Kotlin Map/Set ↔ 平行数组；MSAV 字节格式、原子写、last-good 回退与 quarantine
// 全部在 infra/store。返回码直接映射 infra/store::Status（0=ok），Kotlin 侧据此
// 保持 ModuleSaveStore 的公开语义。
//
// Kotlin 侧 external 方法名见 NativeBridge.kt（nativeStore<Action>）；store 初始化
// 仍由 nativeSaveBackupInit 注入 externalFilesDir（infra/store::init）。

#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "infra/store.h"

namespace {

int status_code(qol::infra::store::Status status) {
    return static_cast<int>(status);
}

// jbyteArray → vector；null 视为空 payload。读取失败返回 false（不抛异常）。
bool to_bytes(JNIEnv* env, jbyteArray array, std::vector<uint8_t>* out) {
    out->clear();
    if (array == nullptr) return true;
    const jsize len = env->GetArrayLength(array);
    if (len <= 0) return true;
    out->resize(static_cast<std::size_t>(len));
    env->GetByteArrayRegion(array, 0, len, reinterpret_cast<jbyte*>(out->data()));
    return !env->ExceptionCheck();
}

// vector → 新 jbyteArray；分配失败返回 null，由调用方按失败处理。
jbyteArray from_bytes(JNIEnv* env, const std::vector<uint8_t>& bytes) {
    const jsize len = static_cast<jsize>(bytes.size());
    jbyteArray result = env->NewByteArray(len);
    if (result == nullptr) return nullptr;
    if (len > 0) {
        env->SetByteArrayRegion(result, 0, len,
                                reinterpret_cast<const jbyte*>(bytes.data()));
    }
    return result;
}

// jobjectArray<String> → vector<string>。
bool to_names(JNIEnv* env, jobjectArray array, std::vector<std::string>* out) {
    out->clear();
    if (array == nullptr) return true;
    const jsize count = env->GetArrayLength(array);
    out->reserve(static_cast<std::size_t>(count));
    for (jsize i = 0; i < count; ++i) {
        jstring item = static_cast<jstring>(env->GetObjectArrayElement(array, i));
        if (item == nullptr) return false;
        const char* chars = env->GetStringUTFChars(item, nullptr);
        if (chars == nullptr) {
            env->DeleteLocalRef(item);
            return false;
        }
        out->emplace_back(chars);
        env->ReleaseStringUTFChars(item, chars);
        env->DeleteLocalRef(item);
    }
    return !env->ExceptionCheck();
}

// 平行数组（names/versions/payloads）→ ReplaceMap，保持 Kotlin Map 的插入序。
bool to_replace_map(JNIEnv* env, jobjectArray names, jintArray versions, jobjectArray payloads,
                    qol::infra::store::ReplaceMap* out) {
    out->clear();
    if (names == nullptr || payloads == nullptr || versions == nullptr) return false;
    const jsize count = env->GetArrayLength(names);
    if (env->GetArrayLength(payloads) != count || env->GetArrayLength(versions) != count) {
        return false;
    }
    std::vector<jint> version_values(static_cast<std::size_t>(count));
    if (count > 0) {
        env->GetIntArrayRegion(versions, 0, count, version_values.data());
    }
    for (jsize i = 0; i < count; ++i) {
        jstring name = static_cast<jstring>(env->GetObjectArrayElement(names, i));
        if (name == nullptr) return false;
        const char* chars = env->GetStringUTFChars(name, nullptr);
        if (chars == nullptr) {
            env->DeleteLocalRef(name);
            return false;
        }
        qol::infra::store::Section section;
        section.version =
            static_cast<uint16_t>(version_values[static_cast<std::size_t>(i)] & 0xFFFF);
        jbyteArray payload = static_cast<jbyteArray>(env->GetObjectArrayElement(payloads, i));
        const bool payload_ok = to_bytes(env, payload, &section.payload);
        if (payload != nullptr) env->DeleteLocalRef(payload);
        if (!payload_ok) {
            env->ReleaseStringUTFChars(name, chars);
            env->DeleteLocalRef(name);
            return false;
        }
        out->emplace_back(std::string(chars), std::move(section));
        env->ReleaseStringUTFChars(name, chars);
        env->DeleteLocalRef(name);
    }
    return !env->ExceptionCheck();
}

}  // namespace

// store 是否可用（未 init 或目录无效 → false）。Kotlin isInitialized() 据此判定。
extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeStoreAvailable(JNIEnv*, jclass) {
    return qol::infra::store::available() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_inotia4_qol_NativeBridge_nativeStoreEnsureSlot(JNIEnv*, jclass, jint slot) {
    return status_code(qol::infra::store::ensure_slot(static_cast<int>(slot)));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_inotia4_qol_NativeBridge_nativeStoreResetSlot(JNIEnv*, jclass, jint slot) {
    return status_code(qol::infra::store::reset_slot(static_cast<int>(slot)));
}

// 读 section：成功返回 payload（可能是空数组）且 versionOut[0]=version；
// 未命中/非法/不可用返回 null（Kotlin readSection → null）。
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_inotia4_qol_NativeBridge_nativeStoreReadSection(JNIEnv* env, jclass, jint slot,
                                                         jstring name, jintArray versionOut) {
    if (name == nullptr || versionOut == nullptr || env->GetArrayLength(versionOut) < 1) {
        return nullptr;
    }
    const char* chars = env->GetStringUTFChars(name, nullptr);
    if (chars == nullptr) return nullptr;
    std::vector<uint8_t> payload;
    uint16_t version = 0;
    const auto status =
        qol::infra::store::read_section(static_cast<int>(slot), chars, &payload, &version);
    env->ReleaseStringUTFChars(name, chars);
    if (status != qol::infra::store::Status::kOk) return nullptr;
    jint version_value = static_cast<jint>(version);
    env->SetIntArrayRegion(versionOut, 0, 1, &version_value);
    return from_bytes(env, payload);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_inotia4_qol_NativeBridge_nativeStoreWriteSection(JNIEnv* env, jclass, jint slot,
                                                          jstring name, jint version,
                                                          jbyteArray payload) {
    if (name == nullptr) return status_code(qol::infra::store::Status::kBadName);
    const char* chars = env->GetStringUTFChars(name, nullptr);
    if (chars == nullptr) return status_code(qol::infra::store::Status::kIoError);
    std::vector<uint8_t> bytes;
    if (!to_bytes(env, payload, &bytes)) {
        env->ReleaseStringUTFChars(name, chars);
        return status_code(qol::infra::store::Status::kIoError);
    }
    const auto status = qol::infra::store::write_section(
        static_cast<int>(slot), chars, static_cast<uint16_t>(version & 0xFFFF), bytes.data(),
        bytes.size());
    env->ReleaseStringUTFChars(name, chars);
    return status_code(status);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_inotia4_qol_NativeBridge_nativeStoreRemoveSection(JNIEnv* env, jclass, jint slot,
                                                           jstring name) {
    if (name == nullptr) return status_code(qol::infra::store::Status::kBadName);
    const char* chars = env->GetStringUTFChars(name, nullptr);
    if (chars == nullptr) return status_code(qol::infra::store::Status::kIoError);
    const auto status = qol::infra::store::remove_section(static_cast<int>(slot), chars);
    env->ReleaseStringUTFChars(name, chars);
    return status_code(status);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_inotia4_qol_NativeBridge_nativeStoreReplaceSections(JNIEnv* env, jclass, jint slot,
                                                             jobjectArray names,
                                                             jintArray versions,
                                                             jobjectArray payloads,
                                                             jobjectArray removals) {
    qol::infra::store::ReplaceMap replacements;
    qol::infra::store::RemoveSet remove_set;
    if (!to_replace_map(env, names, versions, payloads, &replacements)) {
        return status_code(qol::infra::store::Status::kBadName);
    }
    if (!to_names(env, removals, &remove_set)) {
        return status_code(qol::infra::store::Status::kBadName);
    }
    return status_code(
        qol::infra::store::replace_sections(static_cast<int>(slot), replacements, remove_set));
}

// 主容器原始字节：可按该槽解码时返回；否则 null。
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_inotia4_qol_NativeBridge_nativeStoreReadContainer(JNIEnv* env, jclass, jint slot) {
    std::vector<uint8_t> bytes;
    if (qol::infra::store::read_container(static_cast<int>(slot), &bytes) !=
        qol::infra::store::Status::kOk) {
        return nullptr;
    }
    return from_bytes(env, bytes);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_inotia4_qol_NativeBridge_nativeStoreImportContainer(JNIEnv* env, jclass, jint slot,
                                                             jbyteArray payload) {
    std::vector<uint8_t> bytes;
    if (!to_bytes(env, payload, &bytes)) {
        return status_code(qol::infra::store::Status::kIoError);
    }
    if (bytes.empty()) return status_code(qol::infra::store::Status::kCorrupt);
    return status_code(qol::infra::store::import_container(static_cast<int>(slot), bytes.data(),
                                                           bytes.size()));
}

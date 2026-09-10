#include "feature/extension_bag/extension_bag_context.h"

#include <algorithm>
#include <cstring>

#include <android/log.h>

#include "feature/extension_bag/game_ui_virtbag.h"

namespace {

constexpr char kLogTag[] = "Inotia4VirtBag";

std::string occupied_item_summary(const virtual_bag::State& state) {
    std::string summary;
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            const virtual_bag::Item& item = state.items[bag][slot];
            if (item.category <= 0 || item.count <= 0) continue;
            if (!summary.empty()) summary += ',';
            summary += std::to_string(bag) + '/' + std::to_string(slot) + '=' +
                       std::to_string(item.category) + 'x' + std::to_string(item.count);
        }
    }
    return summary.empty() ? "empty" : summary;
}

bool call_save_callback(JNIEnv* env, jclass bridge_class, const char* method_name,
                        int slot, const char* transaction_id) {
    if (env == nullptr || bridge_class == nullptr || transaction_id == nullptr) return false;
    jmethodID method = env->GetStaticMethodID(
        bridge_class, method_name, "(ILjava/lang/String;)Ljava/lang/String;");
    if (method == nullptr) {
        env->ExceptionClear();
        return false;
    }
    jstring tx = env->NewStringUTF(transaction_id);
    if (tx == nullptr) return false;
    jstring result = static_cast<jstring>(
        env->CallStaticObjectMethod(bridge_class, method, slot, tx));
    env->DeleteLocalRef(tx);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    if (result == nullptr) return false;
    const char* utf = env->GetStringUTFChars(result, nullptr);
    const bool ok = utf != nullptr && std::strcmp(utf, "ok") == 0;
    if (utf != nullptr) env->ReleaseStringUTFChars(result, utf);
    env->DeleteLocalRef(result);
    return ok;
}

}

bool extension_bag_load_state_from_store(int slot) {
    JNIEnv* env = extension_bag_current_env();
    jclass bridge_class = extension_bag_bridge_class();
    virtual_bag::State* state = extension_bag_state();
    if (env == nullptr || bridge_class == nullptr || state == nullptr) return false;
    jmethodID method = env->GetStaticMethodID(bridge_class, "loadStateJson", "(I)Ljava/lang/String;");
    if (method == nullptr) {
        env->ExceptionClear();
        return false;
    }
    jstring result = static_cast<jstring>(env->CallStaticObjectMethod(bridge_class, method, slot));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    if (result == nullptr) return false;
    const char* utf = env->GetStringUTFChars(result, nullptr);
    bool parsed = false;
    if (utf != nullptr) {
        virtual_bag::PendingTransfer rejected_pending{};
        const bool has_pending = std::strstr(utf, "\"pending\":") != nullptr;
        const virtual_bag::PendingTransferParseResult pending_result =
            has_pending ? virtual_bag::parse_pending_transfer_result(utf, &rejected_pending)
                        : virtual_bag::PendingTransferParseResult::kOk;
        if (pending_result == virtual_bag::PendingTransferParseResult::kInvalidTransactionDomain) {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "pending isolated reason=invalid_transaction_domain direction=%u src=%u/%u dst=%u/%u",
                                static_cast<unsigned int>(rejected_pending.direction),
                                static_cast<unsigned int>(rejected_pending.src_bag),
                                static_cast<unsigned int>(rejected_pending.src_slot),
                                static_cast<unsigned int>(rejected_pending.dst_bag),
                                static_cast<unsigned int>(rejected_pending.dst_slot));
        } else if (pending_result == virtual_bag::PendingTransferParseResult::kMalformed) {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "pending isolated reason=malformed_pending_record");
        }
        parsed = virtual_bag::parse_state_json_at(
            utf, state, extension_bag_isolation_now_ms(), virtual_bag_category_uses_stack_count);
        if (has_pending && pending_result != virtual_bag::PendingTransferParseResult::kOk) {
            virtual_bag::IsolationRecord record{};
            record.valid = true;
            record.observed_at_ms = extension_bag_isolation_now_ms();
            if (pending_result == virtual_bag::PendingTransferParseResult::kInvalidTransactionDomain) {
                record.reason = virtual_bag::isolation_reason::kInvalidTransactionDomain;
                record.direction = rejected_pending.direction;
                record.src_bag = rejected_pending.src_bag;
                record.src_slot = rejected_pending.src_slot;
                record.dst_bag = rejected_pending.dst_bag;
                record.dst_slot = rejected_pending.dst_slot;
                std::strncpy(record.transaction_id, rejected_pending.transaction_id,
                             sizeof(record.transaction_id) - 1);
                record.payload_size = std::min<uint16_t>(
                    rejected_pending.payload_size,
                    static_cast<uint16_t>(rejected_pending.payload.size()));
                record.payload = rejected_pending.payload;
                record.source_payload_size = std::min<uint16_t>(
                    rejected_pending.source_payload_size,
                    static_cast<uint16_t>(rejected_pending.source_payload.size()));
                record.source_payload = rejected_pending.source_payload;
                record.detail = "pending_rejected_at_load";
            } else {
                record.reason = virtual_bag::isolation_reason::kInvalidPayload;
                record.detail = "malformed_pending_record";
            }
            virtual_bag::push_isolation(state, record);
        }
    }
    if (utf != nullptr) env->ReleaseStringUTFChars(result, utf);
    env->DeleteLocalRef(result);
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "sidecar load slot=%d parsed=%d items=%s", slot, parsed ? 1 : 0,
                        occupied_item_summary(*state).c_str());
    return parsed;
}

bool extension_bag_save_state_to_store(int slot) {
    JNIEnv* env = extension_bag_current_env();
    jclass bridge_class = extension_bag_bridge_class();
    virtual_bag::State* state = extension_bag_state();
    if (env == nullptr || bridge_class == nullptr || state == nullptr) return false;
    jmethodID method = env->GetStaticMethodID(bridge_class, "saveStateJson",
                                               "(ILjava/lang/String;)Ljava/lang/String;");
    if (method == nullptr) {
        env->ExceptionClear();
        return false;
    }
    const std::string json = virtual_bag::state_json(*state);
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "legacy sidecar save slot=%d items=%s", slot,
                        occupied_item_summary(*state).c_str());
    jstring payload = env->NewStringUTF(json.c_str());
    if (payload == nullptr) return false;
    jstring result = static_cast<jstring>(env->CallStaticObjectMethod(bridge_class, method, slot, payload));
    env->DeleteLocalRef(payload);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    if (result == nullptr) return false;
    const char* utf = env->GetStringUTFChars(result, nullptr);
    const bool ok = utf != nullptr && std::strcmp(utf, "ok") == 0;
    if (utf != nullptr) env->ReleaseStringUTFChars(result, utf);
    env->DeleteLocalRef(result);
    return ok;
}

bool extension_bag_prepare_save_to_store(int slot, const char* transaction_id) {
    JNIEnv* env = extension_bag_current_env();
    jclass bridge_class = extension_bag_bridge_class();
    virtual_bag::State* state = extension_bag_state();
    if (env == nullptr || bridge_class == nullptr || state == nullptr || transaction_id == nullptr) return false;
    jmethodID method = env->GetStaticMethodID(
        bridge_class, "prepareSave", "(ILjava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (method == nullptr) {
        env->ExceptionClear();
        return false;
    }
    const std::string json = virtual_bag::state_json(*state);
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "sidecar prepare slot=%d tx=%s items=%s", slot, transaction_id,
                        occupied_item_summary(*state).c_str());
    jstring tx = env->NewStringUTF(transaction_id);
    jstring payload = env->NewStringUTF(json.c_str());
    if (tx == nullptr || payload == nullptr) {
        if (tx != nullptr) env->DeleteLocalRef(tx);
        if (payload != nullptr) env->DeleteLocalRef(payload);
        return false;
    }
    jstring result = static_cast<jstring>(
        env->CallStaticObjectMethod(bridge_class, method, slot, tx, payload));
    env->DeleteLocalRef(tx);
    env->DeleteLocalRef(payload);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    if (result == nullptr) return false;
    const char* utf = env->GetStringUTFChars(result, nullptr);
    const bool ok = utf != nullptr && std::strcmp(utf, "ok") == 0;
    if (utf != nullptr) env->ReleaseStringUTFChars(result, utf);
    env->DeleteLocalRef(result);
    return ok;
}

bool extension_bag_commit_save_to_store(int slot, const char* transaction_id) {
    JNIEnv* env = extension_bag_current_env();
    jclass bridge_class = extension_bag_bridge_class();
    virtual_bag::State* state = extension_bag_state();
    if (env == nullptr || bridge_class == nullptr || state == nullptr || transaction_id == nullptr) {
        return false;
    }
    jmethodID method = env->GetStaticMethodID(
        bridge_class, "commitSave", "(ILjava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (method == nullptr) {
        env->ExceptionClear();
        return false;
    }
    const std::string json = virtual_bag::state_json(*state);
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "sidecar commit slot=%d tx=%s items=%s", slot, transaction_id,
                        occupied_item_summary(*state).c_str());
    jstring tx = env->NewStringUTF(transaction_id);
    jstring payload = env->NewStringUTF(json.c_str());
    if (tx == nullptr || payload == nullptr) {
        if (tx != nullptr) env->DeleteLocalRef(tx);
        if (payload != nullptr) env->DeleteLocalRef(payload);
        return false;
    }
    jstring result = static_cast<jstring>(
        env->CallStaticObjectMethod(bridge_class, method, slot, tx, payload));
    env->DeleteLocalRef(tx);
    env->DeleteLocalRef(payload);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    if (result == nullptr) return false;
    const char* utf = env->GetStringUTFChars(result, nullptr);
    const bool ok = utf != nullptr && std::strcmp(utf, "ok") == 0;
    if (utf != nullptr) env->ReleaseStringUTFChars(result, utf);
    env->DeleteLocalRef(result);
    return ok;
}

bool extension_bag_abort_known_failed_save(int slot, const char* transaction_id) {
    return call_save_callback(extension_bag_current_env(), extension_bag_bridge_class(),
                              "abortKnownFailedSave", slot, transaction_id);
}

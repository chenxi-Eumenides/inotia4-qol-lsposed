#include "save_port.h"

#include <cstring>

#include "contracts/result.h"
#include "../state/state.h"

namespace save_backup {
namespace port {
namespace {

uint16_t rd_u16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 static_cast<uint16_t>(p[1]) << 8);
}

}  // namespace

bool save_api_ready() {
    const auto ctx = state::snapshot();
    return ctx.save_facts != nullptr && ctx.save_facts->load_data != nullptr &&
           ctx.save_call != nullptr && ctx.save_call->write.free_data != nullptr;
}

bool encrypt_available() {
    const auto ctx = state::snapshot();
    return ctx.save_call != nullptr && ctx.save_call->write.encrypt != nullptr;
}

const char* hub_key() {
    const auto ctx = state::snapshot();
    if (ctx.save_facts == nullptr || ctx.save_facts->hub_key == nullptr) return nullptr;
    const char* key = nullptr;
    if (!qol::contracts::succeeded(ctx.save_facts->hub_key(&key))) return nullptr;
    return key;
}

bool load_plain(int slot, std::vector<uint8_t>& plain) {
    plain.clear();
    const auto ctx = state::snapshot();
    if (!save_api_ready()) return false;
    void* buf = nullptr;
    int len = 0;
    const QolResult result = ctx.save_facts->load_data(slot, &buf, &len);
    if (!qol::contracts::succeeded(result) || buf == nullptr || len < 14) {
        if (buf != nullptr) (void)ctx.save_call->write.free_data(buf);
        return false;
    }
    plain.assign(static_cast<const uint8_t*>(buf), static_cast<const uint8_t*>(buf) + len);
    (void)ctx.save_call->write.free_data(buf);
    return true;
}

bool encrypt_plain_to_slot(int slot, const std::vector<uint8_t>& plain,
                           std::vector<uint8_t>& cipher) {
    if (!encrypt_available() || plain.size() < 14) return false;
    const size_t block0 = 8 + rd_u16(plain.data() + 8);
    if (block0 + 1 > plain.size()) return false;
    std::vector<uint8_t> buf = plain;
    buf[block0] = static_cast<uint8_t>(slot);
    // ENCRYPT_Process2 加密就地追加 3 字节，需 len+3 容量。
    buf.resize(plain.size() + 3, 0);
    const char* key = hub_key();
    if (key == nullptr) return false;
    if (encrypt_process(buf.data(), static_cast<int>(plain.size()), 0, key) != 1) return false;
    cipher.swap(buf);
    return true;
}

void free_plain(void* data) {
    const auto ctx = state::snapshot();
    if (ctx.save_call != nullptr && ctx.save_call->write.free_data != nullptr && data != nullptr) {
        (void)ctx.save_call->write.free_data(data);
    }
}

void mark_stale() {
    const auto ctx = state::snapshot();
    if (ctx.save_call != nullptr && ctx.save_call->write.mark_stale != nullptr) {
        (void)ctx.save_call->write.mark_stale();
    }
}

int encrypt_process(void* data, int len, int mode, const char* key) {
    const auto ctx = state::snapshot();
    if (ctx.save_call == nullptr || ctx.save_call->write.encrypt == nullptr || data == nullptr ||
        len < 0 || key == nullptr) {
        return 0;
    }
    int result = 0;
    if (!qol::contracts::succeeded(
            ctx.save_call->write.encrypt(data, len, mode, key, &result))) {
        return 0;
    }
    return result;
}

namespace {

int warehouse_decrypt_mode1(void* buffer, int len, const char* key) {
    return encrypt_process(buffer, len, 1, key);
}

int warehouse_encrypt_mode0(void* buffer, int len, const char* key) {
    return encrypt_process(buffer, len, 0, key);
}

}  // namespace

const char* warehouse_state_name(qol::contracts::QolTargetFeatureState state) {
    switch (state) {
        case qol::contracts::QolTargetFeatureState::kUnknown: return "unknown";
        case qol::contracts::QolTargetFeatureState::kUnsupported: return "unsupported";
        case qol::contracts::QolTargetFeatureState::kAvailable: return "available";
        case qol::contracts::QolTargetFeatureState::kUnavailable: return "unavailable";
    }
    return "unknown";
}

bool warehouse_blob_usable_on_this_device(const uint8_t* data, size_t len, const char* key) {
    const save_backup::WarehouseDecryptFn decrypt =
        encrypt_available() ? &warehouse_decrypt_mode1 : nullptr;
    return save_backup::warehouse_blob_decryptable(data, len, key, decrypt);
}

save_backup::WarehouseReencryptStatus warehouse_reencrypt_on_device(
    const save_backup::WarehouseFile& file, int slot, const char* key,
    std::vector<uint8_t>& out_cipher) {
    const bool available = encrypt_available();
    const save_backup::WarehouseEncryptFn encrypt =
        available ? &warehouse_encrypt_mode0 : nullptr;
    const save_backup::WarehouseDecryptFn decrypt =
        available ? &warehouse_decrypt_mode1 : nullptr;
    return save_backup::warehouse_reencrypt_for_slot(file.data, slot, key, encrypt, decrypt,
                                                      out_cipher);
}

}  // namespace port
}  // namespace save_backup

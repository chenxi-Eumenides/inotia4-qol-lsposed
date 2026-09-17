#include "infra/config.h"

// 配置实现（见 infra/config.h 的接口契约）。
// 键存储：每个槽位内联一个 64 字节缓冲，避免跨边界持有调用方指针。

#include <cstring>

#include "contracts/events.h"
#include "infra/events.h"

namespace qol::infra {
namespace {

constexpr uint32_t kMaxConfigEntries = 64;
constexpr uint32_t kMaxKeyLength = 63; // 含 NUL 的缓冲为 64

struct Entry {
    char key[kMaxKeyLength + 1];
    int32_t value;
};

Entry g_entries[kMaxConfigEntries];
uint32_t g_count = 0;

Entry* find_entry(const char* key) {
    for (uint32_t i = 0; i < g_count; ++i) {
        if (std::strcmp(g_entries[i].key, key) == 0) {
            return &g_entries[i];
        }
    }
    return nullptr;
}

bool valid_key(const char* key) {
    if (key == nullptr || key[0] == '\0') {
        return false;
    }
    return std::strlen(key) <= kMaxKeyLength;
}

}  // namespace

bool config_set_int(const char* key, int32_t value) {
    if (!valid_key(key)) {
        return false;
    }
    Entry* entry = find_entry(key);
    if (entry == nullptr) {
        if (g_count >= kMaxConfigEntries) {
            return false;
        }
        entry = &g_entries[g_count];
        std::strcpy(entry->key, key);
        ++g_count;
        entry->value = value;
    } else if (entry->value == value) {
        return true; // 同值不重复广播
    } else {
        entry->value = value;
    }
    // key 指向槽位内联缓冲：静态存储，事件处理器可安全读取。
    const contracts::ConfigChangedEvent event{entry->key, value};
    publish(contracts::kEventConfigChanged, &event);
    return true;
}

int32_t config_get_int(const char* key, int32_t default_value) {
    if (key == nullptr) {
        return default_value;
    }
    Entry* entry = find_entry(key);
    return entry == nullptr ? default_value : entry->value;
}

bool config_set_bool(const char* key, bool value) {
    return config_set_int(key, value ? 1 : 0);
}

bool config_get_bool(const char* key, bool default_value) {
    return config_get_int(key, default_value ? 1 : 0) != 0;
}

void config_reset_for_test() {
    g_count = 0;
}

}  // namespace qol::infra

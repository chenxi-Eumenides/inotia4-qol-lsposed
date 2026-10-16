#include "game/access/attr_range_ui.h"

#include <cstdint>
#include <cstring>

#include "game/symbols/game_symbols.h"
#include "game/access/game_access.h"

namespace qol::game {
namespace {

}  // namespace

int attr_item_category(const void* item) {
    if (item == nullptr) return -1;
    const uint16_t flags =
        *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(item) + I_TYPE);
    return static_cast<int>((flags >> 6) & 0x3FF);
}

QolResult attr_item_uses_fixed_option_table(int32_t category, int32_t* out_uses) {
    if (category < 0 || out_uses == nullptr) {
        static constexpr char kInvalidArgument[] = "invalid argument";
        return contracts::error(kQolErrInvalidArgument, kInvalidArgument,
                                sizeof(kInvalidArgument) - 1);
    }
    if (g_base == 0) {
        static constexpr char kNotReady[] = "not ready";
        return contracts::error(kQolErrNotReady, kNotReady, sizeof(kNotReady) - 1);
    }
    void* base_ref = *reinterpret_cast<void**>(g_base + G_ITEMCLASS_DATA_GOT_VMA);
    if (base_ref == nullptr) {
        static constexpr char kNotReady[] = "not ready";
        return contracts::error(kQolErrNotReady, kNotReady, sizeof(kNotReady) - 1);
    }
    uint8_t* base = *reinterpret_cast<uint8_t**>(base_ref);
    uint8_t* size_ptr = *reinterpret_cast<uint8_t**>(g_base + G_ITEMCLASS_SIZE_GOT_VMA);
    if (base == nullptr || size_ptr == nullptr) {
        static constexpr char kNotReady[] = "not ready";
        return contracts::error(kQolErrNotReady, kNotReady, sizeof(kNotReady) - 1);
    }
    const uint8_t stride = *size_ptr;
    if (stride <= 6) {
        static constexpr char kNotReady[] = "not ready";
        return contracts::error(kQolErrNotReady, kNotReady, sizeof(kNotReady) - 1);
    }
    *out_uses = (base[static_cast<size_t>(category) * stride + 6] & 0x02) != 0 ? 1 : 0;
    return contracts::ok();
}

QolResult attr_fixed_option_percent_pair(int32_t category, int32_t option_id,
                                          int32_t* out_p1, int32_t* out_p2) {
    if (category < 0 || option_id < 0 || out_p1 == nullptr || out_p2 == nullptr) {
        static constexpr char kInvalidArgument[] = "invalid argument";
        return contracts::error(kQolErrInvalidArgument, kInvalidArgument,
                                sizeof(kInvalidArgument) - 1);
    }
    if (g_base == 0) {
        static constexpr char kNotReady[] = "not ready";
        return contracts::error(kQolErrNotReady, kNotReady, sizeof(kNotReady) - 1);
    }
    void* base_ref = *reinterpret_cast<void**>(g_base + G_ITEMSTATICOPT_TABLE_DATA_GOT_VMA);
    if (base_ref == nullptr) {
        static constexpr char kNotReady[] = "not ready";
        return contracts::error(kQolErrNotReady, kNotReady, sizeof(kNotReady) - 1);
    }
    uint8_t* base = *reinterpret_cast<uint8_t**>(base_ref);
    uint8_t* size_ptr = *reinterpret_cast<uint8_t**>(g_base + G_ITEMSTATICOPT_TABLE_SIZE_GOT_VMA);
    uint8_t* count_ptr = *reinterpret_cast<uint8_t**>(g_base + G_ITEMSTATICOPT_TABLE_COUNT_GOT_VMA);
    if (base == nullptr || size_ptr == nullptr || count_ptr == nullptr) {
        static constexpr char kNotReady[] = "not ready";
        return contracts::error(kQolErrNotReady, kNotReady, sizeof(kNotReady) - 1);
    }
    const uint8_t stride = *size_ptr;
    if (stride < 5) {
        static constexpr char kNotReady[] = "not ready";
        return contracts::error(kQolErrNotReady, kNotReady, sizeof(kNotReady) - 1);
    }
    const uint16_t count = static_cast<uint16_t>(count_ptr[0] | (count_ptr[1] << 8));
    bool found = false;
    int32_t matched_p1 = 0;
    int32_t matched_p2 = 0;
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* record = base + static_cast<size_t>(i) * stride;
        const uint16_t record_category = static_cast<uint16_t>(record[0] | (record[1] << 8));
        if (record_category != static_cast<uint16_t>(category)) continue;
        if (static_cast<int32_t>(static_cast<int8_t>(record[2])) != option_id) continue;
        matched_p1 = static_cast<int32_t>(static_cast<int8_t>(record[3]));
        matched_p2 = static_cast<int32_t>(static_cast<int8_t>(record[4]));
        found = true;  // 同索引重复出现时最后一条生效（游戏覆盖语义）
    }
    if (!found) {
        static constexpr char kNotFound[] = "fixed option not found";
        return contracts::error(kQolErrNotFound, kNotFound, sizeof(kNotFound) - 1);
    }
    *out_p1 = matched_p1;
    *out_p2 = matched_p2;
    return contracts::ok();
}

int attr_ability_level(const void* item) {
    if (fn_item_get_ability_level == nullptr || item == nullptr) return -1;
    return fn_item_get_ability_level(const_cast<void*>(item));
}

bool attr_is_jewel_category(int category) {
    return fn_is_jewel != nullptr && fn_is_jewel(category) != 0;
}

bool attr_call_option_value(int option_index, int level, int flag, void* item, int* out_value) {
    if (fn_item_system_get_option_value == nullptr || item == nullptr || option_index < 0 ||
        out_value == nullptr) {
        return false;
    }
    *out_value = fn_item_system_get_option_value(option_index, level, flag, item);
    return true;
}

bool attr_call_jewel_value(int type, void* item, int* out_value) {
    if (fn_item_system_get_jewel_option_value == nullptr || item == nullptr || type < 0 ||
        out_value == nullptr) {
        return false;
    }
    *out_value = fn_item_system_get_jewel_option_value(type, item);
    return true;
}

bool attr_uidesc_text_buffer_base(char** out_buffer) {
    if (out_buffer == nullptr) return false;
    *out_buffer = nullptr;
    if (g_base == 0 || g_base > UINTPTR_MAX - G_UIDESC_TEXT_BUF_VMA) return false;
    *out_buffer = reinterpret_cast<char*>(g_base + G_UIDESC_TEXT_BUF_VMA);
    return true;
}

bool attr_insert_value_color(char* before, char* after, char code) {
    if (before == nullptr || after == nullptr || after <= before || g_base == 0) return false;

    // 行格式： "$<原色><标签>: <数值>$B"。只染数值：在数值前插入 "$<目标色>"，
    // 标签保持原色，行尾已有 "$B" 收束。插入需右移数值段 4 字节（"$B$<code>"）。
    char* insert_at = nullptr;
    for (char* p = before; p < after; ++p) {
        if (*p == ':') {
            insert_at = p + 1;
            while (insert_at < after && *insert_at == ' ') ++insert_at;
            break;
        }
    }
    if (insert_at == nullptr) {
        for (char* p = before; p < after; ++p) {
            if (*p >= '0' && *p <= '9') {
                insert_at = p;
                break;
            }
        }
    }
    if (insert_at == nullptr) return false;
    if (insert_at > before && (insert_at[-1] == '-' || insert_at[-1] == '+')) --insert_at;

    // 游戏内联色约定为 "$<码>文本$B"（段内着色、$B 收束）；直接插入 "$<码>" 会让
    // 前一段（标签）回落到默认色。故先插入 "$B" 关闭标签段，再插入 "$<目标色>" 开值段。
    const uintptr_t buffer_end = g_base + G_UIDESC_TEXT_BUF_VMA + G_UIDESC_TEXT_BUF_SIZE;
    if (reinterpret_cast<uintptr_t>(after) + 4 > buffer_end) return false;

    std::memmove(insert_at + 4, insert_at, static_cast<size_t>(after - insert_at));
    insert_at[0] = '$';
    insert_at[1] = 'B';
    insert_at[2] = '$';
    insert_at[3] = code;
    return true;
}

}  // namespace qol::game

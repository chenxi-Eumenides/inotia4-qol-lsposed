#include "game/adapter_ports/craft_lottery_port.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "game/access/game_access.h"
#include "game/adapter_ports/game_host_port.h"
#include "contracts/game_ports/ports.h"

namespace qol::game {
namespace {

using QolResult = ::QolResult;

constexpr uint32_t kItemMixLinkRecordMinSize = 4;
constexpr uint16_t kRecipePopupFormatRecord = 17;
constexpr size_t kRecipePopupBufferSize = 192;

QolResult invalid_argument() {
    return contracts::error(kQolErrInvalidArgument, "invalid argument", 16);
}

QolResult not_ready() { return contracts::error(kQolErrNotReady, "not ready", 9); }

QolResult wrong_thread() { return contracts::error(kQolErrBlocked, "wrong thread", 12); }

QolResult recipe_pool_capacity_error() {
    static constexpr char kMessage[] = "recipe pool capacity insufficient";
    return contracts::error(kQolErrInvalidArgument, kMessage, sizeof(kMessage) - 1);
}

template <typename Function>
Function game_function(uintptr_t vma) {
    if (g_base == 0) return nullptr;
    const uintptr_t address = g_base + vma;
    if (!game_memory_accessible(reinterpret_cast<const void*>(address), sizeof(uint32_t), 'x')) {
        return nullptr;
    }
    return reinterpret_cast<Function>(address);
}

bool read_item_mix_link_table(const uint8_t** out_data, uint8_t* out_record_size,
                              uint16_t* out_record_count) {
    if (out_data == nullptr || out_record_size == nullptr || out_record_count == nullptr ||
        g_base == 0) {
        return false;
    }

    auto* data_slot = reinterpret_cast<void**>(g_base + G_ITEMMIXLINKBASE_DATA_VMA);
    auto* size_slot = reinterpret_cast<uint8_t*>(g_base + G_ITEMMIXLINKBASE_N_RECORD_SIZE_VMA);
    auto* count_slot =
        reinterpret_cast<uint16_t*>(g_base + G_ITEMMIXLINKBASE_N_RECORD_COUNT_VMA);
    if (!game_memory_accessible(data_slot, sizeof(*data_slot), 'r') ||
        !game_memory_accessible(size_slot, sizeof(*size_slot), 'r') ||
        !game_memory_accessible(count_slot, sizeof(*count_slot), 'r')) {
        return false;
    }

    const auto* data = static_cast<const uint8_t*>(*data_slot);
    const uint8_t record_size = *size_slot;
    const uint16_t record_count = *count_slot;
    if (data == nullptr || record_size < kItemMixLinkRecordMinSize) return false;
    const size_t table_size = static_cast<size_t>(record_size) * record_count;
    if (table_size != 0 && !game_memory_accessible(data, table_size, 'r')) return false;

    *out_data = data;
    *out_record_size = record_size;
    *out_record_count = record_count;
    return true;
}

bool read_recipe_table(const uint8_t** out_data, uint8_t* out_record_size,
                       uint16_t* out_record_count) {
    if (out_data == nullptr || out_record_size == nullptr || out_record_count == nullptr ||
        g_base == 0) {
        return false;
    }

    auto* data_slot = reinterpret_cast<void**>(g_base + G_RECIPEBASE_DATA_VMA);
    auto* size_slot = reinterpret_cast<uint8_t*>(g_base + G_RECIPEBASE_SIZE_VMA);
    auto* count_slot = reinterpret_cast<uint16_t*>(g_base + G_RECIPEBASE_COUNT_VMA);
    if (!game_memory_accessible(data_slot, sizeof(*data_slot), 'r') ||
        !game_memory_accessible(size_slot, sizeof(*size_slot), 'r') ||
        !game_memory_accessible(count_slot, sizeof(*count_slot), 'r')) {
        return false;
    }

    const auto* data = static_cast<const uint8_t*>(*data_slot);
    const uint8_t record_size = *size_slot;
    const uint16_t record_count = *count_slot;
    if (data == nullptr || record_size < sizeof(uint16_t)) return false;
    const size_t table_size = static_cast<size_t>(record_size) * record_count;
    if (table_size != 0 && !game_memory_accessible(data, table_size, 'r')) return false;

    *out_data = data;
    *out_record_size = record_size;
    *out_record_count = record_count;
    return true;
}

uint16_t read_u16(const uint8_t* bytes) {
    uint16_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

QolResult pool_read(uint32_t use_type, uint16_t* out_recipe_idx, uint32_t cap,
                    uint32_t* out_count) {
    if (out_count == nullptr || use_type > UINT16_MAX || (cap != 0 && out_recipe_idx == nullptr)) {
        return invalid_argument();
    }
    *out_count = 0;

    const uint8_t* data = nullptr;
    uint8_t record_size = 0;
    uint16_t record_count = 0;
    if (!read_item_mix_link_table(&data, &record_size, &record_count)) return not_ready();

    uint32_t needed = 0;
    for (uint16_t i = 0; i < record_count; ++i) {
        const uint8_t* record = data + static_cast<size_t>(i) * record_size;
        if (read_u16(record) == static_cast<uint16_t>(use_type)) ++needed;
    }
    *out_count = needed;
    if (needed > cap) return recipe_pool_capacity_error();
    if (needed == 0) return contracts::ok();

    uint32_t written = 0;
    for (uint16_t i = 0; i < record_count; ++i) {
        const uint8_t* record = data + static_cast<size_t>(i) * record_size;
        if (read_u16(record) != static_cast<uint16_t>(use_type)) continue;
        out_recipe_idx[written++] = read_u16(record + sizeof(uint16_t));
    }
    return contracts::ok();
}

QolResult recipe_learned(uint16_t recipe_idx, bool* out_learned) {
    if (out_learned == nullptr) return invalid_argument();
    *out_learned = false;
    if (g_base == 0) return not_ready();

    using GetRecipeCountFn = int (*)(int);
    using GetRecipeBookSizeFn = int (*)();
    const auto get_recipe_count = game_function<GetRecipeCountFn>(F_MIXSYSTEM_GET_RECIPE_COUNT_VMA);
    const auto get_recipe_book_size =
        game_function<GetRecipeBookSizeFn>(F_MIXSYSTEM_GET_RECIPE_BOOK_SIZE_VMA);
    auto* book_slot = reinterpret_cast<void**>(g_base + G_MIXSYSTEM_P_RECIPE_BOOK_VMA);
    if (get_recipe_count == nullptr || get_recipe_book_size == nullptr ||
        !game_memory_accessible(book_slot, sizeof(*book_slot), 'r')) {
        return not_ready();
    }

    const int count_group5 = get_recipe_count(5);
    const int count_all = get_recipe_count(0);
    const int book_size = get_recipe_book_size();
    if (count_group5 < 0 || count_all < 0 || book_size < 0) return not_ready();

    const int64_t bit_index = static_cast<int64_t>(recipe_idx) + count_group5 - count_all;
    if (bit_index < 0) {
        static constexpr char kInvalidIndex[] = "recipe book bit index invalid";
        return contracts::error(kQolErrInvalidArgument, kInvalidIndex, sizeof(kInvalidIndex) - 1);
    }
    if (static_cast<uint64_t>(bit_index >> 3) >= static_cast<uint64_t>(book_size)) {
        static constexpr char kOutOfBounds[] = "recipe book bit outside bitmap";
        return contracts::error(kQolErrNotReady, kOutOfBounds, sizeof(kOutOfBounds) - 1);
    }

    auto* book = static_cast<const uint8_t*>(*book_slot);
    if (book == nullptr || !game_memory_accessible(book + (bit_index >> 3), sizeof(uint8_t), 'r')) {
        return not_ready();
    }
    *out_learned = (book[bit_index >> 3] & (1u << (bit_index & 7))) != 0;
    return contracts::ok();
}

QolResult decide_recipe_pure(uint32_t use_type, int32_t* out_recipe_idx) {
    if (out_recipe_idx == nullptr) return invalid_argument();
    *out_recipe_idx = -1;
    if (!game_host_is_main_thread()) return wrong_thread();
    using DecideRecipeFn = int (*)(uint32_t);
    const auto decide_recipe = game_function<DecideRecipeFn>(F_ITEMSYSTEM_DECIDE_RECIPE_ID_VMA);
    if (decide_recipe == nullptr) return not_ready();
    *out_recipe_idx = static_cast<int32_t>(decide_recipe(use_type));
    return contracts::ok();
}

QolResult add_recipe_book(uint16_t recipe_idx) {
    if (!game_host_is_main_thread()) return wrong_thread();
    using AddRecipeBookFn = int (*)(int);
    const auto add = game_function<AddRecipeBookFn>(F_MIXSYSTEM_ADD_RECIPE_BOOK_VMA);
    if (add == nullptr) return not_ready();
    add(static_cast<int>(recipe_idx));
    return contracts::ok();
}

QolResult get_random(int32_t n, int32_t* out) {
    if (out == nullptr || n < 0) return invalid_argument();
    *out = 0;
    if (!game_host_is_main_thread()) return wrong_thread();
    if (fn_math_get_random == nullptr) return not_ready();
    const int value = fn_math_get_random(0, n);
    if (value < 0 || value > n) {
        static constexpr char kRandomOutOfRange[] = "random result out of range";
        return contracts::error(kQolErrInternal, kRandomOutOfRange, sizeof(kRandomOutOfRange) - 1);
    }
    *out = static_cast<int32_t>(value);
    return contracts::ok();
}

bool read_item_database_table(const uint8_t** out_data, uint8_t* out_record_size,
                              uint16_t* out_record_count) {
    if (out_data == nullptr || out_record_size == nullptr || out_record_count == nullptr ||
        g_base == 0) {
        return false;
    }

    auto* data_slot = reinterpret_cast<void**>(g_base + G_ITEMDATABASE_DATA_VMA);
    auto* size_slot = reinterpret_cast<uint8_t*>(g_base + G_ITEMDATABASE_N_RECORD_SIZE_VMA);
    auto* count_slot = reinterpret_cast<uint16_t*>(g_base + G_ITEMDATABASE_N_RECORD_COUNT_VMA);
    if (!game_memory_accessible(data_slot, sizeof(*data_slot), 'r') ||
        !game_memory_accessible(size_slot, sizeof(*size_slot), 'r') ||
        !game_memory_accessible(count_slot, sizeof(*count_slot), 'r')) {
        return false;
    }

    const auto* data = static_cast<const uint8_t*>(*data_slot);
    const uint8_t record_size = *size_slot;
    const uint16_t record_count = *count_slot;
    if (data == nullptr || record_size < sizeof(uint16_t)) return false;
    const size_t table_size = static_cast<size_t>(record_size) * record_count;
    if (table_size != 0 && !game_memory_accessible(data, table_size, 'r')) return false;

    *out_data = data;
    *out_record_size = record_size;
    *out_record_count = record_count;
    return true;
}

size_t bounded_length(const char* text, size_t cap) {
    size_t length = 0;
    while (length < cap && text[length] != '\0') ++length;
    return length;
}

QolResult show_recipe_popup(uint16_t recipe_idx, uint32_t use_type, const char* extra_utf8) {
    if (!game_host_is_main_thread()) return wrong_thread();
    if (g_base == 0 || fn_memorytext_get_text == nullptr || fn_mem_malloc == nullptr ||
        fn_mem_free == nullptr) {
        return not_ready();
    }

    const auto popup = game_function<void (*)(char*, int, int, int, int)>(
        F_UIPOPUPMSG_CREATE_OK_VMA);
    const auto sprintk =
        game_function<int (*)(char*, const char*, const char*)>(F_CS_KNL_SPRINTK_VMA);
    if (popup == nullptr || sprintk == nullptr) return not_ready();

    const uint8_t* recipe_data = nullptr;
    uint8_t record_size = 0;
    uint16_t record_count = 0;
    if (!read_recipe_table(&recipe_data, &record_size, &record_count) ||
        recipe_idx >= record_count || kRecipePopupFormatRecord >= record_count) {
        return not_ready();
    }

    const uint16_t format_text_id = read_u16(
        recipe_data + static_cast<size_t>(record_size) * kRecipePopupFormatRecord);
    const uint16_t name_text_id =
        read_u16(recipe_data + static_cast<size_t>(record_size) * recipe_idx);
    const char* format_text = fn_memorytext_get_text(format_text_id);
    const char* recipe_name = fn_memorytext_get_text(name_text_id);
    if (format_text == nullptr || recipe_name == nullptr ||
        !game_memory_accessible(format_text, 1, 'r') ||
        !game_memory_accessible(recipe_name, 1, 'r')) {
        return not_ready();
    }

    // 套装名：ITEMDATABASE[use_type] 首 u16 即名称文本 id（物品 id 与文本 id 同域同值）。
    const char* item_name = nullptr;
    const uint8_t* item_data = nullptr;
    uint8_t item_record_size = 0;
    uint16_t item_record_count = 0;
    if (use_type <= UINT16_MAX &&
        read_item_database_table(&item_data, &item_record_size, &item_record_count) &&
        static_cast<uint16_t>(use_type) < item_record_count) {
        const uint16_t item_name_text_id = read_u16(
            item_data + static_cast<size_t>(item_record_size) * use_type);
        item_name = fn_memorytext_get_text(item_name_text_id);
        if (item_name != nullptr && !game_memory_accessible(item_name, 1, 'r')) {
            item_name = nullptr;
        }
    }

    auto* buffer = static_cast<char*>(fn_mem_malloc(kRecipePopupBufferSize));
    if (buffer == nullptr) return not_ready();
    std::memset(buffer, 0, kRecipePopupBufferSize);
    sprintk(buffer, format_text, recipe_name);
    buffer[kRecipePopupBufferSize - 1] = '\0';

    size_t used = bounded_length(buffer, kRecipePopupBufferSize);
    if (used == kRecipePopupBufferSize) {
        used = kRecipePopupBufferSize - 1;
        buffer[used] = '\0';
    }
    if (extra_utf8 != nullptr && used < kRecipePopupBufferSize - 3) {
        const uintptr_t extra_address = reinterpret_cast<uintptr_t>(extra_utf8);
        if (game_memory_accessible(extra_utf8, 1, 'r') && extra_utf8[0] != '\0') {
            // 游戏文本域换行标记是双字符 "&N"，不是 '\n'（MEMORYTEXT 原文与 fmt 文本同域）。
            buffer[used++] = '&';
            buffer[used++] = 'N';
            buffer[used] = '\0';
            // 第二行行首为套装物品名（名称文本 id = 物品 id，同域同值）；读取失败则只显示 extra。
            if (item_name != nullptr) {
                while (used < kRecipePopupBufferSize - 1 && *item_name != '\0') {
                    buffer[used++] = *item_name++;
                }
                buffer[used] = '\0';
            }
            size_t extra_offset = 0;
            while (used < kRecipePopupBufferSize - 1) {
                if (extra_address > std::numeric_limits<uintptr_t>::max() -
                                        extra_offset) {
                    break;
                }
                const char* next = reinterpret_cast<const char*>(extra_address + extra_offset);
                if (!game_memory_accessible(next, 1, 'r') || *next == '\0') break;
                buffer[used++] = *next;
                buffer[used] = '\0';
                ++extra_offset;
            }
        }
    }

    popup(buffer, 2, 0, 0, 0);
    fn_mem_free(buffer);
    return contracts::ok();
}

const contracts::QolCraftLotteryWritePort kCraftLotteryWritePort{
    contracts::kQolPortCraftLotteryWriteVersion,
    &pool_read,
    &recipe_learned,
    &decide_recipe_pure,
    &add_recipe_book,
    &get_random,
    &show_recipe_popup,
};

}  // namespace

const contracts::QolCraftLotteryWritePort& craft_lottery_write_port() {
    return kCraftLotteryWritePort;
}

}  // namespace qol::game

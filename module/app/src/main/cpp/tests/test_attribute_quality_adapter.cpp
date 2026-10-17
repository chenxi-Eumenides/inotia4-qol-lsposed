#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "contracts/attribute_quality.h"
#include "contracts/game_ports/ports.h"
#include "contracts/item_facts.h"
#include "game/access/attr_range_ui.h"
#include "game/access/game_access.h"
#include "game/adapter_ports/item_extras_port.h"
#include "game/symbols/game_symbols.h"

uintptr_t g_base = 0;
ItemGetAbilityLevelFn fn_item_get_ability_level = nullptr;
IsJewelFn fn_is_jewel = nullptr;
ItemSystemGetOptionValueFn fn_item_system_get_option_value = nullptr;
ItemSystemGetJewelOptionValueFn fn_item_system_get_jewel_option_value = nullptr;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(condition) do { \
    if (condition) { \
        ++g_pass; \
    } else { \
        ++g_fail; \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (false)

namespace {

constexpr uintptr_t kLastGotVma =
    G_ITEMCLASS_SIZE_GOT_VMA > G_ITEMSTATICOPT_TABLE_COUNT_GOT_VMA
        ? G_ITEMCLASS_SIZE_GOT_VMA
        : G_ITEMSTATICOPT_TABLE_COUNT_GOT_VMA;
constexpr size_t kGameImageSize = static_cast<size_t>(kLastGotVma) + sizeof(void*);

alignas(std::max_align_t) std::array<uint8_t, kGameImageSize> g_game_image{};
std::array<uint8_t, 64 * 8> g_item_class_table{};
std::array<uint8_t, 8 * 5> g_fixed_option_table{};
uint8_t g_item_class_stride = 8;
uint8_t g_fixed_option_stride = 5;
std::array<uint8_t, 2> g_fixed_option_count{};
uint8_t* g_item_class_table_data = nullptr;
uint8_t* g_item_class_stride_data = nullptr;
uint8_t* g_fixed_option_table_data = nullptr;
uint8_t* g_fixed_option_stride_data = nullptr;

void write_got_pointer(uintptr_t vma, void* pointer) {
    std::memcpy(g_game_image.data() + vma, &pointer, sizeof(pointer));
}

void write_little_endian_u16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

void set_fixed_option(uint32_t row, uint16_t category, int8_t option,
                      int8_t minimum_percent, int8_t maximum_percent) {
    uint8_t* record = g_fixed_option_table.data() + row * g_fixed_option_stride;
    write_little_endian_u16(record, category);
    record[2] = static_cast<uint8_t>(option);
    record[3] = static_cast<uint8_t>(minimum_percent);
    record[4] = static_cast<uint8_t>(maximum_percent);
}

void configure_tables() {
    g_game_image.fill(0);
    g_item_class_table.fill(0);
    g_fixed_option_table.fill(0);
    g_fixed_option_count = {0, 0};
    g_item_class_stride = 8;
    g_fixed_option_stride = 5;
    g_item_class_table_data = g_item_class_table.data();
    g_item_class_stride_data = &g_item_class_stride;
    g_fixed_option_table_data = g_fixed_option_table.data();
    g_fixed_option_stride_data = &g_fixed_option_stride;

    write_got_pointer(G_ITEMCLASS_DATA_GOT_VMA, &g_item_class_table_data);
    write_got_pointer(G_ITEMCLASS_SIZE_GOT_VMA, g_item_class_stride_data);
    write_got_pointer(G_ITEMSTATICOPT_TABLE_DATA_GOT_VMA, &g_fixed_option_table_data);
    write_got_pointer(G_ITEMSTATICOPT_TABLE_SIZE_GOT_VMA, g_fixed_option_stride_data);
    write_got_pointer(G_ITEMSTATICOPT_TABLE_COUNT_GOT_VMA, g_fixed_option_count.data());
    g_base = reinterpret_cast<uintptr_t>(g_game_image.data());
}

void check_static_not_ready_message(const QolResult& result) {
    constexpr char kExpected[] = "not ready";
    CHECK(result.code == kQolErrNotReady);
    CHECK(result.message != nullptr && result.message_len == sizeof(kExpected) - 1 &&
          std::strcmp(result.message, kExpected) == 0);
}

int fake_ability_level(void* item) { return item == nullptr ? -1 : 27; }

void test_fixed_table_adapter_states() {
    int32_t uses_table = -1;
    int32_t minimum = 111;
    int32_t maximum = 222;

    g_base = 0;
    CHECK(qol::game::attr_item_uses_fixed_option_table(17, &uses_table).code ==
          kQolErrNotReady);
    CHECK(qol::game::attr_fixed_option_percent_pair(17, 5, &minimum, &maximum).code ==
          kQolErrNotReady);

    configure_tables();
    constexpr uint16_t kCategory = 37;
    constexpr int32_t kOption = 9;
    g_item_class_table[kCategory * g_item_class_stride + 6] = 0;
    uses_table = -1;
    CHECK(qol::game::attr_item_uses_fixed_option_table(kCategory, &uses_table).ok == kQolOk);
    CHECK(uses_table == 0);  // Fixed-table non-applicable is success with false, not an error.

    g_item_class_table[kCategory * g_item_class_stride + 6] = 0x02;
    uses_table = -1;
    CHECK(qol::game::attr_item_uses_fixed_option_table(kCategory, &uses_table).ok == kQolOk);
    CHECK(uses_table == 1);

    set_fixed_option(0, kCategory, static_cast<int8_t>(kOption), 3, 44);
    set_fixed_option(1, kCategory, static_cast<int8_t>(kOption), -7, 83);
    set_fixed_option(2, static_cast<uint16_t>(kCategory + 1), static_cast<int8_t>(kOption),
                     12, 45);
    g_fixed_option_count = {3, 0};

    minimum = 111;
    maximum = 222;
    CHECK(qol::game::attr_fixed_option_percent_pair(kCategory, kOption, &minimum, &maximum).ok ==
          kQolOk);
    CHECK(minimum == -7 && maximum == 83);  // Duplicate keys use the last matching row.

    minimum = 111;
    maximum = 222;
    const QolResult no_match =
        qol::game::attr_fixed_option_percent_pair(kCategory, kOption + 1, &minimum, &maximum);
    CHECK(no_match.code == kQolErrNotFound);
    CHECK(minimum == 111 && maximum == 222);

    g_fixed_option_stride = 4;
    CHECK(qol::game::attr_fixed_option_percent_pair(kCategory, kOption, &minimum, &maximum).code ==
          kQolErrNotReady);
    g_fixed_option_stride = 5;
    g_item_class_stride = 6;
    CHECK(qol::game::attr_item_uses_fixed_option_table(kCategory, &uses_table).code ==
          kQolErrNotReady);
}

void test_null_intermediate_got_references_return_not_ready() {
    configure_tables();
    write_got_pointer(G_ITEMCLASS_DATA_GOT_VMA, nullptr);
    int32_t uses_table = 123;
    const QolResult item_class_not_ready =
        qol::game::attr_item_uses_fixed_option_table(17, &uses_table);
    check_static_not_ready_message(item_class_not_ready);
    CHECK(uses_table == 123);

    configure_tables();
    write_got_pointer(G_ITEMSTATICOPT_TABLE_DATA_GOT_VMA, nullptr);
    int32_t minimum = 111;
    int32_t maximum = 222;
    const QolResult static_option_not_ready =
        qol::game::attr_fixed_option_percent_pair(17, 5, &minimum, &maximum);
    check_static_not_ready_message(static_option_not_ready);
    CHECK(minimum == 111 && maximum == 222);
}

void test_ability_level_not_ready() {
    int item = 0;
    int32_t level = 1234;
    fn_item_get_ability_level = nullptr;
    const auto& port = qol::game::item_extras_port();
    const QolResult not_ready = port.read.ability_level(&item, &level);
    CHECK(not_ready.code == kQolErrNotReady);
    CHECK(level == 1234);
    CHECK(qol::game::attr_ability_level(&item) == -1);

    fn_item_get_ability_level = &fake_ability_level;
    CHECK(port.read.ability_level(&item, &level).ok == kQolOk);
    CHECK(level == 27);
    CHECK(qol::game::attr_ability_level(nullptr) == -1);
    fn_item_get_ability_level = nullptr;
}

}  // namespace

int main() {
    static_assert(qol::contracts::kAttributeQualityVersionMajor == 2 &&
                      qol::contracts::kAttributeQualityVersionMinor == 0,
                  "attribute.quality adapter fixture expects v2.0");
    static_assert(qol::contracts::kItemFactsVersion.major == 3 &&
                      qol::contracts::kItemFactsVersion.minor == 4,
                  "item.facts adapter fixture expects v3.4");
    static_assert(qol::contracts::kQolPortItemVersion.major == 3 &&
                      qol::contracts::kQolPortItemVersion.minor == 4,
                  "Item Port adapter fixture expects v3.4");
    static_assert(offsetof(qol::contracts::AttributeQualityApi, query_option_context) ==
                      offsetof(qol::contracts::AttributeQualityApi, query_item) +
                          sizeof(qol::contracts::AttributeQualityQueryItemFn),
                  "attribute.quality v2 slots must remain contiguous");
    static_assert(offsetof(qol::contracts::AttributeQualityApi, evaluate) ==
                      offsetof(qol::contracts::AttributeQualityApi, query_option_context) +
                          sizeof(qol::contracts::AttributeQualityQueryOptionContextFn),
                  "attribute.quality v2 evaluate slot must be tail-appended");
    static_assert(sizeof(qol::contracts::AttributeQualityApi) ==
                      offsetof(qol::contracts::AttributeQualityApi, evaluate) +
                          sizeof(qol::contracts::AttributeQualityEvaluateFn),
                  "attribute.quality struct_size must cover all three v2 slots");
    test_fixed_table_adapter_states();
    test_null_intermediate_got_references_return_not_ready();
    test_ability_level_not_ready();
    std::printf("attribute_quality_adapter: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

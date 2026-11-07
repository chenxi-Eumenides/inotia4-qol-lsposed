// host 单测（自动出售阶段 A / 帧任务纯逻辑）：
//   1) save_enter 声明式资源入口存在性（触发判定已迁移到事件状态）
//   2) 数据层 item_is_equip 的编码分支 item_count_encoding_from_flags（注入假类别表）
// 纯头文件逻辑，不依赖 Android/游戏内存。
// 帧调度的去重/interval/count 语义由 test_infra_scheduler.cpp 覆盖。

#include "patch/save_enter.h"
#include "patch/save_exit.h"
#include "contracts/game_host.h"
#include "game/read/item_class.h"

#include <cstdint>
#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using qol::contracts::QolItemCountEncoding;

// save_exit 无纯逻辑可测；此处仅以编译性断言保证主机端可包含该头（声明式资源入口存在）。
static_assert(sizeof(&save_exit_claim_count) > 0, "save_exit claims accessor must be declared");

namespace {

uint16_t type_flags_for(int category) {
    return static_cast<uint16_t>((category & 0x3FF) << 6);
}

}  // namespace

static void test_item_is_equip_encoding() {
    // 假类别表：步长 8，覆盖 category 0..3；记录 +6 bit0 = 可堆叠标记。
    uint8_t table[4 * 8] = {};
    table[1 * 8 + 6] = 0x01;  // category 1：bit0=1 -> 可堆叠（kEncoded）
    table[2 * 8 + 6] = 0x03;  // category 2：bit0=1 且 bit1=1 -> 仍可堆叠（只看 bit0）
    // category 0 / 3 保持 0 -> 装备（kNotEncoded）
    const uint8_t stride = 8;

    CHECK(item_count_encoding_from_flags(type_flags_for(0), table, stride) ==
          QolItemCountEncoding::kNotEncoded);
    CHECK(item_count_encoding_from_flags(type_flags_for(1), table, stride) ==
          QolItemCountEncoding::kEncoded);
    CHECK(item_count_encoding_from_flags(type_flags_for(2), table, stride) ==
          QolItemCountEncoding::kEncoded);
    CHECK(item_count_encoding_from_flags(type_flags_for(3), table, stride) ==
          QolItemCountEncoding::kNotEncoded);

    // item_is_equip 口径：仅 kNotEncoded 是装备；kEncoded/kUnknown 均非装备。
    for (int c = 0; c < 4; ++c) {
        const bool equip =
            item_count_encoding_from_flags(type_flags_for(c), table, stride) ==
            QolItemCountEncoding::kNotEncoded;
        CHECK(equip == (c == 0 || c == 3));
    }

    // rarity 位（bit2-5）与低位不影响 category 解析。
    const uint16_t with_rarity = static_cast<uint16_t>(type_flags_for(1) | (0x3 << 2));
    CHECK(item_count_encoding_from_flags(with_rarity, table, stride) ==
          QolItemCountEncoding::kEncoded);

    // 表不可用：base=nullptr / stride=0 一律 fail-closed 为 kUnknown -> 非装备。
    CHECK(item_count_encoding_from_flags(type_flags_for(0), nullptr, stride) ==
          QolItemCountEncoding::kUnknown);
    CHECK(item_count_encoding_from_flags(type_flags_for(0), table, 0) ==
          QolItemCountEncoding::kUnknown);
    CHECK(!(item_count_encoding_from_flags(type_flags_for(1), nullptr, stride) ==
            QolItemCountEncoding::kNotEncoded));

    // 不同 stride 下定位公式 category*stride+6 正确。
    uint8_t table4[8 * 4] = {};
    table4[5 * 4 + 6] = 0x01;
    CHECK(item_count_encoding_from_flags(type_flags_for(5), table4, 4) ==
          QolItemCountEncoding::kEncoded);
    CHECK(item_count_encoding_from_flags(type_flags_for(6), table4, 4) ==
          QolItemCountEncoding::kNotEncoded);
}

static void test_item_is_backpack_encoding() {
    // 假类别表：步长 8；记录 +2 == 0x1f 判为背包类。
    uint8_t table[8 * 8] = {};
    table[2 * 8 + 2] = 0x1f;  // category 2：+2=0x1f -> 背包
    table[3 * 8 + 2] = 0x1e;  // category 3：+2!=0x1f -> 非背包
    const uint8_t stride = 8;

    CHECK(item_is_backpack_from_class_data(2, table, stride));
    CHECK(!item_is_backpack_from_class_data(3, table, stride));
    CHECK(!item_is_backpack_from_class_data(0, table, stride));

    // 其它字节不影响 +2 判定。
    table[2 * 8 + 1] = 0xff;
    table[2 * 8 + 3] = 0xff;
    CHECK(item_is_backpack_from_class_data(2, table, stride));

    // 表不可用 / stride==0 / category<0 一律 fail-closed 为 false。
    CHECK(!item_is_backpack_from_class_data(2, nullptr, stride));
    CHECK(!item_is_backpack_from_class_data(2, table, 0));
    CHECK(!item_is_backpack_from_class_data(-1, table, stride));

    // 不同 stride 下定位公式 category*stride+2 正确。
    uint8_t table4[6 * 4] = {};
    table4[4 * 4 + 2] = 0x1f;
    CHECK(item_is_backpack_from_class_data(4, table4, 4));
    CHECK(!item_is_backpack_from_class_data(3, table4, 4));
}

int main() {
    test_item_is_equip_encoding();
    test_item_is_backpack_encoding();
    std::printf("infra_data_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

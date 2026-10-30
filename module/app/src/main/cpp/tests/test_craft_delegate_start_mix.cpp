// 委托原版 UIMix_StartMix 的 mixType 恢复回归测试。
//
// 直接编译 `game/access/craft.cpp`：原版 StartMix 全程只读 `[g_uimix+0x48]`，成功链末尾的
// InitMixingState 也按传入的原版宝石配方重算，本身不还原。委托返回后必须恢复调用前的
// 模块「合成」记录并重算 stuff/费用，否则面板会停在原版宝石配方（12..15），后续放料按
// 原版 CheckMixture 校验，无法继续填入其他宝石。

#include "game/access/craft.h"
#include "game/access/game_access.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

alignas(8) std::uint8_t g_uimix_storage[0x120] = {};

constexpr size_t kMixTypeOffset = UIMIX_SLOT_MIXTYPE;
constexpr size_t kCostOffset = UIMIX_SLOT_COST;

struct StartMixProbe {
    int start_calls = 0;
    std::uint32_t mix_type_at_start = 0;
    std::uint64_t cost_at_start = 0;
    int init_calls = 0;
    std::uint32_t mix_type_at_init = 0;
};

StartMixProbe g_probe;

std::uint32_t read_u32(size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, g_uimix_storage + offset, sizeof(value));
    return value;
}

std::uint64_t read_u64(size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, g_uimix_storage + offset, sizeof(value));
    return value;
}

void write_u32(size_t offset, std::uint32_t value) {
    std::memcpy(g_uimix_storage + offset, &value, sizeof(value));
}

void write_u64(size_t offset, std::uint64_t value) {
    std::memcpy(g_uimix_storage + offset, &value, sizeof(value));
}

void reset_state(std::uint32_t mix_type, std::uint64_t cost) {
    std::memset(g_uimix_storage, 0, sizeof(g_uimix_storage));
    write_u32(kMixTypeOffset, mix_type);
    write_u64(kCostOffset, cost);
    g_probe = StartMixProbe{};
}

std::int64_t fake_get_cost(std::int32_t mix_type, void*) { return mix_type == 12 ? 500 : -1; }

std::int64_t always_invalid_cost(std::int32_t, void*) { return -1; }

void fake_start_mix() {
    ++g_probe.start_calls;
    g_probe.mix_type_at_start = read_u32(kMixTypeOffset);
    g_probe.cost_at_start = read_u64(kCostOffset);
    // 原版成功链会重算并扣费用；这里只标记费用已被改写，便于验证后续恢复。
    write_u64(kCostOffset, 0);
}

void fake_init_mixing_state() {
    ++g_probe.init_calls;
    g_probe.mix_type_at_init = read_u32(kMixTypeOffset);
    write_u64(kCostOffset, 777);
}

}  // namespace

// craft.cpp 引用的游戏符号/状态（game_access.h 在全局命名空间声明）：
// 本测试只驱动委托函数，其余提供空实现。
void* g_uimix = g_uimix_storage;
GetMemberFn fn_get_member = nullptr;
GetItemCountFn fn_inven_get_item_count = nullptr;
MathGetRandomFn fn_math_get_random = nullptr;
ControlObjectGetDataFn fn_control_object_get_data = nullptr;
UiEquipIsApplyStuffFn fn_ui_equip_is_apply_stuff = nullptr;
UiMixRefreshInvenItemFn fn_ui_mix_refresh_inven_item = nullptr;
UIMixGetTypeFn fn_uimix_get_type = nullptr;
UIMixInitMixingStateFn fn_uimix_init_mixing_state = nullptr;
UIMixResetStuffItemControlFn fn_uimix_reset_stuff_item_control = nullptr;
UIMixSetTypeFn fn_uimix_set_type = nullptr;
ControlObjectGetCursorFn fn_control_object_get_cursor = nullptr;
UiPopupMsgCreateOkFromTextDataFn fn_popup_create_ok_from_textdata = nullptr;
UiDescSetOffFn fn_ui_desc_set_off = nullptr;
SoundSystemPlayFn fn_sound_system_play = nullptr;
ControlItemSetItemFn fn_control_item_set_item = nullptr;
ControlItemGetItemFn fn_control_item_get_item = nullptr;
ControlObjectGetChildFn fn_control_object_get_child = nullptr;
GetCostFn fn_get_cost = nullptr;
UIMixStartMixFn fn_uimix_start_mix = nullptr;
UiPopupMsgCreateYesNoFromTextDataFn fn_popup_create_yesno_from_textdata = nullptr;

int main() {
    // 正常委托：写原版配方 12，StartMix 结束后恢复模块「合成」记录 69 并重算。
    reset_state(69, 0);
    fn_get_cost = &fake_get_cost;
    fn_uimix_start_mix = &fake_start_mix;
    fn_uimix_init_mixing_state = &fake_init_mixing_state;
    assert(qol::game::craft_delegate_start_mix(12));
    assert(g_probe.start_calls == 1);
    assert(g_probe.mix_type_at_start == 12);
    assert(g_probe.cost_at_start == 500);
    assert(g_probe.init_calls == 1);
    assert(g_probe.mix_type_at_init == 69);
    assert(read_u32(kMixTypeOffset) == 69);
    assert(read_u64(kCostOffset) == 777);

    // 非法配方：未进入 StartMix，状态保持原样。
    reset_state(69, 321);
    fn_get_cost = &always_invalid_cost;
    assert(!qol::game::craft_delegate_start_mix(12));
    assert(g_probe.start_calls == 0);
    assert(g_probe.init_calls == 0);
    assert(read_u32(kMixTypeOffset) == 69);
    assert(read_u64(kCostOffset) == 321);

    // 恢复所需的 InitMixingState 未解析：fail-closed，不进入 StartMix。
    reset_state(69, 0);
    fn_get_cost = &fake_get_cost;
    fn_uimix_init_mixing_state = nullptr;
    assert(!qol::game::craft_delegate_start_mix(12));
    assert(g_probe.start_calls == 0);
    assert(read_u32(kMixTypeOffset) == 69);

    std::puts("craft_delegate_start_mix restore tests passed");
    return 0;
}

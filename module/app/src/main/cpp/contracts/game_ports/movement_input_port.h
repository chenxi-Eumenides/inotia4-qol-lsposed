#pragma once

// 全向移动输入端口：插件只声明输入/补步/碰撞 hook 资源，游戏层负责触点几何、
// 键码翻译、双轴移动、补步门禁与碰撞查询。端口为 eight_way_move 私有。

#include <cstdint>
#include <type_traits>

#include "../version.h"

namespace qol::contracts {

using QolMapBlockingQueryFn = int32_t (*)(int32_t x, int32_t y);

struct QolMovementInputApi {
    // UIPlay_ButtonDirection 原函数执行后调用；event 为原函数第二参数，point 为 {x,y}。
    void (*on_direction_touch)(void* ctrl, uint64_t event, const void* point, uint64_t param3);
    // GAMESTATE_PressKeyPlay 入口翻译；返回应传给原函数的键码，并负责写回 KEY_nCode。
    int32_t (*translate_press_key)(int32_t key);
    // GAMESTATE_PressKeyPlay+0x21c 的 CHAR_Move 替换体；内部必须能调用原 CHAR_Move。
    int32_t (*char_move)(void* ch, int32_t dir, int32_t delta, uint8_t flag);
    // UIPlayPorting_Draw 原函数返回后调用：补画第二根方向箭头。
    void (*draw_overlay)();
    void (*set_enabled)(uint8_t enabled);
    void (*reset)();
    // 每逻辑帧主线程检查：离开游戏态清理、主控切换吸附。
    void (*poll)();
    // CHAR_MoveOnWaitCoord 入口：返回 1 表示已处理（跳过原函数），0 表示继续原函数。
    uint8_t (*handle_wait_coord)(void* ch);
    // MAP_IsBlockingByPixel 替换体：original 是 hook 提供的原函数 trampoline。
    int32_t (*map_blocking_query)(int32_t x, int32_t y, QolMapBlockingQueryFn original);
};

struct QolMovementInputPort {
    QolVersion abi_version;
    QolMovementInputApi api;
};

static_assert(std::is_standard_layout<QolMovementInputPort>::value,
              "QolMovementInputPort must remain standard layout");
static_assert(std::is_trivially_copyable<QolMovementInputPort>::value,
              "QolMovementInputPort must remain trivially copyable");

}  // namespace qol::contracts

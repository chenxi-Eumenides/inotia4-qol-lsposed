#pragma once

// 八向/全向移动输入端口：插件只声明输入 hook 资源，游戏层负责触点几何、键码翻译与
// CHAR_Move 双轴扩展。端口为 eight_way_move 私有，仅该插件可声明。

#include <cstdint>
#include <type_traits>

#include "../version.h"

namespace qol::contracts {

struct QolMovementInputApi {
    // UIPlay_ButtonDirection 原函数执行后调用；event 为原函数第二参数，point 为 {x,y}。
    void (*on_direction_touch)(void* ctrl, uint64_t event, const void* point, uint64_t param3);
    // GAMESTATE_PressKeyPlay 入口翻译；返回应传给原函数的键码，并负责写回 KEY_nCode。
    int32_t (*translate_press_key)(int32_t key);
    // GAMESTATE_PressKeyPlay+0x21c 的 CHAR_Move 替换体；内部必须能调用原 CHAR_Move。
    int32_t (*char_move)(void* ch, int32_t dir, int32_t delta, uint8_t flag);
    void (*set_enabled)(uint8_t enabled);
    void (*reset)();
    // 低频主线程检查：离开游戏态时清理触摸状态和残留自定义键码。
    void (*poll)();
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

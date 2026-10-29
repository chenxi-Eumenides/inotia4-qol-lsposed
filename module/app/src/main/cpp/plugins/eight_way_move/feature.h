#pragma once

#include <cstdint>

#include "../../contracts/game_ports/movement_input_port.h"
#include "../../contracts/plugin.h"

namespace qol::features::eight_way_move {

const contracts::QolHostSdk* host_sdk();
void set_host_sdk(const contracts::QolHostSdk* sdk);
const contracts::QolMovementInputPort* input_port();
void set_input_port(const contracts::QolMovementInputPort* port);

void ensure_poll_task();
void remove_poll_task();
int32_t state_poll_tick(int64_t frame, void* ctx);

void set_gate(bool enabled);
bool gate_enabled();

inline constexpr const char* kResourceIdDirectionTouch = "hook:UIPlay_ButtonDirection";
inline constexpr const char* kResourceIdPressKeyPlay = "hook:GAMESTATE_PressKeyPlay";
inline constexpr const char* kResourceIdCharMove = "patch:GAMESTATE_PressKeyPlay+0x21c";
inline constexpr const char* kResourceIdPortingDraw = "hook:UIPlayPorting_Draw";
inline constexpr const char* kResourceIdWaitCoord = "hook:CHAR_MoveOnWaitCoord";
inline constexpr const char* kResourceIdMapBlocking = "hook:MAP_IsBlockingByPixel";
inline constexpr const char* kResourceIdReleaseAlign = "patch:GAMESTATE_PressKey+0x1c0";
inline constexpr const char* kResourceIdSetActionAlign = "patch:CHAR_SetAction+0x26c";

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup);

// 入口 hook / call patch wrapper（由宿主按 ResourceClaim.handler 安装）。
uint64_t direction_touch_wrapper(void* ctrl, uint64_t event, int64_t* point,
                                 uint64_t param3);
void press_key_play_wrapper(int32_t key);
int32_t eight_way_char_move_wrapper(void* ch, int32_t dir, int32_t delta, uint8_t flag);
void porting_draw_wrapper();
void move_on_wait_coord_wrapper(void* ch);
int32_t map_blocking_wrapper(int32_t x, int32_t y);
QolResult on_choice(contracts::PluginContext& ctx, const char* value);

}  // namespace qol::features::eight_way_move

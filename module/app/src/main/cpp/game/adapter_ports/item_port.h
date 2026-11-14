#pragma once

#include "contracts/game_ports/item_port.h"

namespace qol::game {

using ItemDrawIconFn = void (*)(void* item, int32_t x, int32_t y, int32_t draw_type,
                                int32_t flip);
using ItemDrawIconAddressFn = void (*)(uintptr_t item_address, int32_t x, int32_t y,
                                       int32_t draw_type, int32_t flip);
using ItemDrawIconIdFn = void (*)(int32_t category, int32_t x, int32_t y, int32_t draw_type,
                                  int32_t flip);

const contracts::QolItemPort& item_port();
// 游戏启动期注入原版绘制入口；host 测试保持 nullptr 时对应接口返回 NotReady。
void item_port_bind_draw_icon(ItemDrawIconFn handle_fn, ItemDrawIconAddressFn address_fn,
                              ItemDrawIconIdFn id_fn);

// 启动期由 Kotlin 下发静态名称表；payload 为行式文本，nullptr 表示忽略。运行期不再写入。
void item_port_load_static_text(const char* payload);

}  // namespace qol::game

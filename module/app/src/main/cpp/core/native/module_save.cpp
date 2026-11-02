#include "contracts/module_save_port.h"

#include "game/access/game_access.h"
#include "game/state/game_state.h"

// P3c 退役：旧模块参与者编排（prepare/原版/commit/abort + transaction 计数）已删除。
// 联合保存由 core.save 协调器唯一承担（capture→prepared journal→受控原版→sidecar 提交），
// 参与者经 save.slot_data 注册；此处不得保留第二套参与者顺序。
//
// module_save_game() 保留兼容转发：调用方（对话 save action / JNI op_save）经被
// hook:SAVE_Save 接管的入口消费一次完整保存事务——入口 wrapper 进入协调器，游戏可见
// 原版真实 int 返回。fn_save 的直接符号调用只剩本兼容转发与 Adapter 受控 trampoline
//（经 hook 后实际执行体是 wrapper，不再绕过协调器）。
bool module_save_game() {
    if (!game_in_world()) return false;
    if (fn_save == nullptr) return false;
    return fn_save() != 0;
}

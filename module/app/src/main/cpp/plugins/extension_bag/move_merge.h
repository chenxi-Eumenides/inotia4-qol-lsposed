#pragma once

// move_merge / extension source protection 开关（D2c-2b-2 由 feature/patch/game_patch.{h,cpp}
// 迁入 EB）。X1（`ptrslot:MOVE_MERGE_GOT`）wrapper 常驻、内自门控；本组只切请求标志。
// 定义在 plugins/extension_bag/move_merge.inc（game_ui_virtbag.cpp 末尾 include），
// plugin.cpp 经本头调用（门禁链路 kEventConfigChanged → set_move_merge_enabled）。

bool set_move_merge_enabled(bool enabled);
bool move_merge_enabled();
bool set_extension_source_protection_enabled(bool enabled);

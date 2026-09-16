#pragma once

// 推怪（推动增强）全局开关：patch CHAR_Move 推动分支的两个 blocker 资格闸门，
// 使主控/队友移动可推开怪物/队友/召唤物；场景设施（NPC/商人）保持不可推。
// 常量与闸门语义见 data/native/game_symbols.h；安装/还原 fail-closed 语义见 push_all.cpp。
bool push_all_set_enabled(bool on);

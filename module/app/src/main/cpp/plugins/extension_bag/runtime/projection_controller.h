#pragma once

#include <cstdint>

// 投影控制器（状态部分）：原版宿主袋容量字、当前袋读写与 C18 原生袋页计数。
// 安装/恢复与物品区刷新仍由 runtime/projection.cpp 负责，待对象层解耦后迁入。
bool bag_size_word_locked(int bag, uint32_t* out_word);
bool set_bag_size_word_locked(int bag, uint32_t word);
int original_bag_locked();
int raw_current_bag_locked();
void set_original_bag_locked(int bag);
int native_bag_page_count_locked();
bool native_source_bag_allowed_locked(int bag);

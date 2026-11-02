#pragma once

#include <cstdint>

// patch 点占用声明（ARCHITECTURE.md §4.1 / §4.2）：同一 patch 点（按运行期地址）
// 只允许一个所有者。与 hooks/registry 的「一资源一安装者」同构，按地址而非资源 id 仲裁。
//
// 固定容量 64 条，静态数组实现；claim_id 为静态字符串（生命周期 ≥ 进程）。
// 线程纪律：声明/释放只在装配期/主线程发生，本模块不引入锁。

namespace qol::patch {

// 声明占用：同地址不同 claim_id → false；同 claim_id 幂等 true；空 claim_id → false；满容 → false。
bool claim_point(uintptr_t addr, const char* claim_id);

// 释放占用：未占用 → false。
bool release_point(uintptr_t addr);

// 保存声明式 call patch 安装前观测到的指令字；仅已占用地址可设置或读取。
bool set_saved_word(uintptr_t addr, uint32_t word);
bool get_saved_word(uintptr_t addr, uint32_t* out_word);

// 未占用 → nullptr。
const char* point_owner(uintptr_t addr);

uint32_t claimed_count();

// 测试用：清空占用表。
void claims_reset_for_test();

}  // namespace qol::patch

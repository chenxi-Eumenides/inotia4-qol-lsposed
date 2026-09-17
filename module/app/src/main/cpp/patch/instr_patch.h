#pragma once

#include <cstdint>

#include "contracts/plugin.h"

// 指令 patch 机制层（ARCHITECTURE.md §4.1 调用点 patch、§4.2 物理归还）。
//
// 机制原语（BL 改写 / 指令字替换）由宿主注入：过渡期指向 core/native/call_patch，
// P5 迁入本层后不再跨层取原语。本层只做站点校验、占用声明与返回错误语义。

namespace qol::patch {

struct Primitives {   // 由 host 注入（过渡期指向 core/native 的调用点 patch 原语；P5 迁入本层）
    bool (*install_bl)(uintptr_t addr, uint32_t expected_word, void* wrapper);
    bool (*revert_bl)(uintptr_t addr, uint32_t original_word);
    bool (*install_word)(uintptr_t addr, uint32_t expected_word, uint32_t new_word);
    bool (*revert_word)(uintptr_t addr, uint32_t original_word);
};
void set_primitives(const Primitives& primitives);

struct Outcome {
    int32_t ok;
    const char* error;
};

// 安装一条 kCallPatch 声明：claim.symbol 为 patch 点登记名；claim.handler 为 BL 目标
// （kWordReplace 必须为 nullptr）。
// 流程：解析 site → 地址一致性校验 → claims 占用 → 原语应用（kBlRedirect: install_bl；
// kWordReplace: install_word）。
// 失败锚点（error 静态字符串）：未注入原语 "primitives not set"；site 未登记 "site not found"；
// site.addr != site_addr "site address mismatch"；kind 与 handler 不匹配 "kind mismatch"；
// 已占用（不同 claim）"point already claimed"；原语返回 false "primitive rejected"。
Outcome install_claim(const contracts::ResourceClaim& claim, uintptr_t site_addr);

// 还原（§4.2 物理归还）：先原语还原（用 site.expected_word 作为原字），成功后才释放占用；
// 还原失败不释放占用（保留 owner），由引擎按 tombstone + 隔离语义处理。
Outcome release_claim(const contracts::ResourceClaim& claim, uintptr_t site_addr);

// 扫描式例外：按运行期地址直接占用 + 字替换，不要求站点预先登记。仅用于按特征字节
// 定位的兼容 patch（monster 物品数量上界；地址无法在 game/access/patches 静态登记）。
// 与 install_claim 共用同一占用表与注入原语，是「扫描命中」而非「声明式站点」。
Outcome install_word_at(const char* claim_id, uintptr_t addr, uint32_t expected_word,
                        uint32_t new_word);

}  // namespace qol::patch

#pragma once

#include <cstdint>

#include "contracts/plugin.h"

// 指令 patch 机制层（ARCHITECTURE.md §4.1 调用点 patch、§4.2 物理归还）。
//
// 机制原语（BL 改写 / 指令字替换）由宿主注入：过渡期指向 patch/call_patch，
// P5 迁入本层后不再跨层取原语。本层只做站点校验、占用声明与返回错误语义。

namespace qol::patch {

struct Primitives {   // 由 host 注入调用点 patch 原语
    bool (*install_bl)(uintptr_t addr, uint32_t expected_word, void* wrapper);
    bool (*revert_bl)(uintptr_t addr, uint32_t original_word);
    bool (*install_word)(uintptr_t addr, uint32_t expected_word, uint32_t new_word);
    bool (*revert_word)(uintptr_t addr, uint32_t original_word);
    // 指针槽机制（kPtrSlot）：写/还原 8 字节函数指针；失败必须 fail-closed（返回 false 且不保证已写）。
    bool (*write_ptr)(uintptr_t slot_addr, void* value);
    bool (*restore_ptr)(uintptr_t slot_addr, void* value);
    // B 尾调用机制（kBranchRedirect）：宿主在站点 ±128MB 内分配 thunk
    //（`ldr x16,#8; br x16; .quad handler`）并把站点 B 改指 thunk；返回 thunk 运行期地址，
    // 0 = 失败（未改写任何字节）。返回地址供本层收口 B 编码可达性。
    uintptr_t (*install_branch)(uintptr_t addr, uint32_t expected_word, void* handler);
    bool (*revert_branch)(uintptr_t addr, uint32_t original_word);
    bool (*install_branch_with_return)(uintptr_t addr, uint32_t expected_word,
                                       uint32_t alternate_expected_word, void* handler,
                                       uintptr_t return_addr, uint32_t tail_word);
    bool (*revert_branch_with_return)(uintptr_t addr, uint32_t expected_word,
                                      uint32_t alternate_expected_word);
};
void set_primitives(const Primitives& primitives);

// 已注入原语表；未注入返回 nullptr。供同层 kPtrSlot 机制（patch/ptr_slot.cpp）读取共享表，
// 避免两个机制各维护一份注入状态而在 host 单测中出现分歧。
const Primitives* primitives();

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

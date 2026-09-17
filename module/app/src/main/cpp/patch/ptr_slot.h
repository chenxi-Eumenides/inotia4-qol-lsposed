#pragma once

#include <cstdint>

#include "contracts/plugin.h"
#include "game/access/ptr_slots.h"
#include "patch/instr_patch.h"

// 指针槽机制层（ARCHITECTURE.md §4.1 指针槽、§4.2 物理归还）。
//
// 站点（slot_addr + expected_value）由 game/access 解析后传入，本层只做当前值校验、
// 占用声明、原语调用与返回错误语义。注入原语表与指令 patch 共用 Primitives
// （set_primitives 一次注入 write_ptr/restore_ptr）。

namespace qol::patch {

// 安装一条 kPtrSlot 声明：claim.handler 为覆盖槽值的 wrapper 入口。
// 流程：当前值==expected（或已由本 claim 持有且值==handler）→ claims 占用 → 写 handler。
// 失败锚点（error 静态字符串）：未注入原语 "primitives not set"；空站点 "slot not found"；
// handler 为空 "handler missing"；槽值不符 "slot value mismatch"；已占用（不同 claim）
// "point already claimed"；原语返回 false "primitive rejected"。
Outcome install_claim(const contracts::ResourceClaim& claim, const qol::game::PtrSlotSite& site);

// 归还（§4.2 物理归还）：当前值==handler → 写回 expected → 释放占用；已是 expected →
// 幂等返回成功；其余不匹配 → 保留占用（tombstone 语义），由引擎按隔离语义处理。
Outcome release_claim(const contracts::ResourceClaim& claim, const qol::game::PtrSlotSite& site);

// 具体写入原语（fail-closed：参数非法或 mprotect 失败返回 false 且不写）。
// 装配期由 host 注入 Primitives；host 单测可替换为假原语以覆盖失败分支。
bool ptr_slot_write_raw(uintptr_t slot_addr, void* value);
bool ptr_slot_restore_raw(uintptr_t slot_addr, void* value);

}  // namespace qol::patch

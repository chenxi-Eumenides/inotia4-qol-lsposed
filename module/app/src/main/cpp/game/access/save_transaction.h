#pragma once

#include <cstdint>

#include "contracts/game_ports/save_transaction_port.h"
#include "contracts/result.h"

namespace qol::game {

// ---- SAVE_LoadCharacterAll → SAVE_Save loading 旁路（严格 fail-closed）----
// 安装/绑定期解析 LOAD_CHARACTER_ALL 内部到 SAVE_Save 的唯一 BL 建立合法返回点；
// 禁止在代码中保存该 BL 的裸地址。

// 旁路扫描结果；valid=false 表示未建立（零命中或多命中均拒绝）。
struct SaveBypassPlan {
    uintptr_t callsite;
    uintptr_t save_entry;
    bool valid;
};

// 扫描窗口 [entry, entry+span) 内目标为 save_entry 的 BL；唯一命中才有效。
// 内存读取经注入的 reader（生产为 game_memory_accessible + 解引用），使扫描纯逻辑可测。
using SaveBypassReadFn = bool (*)(void* ctx, uintptr_t site, uint32_t* out_word);
SaveBypassPlan save_bypass_scan(uintptr_t entry, uintptr_t span, uintptr_t save_entry,
                                SaveBypassReadFn read, void* read_ctx);
// 生产 reader（game_memory_accessible + 解引用）的便捷入口。
SaveBypassPlan save_bypass_scan_production(uintptr_t entry, uintptr_t span, uintptr_t save_entry);

// ---- Game thunk invocation：仅当前回调有效的旁路凭据 ----
// 只能由持有分配权的 Game 层 thunk 调用；HTTP/插件面无法取得，origin 因此不可自报。
uint64_t save_invocation_begin(uint32_t kind, int32_t target_slot);
bool save_invocation_valid(uint64_t invocation, uint32_t kind, int32_t target_slot);
// 当前 invocation 是否匹配 kind/target（不要求调用方持有 id；loading 旁路仲裁用）。
bool save_invocation_current_matches(uint32_t kind, int32_t target_slot);
void save_invocation_end(uint64_t invocation);

// ---- loading 旁路放行判定矩阵 ----
struct SaveLoadingBypassInput {
    bool on_game_main_thread;
    bool invocation_verified;   // invocation 经 Adapter 验证来自该 BL 的 thunk
    bool has_unready_load_transition;
    int32_t transition_target;
    int32_t current_slot;
    bool lease_covers_slot;     // gate 判定 lease 有效且覆盖该槽
    bool joint_save_in_flight;  // 存在普通联合保存事务嵌套
    bool session_ready;         // 模块 session.ready；ready 时非旁路管辖，放行普通路径
};
// 放行条件（同时满足）：主线程 + invocation 来自该 BL + 存在尚未 ready 的读档 transition
// 且 target==current_slot + lease 覆盖该槽 + 无联合保存事务嵌套。任一不满足且 session
// 非 ready：Blocked，invoked=0，不执行原版；不允许「凡 loading 一律透传」。
QolResult save_admit_loading_bypass(const SaveLoadingBypassInput& input);

// P3c 生产仲裁：Adapter 侧收集 Game 层事实（当前读档 invocation、未就绪读档转换、
// 当前槽、gate 对 lease 的覆盖校验、联合保存嵌套），session_ready 恒按 false 执行矩阵
//（探针只在 session 非 ready 时被调用）。主线程由调用方（core.save wrapper）前置判定。
// lease 为 core.save 未决转换的凭据；放行 ok，拒绝 Blocked。
QolResult save_loading_bypass_admit(const contracts::QolSlotOperationLease* lease, int32_t slot);

// SAVE_Save 入口 hook 的 trampoline 绑定类别常量（kQolSaveBackupEntrySaveSave）
// 收口在 contracts/game_ports/save_transaction_port.h；本层不另设值。

// ---- 读档 transition 与联合保存事务状态（P3c 生产接线；本批由宿主/测试驱动）----
void save_set_pending_transition(uint64_t transition, int32_t target_slot, bool ready);
void save_clear_pending_transition();
bool save_has_unready_load_transition(int32_t* out_target);
void save_set_joint_save_in_flight(bool in_flight);

// ---- gate / 平台路径注入（宿主装配期；测试注入 fake）----
struct SaveTransactionGate {
    void* ctx;
    bool (*operation_admits)(void* ctx, contracts::QolOwnerToken owner,
                             const contracts::QolSlotOperationLease* lease, int32_t slot);
};
void save_transaction_bind_gate(const SaveTransactionGate* gate);
// 经既有 nativeSaveBackupInit 注入点传入 dataDir；未注入根时 read_evidence 返回 NotReady。
void save_transaction_set_platform_dir(const char* root);
// SAVE_GetSaveFileName 的可替换缝：生产装配期注入解析地址，测试注入 fake。
void save_transaction_set_get_name_fn(void (*fn)(int32_t slot, char* out));
// 当前槽查询的可替换缝：生产装配期注入（save_current_slot），测试注入 fake。
void save_transaction_set_current_slot_fn(int (*fn)());

// ---- port 支撑（game/adapter_ports/save_transaction_port 转接）----
QolResult save_transaction_bind_backup(uint32_t entry_kind, void* framework_backup);
QolResult save_transaction_call_original_save(const contracts::QolSlotOperationLease* lease,
                                              int32_t expected_slot,
                                              contracts::QolSaveOriginalResult* out);
QolResult save_transaction_call_transition(const contracts::QolSlotOperationLease* lease,
                                           const contracts::QolSaveTransitionCall* call,
                                           contracts::QolSaveOriginalResult* out);
QolResult save_transaction_read_evidence(const contracts::QolSlotOperationLease* lease,
                                         int32_t slot, contracts::QolSaveFileEvidence* out);

// 相对存档文件名校验：非空、无路径分隔、无 ".."、白名单字符集、不以 "." 开头、长度受限。
bool save_transaction_dat_relative_name_valid(const char* name);

// ---- P3c：API 与按钮读档共用的受控转换上下文（作用域释放）----
// begin：设置未就绪读档 transition（SAVE_Save loading 旁路矩阵输入）并登记仅本次回调
// 有效的 invocation；返回 invocation id。end：清除 pending transition 并结束 invocation。
uint64_t save_load_bypass_begin(uint64_t transition, int32_t target_slot, uint32_t kind);
void save_load_bypass_end(uint64_t invocation);

// 原版转换函数注入（game 层装配期从 symbol 解析后注入；测试注入 fake）。
void save_transaction_set_resume_fn(int (*fn)(int32_t));
void save_transaction_set_new_game_fn(void (*fn)(int32_t));

void save_transaction_reset_for_test();

}  // namespace qol::game

#include "game/adapter_ports/save_transaction_port.h"

#include <cstring>

#include "contracts/game_ports/ports.h"
#include "game/access/game_access.h"
#include "game/access/save.h"
#include "game/access/save_transaction.h"
#include "game/access/symbols.h"
#include "game/symbols/game_symbols.h"

namespace qol::game {
namespace {

QolResult adapter_read_state(void*, contracts::QolSaveAdapterState* out) {
    if (out == nullptr || out->size < sizeof(*out))
        return contracts::error(kQolErrInvalidArgument, "invalid adapter state", 19);
    const int current = save_current_slot();
    if (current < 0) return contracts::error(kQolErrNotReady, "slot unavailable", 16);
    const int ok = save_is_ok();
    if (ok < 0) return contracts::error(kQolErrNotReady, "world state unavailable", 23);
    out->current_slot = current;
    out->world_ready = ok == 1 ? 1u : 0u;
    // 原版新档流程标志暂无可靠观测点，恒 0；转换状态由 core.save 侧维护。
    out->new_game_flag = 0;
    return contracts::ok();
}

QolResult adapter_bind_backup(void*, uint32_t entry_kind, void* framework_backup) {
    return save_transaction_bind_backup(entry_kind, framework_backup);
}

QolResult adapter_call_original_save(void*, const contracts::QolSlotOperationLease* lease,
                                     int32_t expected_slot,
                                     contracts::QolSaveOriginalResult* out) {
    return save_transaction_call_original_save(lease, expected_slot, out);
}

QolResult adapter_call_transition(void*, const contracts::QolSlotOperationLease* lease,
                                  const contracts::QolSaveTransitionCall* call,
                                  contracts::QolSaveOriginalResult* out) {
    return save_transaction_call_transition(lease, call, out);
}

QolResult adapter_read_evidence(void*, const contracts::QolSlotOperationLease* lease, int32_t slot,
                                contracts::QolSaveFileEvidence* out) {
    return save_transaction_read_evidence(lease, slot, out);
}

const contracts::QolSaveTransactionPort kPort{
    {sizeof(contracts::QolSaveTransactionPort),
     contracts::kQolPortSaveTransactionVersion.major,
     contracts::kQolPortSaveTransactionVersion.minor},
    nullptr,
    &adapter_read_state,
    &adapter_bind_backup,
    &adapter_call_original_save,
    &adapter_call_transition,
    &adapter_read_evidence,
};

}  // namespace

const contracts::QolSaveTransactionPort& save_transaction_port() { return kPort; }

// 生产内存读取：可访问才读，不可访问按读取失败处理（扫描提前结束，fail-closed）。
namespace {
bool bypass_read_production(void*, uintptr_t site, uint32_t* out_word) {
    if (!game_memory_accessible(reinterpret_cast<const void*>(site), 4, 'r')) return false;
    *out_word = *reinterpret_cast<const uint32_t*>(site);
    return true;
}
}  // namespace

SaveBypassPlan save_bypass_scan_production(uintptr_t entry, uintptr_t span,
                                           uintptr_t save_entry) {
    return save_bypass_scan(entry, span, save_entry, &bypass_read_production, nullptr);
}

void save_transaction_install() {
    // elf-export 名解析，VMA 仅兜底（symbol_registry 登记）；monster 跳板跟随由
    // resolve_hook_entry 统一处理。BL 目标必须是 SAVE_Save 的解析入口。
    const uintptr_t load_all = resolve_hook_entry("elf-export:SAVE_LoadCharacterAll", nullptr);
    const uintptr_t save_entry = resolve_hook_entry("elf-export:SAVE_Save", nullptr);
    if (load_all != 0 && save_entry != 0) {
        // 本批不装 hook；callsite 由 P3c 的 thunk 消费。扫描结果不落任何全局状态，
        // 每次安装期重算，避免持有跨进程生命周期的裸地址副本。
        (void)save_bypass_scan_production(load_all, F_SAVE_LOAD_CHARACTER_ALL_SPAN, save_entry);
    }
    // 相对名缝与当前槽缝：已登记 symbol_registry 的解析入口。
    if (g_base != 0) {
        const uintptr_t get_name =
            g_base + fn_resolve("F_SAVE_GET_SAVE_FILE_NAME_VMA", F_SAVE_GET_SAVE_FILE_NAME_VMA);
        if (get_name != g_base) {
            save_transaction_set_get_name_fn(reinterpret_cast<void (*)(int32_t, char*)>(get_name));
        }
        save_transaction_set_current_slot_fn(&save_current_slot);
    }
}

}  // namespace qol::game

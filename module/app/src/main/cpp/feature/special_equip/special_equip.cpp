// special_equip.cpp —— 特殊装备解锁：从 ITEMDATABASE +7 bit4 的 NPC 保护中放行非特殊 NPC。
//
// 设计口径、取舍理由与角色闸门归属见 special_equip.h 头部注释。

#include "feature/special_equip/special_equip.h"

#include "core/native/qol_log.h"
#include "feature/patch/native_inventory_hook.h"
#include "game_access.h"
#include "game_symbols.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace {

std::atomic<bool> g_attempted{false};
std::atomic<bool> g_installed{false};

// LSPosed NativeHook 写入的原始函数指针：wrapper 必须经 backup 转发，否则原版判定丢失。
CanEquipFn g_backup_can_equip = nullptr;
CanUnequipFn g_backup_can_unequip = nullptr;

// 真机验收用的「翻转计数」：只有「原版拒绝 → 本域放行」才 +1，便于确认生效范围。
std::atomic<uint64_t> g_override_count{0};

// 真机排障用的「未放行原因」预算：UI 每次构建详情菜单都会调，不设上限会刷爆日志。
std::atomic<int> g_reject_log_budget{120};

// ---------------------------------------------------------------------------
// 表访问（只读，仅用于诊断：确认 category 与 +7 bit4 的实际取值）
// ---------------------------------------------------------------------------

int item_category(void* item) {
    if (item == nullptr) return -1;
    const uint16_t flags = *reinterpret_cast<const uint16_t*>(
        static_cast<const uint8_t*>(item) + I_TYPE);
    return static_cast<int>((flags >> 6) & 0x3FF);
}

uint8_t* itemdb_record(int category) {
    if (g_base == 0 || category < 0) return nullptr;
    uint8_t* base = *reinterpret_cast<uint8_t**>(
        *reinterpret_cast<void**>(g_base + G_ITEMCLASS_DATA_GOT_VMA));
    uint8_t* size_ptr = *reinterpret_cast<uint8_t**>(g_base + G_ITEMCLASS_SIZE_GOT_VMA);
    if (base == nullptr || size_ptr == nullptr) return nullptr;
    const uint8_t stride = *size_ptr;
    if (stride == 0) return nullptr;
    return base + static_cast<size_t>(category) * stride;
}

// ---------------------------------------------------------------------------
// 原版判定链的「跳过 bit4」重放（纯读，无副作用）
// ---------------------------------------------------------------------------

// `CHAR_CanEquipItem @ 0xe4eb4` 的四道检查，逐条 objdump 核对（0xe4ec8-0xe4f60）：
//   ① ITEM_IsRealEquip(item) != 0                      @0xe4edc-0xe4ee4
//   ② (ITEMDATABASE[cat].+7 & 0x10) == 0               @0xe4f30-0xe4f3c  ← 本域跳过
//   ③ CHAR_CanChangeEquip(ch) != 0                     @0xe4f40-0xe4f4c
//   ④ *(u8*)(ch + C_LEVEL) >= ITEM_GetEquipLevel(item) @0xe4f50-0xe4f60
// ⑤「职业掩码」段（0xe4f64-0xe4fcc）读槽位表 [type].+5 后 `asr`，随即被 `mov w0,#1` 覆盖，
//   等价于恒真、不参与返回值，重放时一并省略（与真机行为一致）。
//
// 调用前提：原版已返回 0。因此「本函数返回真」精确等价于「原版只因 ② 而拒绝」。
// 依赖缺失时返回假（fail-closed：保持原版拒绝，绝不误放行）。
bool equip_allows_without_no_equip_bit(void* ch, void* item) {
    if (fn_item_is_real_equip == nullptr || fn_char_can_change_equip == nullptr ||
        fn_item_get_equip_level == nullptr) {
        return false;
    }
    if (!fn_item_is_real_equip(item)) return false;              // ①
    if (!fn_char_can_change_equip(ch)) return false;             // ③ ← 特殊 NPC 在此被拦
    // ④：原版用 `ldrb` 取等级（零扩展 u8）、`cmp` + `b.lt`（32 位有符号比较），此处照抄。
    const int32_t char_level =
        *reinterpret_cast<const uint8_t*>(static_cast<const uint8_t*>(ch) + C_LEVEL);
    return char_level >= fn_item_get_equip_level(item);
}

// `CHAR_CanUnequipItem @ 0xe4e2c` 的两道检查（0xe4e38-0xe4eb0）：
//   ① (ITEMDATABASE[cat].+7 & 0x10) == 0   @0xe4e74-0xe4e80  ← 本域跳过
//   ② CHAR_CanChangeEquip(ch) != 0         @0xe4e94-0xe4ea4
bool unequip_allows_without_no_equip_bit(void* ch) {
    if (fn_char_can_change_equip == nullptr) return false;
    return fn_char_can_change_equip(ch) != 0;                    // ② ← 特殊 NPC 在此被拦
}

// 仅 debug 日志开启时记录，避免热路径噪声（穿装/卸下都是用户操作级频率，无需预算门控）。
void log_override(const char* what, void* ch, void* item) {
    if (!qol_log_debug_enabled()) return;
    QOL_LOG_DEBUG(QolDomain::kSpecialEquip, "%s overridden ch=%p item=%p count=%llu", what, ch, item,
                  static_cast<unsigned long long>(g_override_count.load(std::memory_order_relaxed)));
}

// 「原版拒绝且本域也未放行」的原因快照。UI 的装备按钮灰化正是走这条路径，
// 打出来即可判定是 ①真装备 / ③角色权限 / ④等级 中的哪一道挡住的。
void log_reject(int category, void* ch, void* item) {
    if (!qol_log_debug_enabled()) return;
    if (g_reject_log_budget.fetch_sub(1, std::memory_order_relaxed) <= 0) return;
    const uint8_t* c = static_cast<const uint8_t*>(ch);
    const uint8_t* rec = itemdb_record(category);
    QOL_LOG_DEBUG(QolDomain::kSpecialEquip,
                  "not overridden ch=%p item=%p cat=%d bit4=%d ctype=%d mercSlot=%d "
                  "realEquip=%d canChange=%d chLv=%d itemLv=%d",
                  ch, item, category, rec != nullptr ? ((rec[7] >> 4) & 1) : -1,
                  static_cast<int>(*reinterpret_cast<const int8_t*>(c + C_TYPE)),
                  static_cast<int>(*reinterpret_cast<const int8_t*>(c + C_MERC_SLOT)),
                  fn_item_is_real_equip != nullptr ? fn_item_is_real_equip(item) : -1,
                  fn_char_can_change_equip != nullptr ? fn_char_can_change_equip(ch) : -1,
                  static_cast<int>(*reinterpret_cast<const uint8_t*>(c + C_LEVEL)),
                  fn_item_get_equip_level != nullptr ? fn_item_get_equip_level(item) : -1);
}

int can_equip_wrapper(void* ch, void* item) {
    if (g_backup_can_equip == nullptr) return 0;  // 未安装完成：不下放行（不会发生）
    if (g_backup_can_equip(ch, item) != 0) return 1;  // 原版放行 → 快速路径，不介入
    if (ch == nullptr || item == nullptr) return 0;
    if (!equip_allows_without_no_equip_bit(ch, item)) {
        log_reject(item_category(item), ch, item);  // UI 灰化走这条路径
        return 0;
    }
    g_override_count.fetch_add(1, std::memory_order_relaxed);
    log_override("can_equip", ch, item);
    return 1;
}

int can_unequip_wrapper(void* ch, void* item) {
    if (g_backup_can_unequip == nullptr) return 0;
    if (g_backup_can_unequip(ch, item) != 0) return 1;
    if (ch == nullptr || item == nullptr) return 0;
    if (!unequip_allows_without_no_equip_bit(ch)) {
        log_reject(item_category(item), ch, item);
        return 0;
    }
    g_override_count.fetch_add(1, std::memory_order_relaxed);
    log_override("can_unequip", ch, item);
    return 1;
}

// 依赖是否齐备。缺任一即不安装（fail-closed），也不重试（避免每帧刷日志，
// 与 attribute_range / simple_mode 的 once 语义一致）。
bool deps_ready() {
    return fn_can_equip != nullptr && fn_can_unequip != nullptr &&
           fn_item_is_real_equip != nullptr && fn_char_can_change_equip != nullptr &&
           fn_item_get_equip_level != nullptr;
}

bool install_one(NativeHookFunType hook, uintptr_t target, void* replacement, void** backup,
                 const char* name) {
    const int rc = hook(reinterpret_cast<void*>(target), replacement, backup);
    if (rc != 0 || backup == nullptr || *backup == nullptr) {
        QOL_LOG_ERROR(QolDomain::kSpecialEquip, "%s hook failed rc=%d target=%p", name, rc,
                      reinterpret_cast<void*>(target));
        return false;
    }
    return true;
}

}  // namespace

void special_equip_install_if_ready() {
    if (g_installed.load(std::memory_order_acquire)) return;
    if (!bridge_ready()) return;
    NativeHookFunType hook = native_hook_func();
    if (hook == nullptr) return;

    bool expected = false;
    if (!g_attempted.compare_exchange_strong(expected, true)) return;

    if (!deps_ready()) {
        QOL_LOG_ERROR(QolDomain::kSpecialEquip,
                      "symbols unresolved (can_equip=%d can_unequip=%d is_real_equip=%d "
                      "can_change_equip=%d equip_level=%d); not installed",
                      fn_can_equip != nullptr, fn_can_unequip != nullptr,
                      fn_item_is_real_equip != nullptr, fn_char_can_change_equip != nullptr,
                      fn_item_get_equip_level != nullptr);
        return;
    }

    if (!install_one(hook, reinterpret_cast<uintptr_t>(fn_can_equip),
                     reinterpret_cast<void*>(&can_equip_wrapper),
                     reinterpret_cast<void**>(&g_backup_can_equip), "CHAR_CanEquipItem")) {
        return;
    }
    if (!install_one(hook, reinterpret_cast<uintptr_t>(fn_can_unequip),
                     reinterpret_cast<void*>(&can_unequip_wrapper),
                     reinterpret_cast<void**>(&g_backup_can_unequip), "CHAR_CanUnequipItem")) {
        return;
    }

    g_installed.store(true, std::memory_order_release);
    QOL_LOG_INFO(QolDomain::kSpecialEquip,
                 "hooks installed (always-on, no config gate): canEquip=%p canUnequip=%p",
                 reinterpret_cast<void*>(fn_can_equip), reinterpret_cast<void*>(fn_can_unequip));
}

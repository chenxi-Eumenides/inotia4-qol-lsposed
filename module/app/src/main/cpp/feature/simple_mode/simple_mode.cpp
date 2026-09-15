#include "feature/simple_mode/simple_mode.h"

#include "feature/simple_mode/simple_mode_rules.h"

#include "core/native/qol_log.h"
#include "feature/patch/native_inventory_hook.h"
#include "game_access.h"
#include "game_symbols.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstddef>

namespace {

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_attempted{false};
std::atomic<bool> g_installed{false};

// LSPosed NativeHook 写入的原始函数指针：wrapper 必须经 backup 转发，否则游戏逻辑丢失。
CharAddDamageFn g_backup_add_damage = nullptr;
CharUpdateAttrFromMonsterFn g_backup_update_attr_from_monster = nullptr;

// 诊断（真机验收用）：记录两个 hook 是否走了「跳板跟随」，以及依赖是否齐备。
std::atomic<bool> g_damage_entry_followed{false};
std::atomic<bool> g_faction_deps_missing_logged{false};
std::atomic<bool> g_pool_slot_unresolved_logged{false};

using simple_mode::Side;

// ---------------------------------------------------------------------------
// 阵营判定
// ---------------------------------------------------------------------------

// 阵营判定依赖的三个游戏函数是否都已解析。缺失时一律判为「非玩家侧」并只记一次日志：
// fail-safe 方向是「不发加成」，绝不因缺符号而把敌人误判成友方（那会让玩家挨打不掉血）。
bool faction_deps_ready() {
    return fn_char_get_party_index != nullptr && fn_char_is_active_player_group != nullptr &&
           fn_char_get_summoner != nullptr;
}

void note_faction_deps_missing_once() {
    if (g_faction_deps_missing_logged.exchange(true)) return;
    QOL_LOG_WARN(QolDomain::kSimpleMode,
                 "faction symbols unresolved (party_index=%d active_group=%d summoner=%d); "
                 "no bonus will be applied",
                 fn_char_get_party_index != nullptr, fn_char_is_active_player_group != nullptr,
                 fn_char_get_summoner != nullptr);
}

// 召唤链递归深度上限：正常链路只有「召唤者 → 召唤物」一层，留余量防异常数据自环。
constexpr int k_max_summon_depth = 4;

// 是否属玩家侧：主角 / 队友 / 主控或队友的召唤物。
// 三个原语各自的缺口：
//   CHAR_GetPartyIndex       覆盖主角 + 2 名队友，不含任何召唤物；
//   CHAR_IsActivePlayerGroup 覆盖主控本人 + 主控的召唤物，不含队友；
//   CHAR_GetSummoner         反查召唤者，用于递归补齐「队友的召唤物」。
bool is_player_side_impl(const void* ch, int depth) {
    if (ch == nullptr) return false;
    if (!faction_deps_ready()) {
        note_faction_deps_missing_once();
        return false;
    }
    void* mutable_ch = const_cast<void*>(ch);
    if (fn_char_get_party_index(mutable_ch) != -1) return true;
    if (fn_char_is_active_player_group(mutable_ch) != 0) return true;
    if (depth >= k_max_summon_depth) return false;
    void* summoner = fn_char_get_summoner(mutable_ch);
    if (summoner == nullptr || summoner == ch) return false;
    return is_player_side_impl(summoner, depth + 1);
}

// 完整归类。C_TYPE 语义见 game_symbols.h：0=玩家侧角色 1=怪物 2=NPC/装饰物。
// 注意顺序：先判玩家侧——玩家召唤物的 C_TYPE 也是 1，只看类型会把它们误判成敌人。
Side classify(const void* ch) {
    if (ch == nullptr) return Side::kNeutral;
    if (is_player_side_impl(ch, 0)) return Side::kPlayer;
    const int8_t type = *reinterpret_cast<const int8_t*>(reinterpret_cast<const uint8_t*>(ch) + C_TYPE);
    return type == 1 ? Side::kMonster : Side::kNeutral;
}

// ---------------------------------------------------------------------------
// 角色池槽定位（半血幂等账本用）
// ---------------------------------------------------------------------------

// 把角色指针换算为角色池槽号。返回 false 表示不在池内/未对齐——调用方须 fail-safe 放弃改写。
bool pool_slot_of(const void* ch, int* out_slot) {
    if (g_base == 0 || ch == nullptr || out_slot == nullptr) return false;
    const uint8_t* pool = *reinterpret_cast<const uint8_t* const*>(g_base + G_CHAR_POOL_VMA);
    if (pool == nullptr) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(ch);
    if (p < pool) return false;
    const ptrdiff_t delta = p - pool;
    if (delta % static_cast<ptrdiff_t>(C_OBJ_SIZE) != 0) return false;
    const ptrdiff_t slot = delta / static_cast<ptrdiff_t>(C_OBJ_SIZE);
    if (slot < 0 || slot >= C_CHARSYSTEM_POOL_SLOTS) return false;
    *out_slot = static_cast<int>(slot);
    return true;
}

// 半血幂等账本（按角色池槽）。仅游戏线程访问，无需同步。
//
// 为什么必须有账本：CHAR_UpdateAttrFromMonster 是「读旧槽值 → 变换 → 写回」的幂等调整层
// （唯一写点 e011c `str w20,[x19,#0x24]`，而 w20 初值读自同一槽 e008c；详见 game_symbols.h
//  F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA 的反汇编证据）。若我们每次返回后都无条件除 2，
// 下一次重算会把我们写出的半值读回再被除 2 → 1/2 → 1/4 → 1/8 累积。
// 规则：槽值与账本记录的上次半值相同 ⇒ 本次只是把我们的结果原样写回 ⇒ 跳过；
//       不同 ⇒ 游戏给出了新的满值 ⇒ 重新减半一次。
struct HalfHpMark {
    const void* ch;
    int32_t halved;
};

HalfHpMark g_half_marks[C_CHARSYSTEM_POOL_SLOTS] = {};

// ---------------------------------------------------------------------------
// Hook wrapper
// ---------------------------------------------------------------------------

void char_add_damage_wrapper(void* attacker, void* victim, int32_t damage, int32_t damage_flag,
                            int32_t magic_flag) {
    int32_t out_damage = damage;
    if (g_enabled.load(std::memory_order_relaxed) && damage > 0) {
        const int percent = simple_mode::damage_percent(classify(attacker), classify(victim));
        if (percent != 100) out_damage = simple_mode::scale_by_percent(damage, percent);
    }
    if (g_backup_add_damage == nullptr) return;
    g_backup_add_damage(attacker, victim, out_damage, damage_flag, magic_flag);
}

void char_update_attr_from_monster_wrapper(void* ch, int32_t attr) {
    if (g_backup_update_attr_from_monster == nullptr) return;
    g_backup_update_attr_from_monster(ch, attr);
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    // 只处理最大生命槽；1/2 的 attr 走其它分支，热路径上尽早退出。
    if (ch == nullptr || attr != ATTR_MAX_HP) return;

    const int8_t type = *reinterpret_cast<const int8_t*>(reinterpret_cast<const uint8_t*>(ch) + C_TYPE);
    if (type != 1) return;                 // 本函数只被 C_TYPE==1 分派，此处为双保险
    if (is_player_side_impl(ch, 0)) return;  // 玩家侧召唤物不减半

    int slot = 0;
    if (!pool_slot_of(ch, &slot)) {
        // 定位不到池槽就无法保证幂等（会累积减半），故不改写——fail-safe 方向是「怪物保持满血」。
        if (!g_pool_slot_unresolved_logged.exchange(true)) {
            QOL_LOG_WARN(QolDomain::kSimpleMode, "char not in pool; max-hp halving skipped");
        }
        return;
    }

    uint8_t* base = reinterpret_cast<uint8_t*>(ch);
    const int32_t raw = *reinterpret_cast<const int32_t*>(base + C_MAX_HP);
    HalfHpMark& mark = g_half_marks[slot];
    if (mark.ch != ch) {  // 槽被新角色占用（或首次见）→ 重置该槽账本
        mark.ch = ch;
        mark.halved = 0;
    }
    if (mark.halved != 0 && raw == mark.halved) return;  // 本值已减半过 → 幂等跳过

    const int32_t half = simple_mode::halved_max_hp(raw);
    if (half == raw) return;  // 已是下限 1，无意义
    *reinterpret_cast<int32_t*>(base + C_MAX_HP) = half;
    mark.halved = half;

    // 同步钳制当前 HP：否则「上限已半、当前血仍是满值」会表现为血条超上限。
    const int32_t hp = *reinterpret_cast<const int32_t*>(base + C_HP);
    if (hp > half) *reinterpret_cast<int32_t*>(base + C_HP) = half;
}

// ---------------------------------------------------------------------------
// 入口解析（全版本通用）
// ---------------------------------------------------------------------------

// 解析可安装 hook 的入口地址。
// monster 全系把 CHAR_AddDamage 入口改写成无条件 b 跳板（v20 `b 0x14e280`；v23-v27 `b 0x7450b8`，
// 落于第二可执行段）。跳板入口先 `stp x0..x8` 保存 x0-x7 再修改寄存器，故跳板目标处 ABI 与函数
// 入口完全一致，在目标处 hook 与原位等价；而 inline hook 若需重定位一条 26 位 b，跨 ±128MB 的
// 偏移会失效。因此这里主动跟随一次，目标必须 4 字节对齐且落在已加载的可执行映射内。
uintptr_t hookable_entry(uintptr_t symbol_entry, bool* out_followed) {
    *out_followed = false;
    if (symbol_entry == 0 || !game_memory_accessible(reinterpret_cast<void*>(symbol_entry), 4, 'r')) {
        return symbol_entry;
    }
    const uint32_t word = *reinterpret_cast<const uint32_t*>(symbol_entry);
    constexpr uint32_t k_branch_mask = 0xFC000000u;   // b/bl/ret 等 opcode 掩码
    constexpr uint32_t k_b_opcode = 0x14000000u;      // B（无条件、不写 x30）
    if ((word & k_branch_mask) != k_b_opcode) return symbol_entry;

    int64_t offset = static_cast<int64_t>(word & 0x03FFFFFFu);
    if ((offset & 0x02000000) != 0) offset -= 0x04000000;  // 26 位有符号立即数
    const uintptr_t target = symbol_entry + static_cast<uintptr_t>(offset * 4);
    if ((target & 0x3u) != 0) return symbol_entry;
    if (!game_memory_accessible(reinterpret_cast<void*>(target), 8, 'x')) return symbol_entry;
    *out_followed = true;
    return target;
}

bool install_one(NativeHookFunType hook, uintptr_t target, void* replacement, void** backup,
                 const char* name) {
    const int rc = hook(reinterpret_cast<void*>(target), replacement, backup);
    if (rc != 0 || backup == nullptr || *backup == nullptr) {
        QOL_LOG_ERROR(QolDomain::kSimpleMode, "%s hook failed rc=%d target=%p", name, rc,
                      reinterpret_cast<void*>(target));
        return false;
    }
    return true;
}

}  // namespace

void simple_mode_set_enabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    QOL_LOG_INFO(QolDomain::kSimpleMode, "enabled=%d installed=%d", enabled,
                 g_installed.load(std::memory_order_acquire));
}

bool simple_mode_enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

std::string simple_mode_status_json() {
    char out[512];
    std::snprintf(out, sizeof(out),
                  "{\"enabled\":%s,\"installed\":%s,\"attempted\":%s,"
                  "\"damageTarget\":\"%s\",\"factionDepsReady\":%s}",
                  g_enabled.load(std::memory_order_relaxed) ? "true" : "false",
                  g_installed.load(std::memory_order_relaxed) ? "true" : "false",
                  g_attempted.load(std::memory_order_relaxed) ? "true" : "false",
                  g_damage_entry_followed.load(std::memory_order_relaxed) ? "trampoline"
                                                                          : "entry",
                  faction_deps_ready() ? "true" : "false");
    return std::string(out);
}

void simple_mode_install_if_ready() {
    if (g_installed.load(std::memory_order_acquire)) return;
    if (!bridge_ready()) return;
    NativeHookFunType hook = native_hook_func();
    if (hook == nullptr) return;

    bool expected = false;
    if (!g_attempted.compare_exchange_strong(expected, true)) return;

    // 符号未解析：不安装，也不重试（与 attribute_range 的 once 语义一致，避免每帧重试刷日志）。
    if (fn_char_add_damage == nullptr || fn_char_update_attr_from_monster == nullptr) {
        QOL_LOG_ERROR(QolDomain::kSimpleMode,
                      "symbols unresolved (add_damage=%d update_attr_from_monster=%d); not installed",
                      fn_char_add_damage != nullptr, fn_char_update_attr_from_monster != nullptr);
        return;
    }
    if (!faction_deps_ready()) note_faction_deps_missing_once();

    bool damage_entry_followed = false;
    const uintptr_t damage_entry =
        hookable_entry(reinterpret_cast<uintptr_t>(fn_char_add_damage), &damage_entry_followed);
    g_damage_entry_followed.store(damage_entry_followed, std::memory_order_relaxed);
    const uintptr_t max_hp_entry = reinterpret_cast<uintptr_t>(fn_char_update_attr_from_monster);

    if (!install_one(hook, damage_entry, reinterpret_cast<void*>(&char_add_damage_wrapper),
                     reinterpret_cast<void**>(&g_backup_add_damage), "CHAR_AddDamage")) {
        return;
    }
    if (!install_one(hook, max_hp_entry,
                     reinterpret_cast<void*>(&char_update_attr_from_monster_wrapper),
                     reinterpret_cast<void**>(&g_backup_update_attr_from_monster),
                     "CHAR_UpdateAttrFromMonster")) {
        return;
    }

    g_installed.store(true, std::memory_order_release);
    QOL_LOG_INFO(QolDomain::kSimpleMode, "hooks installed: damageTarget=%s maxHpTarget=entry",
                 g_damage_entry_followed.load(std::memory_order_relaxed) ? "trampoline" : "entry");
}

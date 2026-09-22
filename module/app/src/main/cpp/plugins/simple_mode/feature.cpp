#include "feature.h"

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>

#include "rules.h"

namespace qol::features::simple_mode {
namespace {

std::atomic<bool> g_gate{false};
const contracts::CharacterFactsApi* g_character_facts = nullptr;

// wrapper 入口函数指针类型：与游戏函数 ABI 一致。原函数入口由宿主经 on_resource_ready 交还。
using AddDamageFn = void (*)(void*, void*, int32_t, int32_t, int32_t);
using UpdateAttrFromMonsterFn = void (*)(void*, int32_t);

std::atomic<void*> g_backup_add_damage{nullptr};
std::atomic<void*> g_backup_update_attr_from_monster{nullptr};

// 诊断（真机验收用）：异常态只记一次，避免热路径反复刷日志。
std::atomic<bool> g_faction_deps_missing_logged{false};
std::atomic<bool> g_pool_slot_unresolved_logged{false};

void log_write(int32_t level, const char* format, va_list args) {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->log.write == nullptr || format == nullptr) return;
    char message[contracts::kMessageCap + 1]{};
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    if (length < 0) return;
    const uint32_t message_length = static_cast<uint32_t>(
        length < static_cast<int>(contracts::kMessageCap) ? length : contracts::kMessageCap);
    (void)sdk->log.write(level, 16, "plugins/simple_mode/feature.cpp", message, message_length);
}

void log_info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log_write(1, format, args);
    va_end(args);
}

void log_warn(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log_write(2, format, args);
    va_end(args);
}

// ---------------------------------------------------------------------------
// 阵营判定
// ---------------------------------------------------------------------------

// 阵营判定依赖的三个游戏函数是否都已解析。缺失时一律判为「非玩家侧」并只记一次日志：
// fail-safe 方向是「不发加成」，绝不因缺符号而把敌人误判成友方（那会让玩家挨打不掉血）。
void note_faction_deps_missing_once(bool party_index, bool active_group, bool summoner) {
    if (g_faction_deps_missing_logged.exchange(true)) return;
    log_warn("faction symbols unresolved (party_index=%d active_group=%d summoner=%d); "
             "no bonus will be applied",
             party_index, active_group, summoner);
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
    const auto* game = game_host();
    if (game == nullptr || g_character_facts == nullptr ||
        g_character_facts->is_active_player_group == nullptr ||
        character_extras()->read.is_party_member == nullptr ||
        character_extras()->read.summoner == nullptr) {
        note_faction_deps_missing_once(
            game != nullptr && character_extras()->read.is_party_member != nullptr,
            g_character_facts != nullptr && g_character_facts->is_active_player_group != nullptr,
            game != nullptr && character_extras()->read.summoner != nullptr);
        return false;
    }
    void* mutable_ch = const_cast<void*>(ch);
    int32_t party_member = 0;
    int32_t active_group = 0;
    void* summoner = nullptr;
    const bool party_ok = character_extras()->read.is_party_member(mutable_ch, &party_member).ok == kQolOk;
    const bool active_ok =
        g_character_facts->is_active_player_group(mutable_ch, &active_group).ok == kQolOk;
    const bool summoner_ok = character_extras()->read.summoner(mutable_ch, &summoner).ok == kQolOk;
    if (!party_ok || !active_ok || !summoner_ok) {
        note_faction_deps_missing_once(party_ok, active_ok, summoner_ok);
        return false;
    }
    if (party_member != 0) return true;
    if (active_group != 0) return true;
    if (depth >= k_max_summon_depth) return false;
    if (summoner == nullptr || summoner == ch) return false;
    return is_player_side_impl(summoner, depth + 1);
}

// 完整归类。C_TYPE 语义：0=玩家侧角色 1=怪物 2=NPC/装饰物。
// 注意顺序：先判玩家侧——玩家召唤物的 C_TYPE 也是 1，只看类型会把它们误判成敌人。
Side classify(const void* ch) {
    if (ch == nullptr) return Side::kNeutral;
    if (is_player_side_impl(ch, 0)) return Side::kPlayer;
    int32_t type = -1;
    const auto* game = game_host();
    if (game == nullptr || g_character_facts == nullptr || g_character_facts->type == nullptr ||
        g_character_facts->type(ch, &type).ok != kQolOk) {
        return Side::kNeutral;
    }
    return type == 1 ? Side::kMonster : Side::kNeutral;
}

// ---------------------------------------------------------------------------
// 半血幂等账本（按角色池槽）
// ---------------------------------------------------------------------------

// 仅游戏线程访问，无需同步。
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

constexpr int kCharPoolSlots = 100;
constexpr int32_t kMaxHpAttribute = 0x1e;
HalfHpMark g_half_marks[kCharPoolSlots] = {};

}  // namespace

// ---------------------------------------------------------------------------
// Hook wrapper（定义在命名空间作用域：地址要交给宿主安装，需外部链接；
// 可访问匿名命名空间注入到本命名空间的门禁与 backup 原子）
// ---------------------------------------------------------------------------

void char_add_damage_wrapper(void* attacker, void* victim, int32_t damage, int32_t damage_flag,
                            int32_t magic_flag) {
    const AddDamageFn backup =
        reinterpret_cast<AddDamageFn>(g_backup_add_damage.load(std::memory_order_acquire));
    int32_t out_damage = damage;
    if (g_gate.load(std::memory_order_relaxed) && damage > 0) {
        const int percent = damage_percent(classify(attacker), classify(victim));
        if (percent != 100) out_damage = scale_by_percent(damage, percent);
    }
    // fail-closed 透传：backup 为空（安装失败/尚未交还）时直接返回，不崩。
    if (backup == nullptr) return;
    backup(attacker, victim, out_damage, damage_flag, magic_flag);
}

void char_update_attr_from_monster_wrapper(void* ch, int32_t attr) {
    const UpdateAttrFromMonsterFn backup = reinterpret_cast<UpdateAttrFromMonsterFn>(
        g_backup_update_attr_from_monster.load(std::memory_order_acquire));
    if (backup == nullptr) return;
    backup(ch, attr);
    if (!g_gate.load(std::memory_order_relaxed)) return;
    // 只处理最大生命槽；1/2 的 attr 走其它分支，热路径上尽早退出。
    if (ch == nullptr || attr != kMaxHpAttribute) return;

    const auto* game = game_host();
    if (game == nullptr || g_character_facts == nullptr || g_character_facts->type == nullptr ||
        character_extras()->read.pool_slot == nullptr || character_extras()->read.max_hp == nullptr ||
        game->character_write.max_hp == nullptr || character_extras()->read.hp == nullptr ||
        game->character_write.hp == nullptr) {
        return;
    }
    int32_t type = -1;
    if (g_character_facts->type(ch, &type).ok != kQolOk || type != 1) return;
    if (is_player_side_impl(ch, 0)) return;  // 玩家侧召唤物不减半

    int slot = 0;
    if (character_extras()->read.pool_slot(ch, &slot).ok != kQolOk || slot < 0 ||
        slot >= kCharPoolSlots) {
        // 定位不到池槽就无法保证幂等（会累积减半），故不改写——fail-safe 方向是「怪物保持满血」。
        if (!g_pool_slot_unresolved_logged.exchange(true)) {
            log_warn("char not in pool; max-hp halving skipped");
        }
        return;
    }

    int32_t raw = 0;
    if (character_extras()->read.max_hp(ch, &raw).ok != kQolOk) return;
    HalfHpMark& mark = g_half_marks[slot];
    if (mark.ch != ch) {  // 槽被新角色占用（或首次见）→ 重置该槽账本
        mark.ch = ch;
        mark.halved = 0;
    }
    if (mark.halved != 0 && raw == mark.halved) return;  // 本值已减半过 → 幂等跳过

    const int32_t half = halved_max_hp(raw);
    if (half == raw) return;  // 已是下限 1，无意义
    if (game->character_write.max_hp(ch, half).ok != kQolOk) return;
    mark.halved = half;

    // 同步钳制当前 HP：否则「上限已半、当前血仍是满值」会表现为血条超上限。
    int32_t hp = 0;
    if (character_extras()->read.hp(ch, &hp).ok == kQolOk && hp > half) {
        (void)game->character_write.hp(ch, half);
    }
}

void set_gate(bool enabled) {
    g_gate.store(enabled, std::memory_order_release);
    log_info("enabled=%d", enabled);
}

void set_character_facts(const contracts::CharacterFactsApi* facts) {
    g_character_facts = facts;
}

bool gate_enabled() {
    return g_gate.load(std::memory_order_acquire);
}

// 受限生命周期回调（§3.3）：只缓存 backup，恒返回 0。
int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    (void)ctx;
    if (resource_id == nullptr) return 0;
    if (std::strcmp(resource_id, kResourceIdAddDamage) == 0) {
        g_backup_add_damage.store(backup, std::memory_order_release);
        return 0;
    }
    if (std::strcmp(resource_id, kResourceIdUpdateAttrFromMonster) == 0) {
        g_backup_update_attr_from_monster.store(backup, std::memory_order_release);
        return 0;
    }
    log_warn("unknown resource id: %s", resource_id);
    return 0;
}

}  // namespace qol::features::simple_mode

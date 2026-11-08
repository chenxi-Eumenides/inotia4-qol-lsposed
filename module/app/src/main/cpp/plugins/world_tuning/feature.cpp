#include "feature.h"

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace qol::features::world_tuning {
namespace {

constexpr int32_t kDropLargeWarnThreshold = 16;

// 角色阵营归类结果。判定输入是查询结果而非角色指针，便于穷举。
enum class Side {
    kPlayer,   // 主角 / 队友 / 主控或队友的召唤物
    kMonster,  // C_TYPE==1：怪物（含敌方召唤物）
    kNeutral,  // NPC / 装饰物 / 宝箱泉水 / 无法判定
};

constexpr uint32_t kRateCount = static_cast<uint32_t>(RateId::kCount);
static_assert(kRateCount == 7, "rates table size");

std::atomic<bool> g_gate{false};
const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::QolCharacterExtrasPort* g_extras = nullptr;
const contracts::CharacterFactsApi* g_facts = nullptr;
contracts::QolOwnerToken g_owner{};
const char* g_plugin_id = nullptr;

std::atomic<int32_t> g_rates[kRateCount] = {
    kRateDefault, kRateDefault, kRateDefault, kRateDefault, kRateDefault,
    kDropQuantityDefault, kDropRerollDefault};

// wrapper 入口函数指针类型：与游戏函数 ABI 一致。原函数入口由宿主经 on_resource_ready 交还。
using AddDamageFn = void (*)(void*, void*, int32_t, int32_t, int32_t);
using UpdateAttrFromMonsterFn = void (*)(void*, int32_t);
using AddExperienceFn = int32_t (*)(void*, int32_t, uint8_t);
using AddMoneyFn = int32_t (*)(int64_t);
using DropItemFn = void (*)(void*);
using MakeItemFn = void* (*)(int32_t, int32_t, int32_t);
using MakeEquipDetailFn = void* (*)(int32_t, int32_t, int32_t, int32_t, int32_t);

std::atomic<void*> g_backup_add_damage{nullptr};
std::atomic<void*> g_backup_update_attr_from_monster{nullptr};
std::atomic<void*> g_backup_add_experience{nullptr};
std::atomic<void*> g_backup_add_money{nullptr};
std::atomic<void*> g_backup_drop_item{nullptr};
std::atomic<void*> g_backup_make_item{nullptr};
std::atomic<void*> g_backup_make_equip_detail{nullptr};

// 掉落上下文：只在 DropItem wrapper 的同步调用栈内为非 0，用于把掉落结算与商店/合成/
// 开箱的同一批 maker 调用隔离开。仅游戏线程访问。
thread_local uint32_t g_drop_depth = 0;

// 诊断（真机验收用）：异常态只记一次，避免热路径反复刷日志。
std::atomic<bool> g_faction_deps_missing_logged{false};
std::atomic<bool> g_pool_slot_unresolved_logged{false};
std::atomic<bool> g_large_drop_quantity_logged{false};
std::atomic<bool> g_large_drop_reroll_logged{false};

void log_write(int32_t level, const char* format, va_list args) {
    const auto* sdk = g_sdk;
    if (sdk == nullptr || sdk->log.write == nullptr || format == nullptr) return;
    char message[contracts::kMessageCap + 1]{};
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    if (length < 0) return;
    const uint32_t message_length = static_cast<uint32_t>(
        length < static_cast<int>(contracts::kMessageCap) ? length : contracts::kMessageCap);
    (void)sdk->log.write(level, 16, "plugins/world_tuning/feature.cpp", message, message_length);
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

// 任一阵营依赖不可用时只记一次日志；本次按未知态处理，不缩放伤害或生命。
void note_faction_deps_missing_once() {
    if (g_faction_deps_missing_logged.exchange(true)) return;
    log_warn("faction dependencies unavailable; no scaling applied this time");
}

// 召唤链递归深度上限：正常链路只有「召唤者 → 召唤物」一层，留余量防异常数据自环。
constexpr int k_max_summon_depth = 4;

enum class Faction {
    kPlayer,
    kNonPlayer,
    kUnknown,
};

// 阵营三态判定：主角 / 队友 / 主控或队友的召唤物、明确非玩家侧、无法判定。
// 三个原语各自的缺口：
//   CHAR_GetPartyIndex       覆盖主角 + 2 名队友，不含任何召唤物；
//   CHAR_IsActivePlayerGroup 覆盖主控本人 + 主控的召唤物，不含队友；
//   CHAR_GetSummoner         反查召唤者，用于递归补齐「队友的召唤物」。
Faction faction_impl(const void* ch, int depth) {
    if (ch == nullptr || depth > k_max_summon_depth) return Faction::kUnknown;
    const auto* extras = g_extras;
    const auto* facts = g_facts;
    if (extras == nullptr || facts == nullptr || extras->read.is_party_member == nullptr ||
        facts->is_active_player_group == nullptr || extras->read.summoner == nullptr) {
        note_faction_deps_missing_once();
        return Faction::kUnknown;
    }

    void* mutable_ch = const_cast<void*>(ch);
    int32_t party_member = 0;
    if (extras->read.is_party_member(mutable_ch, &party_member).ok != kQolOk) {
        note_faction_deps_missing_once();
        return Faction::kUnknown;
    }
    if (party_member != 0) return Faction::kPlayer;

    int32_t active_group = 0;
    if (facts->is_active_player_group(mutable_ch, &active_group).ok != kQolOk) {
        note_faction_deps_missing_once();
        return Faction::kUnknown;
    }
    if (active_group != 0) return Faction::kPlayer;

    void* summoner = nullptr;
    if (extras->read.summoner(mutable_ch, &summoner).ok != kQolOk) {
        note_faction_deps_missing_once();
        return Faction::kUnknown;
    }
    if (summoner == nullptr) return Faction::kNonPlayer;
    if (summoner == ch) return Faction::kUnknown;
    return faction_impl(summoner, depth + 1);
}

// 完整归类。C_TYPE 语义：0=玩家侧角色 1=怪物 2=NPC/装饰物。
// 注意顺序：先判玩家侧——玩家召唤物的 C_TYPE 也是 1，只看类型会把它们误判成敌人。
Side classify(const void* ch) {
    if (ch == nullptr) return Side::kNeutral;
    const Faction faction = faction_impl(ch, 0);
    if (faction == Faction::kPlayer) return Side::kPlayer;
    if (faction == Faction::kUnknown) return Side::kNeutral;
    int32_t type = -1;
    const auto* facts = g_facts;
    if (facts == nullptr || facts->type == nullptr || facts->type(ch, &type).ok != kQolOk) {
        return Side::kNeutral;
    }
    return type == 1 ? Side::kMonster : Side::kNeutral;
}

// ---------------------------------------------------------------------------
// 敌怪最大生命幂等账本（按角色池槽）
// ---------------------------------------------------------------------------

// 仅游戏线程访问，无需同步。
//
// 为什么必须有账本：CHAR_UpdateAttrFromMonster 是「读旧槽值 → 变换 → 写回」的幂等调整层
// （唯一写点 e011c `str w20,[x19,#0x24]`，而 w20 初值读自同一槽 e008c；详见 game_symbols.h
//  F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA 的反汇编证据）。若我们每次返回后都无条件缩放，
// 下一次重算会把我们写出的缩放值读回再缩放 → 倍率逐次累积。
//
// 处理方式：进入时若槽值仍是上次我们写出的结果（applied），先把 raw 写回槽再调用原函数，
// 让游戏读到自己的原值；返回后把「游戏写回后的值」按倍率缩放并记录 raw/applied。
// 这样倍率变更也在下一次属性重算时生效，且不会累积。
struct HpMark {
    const void* ch;
    int32_t raw;
    int32_t applied;
};

constexpr int kCharPoolSlots = 100;
constexpr int32_t kMaxHpAttribute = 0x1e;
HpMark g_hp_marks[kCharPoolSlots] = {};

int32_t rate_value(RateId id) {
    const auto index = static_cast<uint32_t>(id);
    return index < kRateCount ? g_rates[index].load(std::memory_order_relaxed) : kRateDefault;
}

}  // namespace

// ---------------------------------------------------------------------------
// Hook wrapper（定义在命名空间作用域：地址要交给宿主安装，需外部链接）
// ---------------------------------------------------------------------------

void char_add_damage_wrapper(void* attacker, void* victim, int32_t damage, int32_t damage_flag,
                             int32_t magic_flag) {
    const auto* sdk = g_sdk;
    const contracts::InFlightGuard guard(
        sdk == nullptr ? nullptr : &sdk->in_flight, {kResourceIdAddDamage});
    const AddDamageFn backup =
        reinterpret_cast<AddDamageFn>(g_backup_add_damage.load(std::memory_order_acquire));
    if (backup == nullptr) return;

    int32_t out_damage = damage;
    if (g_gate.load(std::memory_order_relaxed) && damage > 0 && g_extras != nullptr &&
        g_facts != nullptr) {
        const Side victim_side = classify(victim);
        int32_t percent = kRateDefault;
        if (victim_side == Side::kPlayer) {
            percent = rate_value(RateId::kDamageIn);
        } else if (victim_side == Side::kMonster && classify(attacker) == Side::kPlayer) {
            percent = rate_value(RateId::kDamageOut);
        }
        if (percent != kRateDefault) out_damage = scale_by_percent(damage, percent);
    }
    backup(attacker, victim, out_damage, damage_flag, magic_flag);
}

void char_update_attr_from_monster_wrapper(void* ch, int32_t attr) {
    const auto* sdk = g_sdk;
    const contracts::InFlightGuard guard(
        sdk == nullptr ? nullptr : &sdk->in_flight, {kResourceIdUpdateAttrFromMonster});
    const UpdateAttrFromMonsterFn backup = reinterpret_cast<UpdateAttrFromMonsterFn>(
        g_backup_update_attr_from_monster.load(std::memory_order_acquire));
    if (backup == nullptr) return;

    const auto* game = g_game;
    const auto* extras = g_extras;
    const int32_t percent = rate_value(RateId::kEnemyHp);
    const bool active = g_gate.load(std::memory_order_relaxed) && ch != nullptr &&
                        attr == kMaxHpAttribute && extras != nullptr && game != nullptr &&
                        extras->read.pool_slot != nullptr && extras->read.max_hp != nullptr &&
                        game->character_write.max_hp != nullptr;
    if (!active || classify(ch) != Side::kMonster) {
        backup(ch, attr);
        return;
    }

    int slot = 0;
    if (extras->read.pool_slot(ch, &slot).ok != kQolOk || slot < 0 || slot >= kCharPoolSlots) {
        // 定位不到池槽就无法保证幂等（会累积缩放），故不改写——fail-safe 方向是保持游戏原值。
        if (!g_pool_slot_unresolved_logged.exchange(true)) {
            log_warn("char not in pool; max-hp scaling skipped");
        }
        backup(ch, attr);
        return;
    }

    HpMark& mark = g_hp_marks[slot];
    if (mark.ch != ch) {
        mark.ch = ch;
        mark.raw = 0;
        mark.applied = 0;
    }

    int32_t current = 0;
    const bool current_ok = extras->read.max_hp(ch, &current).ok == kQolOk;
    if (current_ok && mark.applied != 0 && current == mark.applied && mark.raw > 0) {
        // 恢复游戏原值，避免原函数把我们的缩放结果当作输入再缩放。
        (void)game->character_write.max_hp(ch, mark.raw);
        mark.applied = 0;
    }

    backup(ch, attr);

    int32_t raw = 0;
    if (extras->read.max_hp(ch, &raw).ok != kQolOk) return;
    if (percent == kRateDefault) {
        mark.raw = raw;
        mark.applied = 0;
        return;
    }
    const int32_t scaled = scale_attribute(raw, percent);
    if (scaled == raw) {
        mark.raw = raw;
        mark.applied = 0;
        return;
    }
    if (game->character_write.max_hp(ch, scaled).ok != kQolOk) return;
    mark.raw = raw;
    mark.applied = scaled;

    // 同步钳制当前 HP：否则「上限已缩放、当前血仍是原满值」会表现为血条超上限。
    if (extras->read.hp != nullptr && game->character_write.hp != nullptr) {
        int32_t hp = 0;
        if (extras->read.hp(ch, &hp).ok == kQolOk && hp > scaled) {
            (void)game->character_write.hp(ch, scaled);
        }
    }
}

int32_t char_add_experience_wrapper(void* ch, int32_t amount, uint8_t flag) {
    const auto* sdk = g_sdk;
    const contracts::InFlightGuard guard(
        sdk == nullptr ? nullptr : &sdk->in_flight, {kResourceIdAddExperience});
    const AddExperienceFn backup =
        reinterpret_cast<AddExperienceFn>(g_backup_add_experience.load(std::memory_order_acquire));
    if (backup == nullptr) return 0;

    int32_t out = amount;
    if (g_gate.load(std::memory_order_relaxed) && amount > 0 && g_extras != nullptr &&
        g_facts != nullptr && classify(ch) == Side::kPlayer) {
        const int32_t percent = rate_value(RateId::kExp);
        if (percent != kRateDefault) out = scale_by_percent(amount, percent);
    }
    return backup(ch, out, flag);
}

int32_t inven_add_money_wrapper(int64_t amount) {
    const auto* sdk = g_sdk;
    const contracts::InFlightGuard guard(
        sdk == nullptr ? nullptr : &sdk->in_flight, {kResourceIdAddMoney});
    const AddMoneyFn backup =
        reinterpret_cast<AddMoneyFn>(g_backup_add_money.load(std::memory_order_acquire));
    if (backup == nullptr) return 0;

    int64_t out = amount;
    if (g_gate.load(std::memory_order_relaxed) && amount > 0) {
        const int32_t percent = rate_value(RateId::kGold);
        if (percent != kRateDefault) out = scale_money(amount, percent);
    }
    return backup(out);
}


// ---------------------------------------------------------------------------
// 掉落：数量倍率 = 整次结算次数；重抽次数 = 槽内造物重掷取同类别最优
// ---------------------------------------------------------------------------

namespace {

enum class DropQualityGroup { kNone, kScroll, kGem, kBag, kDice };

bool drop_context_active() { return g_drop_depth != 0; }

bool read_item_category(const void* item, int32_t* out) {
    const auto* game = g_game;
    if (game == nullptr || game->abi.minor < contracts::kGameHostItemCategoryMinor ||
        game->item_category.category == nullptr || item == nullptr || out == nullptr) {
        return false;
    }
    return contracts::succeeded(game->item_category.category(item, out));
}

bool read_item_level(const void* item, int32_t* out) {
    const auto* game = g_game;
    if (game == nullptr || game->read_only.item.equip_level == nullptr || item == nullptr ||
        out == nullptr) {
        return false;
    }
    return contracts::succeeded(game->read_only.item.equip_level(item, out));
}

bool read_item_rarity(const void* item, int32_t* out) {
    const auto* game = g_game;
    if (game == nullptr || game->read_only.item.rarity == nullptr || item == nullptr ||
        out == nullptr) {
        return false;
    }
    return contracts::succeeded(game->read_only.item.rarity(item, out));
}

bool free_item(void* item) {
    const auto* game = g_game;
    if (game == nullptr || game->inventory_write.free_item == nullptr || item == nullptr) {
        return false;
    }
    return contracts::succeeded(game->inventory_write.free_item(item));
}

bool item_flag(const void* item, QolResult (*fn)(int32_t, int32_t*)) {
    if (fn == nullptr) return false;
    int32_t category = 0;
    if (!read_item_category(item, &category)) return false;
    int32_t value = 0;
    return contracts::succeeded(fn(category, &value)) && value != 0;
}

// 重抽的类别分流：药水(2)/材料(4)不参与；卷轴/宝石(3)与背包/骰子(5)参与；
// 徽章/技能书/神灯等其余 5 类物品不参与。subtype 来自槽的 lookup_key。
DropQualityGroup quality_group(const void* item, int32_t subtype) {
    if (item == nullptr) return DropQualityGroup::kNone;
    const auto* game = g_game;
    if (game == nullptr) return DropQualityGroup::kNone;
    if (subtype == 3) {
        if (game->read_only.item.is_enchant_scroll == nullptr) {
            return DropQualityGroup::kNone;
        }
        if (item_flag(item, game->read_only.item.is_enchant_scroll)) {
            return DropQualityGroup::kScroll;
        }
        return DropQualityGroup::kGem;
    }
    if (subtype == 5) {
        if (game->read_only.item.is_dice == nullptr ||
            game->read_only.item.is_backpack_category == nullptr) {
            return DropQualityGroup::kNone;
        }
        if (item_flag(item, game->read_only.item.is_dice)) return DropQualityGroup::kDice;
        if (item_flag(item, game->read_only.item.is_backpack_category)) {
            return DropQualityGroup::kBag;
        }
        return DropQualityGroup::kNone;
    }
    return DropQualityGroup::kNone;
}

}  // namespace

void drop_item_wrapper(void* ch) {
    const auto* sdk = g_sdk;
    const contracts::InFlightGuard guard(
        sdk == nullptr ? nullptr : &sdk->in_flight, {kResourceIdDropItem});
    const DropItemFn backup =
        reinterpret_cast<DropItemFn>(g_backup_drop_item.load(std::memory_order_acquire));
    if (backup == nullptr) return;
    if (!g_gate.load(std::memory_order_relaxed)) {
        backup(ch);
        return;
    }
    const int32_t rounds = rate_value(RateId::kDropQuantity);
    if (rounds > kDropLargeWarnThreshold && !g_large_drop_quantity_logged.exchange(true)) {
        log_warn("drop quantity %d > %d; no hard cap applied", rounds, kDropLargeWarnThreshold);
    }
    if (rounds <= 0) return;
    ++g_drop_depth;
    for (int32_t i = 0; i < rounds; ++i) backup(ch);
    --g_drop_depth;
}

void* make_item_wrapper(int32_t category, int32_t lookup_key, int32_t flag) {
    const auto* sdk = g_sdk;
    const contracts::InFlightGuard guard(
        sdk == nullptr ? nullptr : &sdk->in_flight, {kResourceIdMakeItem});
    const MakeItemFn backup =
        reinterpret_cast<MakeItemFn>(g_backup_make_item.load(std::memory_order_acquire));
    if (backup == nullptr) return nullptr;
    void* first = backup(category, lookup_key, flag);
    if (first == nullptr) return nullptr;
    if (!g_gate.load(std::memory_order_relaxed) || !drop_context_active()) return first;

    const int32_t rolls = rate_value(RateId::kDropReroll);
    if (rolls <= 1) return first;
    const DropQualityGroup group = quality_group(first, lookup_key);
    if (group == DropQualityGroup::kNone) return first;
    if (rolls > kDropLargeWarnThreshold && !g_large_drop_reroll_logged.exchange(true)) {
        log_warn("drop reroll %d > %d; no hard cap applied", rolls, kDropLargeWarnThreshold);
    }

    int32_t best_level = 0;
    if (!read_item_level(first, &best_level)) return first;
    void* best = first;
    for (int32_t i = 1; i < rolls; ++i) {
        void* candidate = backup(category, lookup_key, flag);
        if (candidate == nullptr) continue;
        if (quality_group(candidate, lookup_key) != group) {
            (void)free_item(candidate);
            continue;
        }
        int32_t level = 0;
        if (!read_item_level(candidate, &level)) {
            (void)free_item(candidate);
            continue;
        }
        if (level > best_level) {
            (void)free_item(best);
            best = candidate;
            best_level = level;
        } else {
            (void)free_item(candidate);
        }
    }
    return best;
}

void* make_equip_detail_wrapper(int32_t kind, int32_t min_level, int32_t max_level,
                                int32_t item, int32_t option) {
    const auto* sdk = g_sdk;
    const contracts::InFlightGuard guard(
        sdk == nullptr ? nullptr : &sdk->in_flight, {kResourceIdMakeEquipDetail});
    const MakeEquipDetailFn backup = reinterpret_cast<MakeEquipDetailFn>(
        g_backup_make_equip_detail.load(std::memory_order_acquire));
    if (backup == nullptr) return nullptr;
    void* first = backup(kind, min_level, max_level, item, option);
    if (first == nullptr) return nullptr;
    if (!g_gate.load(std::memory_order_relaxed) || !drop_context_active()) return first;

    const int32_t rolls = rate_value(RateId::kDropReroll);
    if (rolls <= 1) return first;
    int32_t best_rarity = 0;
    if (!read_item_rarity(first, &best_rarity)) return first;
    if (rolls > kDropLargeWarnThreshold && !g_large_drop_reroll_logged.exchange(true)) {
        log_warn("drop reroll %d > %d; no hard cap applied", rolls, kDropLargeWarnThreshold);
    }
    void* best = first;
    for (int32_t i = 1; i < rolls; ++i) {
        void* candidate = backup(kind, min_level, max_level, item, option);
        if (candidate == nullptr) continue;
        int32_t rarity = 0;
        if (!read_item_rarity(candidate, &rarity)) {
            (void)free_item(candidate);
            continue;
        }
        if (rarity > best_rarity) {
            (void)free_item(best);
            best = candidate;
            best_rarity = rarity;
        } else {
            (void)free_item(candidate);
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// 生命周期与倍率
// ---------------------------------------------------------------------------

void bind_runtime(const contracts::QolHostSdk* sdk, const contracts::QolGameHost* game,
                  const contracts::QolCharacterExtrasPort* extras,
                  const contracts::CharacterFactsApi* facts, contracts::QolOwnerToken owner,
                  const char* plugin_id) {
    g_sdk = sdk;
    g_game = game;
    g_extras = extras;
    g_facts = facts;
    g_owner = owner;
    g_plugin_id = plugin_id;
}

void clear_runtime() {
    g_gate.store(false, std::memory_order_release);
    g_sdk = nullptr;
    g_game = nullptr;
    g_extras = nullptr;
    g_facts = nullptr;
    g_owner = {};
    g_plugin_id = nullptr;
}

void set_gate(bool enabled) {
    g_gate.store(enabled, std::memory_order_release);
    log_info("enabled=%d", enabled);
}

bool gate_enabled() { return g_gate.load(std::memory_order_acquire); }

const contracts::QolHostSdk* host_sdk() { return g_sdk; }
const contracts::QolGameHost* game_host() { return g_game; }
const contracts::QolCharacterExtrasPort* character_extras() { return g_extras; }
const contracts::CharacterFactsApi* character_facts() { return g_facts; }

Rates current_rates() {
    Rates rates{};
    for (uint32_t i = 0; i < kRateCount; ++i) {
        rates.value[i] = g_rates[i].load(std::memory_order_relaxed);
    }
    return rates;
}

void apply_rates(const Rates& rates) {
    for (uint32_t i = 0; i < kRateCount; ++i) {
        g_rates[i].store(rates.value[i], std::memory_order_relaxed);
    }
}

bool reload_rates_from_config() {
    const auto* sdk = g_sdk;
    const auto* api = sdk == nullptr ? nullptr : contracts::feature_if_available(sdk);
    if (api == nullptr || g_plugin_id == nullptr) return false;
    char buffer[512]{};
    uint32_t size = 0;
    const auto read = api->read_config(api->context, g_owner, g_plugin_id, "rates", buffer,
                                      sizeof(buffer), &size);
    if (!contracts::succeeded(read) || size > sizeof(buffer)) return false;
    Rates parsed = default_rates();
    if (!decode_rates(buffer, size, &parsed)) {
        log_warn("stored rates are invalid; keeping current values");
        return false;
    }
    apply_rates(parsed);
    return true;
}

bool request_persist_rates(const Rates& rates, uint64_t* out_request_id) {
    if (out_request_id != nullptr) *out_request_id = 0;
    const auto* sdk = g_sdk;
    const auto* api = sdk == nullptr ? nullptr : contracts::feature_if_available(sdk);
    if (api == nullptr || g_plugin_id == nullptr) return false;
    const std::string json = encode_rates(rates);
    contracts::FeatureReceipt receipt{};
    const auto result = api->request_set_config(api->context, g_owner, g_plugin_id, "rates",
        json.c_str(), static_cast<uint32_t>(json.size()), &receipt);
    if (!contracts::succeeded(result) ||
        receipt.admission != contracts::FeatureAdmission::kAdmitted || receipt.request_id == 0) {
        return false;
    }
    if (out_request_id != nullptr) *out_request_id = receipt.request_id;
    return true;
}

bool poll_rates_receipt(uint64_t request_id, contracts::FeatureReceipt* out) {
    if (out == nullptr || request_id == 0) return false;
    const auto* sdk = g_sdk;
    const auto* api = sdk == nullptr ? nullptr : contracts::feature_if_available(sdk);
    if (api == nullptr || g_plugin_id == nullptr) return false;
    return contracts::succeeded(api->receipt(api->context, g_owner, request_id, out));
}

bool rates_config_available() {
    const auto* sdk = g_sdk;
    return sdk != nullptr && contracts::feature_if_available(sdk) != nullptr &&
           g_plugin_id != nullptr;
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
    if (std::strcmp(resource_id, kResourceIdAddExperience) == 0) {
        g_backup_add_experience.store(backup, std::memory_order_release);
        return 0;
    }
    if (std::strcmp(resource_id, kResourceIdAddMoney) == 0) {
        g_backup_add_money.store(backup, std::memory_order_release);
        return 0;
    }
    if (std::strcmp(resource_id, kResourceIdDropItem) == 0) {
        g_backup_drop_item.store(backup, std::memory_order_release);
        return 0;
    }
    if (std::strcmp(resource_id, kResourceIdMakeItem) == 0) {
        g_backup_make_item.store(backup, std::memory_order_release);
        return 0;
    }
    if (std::strcmp(resource_id, kResourceIdMakeEquipDetail) == 0) {
        g_backup_make_equip_detail.store(backup, std::memory_order_release);
        return 0;
    }
    log_warn("unknown resource id: %s", resource_id);
    return 0;
}

}  // namespace qol::features::world_tuning

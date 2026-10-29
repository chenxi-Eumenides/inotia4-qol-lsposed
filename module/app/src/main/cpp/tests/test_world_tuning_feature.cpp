#include "contracts/character_facts.h"
#include "contracts/game_host.h"
#include "contracts/game_ports/character_extras_port.h"
#include "contracts/plugin.h"
#include "contracts/host_sdk.h"
#include "plugins/world_tuning/feature.h"
#include "plugins/world_tuning/panel.h"
#include "contracts/world_tuning.h"
#include "plugins/world_tuning/plugin.h"

#include <cassert>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace qol::contracts;
using namespace qol::features::world_tuning;

struct Character {
    int32_t type = 1;
    int32_t party_member = 0;
    int32_t active_group = 0;
    Character* summoner = nullptr;
    int32_t slot = 0;
    int32_t max_hp = 100;
    int32_t hp = 100;
};

QolHostSdk g_sdk{};
QolGameHost g_game{};
QolCharacterExtrasPort g_extras{};
CharacterFactsApi g_facts{};
PluginContext g_context{};
const void* g_ports[] = {nullptr};
Character g_characters[16]{};

bool g_require_succeeds = true;
bool g_require_returns_null = false;
bool g_fail_party = false;
bool g_fail_active = false;
bool g_fail_summoner = false;
bool g_fail_type = false;
bool g_fail_pool_slot = false;
bool g_fail_max_hp = false;
bool g_fail_hp = false;
int g_require_calls = 0;
int g_party_calls = 0;
int g_active_calls = 0;
int g_summoner_calls = 0;
int g_type_calls = 0;
int g_pool_slot_calls = 0;
int g_max_hp_reads = 0;
int g_hp_reads = 0;
int g_max_hp_writes = 0;
int g_hp_writes = 0;
int g_damage_backup_calls = 0;
int g_attr_backup_calls = 0;
int g_exp_backup_calls = 0;
int g_money_backup_calls = 0;
int32_t g_last_exp = 0;
int64_t g_last_money = 0;
int g_in_flight_enters = 0;
int g_in_flight_leaves = 0;
int g_damage_enters = 0;
int g_damage_leaves = 0;
int g_attr_enters = 0;
int g_attr_leaves = 0;
int g_log_writes = 0;
int32_t g_last_damage = 0;
std::vector<std::string> g_log_messages;

QolResult fake_failure() { return error(kQolErrNotReady, "fake failure", 12); }

QolResult fake_log_write(int32_t, int32_t, const char*, const char* message,
                         uint32_t message_len) {
    ++g_log_writes;
    g_log_messages.emplace_back(message == nullptr ? "" : std::string(message, message_len));
    return ok();
}

QolResult fake_require(const char* id, QolVersionRange, const void** out_iface) {
    ++g_require_calls;
    assert(id != nullptr && out_iface != nullptr);
    if (!g_require_succeeds) {
        *out_iface = nullptr;
        return fake_failure();
    }
    *out_iface = g_require_returns_null ? nullptr : &g_facts;
    return ok();
}

QolResult fake_enter(InFlightHandle handle) {
    assert(handle.resource_id != nullptr);
    ++g_in_flight_enters;
    if (std::strcmp(handle.resource_id, kResourceIdAddDamage) == 0) {
        ++g_damage_enters;
    } else if (std::strcmp(handle.resource_id, kResourceIdUpdateAttrFromMonster) == 0) {
        ++g_attr_enters;
    } else if (std::strcmp(handle.resource_id, kResourceIdAddExperience) == 0) {
        // exp/gold 也在途计数，归入 total 即可。
    } else if (std::strcmp(handle.resource_id, kResourceIdAddMoney) == 0) {
    } else {
        assert(false && "unexpected in-flight resource id");
    }
    return ok();
}

QolResult fake_leave(InFlightHandle handle) {
    assert(handle.resource_id != nullptr);
    ++g_in_flight_leaves;
    if (std::strcmp(handle.resource_id, kResourceIdAddDamage) == 0) {
        ++g_damage_leaves;
    } else if (std::strcmp(handle.resource_id, kResourceIdUpdateAttrFromMonster) == 0) {
        ++g_attr_leaves;
    } else if (std::strcmp(handle.resource_id, kResourceIdAddExperience) == 0) {
    } else if (std::strcmp(handle.resource_id, kResourceIdAddMoney) == 0) {
    } else {
        assert(false && "unexpected in-flight resource id");
    }
    return ok();
}

QolResult fake_party_member(const void* character, int32_t* out_member) {
    ++g_party_calls;
    if (g_fail_party) return fake_failure();
    assert(character != nullptr && out_member != nullptr);
    *out_member = static_cast<const Character*>(character)->party_member;
    return ok();
}

QolResult fake_active_group(const void* character, int32_t* out_active) {
    ++g_active_calls;
    if (g_fail_active) return fake_failure();
    assert(character != nullptr && out_active != nullptr);
    *out_active = static_cast<const Character*>(character)->active_group;
    return ok();
}

QolResult fake_summoner(const void* character, void** out_summoner) {
    ++g_summoner_calls;
    if (g_fail_summoner) return fake_failure();
    assert(character != nullptr && out_summoner != nullptr);
    *out_summoner = static_cast<const Character*>(character)->summoner;
    return ok();
}

QolResult fake_type(const void* character, int32_t* out_type) {
    ++g_type_calls;
    if (g_fail_type) return fake_failure();
    assert(character != nullptr && out_type != nullptr);
    *out_type = static_cast<const Character*>(character)->type;
    return ok();
}

QolResult fake_pool_slot(const void* character, int32_t* out_slot) {
    ++g_pool_slot_calls;
    if (g_fail_pool_slot) return fake_failure();
    assert(character != nullptr && out_slot != nullptr);
    *out_slot = static_cast<const Character*>(character)->slot;
    return ok();
}

QolResult fake_max_hp(const void* character, int32_t* out_value) {
    ++g_max_hp_reads;
    if (g_fail_max_hp) return fake_failure();
    assert(character != nullptr && out_value != nullptr);
    *out_value = static_cast<const Character*>(character)->max_hp;
    return ok();
}

QolResult fake_hp(const void* character, int32_t* out_value) {
    ++g_hp_reads;
    if (g_fail_hp) return fake_failure();
    assert(character != nullptr && out_value != nullptr);
    *out_value = static_cast<const Character*>(character)->hp;
    return ok();
}

QolResult fake_write_max_hp(void* character, int32_t value) {
    assert(character != nullptr);
    ++g_max_hp_writes;
    static_cast<Character*>(character)->max_hp = value;
    return ok();
}

QolResult fake_write_hp(void* character, int32_t value) {
    assert(character != nullptr);
    ++g_hp_writes;
    static_cast<Character*>(character)->hp = value;
    return ok();
}

void fake_damage_backup(void*, void*, int32_t damage, int32_t, int32_t) {
    ++g_damage_backup_calls;
    g_last_damage = damage;
}

void fake_attr_backup(void*, int32_t) { ++g_attr_backup_calls; }

int32_t fake_exp_backup(void*, int32_t amount, uint8_t) {
    ++g_exp_backup_calls;
    g_last_exp = amount;
    return 1;
}

int32_t fake_money_backup(int64_t amount) {
    ++g_money_backup_calls;
    g_last_money = amount;
    return 1;
}

void reset_counters() {
    g_require_calls = 0;
    g_party_calls = 0;
    g_active_calls = 0;
    g_summoner_calls = 0;
    g_type_calls = 0;
    g_pool_slot_calls = 0;
    g_max_hp_reads = 0;
    g_hp_reads = 0;
    g_max_hp_writes = 0;
    g_hp_writes = 0;
    g_damage_backup_calls = 0;
    g_attr_backup_calls = 0;
    g_exp_backup_calls = 0;
    g_money_backup_calls = 0;
    g_last_exp = 0;
    g_last_money = 0;
    g_in_flight_enters = 0;
    g_in_flight_leaves = 0;
    g_damage_enters = 0;
    g_damage_leaves = 0;
    g_attr_enters = 0;
    g_attr_leaves = 0;
    g_log_writes = 0;
    g_last_damage = 0;
    g_log_messages.clear();
}

void setup_fake_interfaces() {
    g_sdk = {};
    g_game = {};
    g_extras = {};
    g_facts = {};
    g_context = {};
    for (auto& character : g_characters) character = Character{};
    g_ports[0] = &g_extras;

    g_require_succeeds = true;
    g_require_returns_null = false;
    g_fail_party = false;
    g_fail_active = false;
    g_fail_summoner = false;
    g_fail_type = false;
    g_fail_pool_slot = false;
    g_fail_max_hp = false;
    g_fail_hp = false;
    reset_counters();

    g_sdk.log.write = &fake_log_write;
    g_sdk.capabilities.require = &fake_require;
    g_sdk.in_flight.enter = &fake_enter;
    g_sdk.in_flight.leave = &fake_leave;

    g_extras.read.is_party_member = &fake_party_member;
    g_extras.read.summoner = &fake_summoner;
    g_extras.read.pool_slot = &fake_pool_slot;
    g_extras.read.max_hp = &fake_max_hp;
    g_extras.read.hp = &fake_hp;

    g_game.character_write.max_hp = &fake_write_max_hp;
    g_game.character_write.hp = &fake_write_hp;

    g_facts.type = &fake_type;
    g_facts.is_active_player_group = &fake_active_group;

    g_context.struct_size = sizeof(g_context);
    g_context.sdk_major = kHostSdkMajor;
    g_context.sdk_minor = kHostSdkMinor;
    g_context.sdk = &g_sdk;
    g_context.game = &g_game;
    g_context.plugin_id = "world_tuning";
    g_context.ports = g_ports;
    g_context.port_count = 1;

    set_gate(false);
    const auto& descriptor = plugin_descriptor();
    descriptor.on_resource_ready(g_context, kResourceIdAddDamage, nullptr);
    descriptor.on_resource_ready(g_context, kResourceIdUpdateAttrFromMonster, nullptr);
}

void install_success() {
    setup_fake_interfaces();
    const auto& descriptor = plugin_descriptor();
    assert(descriptor.install(g_context) == 0);
    assert(!gate_enabled());
    assert(host_sdk() == &g_sdk);
    assert(game_host() == &g_game);
    assert(character_extras() == &g_extras);
    assert(character_facts() == &g_facts);
    descriptor.on_resource_ready(g_context, kResourceIdAddDamage,
                                 reinterpret_cast<void*>(&fake_damage_backup));
    descriptor.on_resource_ready(g_context, kResourceIdUpdateAttrFromMonster,
                                 reinterpret_cast<void*>(&fake_attr_backup));
    descriptor.on_resource_ready(g_context, kResourceIdAddExperience,
                                 reinterpret_cast<void*>(&fake_exp_backup));
    descriptor.on_resource_ready(g_context, kResourceIdAddMoney,
                                 reinterpret_cast<void*>(&fake_money_backup));
}

template <typename Invoke>
void assert_one_damage_flight(Invoke&& invoke) {
    const int enters = g_damage_enters;
    const int leaves = g_damage_leaves;
    invoke();
    assert(g_damage_enters == enters + 1);
    assert(g_damage_leaves == leaves + 1);
}

template <typename Invoke>
void assert_one_attr_flight(Invoke&& invoke) {
    const int enters = g_attr_enters;
    const int leaves = g_attr_leaves;
    invoke();
    assert(g_attr_enters == enters + 1);
    assert(g_attr_leaves == leaves + 1);
}

void test_descriptor() {
    const auto& descriptor = plugin_descriptor();
    assert(std::strcmp(descriptor.id, "world_tuning") == 0);
    assert(std::strcmp(descriptor.version, "0.1.0") == 0);
    assert(descriptor.requires_count == 1);
    assert(std::strcmp(descriptor.requires[0].id, kCharacterFactsCapabilityId) == 0);
    assert(descriptor.optional_count == 2);
    assert(descriptor.validate_config == &validate_rates_config);
    assert(descriptor.resources_count == 4);
    assert(descriptor.provides_count == 1);
    assert(std::strcmp(descriptor.provides[0].id, kWorldTuningMoveSpeedCapabilityId) == 0);
    assert(std::strcmp(descriptor.resources[0].id, kResourceIdAddDamage) == 0);
    assert(std::strcmp(descriptor.resources[0].symbol, "F_CHAR_ADD_DAMAGE_VMA") == 0);
    assert(descriptor.resources[0].handler == reinterpret_cast<void*>(&char_add_damage_wrapper));
    assert(std::strcmp(descriptor.resources[1].id, kResourceIdUpdateAttrFromMonster) == 0);
    assert(std::strcmp(descriptor.resources[1].symbol,
                       "F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA") == 0);
    assert(descriptor.resources[1].handler ==
           reinterpret_cast<void*>(&char_update_attr_from_monster_wrapper));
    assert(std::strcmp(descriptor.resources[2].id, kResourceIdAddExperience) == 0);
    assert(std::strcmp(descriptor.resources[2].symbol, "F_ADD_EXP_VMA") == 0);
    assert(std::strcmp(descriptor.resources[3].id, kResourceIdAddMoney) == 0);
    assert(std::strcmp(descriptor.resources[3].symbol, "F_ADD_MONEY_VMA") == 0);
    for (uint32_t i = 0; i < 4; ++i) {
        assert(descriptor.resources[i].requirement == ResourceRequirement::kOptional);
        assert(descriptor.resources[i].failure == ResourceFailure::kFailSoft);
    }
    assert(descriptor.jni_count == 0);
    assert(descriptor.jni == nullptr);
    assert(descriptor.feature_count == 1);
    assert(std::strcmp(descriptor.features[0].id, "world_tuning.panel") == 0);
    assert(descriptor.features[0].type == FeatureType::kEntry);
    assert(descriptor.features[0].default_enabled == 0);
    assert(descriptor.features[0].on_entry != nullptr);
    assert(descriptor.features[0].action_label != nullptr);
}

void assert_install_clean() {
    assert(!gate_enabled());
    assert(host_sdk() == nullptr);
    assert(game_host() == nullptr);
    assert(character_extras() == nullptr);
    assert(character_facts() == nullptr);
}

void test_install_lifecycle_and_toggle() {
    const auto& descriptor = plugin_descriptor();

    setup_fake_interfaces();
    g_context.port_count = 0;
    assert(descriptor.install(g_context) != 0);
    assert_install_clean();

    setup_fake_interfaces();
    g_require_succeeds = false;
    assert(descriptor.install(g_context) != 0);
    assert_install_clean();

    setup_fake_interfaces();
    g_sdk.capabilities.require = nullptr;
    assert(descriptor.install(g_context) != 0);
    assert_install_clean();

    setup_fake_interfaces();
    g_require_returns_null = true;
    assert(descriptor.install(g_context) != 0);
    assert_install_clean();

    install_success();
    assert(!gate_enabled());
    assert(character_facts() == &g_facts && character_extras() == &g_extras);
    descriptor.on_enabled(g_context);
    assert(gate_enabled());
    descriptor.on_disabled(g_context);
    assert(!gate_enabled());
    descriptor.on_enabled(g_context);
    assert(gate_enabled());
    // 面板未绑定（fake SDK 无 UI 能力）时入口返回 NotReady，但不影响门禁。
    assert(descriptor.features[0].on_entry(g_context).ok == kQolErrNotReady);
    assert(gate_enabled());

    descriptor.on_disabled(g_context);
    assert(descriptor.features[0].on_entry(g_context).ok == kQolErrNotReady);
    descriptor.deactivate(g_context);
    assert(!gate_enabled());
    assert(character_extras() == nullptr);
    assert(character_facts() == nullptr);
}

void test_damage_wrapper() {
    install_success();
    const auto& descriptor = plugin_descriptor();
    descriptor.on_enabled(g_context);
    Character& attacker = g_characters[0];
    Character& victim = g_characters[1];
    attacker.party_member = 1;
    victim.type = 1;
    victim.slot = 1;

    descriptor.on_disabled(g_context);
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&attacker, &victim, 100, 3, 4);
    });
    assert(g_last_damage == 100);
    assert(g_party_calls == 0);

    descriptor.on_enabled(g_context);
    {
        Rates rates = default_rates();
        rates.value[static_cast<uint32_t>(RateId::kDamageOut)] = 150;
        rates.value[static_cast<uint32_t>(RateId::kDamageIn)] = 50;
        apply_rates(rates);
    }
    g_party_calls = g_active_calls = g_summoner_calls = 0;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&attacker, &victim, 100, 3, 4);
    });
    assert(g_last_damage == 150);
    // The attacker is already identified by party_member; later faction primitives are skipped.
    assert(g_active_calls == 1 && g_summoner_calls == 1);

    Character& monster = g_characters[2];
    Character& player = g_characters[3];
    monster.type = 1;
    player.party_member = 1;
    g_party_calls = g_active_calls = g_summoner_calls = 0;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&monster, &player, 100, 0, 0);
    });
    assert(g_last_damage == 50);
    assert(g_party_calls == 1 && g_active_calls == 0 && g_summoner_calls == 0);

    Character& neutral = g_characters[4];
    neutral.type = 2;
    Character& player_victim = g_characters[5];
    player_victim.party_member = 1;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&neutral, &player_victim, 100, 0, 0);
    });
    assert(g_last_damage == 50);

    Character& active_player_victim = g_characters[15];
    active_player_victim.active_group = 1;
    g_party_calls = g_active_calls = g_summoner_calls = 0;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&neutral, &active_player_victim, 100, 0, 0);
    });
    assert(g_last_damage == 50);
    assert(g_party_calls == 1 && g_active_calls == 1 && g_summoner_calls == 0);

    Character& player_attacker = g_characters[6];
    player_attacker.party_member = 1;
    Character& neutral_victim = g_characters[7];
    neutral_victim.type = 2;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &neutral_victim, 100, 0, 0);
    });
    assert(g_last_damage == 100);

    Character& unknown_victim = g_characters[8];
    unknown_victim.type = 1;
    g_fail_party = true;
    const auto faction_warning_count = [&] {
        int count = 0;
        for (const auto& message : g_log_messages) {
            if (message.find("faction dependencies unavailable") != std::string::npos) ++count;
        }
        return count;
    };
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 100, 0, 0);
    });
    assert(g_last_damage == 100);
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 100, 0, 0);
    });
    assert(faction_warning_count() == 1);
    g_fail_party = false;

    g_fail_active = true;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 100, 0, 0);
    });
    assert(g_last_damage == 100);
    g_fail_active = false;

    g_fail_summoner = true;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 100, 0, 0);
    });
    assert(g_last_damage == 100);
    g_fail_summoner = false;

    unknown_victim.summoner = &unknown_victim;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 100, 0, 0);
    });
    assert(g_last_damage == 100);
    unknown_victim.summoner = nullptr;

    for (int i = 8; i < 13; ++i) g_characters[i].summoner = &g_characters[i + 1];
    g_characters[13].summoner = &player_attacker;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 100, 0, 0);
    });
    assert(g_last_damage == 100);
    for (int i = 8; i < 14; ++i) g_characters[i].summoner = nullptr;

    const auto active_group_fn = g_facts.is_active_player_group;
    g_facts.is_active_player_group = nullptr;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 100, 0, 0);
    });
    assert(g_last_damage == 100);
    g_facts.is_active_player_group = active_group_fn;

    descriptor.on_resource_ready(g_context, kResourceIdAddDamage, nullptr);
    const int reads_before_null_backup = g_party_calls + g_active_calls + g_summoner_calls;
    const int backups_before_null_backup = g_damage_backup_calls;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 100, 0, 0);
    });
    assert(g_damage_backup_calls == backups_before_null_backup);
    assert(g_party_calls + g_active_calls + g_summoner_calls == reads_before_null_backup);
    descriptor.on_resource_ready(g_context, kResourceIdAddDamage,
                                 reinterpret_cast<void*>(&fake_damage_backup));

    const int reads_before_nonpositive = g_party_calls + g_active_calls + g_summoner_calls;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 0, 0, 0);
    });
    assert(g_last_damage == 0);
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, -7, 0, 0);
    });
    assert(g_last_damage == -7);
    assert(g_party_calls + g_active_calls + g_summoner_calls == reads_before_nonpositive);
}

void test_max_hp_wrapper() {
    install_success();
    const auto& descriptor = plugin_descriptor();
    descriptor.on_enabled(g_context);
    {
        Rates rates = default_rates();
        rates.value[static_cast<uint32_t>(RateId::kEnemyHp)] = 50;
        apply_rates(rates);
    }

    Character& monster = g_characters[9];
    monster.type = 1;
    monster.slot = 20;
    monster.max_hp = 100;
    monster.hp = 120;
    int max_writes_before = g_max_hp_writes;
    int hp_writes_before = g_hp_writes;
    int attr_backups_before = g_attr_backup_calls;
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&monster, 0x1e); });
    assert(g_attr_backup_calls == attr_backups_before + 1);
    assert(monster.max_hp == 50 && monster.hp == 50);
    assert(g_max_hp_writes == max_writes_before + 1);
    assert(g_hp_writes == hp_writes_before + 1);

    max_writes_before = g_max_hp_writes;
    hp_writes_before = g_hp_writes;
    monster.max_hp = 50;
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&monster, 0x1e); });
    assert(monster.max_hp == 50);
    // 先把上次的 applied 还原成 raw，再在游戏结果上重新缩放：两次写入，数值不累积。
    assert(g_max_hp_writes == max_writes_before + 2);
    assert(g_hp_writes == hp_writes_before);

    monster.max_hp = 200;
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&monster, 0x1e); });
    assert(monster.max_hp == 100);
    assert(g_max_hp_writes == max_writes_before + 3);

    {
        // 倍率变化在下一次属性重算时生效，且仍从游戏 raw 值重算。
        Rates rates = current_rates();
        rates.value[static_cast<uint32_t>(RateId::kEnemyHp)] = 200;
        apply_rates(rates);
    }
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&monster, 0x1e); });
    assert(monster.max_hp == 400);
    {
        Rates rates = current_rates();
        rates.value[static_cast<uint32_t>(RateId::kEnemyHp)] = 50;
        apply_rates(rates);
    }

    Character& player_summon = g_characters[10];
    player_summon.type = 1;
    player_summon.party_member = 1;
    player_summon.slot = 21;
    player_summon.max_hp = 100;
    max_writes_before = g_max_hp_writes;
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&player_summon, 0x1e); });
    assert(player_summon.max_hp == 100 && g_max_hp_writes == max_writes_before);

    Character& unknown = g_characters[11];
    unknown.type = 1;
    unknown.slot = 22;
    unknown.max_hp = 100;
    g_fail_party = true;
    max_writes_before = g_max_hp_writes;
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&unknown, 0x1e); });
    assert(unknown.max_hp == 100 && g_max_hp_writes == max_writes_before);
    g_fail_party = false;

    Character& non_max_attr = g_characters[12];
    non_max_attr.type = 1;
    non_max_attr.slot = 23;
    non_max_attr.max_hp = 100;
    max_writes_before = g_max_hp_writes;
    const int pool_reads_before = g_pool_slot_calls;
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&non_max_attr, 0x1d); });
    assert(non_max_attr.max_hp == 100 && g_max_hp_writes == max_writes_before);
    assert(g_pool_slot_calls == pool_reads_before);

    Character& invalid_slot = g_characters[13];
    invalid_slot.type = 1;
    invalid_slot.slot = 100;
    invalid_slot.max_hp = 100;
    const int warnings_before = [&] {
        int count = 0;
        for (const auto& message : g_log_messages) {
            if (message.find("char not in pool; max-hp scaling skipped") != std::string::npos) ++count;
        }
        return count;
    }();
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&invalid_slot, 0x1e); });
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&invalid_slot, 0x1e); });
    const int warnings_after = [&] {
        int count = 0;
        for (const auto& message : g_log_messages) {
            if (message.find("char not in pool; max-hp scaling skipped") != std::string::npos) ++count;
        }
        return count;
    }();
    assert(invalid_slot.max_hp == 100 && warnings_after - warnings_before == 1);

    Character& disabled = g_characters[14];
    disabled.type = 1;
    disabled.slot = 24;
    disabled.max_hp = 100;
    descriptor.on_disabled(g_context);
    max_writes_before = g_max_hp_writes;
    const int pool_before_disabled = g_pool_slot_calls;
    attr_backups_before = g_attr_backup_calls;
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&disabled, 0x1e); });
    assert(g_attr_backup_calls == attr_backups_before + 1);
    assert(g_max_hp_writes == max_writes_before && g_pool_slot_calls == pool_before_disabled);

    descriptor.on_enabled(g_context);
    descriptor.on_resource_ready(g_context, kResourceIdUpdateAttrFromMonster, nullptr);
    attr_backups_before = g_attr_backup_calls;
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&disabled, 0x1e); });
    assert(g_attr_backup_calls == attr_backups_before);
    descriptor.on_resource_ready(g_context, kResourceIdUpdateAttrFromMonster,
                                 reinterpret_cast<void*>(&fake_attr_backup));
    descriptor.on_resource_ready(g_context, kResourceIdAddExperience,
                                 reinterpret_cast<void*>(&fake_exp_backup));
    descriptor.on_resource_ready(g_context, kResourceIdAddMoney,
                                 reinterpret_cast<void*>(&fake_money_backup));
}

void test_deactivate_residual_wrappers() {
    install_success();
    const auto& descriptor = plugin_descriptor();
    descriptor.on_enabled(g_context);
    Character& attacker = g_characters[0];
    Character& victim = g_characters[1];
    attacker.party_member = 1;
    victim.type = 1;

    descriptor.deactivate(g_context);
    assert(!gate_enabled());
    assert(character_extras() == nullptr && character_facts() == nullptr);
    const int damage_before = g_damage_backup_calls;
    const int attr_before = g_attr_backup_calls;
    assert_one_damage_flight([&] { char_add_damage_wrapper(&attacker, &victim, 100, 0, 0); });
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&victim, 0x1e); });
    assert(g_damage_backup_calls == damage_before + 1 && g_last_damage == 100);
    assert(g_attr_backup_calls == attr_before + 1);
    assert(g_max_hp_writes == 0 && g_hp_writes == 0);
}

void test_exp_money_and_move_speed() {
    install_success();
    const auto& descriptor = plugin_descriptor();
    descriptor.on_enabled(g_context);

    Rates rates = default_rates();
    rates.value[static_cast<uint32_t>(RateId::kExp)] = 150;
    rates.value[static_cast<uint32_t>(RateId::kGold)] = 150;
    apply_rates(rates);

    Character player{};
    player.party_member = 1;
    Character monster{};
    monster.type = 1;

    assert(char_add_experience_wrapper(&player, 100, 1) == 1);
    assert(g_last_exp == 150);
    (void)char_add_experience_wrapper(&monster, 100, 1);
    assert(g_last_exp == 100);
    (void)char_add_experience_wrapper(&player, 3, 1);
    assert(g_last_exp == 4);
    (void)char_add_experience_wrapper(&player, 0, 1);
    assert(g_last_exp == 0);

    (void)inven_add_money_wrapper(100);
    assert(g_last_money == 150);
    (void)inven_add_money_wrapper(3);
    assert(g_last_money == 4);
    (void)inven_add_money_wrapper(-5);
    assert(g_last_money == -5);

    const auto* api = static_cast<const WorldTuningMoveSpeedApi*>(
        descriptor.provides[0].iface);
    assert(api != nullptr && api->read != nullptr);
    int32_t speed = 0;
    rates = default_rates();
    rates.value[static_cast<uint32_t>(RateId::kMoveSpeed)] = 100;
    apply_rates(rates);
    assert(api->read(&speed).ok == kQolOk && speed == 8);
    rates.value[static_cast<uint32_t>(RateId::kMoveSpeed)] = 150;
    apply_rates(rates);
    assert(api->read(&speed).ok == kQolOk && speed == 12);
    rates.value[static_cast<uint32_t>(RateId::kMoveSpeed)] = 50;
    apply_rates(rates);
    assert(api->read(&speed).ok == kQolOk && speed == 4);
    rates.value[static_cast<uint32_t>(RateId::kMoveSpeed)] = 10;
    apply_rates(rates);
    assert(api->read(&speed).ok == kQolOk && speed == 1);
    rates.value[static_cast<uint32_t>(RateId::kMoveSpeed)] = 120;
    apply_rates(rates);
    assert(api->read(&speed).ok == kQolOk && speed == 10);
    assert(api->read(nullptr).ok == kQolErrInvalidArgument);

    descriptor.on_disabled(g_context);
    (void)char_add_experience_wrapper(&player, 100, 1);
    assert(g_last_exp == 100);
    (void)inven_add_money_wrapper(100);
    assert(g_last_money == 100);
}

void test_panel_contract() {
    const auto& provider = panel_provider();
    assert(std::strcmp(provider.panel_id, "world_tuning") == 0);
    assert(std::strcmp(provider.owner, "world_tuning") == 0);
    assert(provider.callbacks != nullptr);
    assert(!panel_available());
    install_success();
    assert(!panel_provider().open());  // 门禁关闭时拒绝打开
    const auto& descriptor = plugin_descriptor();
    descriptor.on_enabled(g_context);
    assert(!panel_provider().open());  // 未绑定 UI 时不崩溃且拒绝打开
    descriptor.deactivate(g_context);
}

}  // namespace

int main() {
    test_descriptor();
    test_install_lifecycle_and_toggle();
    test_damage_wrapper();
    test_max_hp_wrapper();
    test_deactivate_residual_wrappers();
    test_exp_money_and_move_speed();
    test_panel_contract();
    assert(g_in_flight_enters == g_in_flight_leaves);
    return 0;
}

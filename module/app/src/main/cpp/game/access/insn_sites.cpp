#include "game/access/insn_sites.h"

#include <cstring>

#include "game/access/game_access.h"
#include "game/symbols/game_symbols.h"
#include "game/variants/game_variant.h"

namespace qol::game {
namespace {
struct SiteDef {
    const char* id; uintptr_t vma; uint32_t kind, a, b, d, divisor; uintptr_t ret;
    uint32_t replay_count = 0; uint32_t replay[4]{}; uintptr_t replay_bl = 0;
    uint32_t expected_d = 0;
    uint32_t expected_word = 0; uint32_t threshold_cap = 0;
    // opt-in 区域指纹：原版点位逐字校验，变体改写后安装期拒绝。
    uint32_t region_count = 0; uint32_t region[12]{};
};
constexpr SiteDef kSites[] = {
    {"INSN_ACT_M_DD_WRAP_A", F_CHAR_ACT_M_DD_WRAP_A_VMA, 0, 23, 0, 2, 1000,
     F_CHAR_ACT_M_DD_WRAP_A_RET_VMA, 0, {}, 0, 0, 0, 0, 5,
     {0x1b007ef7u, 0x9b357ef5u, 0xd360feb5u, 0x13067eb5u, 0x4b977ea2u}},
    {"INSN_ACT_M_DD_WRAP_B", F_CHAR_ACT_M_DD_WRAP_B_VMA, 1, 21, 2, 21, 0, 0,
     0, {}, 0, 0, 0, 0, 1, {0x1b027eb5u}},
    {"INSN_ACT_M_DD_WRAP_BC", F_CHAR_ACT_M_DD_WRAP_BC_VMA, 3, 21, 0, 2, 1000,
     F_CHAR_ACT_M_DD_WRAP_BC_RET_VMA, 0, {}, 0, 0, 0, 0, 10,
     {0x9b217ea1u, 0x72a20c42u, 0xd360fc21u, 0x13067c21u, 0x4b957c35u,
      0x1b007eb5u, 0x9b227ea2u, 0xd360fc42u, 0x13067c42u, 0x4b957c42u}},
    {"INSN_ACT_DD_EF20C", 0xef20c, 4, 27, 22, 22, 1000, 0xef234,
     4, {0x39400000u, 0xf9400042u, 0x1b007ee0u, 0x93407c00u}, 0, 22},
    {"INSN_ACT_DD_EF4D0", 0xef4d0, 4, 27, 0, 0, 1000, 0xef4e8,
     0, {}, 0, 0},
    {"INSN_ACT_DD_EF530", 0xef530, 4, 22, 2, 22, 1000, 0xef560,
     3, {0xffffffffu, 0x52807d03u, 0x4b000060u}, F_GET_ATTR_VMA, 22},
    {"INSN_ACT_DD_EF560", 0xef560, 4, 22, 0, 2, 1000, 0xef574,
     0, {}, 0, 22},
    {"INSN_ACT_DD_EF5D8", 0xef5d8, 4, 22, 1, 2, 1000, 0xef5f0,
     1, {0xaa1303e1u}, 0, 22},
    {"INSN_ATTR_ATTACK_EB1B4", 0xeb1b4, 4, 3, 1, 26, 100, 0xeb1d0,
     2, {0x51005842u, 0x7100145fu}, 0, 1},
    {"INSN_DROP_REROLL_258", F_CHARSYSTEM_DROP_ITEM_VMA + 0x258, 5, 28, 0, 0, 0, 0,
     1, {0x6b00039fu}, 0, 0, 0x6b00039fu},
    {"INSN_DROP_REROLL_3B8", F_CHARSYSTEM_DROP_ITEM_VMA + 0x3b8, 5, 28, 0, 0, 0, 0,
     1, {0x6b1c001fu}, 0, 0, 0x6b1c001fu},
    {"INSN_DROP_REROLL_400", F_CHARSYSTEM_DROP_ITEM_VMA + 0x400, 5, 28, 0, 0, 0, 0,
     1, {0x6b00039fu}, 0, 0, 0x6b00039fu},
    {"INSN_DROP_REROLL_460", F_CHARSYSTEM_DROP_ITEM_VMA + 0x460, 5, 28, 0, 0, 0, 0,
     1, {0x6b1c001fu}, 0, 0, 0x6b1c001fu},
};

bool drop_threshold_cap(const GameVariant& variant, bool equip_site, uint32_t* out_cap) {
    if (out_cap == nullptr || !variant.known || variant.version_label == nullptr) return false;
    const char* version = variant.version_label;
    switch (variant.series) {
        case GameSeries::kOriginal:
            if (std::strcmp(version, "v1.3.2") != 0) return false;
            *out_cap = 10000;
            return true;
        case GameSeries::kC18:
            if (std::strcmp(version, "v586") != 0 && std::strcmp(version, "v514") != 0 &&
                std::strcmp(version, "v520") != 0 && std::strcmp(version, "v603") != 0) {
                return false;
            }
            *out_cap = 10000;
            return true;
        case GameSeries::kMonster:
            if (std::strcmp(version, "v1.3.2") != 0 && std::strcmp(version, "v23") != 0 &&
                std::strcmp(version, "v29") != 0) {
                return false;
            }
            *out_cap = equip_site ? 6000 : 3000;
            return true;
        case GameSeries::kOverhaul:
            if (std::strcmp(version, "20260830") != 0) return false;
            *out_cap = equip_site ? 6000 : 3000;
            return true;
        case GameSeries::kUnknown:
        case GameSeries::kFantasy:
            return false;
    }
    return false;
}
}

bool insn_site_resolve(const char* site_id, InsnSite* out) {
    if (site_id == nullptr || out == nullptr || g_base == 0 || !game_variant_init() ||
        !game_variant().known || game_variant().series == GameSeries::kUnknown) return false;
    // INSN_ACT_M_DD_* 三点是 CHAR_Act_M_DD 的原版内联乘除点位，在 orig/c18/monster/overhaul
    // 逐字一致；按原版点位在全部已知变体安装。安装期校验原指令字，变体若改写过该点位会
    // fail-soft 跳过，不会把原版重放套到变体计算上。
    for (const SiteDef& def : kSites) {
        if (std::strcmp(site_id, def.id) != 0) continue;
        uint32_t threshold_cap = 0;
        if (def.kind == 5 &&
            !drop_threshold_cap(game_variant(), def.vma == F_CHARSYSTEM_DROP_ITEM_VMA + 0x460,
                                &threshold_cap)) {
            return false;
        }
        const uintptr_t addr = g_base + def.vma;
        *out = InsnSite{g_base + def.vma, def.kind, def.a, def.b, def.d, def.divisor,
                        def.kind == 5 ? addr + 4 : (def.ret == 0 ? 0 : g_base + def.ret),
                        def.replay_count,
                        {def.replay[0], def.replay[1], def.replay[2], def.replay[3]},
                        def.replay_bl == 0 ? 0 : g_base + def.replay_bl, def.expected_d,
                        def.expected_word, threshold_cap, def.region_count,
                        {def.region[0], def.region[1], def.region[2], def.region[3],
                         def.region[4], def.region[5], def.region[6], def.region[7],
                         def.region[8], def.region[9], def.region[10], def.region[11]}};
        return true;
    }
    return false;
}
}

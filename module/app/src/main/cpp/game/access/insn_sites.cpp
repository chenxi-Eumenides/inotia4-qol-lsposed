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
};
constexpr SiteDef kSites[] = {
    {"INSN_ACT_M_DD_WRAP_A", F_CHAR_ACT_M_DD_WRAP_A_VMA, 0, 23, 0, 2, 1000,
     F_CHAR_ACT_M_DD_WRAP_A_RET_VMA},
    {"INSN_ACT_M_DD_WRAP_B", F_CHAR_ACT_M_DD_WRAP_B_VMA, 1, 21, 2, 21, 0, 0},
    {"INSN_ACT_M_DD_WRAP_BC", F_CHAR_ACT_M_DD_WRAP_BC_VMA, 3, 21, 0, 2, 1000,
     F_CHAR_ACT_M_DD_WRAP_BC_RET_VMA},
    {"INSN_ACT_DD_EF20C", 0xef20c, 4, 27, 22, 22, 1000, 0xef234,
     4, {0x39400000u, 0xf9400042u, 0x1b007ee0u, 0x93407c00u}, 0, 22},
    {"INSN_ACT_DD_EF4D0", 0xef4d0, 4, 27, 0, 0, 1000, 0xef4e8,
     0, {}, 0, 0},
    {"INSN_ACT_DD_EF530", 0xef530, 4, 22, 2, 22, 1000, 0xef560,
     3, {0xffffffffu, 0x52807d03u, 0x4b000060u}, F_GET_ATTR_VMA, 22},
    {"INSN_ACT_DD_EF560", 0xef560, 4, 22, 0, 22, 1000, 0xef574,
     0, {}, 0, 22},
    {"INSN_ACT_DD_EF5D8", 0xef5d8, 4, 22, 1, 2, 1000, 0xef5f0,
     1, {0xaa1303e1u}, 0, 22},
    {"INSN_ATTR_ATTACK_EB1B4", 0xeb1b4, 4, 3, 1, 26, 100, 0xeb1d0,
     2, {0x51005842u, 0x7100145fu}, 0, 1},
};
}

bool insn_site_resolve(const char* site_id, InsnSite* out) {
    if (site_id == nullptr || out == nullptr || g_base == 0 || !game_variant_init() ||
        !game_variant().known || game_variant().series == GameSeries::kUnknown) return false;
    if (std::strncmp(site_id, "INSN_ACT_M_DD_", 13) == 0 &&
        (game_variant().series != GameSeries::kMonster || game_variant().version_label == nullptr ||
         std::strcmp(game_variant().version_label, "v29") != 0)) return false;
    for (const SiteDef& def : kSites) {
        if (std::strcmp(site_id, def.id) != 0) continue;
        *out = InsnSite{g_base + def.vma, def.kind, def.a, def.b, def.d, def.divisor,
                        def.ret == 0 ? 0 : g_base + def.ret, def.replay_count,
                        {def.replay[0], def.replay[1], def.replay[2], def.replay[3]},
                        def.replay_bl == 0 ? 0 : g_base + def.replay_bl, def.expected_d};
        return true;
    }
    return false;
}
}

#include "game/access/insn_sites.h"

#include <cstring>

#include "game/access/game_access.h"
#include "game/symbols/game_symbols.h"
#include "game/variants/game_variant.h"

namespace qol::game {
namespace {
struct SiteDef { const char* id; uintptr_t vma; uint32_t kind, a, b, d, divisor; uintptr_t ret; };
constexpr SiteDef kSites[] = {
    {"INSN_ACT_M_DD_WRAP_A", F_CHAR_ACT_M_DD_WRAP_A_VMA, 0, 23, 0, 2, 1000,
     F_CHAR_ACT_M_DD_WRAP_A_RET_VMA},
    {"INSN_ACT_M_DD_WRAP_B", F_CHAR_ACT_M_DD_WRAP_B_VMA, 1, 21, 2, 21, 0, 0},
    {"INSN_ACT_M_DD_WRAP_BC", F_CHAR_ACT_M_DD_WRAP_BC_VMA, 3, 21, 0, 2, 1000,
     F_CHAR_ACT_M_DD_WRAP_BC_RET_VMA},
};
bool is_v29(const GameVariant& v) {
    return v.known && v.series == GameSeries::kMonster && v.version_label != nullptr &&
           std::strcmp(v.version_label, "v29") == 0;
}
}

bool insn_site_resolve(const char* site_id, InsnSite* out) {
    if (site_id == nullptr || out == nullptr || g_base == 0 || !game_variant_init() ||
        !is_v29(game_variant())) return false;
    for (const SiteDef& def : kSites) {
        if (std::strcmp(site_id, def.id) != 0) continue;
        *out = InsnSite{g_base + def.vma, def.kind, def.a, def.b, def.d, def.divisor,
                        def.ret == 0 ? 0 : g_base + def.ret};
        return true;
    }
    return false;
}
}

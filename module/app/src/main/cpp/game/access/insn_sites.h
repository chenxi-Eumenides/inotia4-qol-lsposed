#pragma once

#include <cstdint>

namespace qol::game {
struct InsnSite {
    uintptr_t addr = 0;
    uint32_t kind = 0;
    uint32_t a_reg = 0;
    uint32_t b_reg = 0;
    uint32_t d_reg = 0;
    uint32_t divisor = 0;
    uintptr_t ret_addr = 0;
    uint32_t replay_count = 0;
    uint32_t replay_words[4]{};
    uintptr_t replay_bl_target = 0;
    uint32_t expected_d_reg = 0;
};

// Audited exact instruction layouts are registered per supported game series.
bool insn_site_resolve(const char* site_id, InsnSite* out);
}

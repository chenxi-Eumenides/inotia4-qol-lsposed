#pragma once

#include <cstdint>

struct InsnPatchSpec {
    uint32_t kind;       // 0=乘除钳制（无 NZCV 存取，subs 收尾模拟原标志），1=64 位乘积写 X 寄存器，
                         // 3=x21 64 位连除 × W 寄存器后钳制
    uint32_t a_reg;
    uint32_t b_reg;
    uint32_t d_reg;
    uint32_t divisor;
    uintptr_t ret_addr;
};

bool insn_patch_install(uintptr_t site, const InsnPatchSpec* spec, void** backup);
bool insn_patch_release(uintptr_t site);

#if defined(QOL_INSN_PATCH_TEST_SEAMS)
bool insn_patch_test_generate(const InsnPatchSpec* spec, uint32_t* words,
                              uint32_t capacity, uint32_t* count);
#endif

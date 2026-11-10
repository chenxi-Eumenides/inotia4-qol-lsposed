#pragma once

#include <cstdint>

struct InsnPatchSpec {
    uint32_t kind;       // 0=乘除钳制（无 NZCV 存取，subs 收尾模拟原标志），1=64 位乘积写 X 寄存器，
                         // 3=x21 64 位连除 × W 寄存器后钳制；4=区域级有符号乘除钳制；
                         // 5=调用阈值 helper、重放 CMP 后回跳
    uint32_t a_reg;
    uint32_t b_reg;
    uint32_t d_reg;
    uint32_t divisor;
    uintptr_t ret_addr;
    uint32_t replay_count;
    uint32_t replay_words[4];
    uintptr_t replay_bl_target;
    uint32_t expected_d_reg;
    uintptr_t helper;
    uint32_t expected_word;
    uint32_t threshold_cap;
};

bool insn_patch_install(uintptr_t site, const InsnPatchSpec* spec, void** backup);
bool insn_patch_release(uintptr_t site);

#if defined(QOL_INSN_PATCH_TEST_SEAMS)
bool insn_patch_test_generate(const InsnPatchSpec* spec, uint32_t* words,
                              uint32_t capacity, uint32_t* count);
bool insn_patch_test_read(uintptr_t site, uint32_t* words, uint32_t capacity, uint32_t* count);
bool insn_patch_test_slot_base(uintptr_t site, uintptr_t* out_base);
#endif

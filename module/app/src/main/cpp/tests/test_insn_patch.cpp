#include "patch/insn_patch.h"
#include "patch/call_patch.h"
#include "plugins/damage_widen/safe_scale.h"
#include "infra/log/qol_log.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sys/mman.h>
#include <unordered_map>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

// kind4 区域 thunk 的最小 AArch64 子集解释器。只接受生成器实际发出的编码与
// insn_sites 中实机 replay 用到的形态；遇到未知编码直接失败，避免“看起来通过”
// 却静默跳过新指令。用于在 x86 host 上按机器码验证算术与寄存器恢复顺序。
struct Sim {
    uint64_t x[31]{};
    bool n = false, z = false, c = false, v = false;
    uint64_t sp = 0x100000;
    std::unordered_map<uint64_t, uint64_t> mem;

    uint64_t get(uint32_t reg) const { return reg == 31 ? 0 : x[reg & 31]; }
    void set(uint32_t reg, uint64_t value) { if ((reg & 31) != 31) x[reg & 31] = value; }
    void set_w(uint32_t reg, uint64_t value) { set(reg, static_cast<uint32_t>(value)); }

    void flags_sub64(uint64_t a, uint64_t b) {
        const uint64_t r = a - b;
        n = (r >> 63) != 0;
        z = r == 0;
        c = a >= b;
        v = (((a ^ b) & (a ^ r)) >> 63) != 0;
    }
    void flags_sub32(uint32_t a, uint32_t b) {
        const uint32_t r = a - b;
        n = (r >> 31) != 0;
        z = r == 0;
        c = a >= b;
        v = (((a ^ b) & (a ^ r)) >> 31) != 0;
    }
    bool cond(uint32_t cc) const {
        switch (cc & 0xf) {
            case 0x0: return z;
            case 0x1: return !z;
            case 0x2: return c;
            case 0x3: return !c;
            case 0x8: return c && !z;
            case 0x9: return !c || z;
            case 0xa: return n == v;
            case 0xb: return n != v;
            case 0xc: return !z && n == v;
            case 0xd: return z || n != v;
            default: return false;
        }
    }
};

uint32_t rd_field(uint32_t w) { return w & 0x1f; }
uint32_t rn_field(uint32_t w) { return (w >> 5) & 0x1f; }
uint32_t rm_field(uint32_t w) { return (w >> 16) & 0x1f; }
uint32_t ra_field(uint32_t w) { return (w >> 10) & 0x1f; }

bool exec_word(Sim& s, uint32_t w) {
    // 保存/恢复块由生成器固定发出，精确匹配即可；若编码变化，测试应失败并强制复审。
    if (w == 0xa9bd47f0u) { s.sp -= 48; s.mem[s.sp] = s.get(16); s.mem[s.sp + 8] = s.get(17); return true; }
    if (w == 0xa9014ff2u) { s.mem[s.sp + 16] = s.get(18); s.mem[s.sp + 24] = s.get(19); return true; }
    if (w == 0xa90257f4u) { s.mem[s.sp + 32] = s.get(20); s.mem[s.sp + 40] = s.get(21); return true; }
    if (w == 0xa94257f4u) { s.set(20, s.mem[s.sp + 32]); s.set(21, s.mem[s.sp + 40]); return true; }
    if (w == 0xa9414ff2u) { s.set(18, s.mem[s.sp + 16]); s.set(19, s.mem[s.sp + 24]); return true; }
    if (w == 0xa8c347f0u) { s.set(16, s.mem[s.sp]); s.set(17, s.mem[s.sp + 8]); s.sp += 48; return true; }
    if ((w & 0xfc000000u) == 0x14000000u) return true;   // b（安装期重定位后的返回分支同属 B）
    if ((w & 0xfc000000u) == 0x94000000u) return false;  // BL 未建模：不得静默跳过
    if ((w & 0x7f800000u) == 0x52800000u) {              // MOVZ
        const uint32_t hw = (w >> 21) & 3;
        const uint64_t imm = static_cast<uint64_t>((w >> 5) & 0xffff) << (16 * hw);
        if ((w >> 31) != 0) s.set(rd_field(w), imm); else s.set_w(rd_field(w), imm);
        return true;
    }
    if ((w & 0x7f800000u) == 0x72800000u) {              // MOVK
        const uint32_t hw = (w >> 21) & 3;
        const uint64_t imm = static_cast<uint64_t>((w >> 5) & 0xffff) << (16 * hw);
        const uint32_t r = rd_field(w);
        const uint64_t mask = uint64_t{0xffff} << (16 * hw);
        s.set(r, (s.get(r) & ~mask) | imm);
        return true;
    }
    if ((w & 0xfffffc00u) == 0x93407c00u) {              // SXTW
        s.set(rd_field(w), static_cast<uint64_t>(
            static_cast<int64_t>(static_cast<int32_t>(s.get(rn_field(w))))));
        return true;
    }
    if ((w & 0xffe0fc00u) == 0x9ac00c00u) {              // SDIV 64
        const int64_t a = static_cast<int64_t>(s.get(rn_field(w)));
        const int64_t b = static_cast<int64_t>(s.get(rm_field(w)));
        if (b == 0 || (a == std::numeric_limits<int64_t>::min() && b == -1)) return false;
        s.set(rd_field(w), static_cast<uint64_t>(a / b));
        return true;
    }
    if ((w & 0xffe08000u) == 0x9b008000u) {              // MSUB 64：Ra - Rn*Rm
        s.set(rd_field(w), s.get(ra_field(w)) -
                               s.get(rn_field(w)) * s.get(rm_field(w)));
        return true;
    }
    if ((w & 0xffe08000u) == 0x9b000000u && ra_field(w) == 31) {  // MUL 64
        s.set(rd_field(w), s.get(rn_field(w)) * s.get(rm_field(w)));
        return true;
    }
    if ((w & 0xffe0fc00u) == 0x9b207c00u) {              // SMULL
        const int64_t a = static_cast<int32_t>(s.get(rn_field(w)));
        const int64_t b = static_cast<int32_t>(s.get(rm_field(w)));
        s.set(rd_field(w), static_cast<uint64_t>(a * b));
        return true;
    }
    if ((w & 0xffe08000u) == 0x1b000000u && ra_field(w) == 31) {  // MUL 32
        const uint32_t a = static_cast<uint32_t>(s.get(rn_field(w)));
        const uint32_t b = static_cast<uint32_t>(s.get(rm_field(w)));
        s.set_w(rd_field(w), static_cast<uint64_t>(a * b));
        return true;
    }
    if ((w & 0xffe0fc00u) == 0x8b000000u) {              // ADD 64
        s.set(rd_field(w), s.get(rn_field(w)) + s.get(rm_field(w)));
        return true;
    }
    if ((w & 0xffe0ffe0u) == 0xaa0003e0u) {              // MOV 64
        s.set(rd_field(w), s.get(rm_field(w)));
        return true;
    }
    if ((w & 0xffe0ffe0u) == 0x2a0003e0u) {              // MOV 32
        s.set_w(rd_field(w), s.get(rm_field(w)));
        return true;
    }
    if ((w & 0xffe0fc00u) == 0xeb000000u) {              // SUBS 64 / CMP
        const uint64_t a = s.get(rn_field(w));
        const uint64_t b = s.get(rm_field(w));
        s.flags_sub64(a, b);
        if (rd_field(w) != 31) s.set(rd_field(w), a - b);
        return true;
    }
    if ((w & 0xffe00c00u) == 0x9a800000u) {              // CSEL 64
        s.set(rd_field(w), s.cond((w >> 12) & 0xf) ? s.get(rn_field(w))
                                                   : s.get(rm_field(w)));
        return true;
    }
    if ((w & 0xffc00000u) == 0x39400000u) {              // LDRB (unsigned offset)
        const uint64_t addr = s.get(rn_field(w)) + ((w >> 10) & 0xfff);
        const auto it = s.mem.find(addr);
        s.set_w(rd_field(w), it == s.mem.end() ? 0 : (it->second & 0xff));
        return true;
    }
    if ((w & 0xffc00000u) == 0xf9400000u) {              // LDR 64 (unsigned offset)
        const uint64_t addr = s.get(rn_field(w)) + static_cast<uint64_t>((w >> 10) & 0xfff) * 8;
        const auto it = s.mem.find(addr);
        s.set(rd_field(w), it == s.mem.end() ? 0 : it->second);
        return true;
    }
    if ((w & 0xffe0fc00u) == 0x4b000000u) {              // SUB 32 (register)
        const uint32_t a = static_cast<uint32_t>(s.get(rn_field(w)));
        const uint32_t b = static_cast<uint32_t>(s.get(rm_field(w)));
        s.set_w(rd_field(w), static_cast<uint64_t>(a - b));
        return true;
    }
    if ((w & 0xff800000u) == 0x51000000u) {              // SUB 32 (imm)
        const uint32_t imm = (w >> 10) & 0xfff;
        s.set_w(rd_field(w), static_cast<uint64_t>(
            static_cast<uint32_t>(s.get(rn_field(w))) - imm));
        return true;
    }
    if ((w & 0xff800000u) == 0x71000000u) {              // SUBS 32 (imm) / CMP
        const uint32_t a = static_cast<uint32_t>(s.get(rn_field(w)));
        const uint32_t b = (w >> 10) & 0xfff;
        s.flags_sub32(a, b);
        if (rd_field(w) != 31) s.set_w(rd_field(w), static_cast<uint64_t>(a - b));
        return true;
    }
    return false;
}

bool run_thunk(const uint32_t* words, uint32_t count, Sim& sim) {
    for (uint32_t i = 0; i < count; ++i) {
        if (!exec_word(sim, words[i])) return false;
        if ((words[i] & 0xfc000000u) == 0x14000000u) return true;
    }
    return false;  // 必须命中尾部 B，否则结构不符
}

}

void qol_log_write(QolLogLevel, QolDomain, const char*, int, const char*, ...) {}
void qol_log_set_debug_enabled(bool) {}
bool qol_log_debug_enabled() { return false; }

int main(int argc, char** argv) {
    check(qol::features::damage_widen::safe_scale(-1501, 70) == -105,
          "safe_scale truncates negative values toward zero");
    check(qol::features::damage_widen::safe_scale(19, 1000) == 19 &&
              qol::features::damage_widen::safe_scale(141, 1000) == 141,
          "safe_scale preserves low normal-attack base values");
    check(qol::features::damage_widen::safe_scale(2147483647, 2147483647) ==
              (2147483647LL / 1000) * 2147483647LL +
                  ((2147483647LL % 1000) * 2147483647LL) / 1000,
          "safe_scale handles full int32 operands");
    uint32_t words[40]{};
    uint32_t count = 0;
    // A 型：w2 = clamp32((i64)w23 × w0 / 1000)，subs 收尾模拟原习语标志。
    InsnPatchSpec a{0, 23, 0, 2, 1000, 0x123450};
    check(insn_patch_test_generate(&a, words, 24, &count), "A thunk generation");
    check(count == 16 && words[0] == 0xa9bf47f0u && words[1] == 0x9b207ef0u &&
              words[2] == 0xd2807d11u && words[3] == 0x9ad10e10u &&
              words[4] == 0xf100021fu && words[13] == 0x71000202u &&
              words[14] == 0xa8c147f0u && words[15] == 0x14000000u,
          "A thunk instruction words");
    if (argc > 1) {
        std::ofstream output(argv[1], std::ios::binary);
        output.write(reinterpret_cast<const char*>(words), count * sizeof(uint32_t));
        check(output.good(), "write A thunk binary for disassembly");
    }
    InsnPatchSpec bad_divisor{0, 23, 0, 2, 0, 0x123450};
    check(!insn_patch_test_generate(&bad_divisor, words, 24, &count), "reject divisor zero");
    InsnPatchSpec bad_register{1, 31, 2, 21, 0, 0};
    check(!insn_patch_test_generate(&bad_register, words, 24, &count), "reject register > 30");
    // B 型：x21 = (i64)w21 × w2。
    InsnPatchSpec b{1, 21, 2, 21, 0, 0};
    check(insn_patch_test_generate(&b, words, 24, &count) && count == 3 &&
              words[0] == 0x9b227eb0u && words[1] == 0xaa1003f5u &&
              words[2] == 0xd65f03c0u,
          "B thunk instruction words");
    // BC 型：w2 = clamp32(x21 ÷ 1000 × w0 ÷ 1000)。
    InsnPatchSpec bc{3, 21, 0, 2, 1000, 0x123450};
    check(insn_patch_test_generate(&bc, words, 24, &count), "BC thunk generation");
    check(count == 18 && words[0] == 0x52807d10u && words[1] == 0x9ad00eb0u &&
              words[2] == 0x93407c11u && words[3] == 0x9b117e10u &&
              words[4] == 0x52807d11u && words[5] == 0x9ad10e10u &&
              words[6] == 0xf100021fu && words[15] == 0x71000202u &&
              words[16] == 0xa8c147f0u && words[17] == 0x14000000u,
           "BC thunk instruction words");
    // 区域级：safe_scale 分解式（高位先除后乘 + 低位先乘后除），恢复 x16-x21 后再按原序重放。
    InsnPatchSpec region_spec{4, 27, 22, 22, 1000, 0x123450, 3,
                         {0x39400000u, 0xf9400042u, 0x1b007ee0u, 0}, 0, 22};
    check(insn_patch_test_generate(&region_spec, words, 40, &count) && count == 27,
          "region thunk generation with replay");
    check(words[3] == 0x93407f70u && words[4] == 0xd2807d11u &&
              words[5] == 0x9ad10e12u && words[6] == 0x9b11c253u &&
              words[8] == 0x9b157e54u && words[9] == 0x9b367e75u &&
              words[19] == 0x2a1003f6u &&
              words[20] == 0xa94257f4u && words[21] == 0xa9414ff2u &&
              words[22] == 0xa8c347f0u &&
              words[23] == 0x39400000u && words[24] == 0xf9400042u &&
              words[25] == 0x1b007ee0u && words[26] == 0x14000000u,
          "region decomposition, restore, then original-order replay");
    // EF20C 的实际操作数：A=w27, B/D=w22；逐指令断言防止读未初始化 x0/x20。
    InsnPatchSpec ef20c_spec{4, 27, 22, 22, 1000, 0xef234, 4,
                             {0x39400000u, 0xf9400042u, 0x1b007ee0u, 0x93407c00u}, 0, 22};
    check(insn_patch_test_generate(&ef20c_spec, words, 40, &count) && count == 28,
          "EF20C thunk generation with live sxtw replay");
    check(words[6] == 0x9b11c253u &&  // msub x19,x18,x17,x16
              words[8] == 0x9b157e54u &&  // mul x20,x18,x21
              words[9] == 0x9b367e75u,     // smull x21,w19,w22
          "EF20C safe_scale operands use initialized quotient/remainder/input registers");
    // ÷100 与 ÷1000 同构分解式（除数参数化）；先恢复 x16-x21，再按原序重放：
    // 非标志 replay 在前，置标志 replay 最后（区域外分支消费最终标志）。
    InsnPatchSpec region_div100{4, 3, 1, 26, 100, 0x123450, 2,
                                {0x51005842u, 0x7100145fu}, 0, 1};
    check(insn_patch_test_generate(&region_div100, words, 40, &count) && count == 26,
          "div-100 region uses same decomposition");
    check(words[3] == 0x93407c70u && words[4] == 0xd2800c91u &&
              words[20] == 0xa94257f4u && words[21] == 0xa9414ff2u &&
              words[22] == 0xa8c347f0u &&
              words[23] == 0x51005842u && words[24] == 0x7100145fu &&
              words[25] == 0x14000000u,
          "divisor parameterized; restore precedes flag replay at tail");
    // BL 重放保持原序：恢复暂存寄存器后，先 BL 占位，再执行 BL 之后的原始指令。
    InsnPatchSpec region_bl{4, 22, 2, 22, 1000, 0x123450, 3,
                            {0xffffffffu, 0x52807d03u, 0x4b000060u}, 0xdfd18u, 22};
    check(insn_patch_test_generate(&region_bl, words, 40, &count) && count == 27,
          "bl replay placed after restore in original order");
    check(words[3] == 0x93407ed0u && words[19] == 0x2a1003f6u &&
              words[20] == 0xa94257f4u && words[21] == 0xa9414ff2u &&
              words[22] == 0xa8c347f0u &&
              words[23] == 0xffffffffu && words[24] == 0x52807d03u &&
              words[25] == 0x4b000060u && words[26] == 0x14000000u,
          "bl placeholder and post-BL replay ordering");
    // EF5D8：重放 mov x1,x19 必须读到原始 x19（victim），不能读到 safe_scale 余数。
    InsnPatchSpec ef5d8_spec{4, 22, 1, 2, 1000, 0xef5f0, 1,
                             {0xaa1303e1u}, 0, 22};
    check(insn_patch_test_generate(&ef5d8_spec, words, 40, &count) && count == 25,
          "EF5D8 thunk generation with x19 replay");
    check(words[19] == 0x2a1003e2u && words[20] == 0xa94257f4u &&
              words[21] == 0xa9414ff2u && words[22] == 0xa8c347f0u &&
              words[23] == 0xaa1303e1u,
          "EF5D8 writes w2 region output and replay observes restored x19");
    // 暂存区约束：分解式读写 x16-x21，a/b/d 落入其中会读入余数或被恢复覆盖。
    InsnPatchSpec bad_a_scratch{4, 19, 22, 22, 1000, 0x123450, 0, {}, 0, 22};
    check(!insn_patch_test_generate(&bad_a_scratch, words, 40, &count),
          "reject region a_reg in scratch x16-x21");
    InsnPatchSpec bad_b_scratch{4, 27, 20, 22, 1000, 0x123450, 0, {}, 0, 22};
    check(!insn_patch_test_generate(&bad_b_scratch, words, 40, &count),
          "reject region b_reg in scratch x16-x21");
    InsnPatchSpec bad_d_scratch{4, 27, 22, 18, 1000, 0x123450, 0, {}, 0, 18};
    check(!insn_patch_test_generate(&bad_d_scratch, words, 40, &count),
          "reject region d_reg in scratch x16-x21");
    // 参考实现：safe_scale 与精确向零截断一致（覆盖 int32 边界与负值）。
    {
        const int32_t xs[] = {0, 1, 19, 141, 999, 1000, 1001, 2147483647,
                              -1, -999, -1000, -2147483647, INT32_MIN};
        const int32_t ns[] = {0, 1, 700, 1000, -1, 2147483647, -2147483647};
        for (int32_t xv : xs) {
            for (int32_t nv : ns) {
                const __int128 expected = static_cast<__int128>(xv) * nv / 1000;
                char message[128];
                std::snprintf(message, sizeof(message),
                              "safe_scale matches trunc(X*N/1000): X=%d N=%d", xv, nv);
                check(qol::features::damage_widen::safe_scale(xv, nv) ==
                          static_cast<int64_t>(expected), message);
            }
        }
    }
    // 机器码级：按生成器实际产出的指令执行分解式，与精确参考比较（含钳制）。
    {
        const int32_t xs[] = {0, 1, 19, 141, 999, 1000, 1001, 1999, 2000,
                              123456, 2147483647, -1, -999, -1000, -1001,
                              -2147483647, INT32_MIN};
        const int32_t ns[] = {0, 1, 700, 1000, -1, 2147483647, -2147483647};
        for (uint32_t divisor : {1u, 7u, 100u, 1000u, 65535u}) {
            for (int32_t xv : xs) {
                for (int32_t nv : ns) {
                    InsnPatchSpec spec{4, 2, 3, 4, divisor, 0x123450, 0, {}, 0, 4};
                    uint32_t generated = 0;
                    if (!insn_patch_test_generate(&spec, words, 40, &generated)) {
                        check(false, "semantic matrix generation");
                        continue;
                    }
                    Sim sim;
                    sim.set(2, static_cast<uint32_t>(xv));
                    sim.set(3, static_cast<uint32_t>(nv));
                    check(run_thunk(words, generated, sim), "semantic matrix executes");
                    int64_t expected = static_cast<int64_t>(
                        static_cast<__int128>(xv) * nv / divisor);
                    if (expected < 0) expected = 0;
                    if (expected > std::numeric_limits<int32_t>::max())
                        expected = std::numeric_limits<int32_t>::max();
                    char message[160];
                    std::snprintf(message, sizeof(message),
                                  "kind4 machine code matches trunc(X*N/%u) and clamp: X=%d N=%d",
                                  divisor, xv, nv);
                    check(static_cast<int32_t>(sim.get(4)) ==
                              static_cast<int32_t>(expected), message);
                }
            }
        }
    }
    // 伪随机参数补充覆盖：任意 divisor 1..65535、含 int32 全域。
    {
        uint32_t seed = 0x12345678u;
        for (int i = 0; i < 512; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const int32_t xv = static_cast<int32_t>(seed);
            seed = seed * 1664525u + 1013904223u;
            const int32_t nv = static_cast<int32_t>(seed);
            seed = seed * 1664525u + 1013904223u;
            const uint32_t divisor = (seed % 65535u) + 1u;
            InsnPatchSpec spec{4, 2, 3, 4, divisor, 0x123450, 0, {}, 0, 4};
            uint32_t generated = 0;
            if (!insn_patch_test_generate(&spec, words, 40, &generated)) {
                check(false, "random semantic generation");
                continue;
            }
            Sim sim;
            sim.set(2, static_cast<uint32_t>(xv));
            sim.set(3, static_cast<uint32_t>(nv));
            check(run_thunk(words, generated, sim), "random semantic execution");
            int64_t expected = static_cast<int64_t>(
                static_cast<__int128>(xv) * nv / divisor);
            if (expected < 0) expected = 0;
            if (expected > std::numeric_limits<int32_t>::max())
                expected = std::numeric_limits<int32_t>::max();
            char message[160];
            std::snprintf(message, sizeof(message),
                          "random kind4 machine code matches trunc(X*N/%u): X=%d N=%d",
                          divisor, xv, nv);
            check(static_cast<int32_t>(sim.get(4)) == static_cast<int32_t>(expected),
                  message);
        }
    }
    // EF20C 实机组合：w27=19, w22=1000 必须产出 19；replay 的 ldrb/ldr/mul/sxtw 保序。
    {
        InsnPatchSpec spec{4, 27, 22, 22, 1000, 0xef234, 4,
                           {0x39400000u, 0xf9400042u, 0x1b007ee0u, 0x93407c00u}, 0, 22};
        uint32_t generated = 0;
        check(insn_patch_test_generate(&spec, words, 40, &generated),
              "EF20C semantic generation");
        Sim sim;
        sim.set_w(27, 19);
        sim.set_w(22, 1000);
        sim.set_w(23, 5);
        const uint64_t byte_addr = 0x200000;
        sim.mem[byte_addr] = 0;
        sim.set(0, byte_addr);
        const uint64_t table_addr = 0x210000;
        sim.mem[table_addr] = byte_addr;
        sim.set(2, table_addr);
        check(run_thunk(words, generated, sim), "EF20C semantic execution");
        check(static_cast<int32_t>(sim.get(22)) == 19,
              "EF20C machine code yields 19 for X=19,N=1000");
        check(sim.get(2) == byte_addr,
              "EF20C replay preserves ldr x2,[x2] semantics");
    }
    // EF5D8 实机组合：区域输出是 w2（随后 w2 作为物防第三参数），
    // replay mov x1,x19 必须读到恢复后的 victim，而不是 safe_scale 余数。
    {
        InsnPatchSpec spec{4, 22, 1, 2, 1000, 0xef5f0, 1, {0xaa1303e1u}, 0, 22};
        uint32_t generated = 0;
        check(insn_patch_test_generate(&spec, words, 40, &generated),
              "EF5D8 semantic generation");
        Sim sim;
        sim.set(19, 0x1000);  // victim pointer
        sim.set_w(22, 141);   // Act_DD 伤害
        sim.set_w(1, 700);    // 千分比因子
        check(run_thunk(words, generated, sim), "EF5D8 semantic execution");
        check(sim.get(1) == 0x1000,
              "EF5D8 replay reads original x19 (restore precedes replay)");
        check(static_cast<int32_t>(sim.get(2)) == 98,
              "EF5D8 machine code writes w2=141*700/1000=98");
        check(sim.get(19) == 0x1000, "EF5D8 restores original x19");
    }
    // EF560 实机组合：区域输出是 w2（ef574 返回后 Success 读取 w2），不是 w22。
    {
        InsnPatchSpec spec{4, 22, 0, 2, 1000, 0xef574, 0, {}, 0, 22};
        uint32_t generated = 0;
        check(insn_patch_test_generate(&spec, words, 40, &generated) && generated == 24,
              "EF560 semantic generation");
        check(words[19] == 0x2a1003e2u, "EF560 writes w2 region output");
        Sim sim;
        sim.set_w(22, 5000);
        sim.set_w(0, 500);
        check(run_thunk(words, generated, sim), "EF560 semantic execution");
        check(static_cast<int32_t>(sim.get(2)) == 2500,
              "EF560 machine code writes w2=5000*500/1000=2500");
    }
    InsnPatchSpec bad_replay{4, 27, 22, 22, 1000, 0x123450, 5, {}, 0};
    check(!insn_patch_test_generate(&bad_replay, words, 24, &count),
          "reject oversized region replay");
    InsnPatchSpec bad_region_branch{4, 22, 1, 22, 1000, 0x123450, 1,
                                    {0x54000000u, 0, 0, 0}, 0};
    check(!insn_patch_test_generate(&bad_region_branch, words, 40, &count),
          "reject condition branch in region replay");
    bad_region_branch.replay_words[0] = 0x90000000u;
    check(!insn_patch_test_generate(&bad_region_branch, words, 40, &count),
          "reject ADRP in region replay");
    InsnPatchSpec drop_cmp{5, 28, 0, 0, 0, 0x123454, 1,
                           {0x6b00039fu, 0, 0, 0}, 0, 0,
                           0x123456789abcdef0u, 0x6b00039fu, 10000};
    check(insn_patch_test_generate(&drop_cmp, words, 40, &count) && count == 30,
          "drop threshold helper trampoline generation");
    check(words[0] == 0xa9bf07e0u && words[9] == 0xa9bf7bf2u &&
              words[10] == 0x2a1c03e0u && words[16] == 0xd63f0200u &&
              words[17] == 0x2a0003fcu && words[18] == 0xa8c17bf2u &&
              words[27] == 0xa8c107e0u,
          "drop trampoline saves/restores caller registers and LR around helper");
    check(words[28] == 0x6b00039fu && (words[29] & 0xfc000000u) == 0x14000000u,
          "drop trampoline replays CMP immediately before flags-preserving B");
    InsnPatchSpec bad_drop_cmp = drop_cmp;
    bad_drop_cmp.replay_words[0] = 0x6b1c001fu;
    check(!insn_patch_test_generate(&bad_drop_cmp, words, 40, &count),
          "reject replay CMP that differs from expected original word");
    bad_drop_cmp = drop_cmp;
    bad_drop_cmp.expected_word = 0;
    check(!insn_patch_test_generate(&bad_drop_cmp, words, 40, &count),
          "reject unsupported expected CMP word");
    void* region = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(region != MAP_FAILED, "allocate test code page");
    if (region != MAP_FAILED) {
        auto* code = static_cast<uint32_t*>(region);
        const uintptr_t site = reinterpret_cast<uintptr_t>(region);
        void* backup = nullptr;
        // A 型安装：原指令必须为 mul w23,w23,w0（0x1b007ef7）。
        code[0] = 0x1b007ef7u;
        InsnPatchSpec a_patch{0, 23, 0, 2, 1000, site + 0x100};
        check(insn_patch_install(site, &a_patch, &backup), "install multi-slot A trampoline");
        check((code[0] & 0xfc000000u) == 0x94000000u,
              "A site branches to a near thunk");
        check(reinterpret_cast<uintptr_t>(backup) == 0x1b007ef7u, "A backup is original word");
        check(insn_patch_release(site) && code[0] == 0x1b007ef7u,
              "A release restores the original instruction");
        // 原指令不符时拒绝安装（防止错位补丁）。
        code[0] = 0x1b007eb5u;
        check(!insn_patch_install(site, &a_patch, &backup), "reject mismatched original word");
        // B 型安装：原指令必须为 mul w21,w21,w2（0x1b027eb5）。
        code[0] = 0x1b027eb5u;
        InsnPatchSpec b_patch{1, 21, 2, 21, 0, 0};
        check(insn_patch_install(site, &b_patch, &backup), "install B trampoline");
        check((code[0] & 0xfc000000u) == 0x94000000u, "B site branches to a near thunk");
        check(insn_patch_release(site) && code[0] == 0x1b027eb5u,
              "B release restores the original instruction");
        // BC 型安装：原指令必须为 smull x1,w21,w1（0x9b217ea1）。
        code[0] = 0x9b217ea1u;
        InsnPatchSpec bc_patch{3, 21, 0, 2, 1000, site + 0x100};
        check(insn_patch_install(site, &bc_patch, &backup), "install multi-slot BC trampoline");
        check((code[0] & 0xfc000000u) == 0x94000000u, "BC site branches to a near thunk");
        check(insn_patch_release(site) && code[0] == 0x9b217ea1u,
              "BC release restores the original instruction");
        code[0] = 0x1b167f76u;
        InsnPatchSpec region_patch{4, 27, 22, 22, 1000, site + 0x100, 3,
                                   {0x39400000u, 0xf9400042u, 0x1b007ee0u, 0}, 0, 22};
        check(insn_patch_install(site, &region_patch, &backup),
              "install region trampoline");
        check(reinterpret_cast<uintptr_t>(backup) == 0x1b167f76u,
              "region backup contains original instruction");
        // 多槽相邻性回归：kind4 占 4 槽，顺序落空执行要求 4 槽内存相邻。
        {
            void* multi[4]{};
            check(call_patch_test_acquire_near_slots(site, 4, multi) != nullptr,
                  "acquire 4 contiguous near slots");
            check(multi[1] == static_cast<char*>(multi[0]) + 32 &&
                      multi[2] == static_cast<char*>(multi[0]) + 64 &&
                      multi[3] == static_cast<char*>(multi[0]) + 96,
                  "multi-slot chunks are memory contiguous");
            for (auto* m : multi) call_patch_test_release_thunk_slot(m);
        }
        check(insn_patch_release(site) && code[0] == 0x1b167f76u,
              "region release restores original instruction");
        code[0] = 0x1b167f75u;
        check(!insn_patch_install(site, &region_patch, &backup),
              "reject mismatched region start instruction");
        code[0] = 0x1b167f76u;
        InsnPatchSpec region_bl{4, 27, 22, 22, 1000, site + 0x100, 1,
                                {0xffffffffu, 0, 0, 0}, site + 0x200, 22};
        check(insn_patch_test_generate(&region_bl, words, 40, &count),
              "generate region with relocated BL replay");
        check(insn_patch_install(site, &region_bl, &backup),
              "install region with relocated BL replay");
        {
            uint32_t tramp[32]{};
            uint32_t tramp_count = 0;
            check(insn_patch_test_read(site, tramp, 32, &tramp_count),
                  "read installed region trampoline");
            bool placeholder = false;
            bool relocated_bl = false;
            for (uint32_t i = 0; i < count; ++i) {
                if (tramp[i] == 0xffffffffu) placeholder = true;
                if ((tramp[i] & 0xfc000000u) == 0x94000000u) relocated_bl = true;
            }
            check(!placeholder, "BL placeholder must be relocated at install");
            check(relocated_bl, "relocated BL present in region trampoline");
            uintptr_t slot_base = 0;
            check(insn_patch_test_slot_base(site, &slot_base),
                  "read region trampoline slot base");
            int64_t bl_target = -1;
            if (relocated_bl && slot_base != 0) {
                for (uint32_t i = 0; i < tramp_count; ++i) {
                    if ((tramp[i] & 0xfc000000u) != 0x94000000u) continue;
                    const int64_t delta = static_cast<int64_t>(
                        static_cast<int32_t>(tramp[i] << 6) >> 6);  // 符号扩展 imm26
                    bl_target = static_cast<int64_t>(slot_base + i * 4u) + delta * 4;
                    break;
                }
            }
            check(relocated_bl && bl_target == static_cast<int64_t>(site + 0x200),
                  "relocated BL targets replay_bl_target");
        }
        check(insn_patch_release(site) && code[0] == 0x1b167f76u,
              "relocated BL region release restores original instruction");
        code[0] = 0x6b00039fu;
        drop_cmp.ret_addr = site + 4;
        check(insn_patch_install(site, &drop_cmp, &backup),
              "install compare replay threshold trampoline");
        check((code[0] & 0xfc000000u) == 0x14000000u,
              "instruction-point entry uses B rather than BL");
        check(reinterpret_cast<uintptr_t>(backup) == 0x6b00039fu,
              "compare trampoline backup is the exact original CMP word");
        {
            uint32_t tramp[32]{};
            uint32_t tramp_count = 0;
            uintptr_t slot_base = 0;
            check(insn_patch_test_read(site, tramp, 32, &tramp_count) && tramp_count == 32,
                  "read compare replay trampoline");
            check(insn_patch_test_slot_base(site, &slot_base),
                  "read compare replay trampoline slot base");
            check(tramp[28] == 0x6b00039fu && (tramp[29] & 0xfc000000u) == 0x14000000u,
                  "installed trampoline leaves replayed CMP flags for the original b.cond");
            if (slot_base != 0) {
                const int64_t delta = static_cast<int64_t>(
                    static_cast<int32_t>(tramp[29] << 6) >> 6);
                check(static_cast<int64_t>(slot_base + 29u * 4u) + delta * 4 ==
                          static_cast<int64_t>(site + 4),
                      "compare trampoline B returns to the original conditional branch");
            }
        }
        check(insn_patch_release(site) && code[0] == 0x6b00039fu,
              "compare patch release physically restores original CMP");
        code[0] = 0x6b1c001fu;
        check(!insn_patch_install(site, &drop_cmp, &backup),
              "reject mismatched expected CMP word at installation");
        (void)munmap(region, 4096);
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

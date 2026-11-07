#include "patch/insn_patch.h"
#include "infra/log/qol_log.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/mman.h>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

}

void qol_log_write(QolLogLevel, QolDomain, const char*, int, const char*, ...) {}
void qol_log_set_debug_enabled(bool) {}
bool qol_log_debug_enabled() { return false; }

int main(int argc, char** argv) {
    uint32_t words[24]{};
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
        (void)munmap(region, 4096);
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

#include "patch/insn_patch.h"
#include "patch/call_patch.h"
#include "plugins/damage_widen/safe_scale.h"
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
    check(qol::features::damage_widen::safe_scale(-1501, 70) == -105,
          "safe_scale truncates negative values toward zero");
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
    // 区域级：safe_scale 分解式（高位先除后乘 + 低位先乘后除），重放按原序排在分解式之后。
    InsnPatchSpec region_spec{4, 27, 22, 22, 1000, 0x123450, 3,
                         {0x39400000u, 0xf9400042u, 0x1b007ee0u, 0}, 0, 22};
    check(insn_patch_test_generate(&region_spec, words, 40, &count) && count == 27,
          "region thunk generation with replay");
    check(words[3] == 0x93407f70u && words[4] == 0xd2807d11u &&
              words[5] == 0x9ad10e12u && words[6] == 0x9b118253u &&
              words[19] == 0x2a1003f6u &&
              words[20] == 0x39400000u && words[21] == 0xf9400042u &&
              words[22] == 0x1b007ee0u &&
              words[23] == 0xa94257f4u && words[25] == 0xa8c347f0u,
          "region decomposition then original-order replay");
    // ÷100 与 ÷1000 同构分解式（除数参数化）；置标志 replay 位于非标志 replay 之后、恢复之前。
    InsnPatchSpec region_div100{4, 3, 1, 26, 100, 0x123450, 2,
                                {0x51005842u, 0x7100145fu}, 0, 1};
    check(insn_patch_test_generate(&region_div100, words, 40, &count) && count == 26,
          "div-100 region uses same decomposition");
    check(words[3] == 0x93407c70u && words[4] == 0xd2800c91u &&
              words[20] == 0x51005842u && words[21] == 0x7100145fu &&
              words[22] == 0xa94257f4u,
          "divisor parameterized; flag replay ordered after non-flag replay, before restore");
    // BL 重放保持原序：分解式结束后先 BL 占位，再执行 BL 之后的原始指令。
    InsnPatchSpec region_bl{4, 22, 2, 22, 1000, 0x123450, 3,
                            {0xffffffffu, 0x52807d03u, 0x4b000060u}, 0xdfd18u, 22};
    check(insn_patch_test_generate(&region_bl, words, 40, &count) && count == 27,
          "bl replay placed after decomposition in original order");
    check(words[3] == 0x93407ed0u && words[19] == 0x2a1003f6u &&
              words[20] == 0xffffffffu && words[21] == 0x52807d03u &&
              words[22] == 0x4b000060u && words[23] == 0xa94257f4u,
          "bl placeholder and post-BL replay ordering");
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
        }
        check(insn_patch_release(site) && code[0] == 0x1b167f76u,
              "relocated BL region release restores original instruction");
        (void)munmap(region, 4096);
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

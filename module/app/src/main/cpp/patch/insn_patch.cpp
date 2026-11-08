#include "patch/insn_patch.h"

#include <array>
#include <cstring>
#include <mutex>
#include <sys/mman.h>

#include "patch/call_patch.h"

namespace {
constexpr size_t kMaxEntries = 32;
struct Entry {
    uintptr_t site = 0;
    uint32_t original = 0;
    void* slots[4]{};
    uint32_t slot_count = 0;
};
std::array<Entry, kMaxEntries> g_entries{};
size_t g_count = 0;
std::mutex g_mutex;

uint32_t movz_w(uint32_t reg, uint32_t value) {
    return 0x52800000u | ((value & 0xffffu) << 5) | reg;
}
uint32_t movz_x(uint32_t reg, uint32_t value) {
    return 0xd2800000u | ((value & 0xffffu) << 5) | reg;
}
uint32_t csel_x(uint32_t d, uint32_t n, uint32_t m, uint32_t cond) {
    return 0x9a800000u | (m << 16) | (cond << 12) | (n << 5) | d;
}
bool replay_word_supported(uint32_t word) {
    if (word == 0xffffffffu) return true; // BL target is relocated at install time.
    if ((word & 0x1f000000u) == 0x10000000u || // ADR/ADRP
        (word & 0xff000010u) == 0x54000000u || // B.cond
        (word & 0x7c000000u) == 0x14000000u || // B/BL
        (word & 0x7e000000u) == 0x34000000u || // CBZ/CBNZ
        (word & 0x7e000000u) == 0x36000000u)   // TBZ/TBNZ
        return false;
    return true;
}
// 置标志指令（SUBS/ADDS/CMP/CMN/TST/ANDS，imm 与 register 两族）必须排在重放末尾：
// 区域外分支消费的最终标志须等于这些指令的结果，而非 thunk 钳制序列的 cmp。
bool replay_sets_flags(uint32_t word) {
    const uint32_t head = word >> 24;
    return head == 0x31 || head == 0x71 || head == 0xB1 || head == 0xF1 ||
           head == 0x2B || head == 0x6B || head == 0xAB || head == 0xEB ||
           head == 0x6A || head == 0xEA;
}
bool bcond(uint32_t from, uint32_t to, uint32_t condition, uint32_t* out) {
    const int64_t delta = static_cast<int64_t>(to) - from;
    if (out == nullptr || (delta & 3) != 0 || delta < -(1 << 20) || delta >= (1 << 20)) return false;
    *out = 0x54000000u | ((static_cast<uint32_t>(delta / 4) & 0x7ffffu) << 5) | condition;
    return true;
}
bool encode_b(uintptr_t from, uintptr_t to, uint32_t* out) {
    const int64_t delta = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    if (out == nullptr || ((from | to) & 3u) != 0 || (delta & 3) != 0 ||
        delta < -(int64_t{1} << 27) || delta >= (int64_t{1} << 27)) return false;
    *out = 0x14000000u | (static_cast<uint32_t>(delta / 4) & 0x03ffffffu);
    return true;
}
bool encode_bcond(uintptr_t from, uintptr_t to, uint32_t condition, uint32_t* out) {
    const int64_t delta = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    if (out == nullptr || ((from | to) & 3u) != 0 || (delta & 3) != 0 ||
        delta < -(int64_t{1} << 20) || delta >= (int64_t{1} << 20)) return false;
    *out = 0x54000000u | ((static_cast<uint32_t>(delta / 4) & 0x7ffffu) << 5) | condition;
    return true;
}

bool generate(const InsnPatchSpec& s, uint32_t* out, uint32_t cap, uint32_t* count) {
    if (out == nullptr || count == nullptr)
        return false;
    uint32_t n = 0;
    auto emit = [&](uint32_t word) { if (n >= cap) return false; out[n++] = word; return true; };
    if (s.kind == 1) {
        if (s.a_reg > 30 || s.b_reg > 30 || s.d_reg > 30) return false;
        if (!emit(0x9b207c00u | (s.b_reg << 16) | (s.a_reg << 5) | 16u) ||
            !emit(0xaa0003e0u | (16u << 16) | s.d_reg) || !emit(0xd65f03c0u)) return false;
    } else if (s.kind == 0) {
        if (s.a_reg > 30 || s.b_reg > 30 || s.d_reg > 30) return false;
        if (s.divisor == 0 || s.divisor > 0xffff || s.ret_addr == 0 || cap < 16) return false;
        // 语义：wD = clamp32((i64)(sxtw wA)×(sxtw wB) ÷ divisor)；
        // 收尾 subs 模拟原习语 `sub wD,..,..,asr#31` 的标志（Z=商==0，N=符号），b.eq 消费方一致。
        if (!emit(0xa9bf47f0u) ||
            !emit(0x9b207c00u | (s.b_reg << 16) | (s.a_reg << 5) | 16u) ||
            !emit(movz_x(17, s.divisor)) || !emit(0x9ad10e10u) ||
            !emit(0xf100021fu)) return false;
        uint32_t branch = 0;
        if (!bcond(n * 4, 12u * 4, 0xb, &branch) || !emit(branch) ||
            !emit(movz_x(17, 0xffff)) || !emit(0xf2affff1u) ||
            !emit(0xeb11021fu)) return false;
        if (!bcond(n * 4, 13u * 4, 0xd, &branch) || !emit(branch) ||
            !emit(0xaa1103f0u)) return false;
        const uint32_t b_store_delta = static_cast<uint32_t>((13u - n) * 4);
        if ((b_store_delta & 3u) != 0 || b_store_delta >= (1u << 27) ||
            !emit(0x14000000u | (b_store_delta / 4))) return false;
        if (n != 12 || !emit(movz_x(16, 0)) || n != 13 ||
            !emit(0x71000200u | s.d_reg) || !emit(0xa8c147f0u) || !emit(0x14000000u)) return false;
    } else if (s.kind == 4) {
        // 区域替换：千分比运算一律走 safe_scale 分解式（X÷D×N + (X mod D)×N÷D，除法向零截断），
        // 与 trunc(X×N÷D) 整数恒等；中间积 ≤ ~(X÷D)×N，不可能超 int64（用户裁决的硬性形态）。
        // 重放排在分解式之后并按原顺序执行：分解式已消费 a_reg/b_reg，重放可以安全改写它们；
        // BL 保留占位，安装期按实际词位重定位；置标志指令延后到最后（区域外分支消费最终标志）。
        if (s.a_reg > 30 || s.b_reg > 30 || s.d_reg > 30 || s.divisor == 0 ||
            s.divisor > 0xffff || s.ret_addr == 0 || s.replay_count > 4 || cap < 29)
            return false;
        uint32_t relocated_bl_count = 0;
        for (uint32_t i = 0; i < s.replay_count; ++i) {
            if (!replay_word_supported(s.replay_words[i])) return false;
            if (s.replay_words[i] == 0xffffffffu) ++relocated_bl_count;
        }
        if (relocated_bl_count > 1 || (relocated_bl_count != 0 && s.replay_bl_target == 0))
            return false;
        if (!emit(0xa9bd47f0u) || // save x16-x21（x30 已由宿主函数序言保存，thunk 尾 b 返回不用 x30）
            !emit(0xa9014ff2u) || !emit(0xa90257f4u)) return false;
        if (!emit(0x93407c00u | (s.a_reg << 5) | 16u) || // sxtw x16,wA
            !emit(movz_x(17, s.divisor)) ||
            !emit(0x9ad10e12u) || // x18 = x16 ÷ divisor（高位先除）
            !emit(0x9b11c253u) || // msub x19, x18, x17, x16：低 D 位余数 = X − floor0(X/D)×D
            !emit(0x93407c00u | (s.b_reg << 5) | 21u) || // sxtw x21,wB
            !emit(0x9b007c00u | (21u << 16) | (18u << 5) | 20u) ||
                // mul x20, x18, x21：高位段乘
            !emit(0x9b207c00u | (s.b_reg << 16) | (19u << 5) | 21u) ||
            !emit(0x9ad10eb5u) || // x21 = (余数×wB)÷divisor（低位段先乘后除）
            !emit(0x8b150294u) || // x20 += x21
            !emit(0xaa1403f0u)) return false; // x16 = 结果
        if (!emit(0xeb1f021fu) || !emit(csel_x(16, 31, 16, 0xb)) ||
            !emit(movz_x(17, 0xffff)) || !emit(0xf2affff1u) ||
            !emit(0xeb11021fu) || !emit(csel_x(16, 17, 16, 0xc)) ||
            !emit(0x2a1003e0u | s.d_reg)) return false;
        // 重放按原顺序排在写回之后：BL 保留占位（安装期重定位），非标志词原序执行；
        // 置标志指令统一延后到最后。
        for (uint32_t i = 0; i < s.replay_count; ++i) {
            if (replay_sets_flags(s.replay_words[i])) continue;
            if (!emit(s.replay_words[i])) return false;
        }
        // 重放组三：置标志指令最后重放（区域外分支消费最终标志）。
        for (uint32_t i = 0; i < s.replay_count; ++i)
            if (replay_sets_flags(s.replay_words[i]) && !emit(s.replay_words[i])) return false;
        if (!emit(0xa94257f4u) || !emit(0xa9414ff2u) || !emit(0xa8c347f0u) ||
            !emit(0x14000000u)) return false;
    } else if (s.kind == 3) {
        // 语义：wD = clamp32(x21 ÷ divisor × (sxtw wB) ÷ divisor)；x21 = 64 位乘积（点 B 写入）。
        // 替换 monster v29 Act_M_DD 0xee1c4..0xee1eb 九条（smull/lsr/asr/sub ×2 + mul）。
        if (s.a_reg > 30 || s.b_reg > 30 || s.d_reg > 30) return false;
        if (s.divisor == 0 || s.divisor > 0xffff || s.ret_addr == 0 || cap < 18) return false;
        if (!emit(0x52800000u | (s.divisor << 5) | 16u) ||
            !emit(0x9ac00c00u | (16u << 16) | (s.a_reg << 5) | 16u) ||
            !emit(0x93407c00u | (s.b_reg << 5) | 17u) ||
            !emit(0x9b107c00u | (17u << 16) | (16u << 5) | 16u) ||
            !emit(0x52800000u | (s.divisor << 5) | 17u) ||
            !emit(0x9ad10e10u) ||
            !emit(0xf100021fu)) return false;
        uint32_t branch = 0;
        if (!bcond(n * 4, 14u * 4, 0xb, &branch) || !emit(branch) ||
            !emit(movz_x(17, 0xffff)) || !emit(0xf2affff1u) ||
            !emit(0xeb11021fu)) return false;
        if (!bcond(n * 4, 15u * 4, 0xd, &branch) || !emit(branch) ||
            !emit(0xaa1103f0u)) return false;
        const uint32_t b_store_delta = static_cast<uint32_t>((15u - n) * 4);
        if ((b_store_delta & 3u) != 0 || b_store_delta >= (1u << 27) ||
            !emit(0x14000000u | (b_store_delta / 4))) return false;
        if (n != 14 || !emit(movz_x(16, 0)) || n != 15 ||
            !emit(0x71000200u | s.d_reg) || !emit(0xa8c147f0u) || !emit(0x14000000u)) return false;
    } else {
        return false;
    }
    *count = n;
    return true;
}

bool write_word(uintptr_t addr, uint32_t word) {
    const uintptr_t page = addr & ~uintptr_t{4095};
    if (mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;
    *reinterpret_cast<uint32_t*>(addr) = word;
    __builtin___clear_cache(reinterpret_cast<char*>(addr), reinterpret_cast<char*>(addr + 4));
    return true;
}
}

#if defined(QOL_INSN_PATCH_TEST_SEAMS)
bool insn_patch_test_generate(const InsnPatchSpec* spec, uint32_t* words,
                              uint32_t capacity, uint32_t* count) {
    return spec != nullptr && generate(*spec, words, capacity, count);
}

bool insn_patch_test_read(uintptr_t site, uint32_t* words, uint32_t capacity,
                          uint32_t* count) {
    if (words == nullptr || count == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < g_count; ++i) {
        if (g_entries[i].site != site) continue;
        uint32_t n = 0;
        for (uint32_t s = 0; s < g_entries[i].slot_count && n < capacity; ++s) {
            const auto* src = static_cast<const uint32_t*>(g_entries[i].slots[s]);
            for (uint32_t w = 0; w < 8 && n < capacity; ++w) words[n++] = src[w];
        }
        *count = n;
        return true;
    }
    return false;
}
#endif

bool insn_patch_install(uintptr_t site, const InsnPatchSpec* spec, void** backup) {
    if (site == 0 || (site & 3u) != 0 || spec == nullptr || backup == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < g_count; ++i) if (g_entries[i].site == site) {
        *backup = reinterpret_cast<void*>(static_cast<uintptr_t>(g_entries[i].original));
        return true;
    }
    if (g_count == g_entries.size()) return false;
    const uint32_t original = *reinterpret_cast<const uint32_t*>(site);
    std::array<uint32_t, 40> code{};
    uint32_t words = 0;
    if (!generate(*spec, code.data(), code.size(), &words)) return false;
    // 原指令校验：kind 0/1 为 MUL Wd,Wn,Wm = MADD(Wd,Wn,Wm,WZR)，基值 0x1b007c00（Ra=xzr）；
    // kind 3 首条为 smull x1,w21,w1（其真 64 位乘积由点 B 的 thunk 写入 x21）。
    const uint32_t expected_mul = 0x1b007c00u | (spec->b_reg << 16) |
                                  (spec->a_reg << 5) |
                                  (spec->kind == 4 ? spec->expected_d_reg : spec->a_reg);
    const uint32_t expected_original = spec->kind == 3 ? 0x9b217ea1u : expected_mul;
    if (original != expected_original) return false;
    void* slots[4]{};
    const uint32_t slot_count = spec->kind == 1 ? 1u : (spec->kind == 4 ? 4u :
                               (spec->kind == 3 ? 3u : 2u));
    // 多槽 thunk 的槽间 fall-through 依赖槽位内存相邻：slot_count ≥ 2 时必须走
    // 连续多槽分配（空闲表散槽无相邻性，不得复用）；单槽仍走常规空闲表路径。
    if (slot_count == 1) {
        slots[0] = call_patch_acquire_near_slot(site);
        if (slots[0] == nullptr) return false;
    } else if (call_patch_acquire_near_slots(site, slot_count, slots) == nullptr) {
        return false;
    }
    const auto code_pc = [slots](uint32_t index) {
        return reinterpret_cast<uintptr_t>(slots[index / 8]) + (index % 8) * 4;
    };
    uint32_t branch_to_thunk = 0;
    if (!encode_b(site, reinterpret_cast<uintptr_t>(slots[0]), &branch_to_thunk)) {
        for (uint32_t i = 0; i < slot_count; ++i)
            call_patch_release_near_entry(reinterpret_cast<uintptr_t>(slots[i]));
        return false;
    }
    if (spec->kind == 0 || spec->kind == 3 || spec->kind == 4) {
        // kind 0：b_lt@5→12、b_le@9→13、b_store@11→13、b_return@words-1
        // kind 3：b_lt@7→14、b_le@11→15、b_store@13→15、b_return@words-1
        if (spec->kind == 4) {
            // BL 占位由 generate 按重放原序写在分解式之后；按实际占位下标重定位，
            // 不能再按固定词数推算（分解式/重放分组会变）。
            uint32_t bl_index = 0;
            bool has_bl = false;
            for (uint32_t i = 0; i < words; ++i) {
                if (code[i] == 0xffffffffu) { bl_index = i; has_bl = true; break; }
            }
            if (has_bl) {
                const uintptr_t from = code_pc(bl_index);
                const int64_t delta = static_cast<int64_t>(spec->replay_bl_target) -
                                      static_cast<int64_t>(from);
                if (spec->replay_bl_target == 0 || (delta & 3) != 0 ||
                    delta < -(int64_t{1} << 27) || delta >= (int64_t{1} << 27)) {
                    for (uint32_t j = 0; j < slot_count; ++j)
                        call_patch_release_near_entry(reinterpret_cast<uintptr_t>(slots[j]));
                    return false;
                }
                code[bl_index] = 0x94000000u |
                    (static_cast<uint32_t>(delta / 4) & 0x03ffffffu);
            }
            uint32_t branch = 0;
            if (!encode_b(code_pc(words - 1), spec->ret_addr, &branch)) {
                for (uint32_t i = 0; i < slot_count; ++i)
                    call_patch_release_near_entry(reinterpret_cast<uintptr_t>(slots[i]));
                return false;
            }
            code[words - 1] = branch;
        } else {
        const uint32_t b_lt_idx = spec->kind == 0 ? 5u : 7u;
        const uint32_t b_le_idx = spec->kind == 0 ? 9u : 11u;
        const uint32_t b_store_idx = spec->kind == 0 ? 11u : 13u;
        const uint32_t lt_target = spec->kind == 0 ? 12u : 14u;
        const uint32_t le_target = spec->kind == 0 ? 13u : 15u;
        uint32_t b_lt = 0, b_le = 0, b_store = 0, b_return = 0;
        if (!encode_bcond(code_pc(b_lt_idx), code_pc(lt_target), 0xb, &b_lt) ||
            !encode_bcond(code_pc(b_le_idx), code_pc(le_target), 0xd, &b_le) ||
            !encode_b(code_pc(b_store_idx), code_pc(le_target), &b_store) ||
            !encode_b(code_pc(words - 1), spec->ret_addr, &b_return)) {
            for (uint32_t i = 0; i < slot_count; ++i)
                call_patch_release_near_entry(reinterpret_cast<uintptr_t>(slots[i]));
            return false;
        }
        code[b_lt_idx] = b_lt; code[b_le_idx] = b_le; code[b_store_idx] = b_store;
        code[words - 1] = b_return;
        }
    }
    for (uint32_t i = 0; i < slot_count; ++i) {
        const uint32_t begin = i * 8;
        const uint32_t chunk_words = words > begin ? (words - begin < 8 ? words - begin : 8) : 0;
        if (chunk_words == 0) continue;
        std::memcpy(slots[i], code.data() + begin, chunk_words * sizeof(uint32_t));
        __builtin___clear_cache(reinterpret_cast<char*>(slots[i]),
                                reinterpret_cast<char*>(slots[i]) + chunk_words * 4);
    }
    if (!write_word(site, 0x94000000u | (branch_to_thunk & 0x03ffffffu))) {
        for (uint32_t i = 0; i < slot_count; ++i)
            call_patch_release_near_entry(reinterpret_cast<uintptr_t>(slots[i]));
        return false;
    }
    Entry entry{};
    entry.site = site;
    entry.original = original;
    entry.slot_count = slot_count;
    for (uint32_t i = 0; i < slot_count; ++i) entry.slots[i] = slots[i];
    g_entries[g_count++] = entry;
    *backup = reinterpret_cast<void*>(static_cast<uintptr_t>(original));
    return true;
}

bool insn_patch_release(uintptr_t site) {
    if (site == 0) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < g_count; ++i) {
        Entry& entry = g_entries[i];
        if (entry.site != site) continue;
        if (!write_word(site, entry.original)) return false;
        for (uint32_t s = 0; s < entry.slot_count; ++s)
            call_patch_release_near_entry(reinterpret_cast<uintptr_t>(entry.slots[s]));
        g_entries[i] = g_entries[--g_count];
        return true;
    }
    return false;
}

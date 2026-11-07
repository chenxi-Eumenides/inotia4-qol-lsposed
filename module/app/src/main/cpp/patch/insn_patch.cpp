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
    void* slots[3]{};
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
    std::array<uint32_t, 24> code{};
    uint32_t words = 0;
    if (!generate(*spec, code.data(), code.size(), &words)) return false;
    // 原指令校验：kind 0/1 为 MUL Wd,Wn,Wm = MADD(Wd,Wn,Wm,WZR)，基值 0x1b007c00（Ra=xzr）；
    // kind 3 首条为 smull x1,w21,w1（其真 64 位乘积由点 B 的 thunk 写入 x21）。
    const uint32_t expected_mul = 0x1b007c00u | (spec->b_reg << 16) |
                                  (spec->a_reg << 5) | spec->a_reg;
    const uint32_t expected_original = spec->kind == 3 ? 0x9b217ea1u : expected_mul;
    if (original != expected_original) return false;
    void* slots[3]{};
    const uint32_t slot_count = spec->kind == 1 ? 1u : (spec->kind == 3 ? 3u : 2u);
    for (uint32_t i = 0; i < slot_count; ++i) {
        slots[i] = call_patch_acquire_near_slot(site);
        if (slots[i] == nullptr) {
            for (uint32_t j = 0; j < i; ++j)
                call_patch_release_near_entry(reinterpret_cast<uintptr_t>(slots[j]));
            return false;
        }
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
    if (spec->kind == 0 || spec->kind == 3) {
        // kind 0：b_lt@5→12、b_le@9→13、b_store@11→13、b_return@words-1
        // kind 3：b_lt@7→14、b_le@11→15、b_store@13→15、b_return@words-1
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

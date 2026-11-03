#include "patch/short_hook.h"

#include <array>
#include <cstring>
#include <mutex>

#include "patch/call_patch.h"

namespace qol::patch {
namespace {

constexpr size_t kSlotCapacity = 32;  // 与 call_patch 的 thunk 槽一致
constexpr size_t kMaxEntries = 128;

struct ShortHookEntry {
    uintptr_t entry = 0;
    uint32_t original_word = 0;
    uintptr_t jump_slot = 0;
    uintptr_t trampoline_slot = 0;
};

std::array<ShortHookEntry, kMaxEntries> g_entries{};
size_t g_entry_count = 0;
std::mutex g_mutex;

bool is_b(uint32_t w) { return (w & 0xFC000000u) == 0x14000000u; }
bool is_bl(uint32_t w) { return (w & 0xFC000000u) == 0x94000000u; }
bool is_b_cond(uint32_t w) { return (w & 0xFF000010u) == 0x54000000u; }
bool is_cbz(uint32_t w) { return (w & 0x7E000000u) == 0x34000000u; }
bool is_tbz(uint32_t w) { return (w & 0x7E000000u) == 0x36000000u; }
bool is_adr(uint32_t w) { return (w & 0x9F000000u) == 0x10000000u; }
bool is_adrp(uint32_t w) { return (w & 0x9F000000u) == 0x90000000u; }
bool is_ldr_literal(uint32_t w) { return (w & 0x3B000000u) == 0x18000000u; }

bool pc_relative_unsupported(uint32_t w) {
    return is_bl(w) || is_b_cond(w) || is_adr(w) || is_adrp(w) || is_ldr_literal(w);
}

// adrp x16, page; add x16, x16, #off; br x16
size_t emit_absolute_jump(uint32_t* out, uintptr_t pc, uintptr_t target) {
    const int64_t page_delta = static_cast<int64_t>((target & ~uintptr_t{0xFFF}) -
                                                    (pc & ~uintptr_t{0xFFF}));
    const int64_t imm = page_delta >> 12;
    if (imm < -(int64_t{1} << 20) || imm >= (int64_t{1} << 20)) return 0;
    const uint32_t immlo = static_cast<uint32_t>(imm & 0x3);
    const uint32_t immhi = static_cast<uint32_t>((imm >> 2) & 0x7FFFF);
    out[0] = 0x90000010u | (immlo << 29) | (immhi << 5);             // adrp x16
    out[1] = 0x91000210u | (static_cast<uint32_t>(target & 0xFFF) << 10);  // add x16,x16,#off
    out[2] = 0xd61f0200u;                                            // br x16
    return 12;
}

// 相对 b（±128MB）。
uint32_t encode_b(uintptr_t from, uintptr_t to) {
    const int64_t delta = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    return 0x14000000u | (static_cast<uint32_t>(delta >> 2) & 0x03FFFFFFu);
}

uintptr_t decode_b_target(uintptr_t entry, uint32_t word) {
    int64_t offset = static_cast<int64_t>(word & 0x03FFFFFFu);
    if ((offset & 0x02000000) != 0) offset -= 0x04000000;
    return entry + static_cast<uintptr_t>(offset * 4);
}

// 条件分支改为「条件成立时跳过 12 字节绝对跳转」。
uint32_t skip_over_absolute_jump(uint32_t w) {
    if (is_cbz(w)) {
        // CBZ/CBNZ：bit24 是 op，imm19 在 bit23:5。
        return ((w ^ 0x01000000u) & ~0x00FFFFE0u) | (3u << 5);
    }
    // TBZ/TBNZ：imm14 在 bit18:5。
    return ((w ^ 0x01000000u) & ~0x0007FFE0u) | (3u << 5);
}

uintptr_t decode_cond_target(uintptr_t entry, uint32_t w) {
    if (is_cbz(w)) {
        int64_t imm = static_cast<int64_t>((w >> 5) & 0x7FFFF);
        if ((imm & 0x40000) != 0) imm -= 0x80000;
        return entry + static_cast<uintptr_t>(imm * 4);
    }
    int64_t imm = static_cast<int64_t>((w >> 5) & 0x3FFF);
    if ((imm & 0x2000) != 0) imm -= 0x4000;
    return entry + static_cast<uintptr_t>(imm * 4);
}

// 在 trampoline 写「重定位后的首条指令 + 跳回 entry+4」，返回写入字节数（0 = 不支持）。
size_t build_trampoline(uint32_t* out, uintptr_t trampoline_addr, uintptr_t entry,
                        uint32_t word) {
    size_t index = 0;
    if (is_b(word)) {
        return emit_absolute_jump(out, trampoline_addr, decode_b_target(entry, word));
    }
    if (is_cbz(word) || is_tbz(word)) {
        out[index++] = skip_over_absolute_jump(word);
        const uintptr_t target = decode_cond_target(entry, word);
        const size_t jump =
            emit_absolute_jump(out + index, trampoline_addr + index * 4, target);
        if (jump == 0) return 0;
        index += jump / 4;
        out[index++] = encode_b(trampoline_addr + index * 4, entry + 4);
        return index * 4;
    }
    if (pc_relative_unsupported(word)) return 0;
    // 非 PC 相对：原样复制后跳回入口+4。
    out[index++] = word;
    out[index++] = encode_b(trampoline_addr + index * 4, entry + 4);
    return index * 4;
}

ShortHookEntry* find_locked(uintptr_t entry) {
    for (size_t i = 0; i < g_entry_count; ++i) {
        if (g_entries[i].entry == entry) return &g_entries[i];
    }
    return nullptr;
}

}  // namespace

ShortHookResult short_hook_install(uintptr_t entry, void* handler) {
    if (entry == 0 || handler == nullptr || (entry & 0x3u) != 0) {
        return ShortHookResult{1, "invalid entry", nullptr};
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (find_locked(entry) != nullptr) {
        return ShortHookResult{1, "already installed", nullptr};
    }
    if (g_entry_count >= kMaxEntries) {
        return ShortHookResult{1, "capacity exhausted", nullptr};
    }

    const uint32_t word = *reinterpret_cast<const uint32_t*>(entry);
    if (!is_b(word) && pc_relative_unsupported(word)) {
        return ShortHookResult{1, "unsupported first instruction", nullptr};
    }

    void* jump_slot = call_patch_acquire_near_slot(entry);
    if (jump_slot == nullptr) return ShortHookResult{1, "near slot unavailable", nullptr};
    void* trampoline_slot = call_patch_acquire_near_slot(entry);
    if (trampoline_slot == nullptr) {
        call_patch_release_near_entry(reinterpret_cast<uintptr_t>(jump_slot));
        return ShortHookResult{1, "near slot unavailable", nullptr};
    }

    const uintptr_t jump_addr = reinterpret_cast<uintptr_t>(jump_slot);
    const uintptr_t trampoline_addr = reinterpret_cast<uintptr_t>(trampoline_slot);

    // jump slot: ldr x16, #8; br x16; .quad handler
    uint32_t jump_code[4] = {0x58000050u, 0xd61f0200u,
                             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handler)),
                             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handler) >> 32)};
    std::memcpy(jump_slot, jump_code, sizeof(jump_code));
    __builtin___clear_cache(reinterpret_cast<char*>(jump_slot),
                            reinterpret_cast<char*>(jump_slot) + sizeof(jump_code));

    uint32_t trampoline[kSlotCapacity / 4]{};
    const size_t trampoline_bytes =
        build_trampoline(trampoline, trampoline_addr, entry, word);
    if (trampoline_bytes == 0 || trampoline_bytes > kSlotCapacity) {
        call_patch_release_near_entry(jump_addr);
        call_patch_release_near_entry(trampoline_addr);
        return ShortHookResult{1, "relocation unsupported", nullptr};
    }
    std::memcpy(trampoline_slot, trampoline, trampoline_bytes);
    __builtin___clear_cache(reinterpret_cast<char*>(trampoline_slot),
                            reinterpret_cast<char*>(trampoline_slot) + trampoline_bytes);

    if (!call_patch_install_word(entry, word, encode_b(entry, jump_addr))) {
        call_patch_release_near_entry(jump_addr);
        call_patch_release_near_entry(trampoline_addr);
        return ShortHookResult{1, "patch write failed", nullptr};
    }

    g_entries[g_entry_count++] =
        ShortHookEntry{entry, word, jump_addr, trampoline_addr};
    return ShortHookResult{0, nullptr, trampoline_slot};
}

bool short_hook_contains(uintptr_t entry) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return find_locked(entry) != nullptr;
}

int32_t short_hook_install_adapter(uintptr_t entry, void* handler, void** backup) {
    const ShortHookResult result = short_hook_install(entry, handler);
    if (backup != nullptr) *backup = result.backup;
    return result.ok;
}

bool short_hook_remove(uintptr_t entry) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ShortHookEntry* record = find_locked(entry);
    if (record == nullptr) return false;
    if (!call_patch_revert_word(entry, record->original_word)) return false;
    call_patch_release_near_entry(record->jump_slot);
    call_patch_release_near_entry(record->trampoline_slot);
    *record = g_entries[--g_entry_count];
    return true;
}

}  // namespace qol::patch

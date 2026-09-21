#include "symbol_resolver.h"

#include <elf.h>
#include <cstring>

SymbolResolver g_resolver;

namespace {

// SVR4 SysV ELF hash（bionic 同款）
uint32_t elf_hash(const char* name) {
    uint32_t h = 0, g;
    while (*name != '\0') {
        h = (h << 4) + static_cast<uint8_t>(*name++);
        g = h & 0xf0000000u;
        if (g != 0) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

bool in_range(uintptr_t v, uintptr_t lo, uintptr_t hi) {
    return v >= lo && v < hi;
}

}  // namespace

void SymbolResolver::attach(uintptr_t base) {
    if (attached_) return;
    base_ = base;
    if (base == 0) return;

    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(base);
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 || ehdr->e_ident[EI_MAG3] != ELFMAG3 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        return;  // 非 64 位 ELF（本库现状）；32 位兼容留待需要时加 Elf32 分支
    }

    // load_bias：通用形式（本库首段 p_vaddr=0 故等于 base，写成通用形式防未来换库）
    uintptr_t bias = base;
    uintptr_t lo = ~0ull, hi = 0;
    const auto* ph = reinterpret_cast<const Elf64_Phdr*>(base + ehdr->e_phoff);
    bool have_load = false;
    bool have_dynamic = false;
    uintptr_t dyn_vaddr = 0;  // PT_DYNAMIC 虚拟地址（内存定位必须用 load_bias + p_vaddr）
    for (int i = 0; i < ehdr->e_phnum; ++i) {
        if (ph[i].p_type == PT_LOAD) {
            if (!have_load) {
                bias = base - ph[i].p_vaddr;
                have_load = true;
            }
            uintptr_t seg_lo = bias + ph[i].p_vaddr;
            uintptr_t seg_hi = seg_lo + ph[i].p_memsz;
            if (seg_lo < lo) lo = seg_lo;
            if (seg_hi > hi) hi = seg_hi;
        } else if (ph[i].p_type == PT_DYNAMIC) {
            dyn_vaddr = ph[i].p_vaddr;
            have_dynamic = true;
        }
    }
    if (!have_load || !have_dynamic || lo > hi) return;
    load_bias_ = bias;
    lo_ = lo;
    hi_ = hi;

    // dynamic 段：必须用 load_bias + p_vaddr 定位。本库第二 LOAD 段 p_offset != p_vaddr
    //（差 0x10000），用 base + p_offset 会指向野地址导致 SIGSEGV（v0.5.16 真机修复）。
    // d_ptr 本身是虚拟地址，后续 DT_SYMTAB/STRTAB/HASH/RELA 均用 load_bias + d_ptr 定位。
    const auto* dyn = reinterpret_cast<const Elf64_Dyn*>(bias + dyn_vaddr);
    bool have_sym = false, have_str = false, have_hash = false;
    for (; dyn->d_tag != DT_NULL; ++dyn) {
        switch (dyn->d_tag) {
            case DT_SYMTAB:
                symtab_ = reinterpret_cast<const uint8_t*>(bias + dyn->d_un.d_ptr);
                have_sym = true;
                break;
            case DT_STRTAB:
                strtab_ = reinterpret_cast<const char*>(bias + dyn->d_un.d_ptr);
                have_str = true;
                break;
            case DT_HASH:
                hash_bucket_ = reinterpret_cast<const uint32_t*>(bias + dyn->d_un.d_ptr);
                have_hash = true;
                break;
            case DT_SYMENT:
                sym_entsize_ = dyn->d_un.d_val;
                break;
            case DT_RELA:
                rela_ = reinterpret_cast<const void*>(bias + dyn->d_un.d_ptr);
                break;
            case DT_RELASZ:
                if (dyn->d_un.d_val > 0) rela_count_ = dyn->d_un.d_val;
                break;
            case DT_RELAENT:
                // 条目尺寸：非标准（Elf64_Rela=24B）时清空反查表（本库恒 24B）
                if (dyn->d_un.d_val != sizeof(Elf64_Rela)) rela_ = nullptr;
                break;
            default:
                break;
        }
    }
    if (!have_sym || !have_str || !have_hash) {
        symtab_ = nullptr;
        strtab_ = nullptr;
        hash_bucket_ = nullptr;
        rela_ = nullptr;
        rela_count_ = 0;
        return;
    }
    // SysV hash 头：nbucket, nchain, bucket[], chain[]
    hash_nbucket_ = hash_bucket_[0];
    hash_nchain_ = hash_bucket_[1];
    hash_chain_ = hash_bucket_ + 2 + hash_nbucket_;
    if (hash_nbucket_ == 0 || hash_nchain_ == 0) {
        symtab_ = nullptr;
        strtab_ = nullptr;
        hash_bucket_ = nullptr;
        rela_ = nullptr;
        rela_count_ = 0;
        return;
    }
    if (rela_ != nullptr && rela_count_ > 0) {
        rela_count_ /= sizeof(Elf64_Rela);  // DT_RELASZ 是字节数，转为条目数
    }
    attached_ = true;
}

uintptr_t SymbolResolver::resolveByName(const char* name) const {
    if (!attached_ || name == nullptr || name[0] == '\0') return 0;
    uint32_t h = elf_hash(name) % hash_nbucket_;
    for (uint32_t idx = hash_bucket_[2 + h]; idx != 0 && idx < hash_nchain_; idx = hash_chain_[idx]) {
        const auto* sym = reinterpret_cast<const Elf64_Sym*>(symtab_ + static_cast<uintptr_t>(idx) * sym_entsize_);
        if (ELF64_ST_TYPE(sym->st_info) == STT_TLS) continue;   // TLS 偏移不是 vaddr
        if (sym->st_shndx == SHN_UNDEF) continue;               // 外部导入，st_value 无意义
        const char* sn = strtab_ + sym->st_name;
        if (strcmp(sn, name) != 0) continue;
        uintptr_t v = load_bias_ + sym->st_value;
        if (in_range(v, lo_, hi_)) return v - base_;  // 返回相对基址偏移（防同名撞库范围校验）
    }
    return 0;
}

uintptr_t SymbolResolver::resolveSlot(uintptr_t slot_vma) const {
    if (!attached_ || rela_ == nullptr || rela_count_ == 0) return 0;
    const auto* rela = static_cast<const Elf64_Rela*>(rela_);
    // 线性扫描 .rela.dyn（约 3.7k 条，启动期一次，可接受）；按 r_offset 匹配 GOT 槽地址
    for (uint64_t i = 0; i < rela_count_; ++i) {
        if (ELF64_R_TYPE(rela[i].r_info) != R_AARCH64_RELATIVE) continue;
        if (rela[i].r_offset == slot_vma) {
            uintptr_t target = load_bias_ + static_cast<uintptr_t>(rela[i].r_addend);
            if (in_range(target, lo_, hi_)) return target - base_;
            return 0;
        }
    }
    return 0;
}

ResolvedSymbol SymbolResolver::resolve(const char* name, uintptr_t vma) const {
    ResolvedSymbol r;
    r.offset = vma;
    if (name == nullptr || name[0] == '\0') {
        // 无符号名（GOT 槽/表项）：先 RELATIVE 反查（P2），未命中回退 VMA
        uintptr_t slot = resolveSlot(vma);
        if (slot != 0) {
            r.offset = slot;
            r.source = SymbolSource::SLOT;
            ++slot_ok_;
        } else {
            r.source = SymbolSource::VMA;
            ++fallback_vma_;
        }
        r.has_name = false;
        return r;
    }
    r.has_name = true;
    uintptr_t dyn = resolveByName(name);
    if (dyn != 0) {
        r.offset = dyn;
        r.source = SymbolSource::ELF;
        ++elf_ok_;
    } else {
        r.source = SymbolSource::MISS;  // 有符号名但未命中/越界 → 回退 VMA
        ++missed_;
    }
    return r;
}

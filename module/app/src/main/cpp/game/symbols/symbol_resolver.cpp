#include "symbol_resolver.h"

#include <elf.h>
#include <cstring>
#include <limits>

SymbolResolver g_resolver;

namespace {

constexpr uint32_t kMaxProgramHeaders = 64;
constexpr uint32_t kMaxLoadSegments = 16;

uint32_t elf_hash(const char* name) {
    uint32_t hash = 0;
    while (*name != '\0') {
        hash = (hash << 4) + static_cast<uint8_t>(*name++);
        const uint32_t high = hash & 0xf0000000u;
        if (high != 0) hash ^= high >> 24;
        hash &= ~high;
    }
    return hash;
}

bool checked_add(uintptr_t left, uintptr_t right, uintptr_t* out) {
    if (right > std::numeric_limits<uintptr_t>::max() - left) return false;
    *out = left + right;
    return true;
}

bool checked_mul(size_t left, size_t right, size_t* out) {
    if (left != 0 && right > std::numeric_limits<size_t>::max() / left) return false;
    *out = left * right;
    return true;
}

}  // namespace

bool SymbolResolver::contains(uintptr_t address, size_t size, uint32_t flags) const {
    uintptr_t end = 0;
    if (!checked_add(address, size, &end)) return false;
    for (uint32_t i = 0; i < load_segment_count_; ++i) {
        const LoadSegment& segment = load_segments_[i];
        if ((segment.flags & flags) == flags && address >= segment.begin && end <= segment.end) {
            return true;
        }
    }
    return false;
}

void SymbolResolver::attach(uintptr_t base) {
    if (attached_) return;

    base_ = 0;
    symtab_ = nullptr;
    strtab_ = nullptr;
    hash_bucket_ = nullptr;
    hash_chain_ = nullptr;
    hash_nbucket_ = 0;
    hash_nchain_ = 0;
    sym_entsize_ = 0;
    strtab_size_ = 0;
    rela_ = nullptr;
    rela_count_ = 0;
    load_bias_ = 0;
    lo_ = 0;
    hi_ = 0;
    load_segment_count_ = 0;
    for (LoadSegment& segment : load_segments_) segment = LoadSegment{};
    if (base == 0) return;

    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(base);
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 || ehdr->e_ident[EI_MAG3] != ELFMAG3 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64 || ehdr->e_phentsize != sizeof(Elf64_Phdr) ||
        ehdr->e_phnum == 0 || ehdr->e_phnum > kMaxProgramHeaders) return;

    size_t phdr_size = 0;
    uintptr_t phdr_address = 0;
    if (!checked_mul(ehdr->e_phnum, sizeof(Elf64_Phdr), &phdr_size) ||
        !checked_add(base, ehdr->e_phoff, &phdr_address)) return;
    const auto* phdrs = reinterpret_cast<const Elf64_Phdr*>(phdr_address);

    bool have_load = false;
    bool have_dynamic = false;
    uintptr_t bias = 0;
    uintptr_t dynamic_address = 0;
    size_t dynamic_size = 0;
    uintptr_t lowest = std::numeric_limits<uintptr_t>::max();
    uintptr_t highest = 0;
    for (uint32_t i = 0; i < ehdr->e_phnum; ++i) {
        const Elf64_Phdr& phdr = phdrs[i];
        if (phdr.p_type == PT_LOAD) {
            if (load_segment_count_ >= kMaxLoadSegments || phdr.p_memsz > SIZE_MAX) return;
            if (!have_load) {
                if (phdr.p_vaddr > base) return;
                bias = base - static_cast<uintptr_t>(phdr.p_vaddr);
                have_load = true;
            }
            uintptr_t begin = 0;
            uintptr_t end = 0;
            if (!checked_add(bias, static_cast<uintptr_t>(phdr.p_vaddr), &begin) ||
                !checked_add(begin, static_cast<uintptr_t>(phdr.p_memsz), &end) || begin == end)
                return;
            // Loaded PT_LOAD addresses must agree with the single load bias.
            uintptr_t expected = 0;
            if (!checked_add(bias, static_cast<uintptr_t>(phdr.p_vaddr), &expected) ||
                expected != begin) return;
            load_segments_[load_segment_count_++] = LoadSegment{begin, end, phdr.p_flags};
            if (begin < lowest) lowest = begin;
            if (end > highest) highest = end;
        } else if (phdr.p_type == PT_DYNAMIC) {
            if (have_dynamic || phdr.p_memsz == 0 || phdr.p_memsz > SIZE_MAX) return;
            dynamic_address = phdr.p_vaddr;
            dynamic_size = static_cast<size_t>(phdr.p_memsz);
            have_dynamic = true;
        }
    }
    if (!have_load || !have_dynamic) return;
    load_bias_ = bias;
    lo_ = lowest;
    hi_ = highest;

    uintptr_t phdr_end = 0;
    if (!checked_add(phdr_address, phdr_size, &phdr_end) ||
        !contains(phdr_address, phdr_size, PF_R) ||
        !contains(base, sizeof(Elf64_Ehdr), PF_R)) return;
    (void)phdr_end;

    uintptr_t dynamic_runtime = 0;
    if (!checked_add(bias, dynamic_address, &dynamic_runtime) ||
        dynamic_size % sizeof(Elf64_Dyn) != 0 ||
        !contains(dynamic_runtime, dynamic_size, PF_R)) return;

    const auto dynamic_pointer = [this, bias](uint64_t value, uintptr_t* out) {
        const uintptr_t raw = static_cast<uintptr_t>(value);
        if (contains(raw, 1, PF_R)) {
            *out = raw;
            return true;
        }
        return checked_add(bias, raw, out) && contains(*out, 1, PF_R);
    };

    uintptr_t parsed_symtab = 0;
    uintptr_t parsed_strtab = 0;
    uintptr_t parsed_hash = 0;
    uintptr_t parsed_rela = 0;
    size_t parsed_strsz = 0;
    size_t parsed_relasz = 0;
    uint64_t parsed_syment = 0;
    uint64_t parsed_relaent = 0;
    bool have_symtab = false;
    bool have_strtab = false;
    bool have_strsz = false;
    bool have_hash = false;
    bool have_syment = false;
    bool have_rela = false;
    bool have_relasz = false;
    bool have_relaent = false;
    bool found_null = false;
    const auto* dynamic = reinterpret_cast<const Elf64_Dyn*>(dynamic_runtime);
    const size_t dynamic_count = dynamic_size / sizeof(Elf64_Dyn);
    for (size_t i = 0; i < dynamic_count; ++i) {
        const Elf64_Dyn& entry = dynamic[i];
        if (entry.d_tag == DT_NULL) {
            found_null = true;
            break;
        }
        switch (entry.d_tag) {
            case DT_SYMTAB:
                if (!dynamic_pointer(entry.d_un.d_ptr, &parsed_symtab)) return;
                have_symtab = true;
                break;
            case DT_STRTAB:
                if (!dynamic_pointer(entry.d_un.d_ptr, &parsed_strtab)) return;
                have_strtab = true;
                break;
            case DT_STRSZ:
                if (entry.d_un.d_val == 0 || entry.d_un.d_val > SIZE_MAX) return;
                parsed_strsz = static_cast<size_t>(entry.d_un.d_val);
                have_strsz = true;
                break;
            case DT_HASH:
                if (!dynamic_pointer(entry.d_un.d_ptr, &parsed_hash)) return;
                have_hash = true;
                break;
            case DT_SYMENT:
                parsed_syment = entry.d_un.d_val;
                have_syment = true;
                break;
            case DT_RELA:
                if (!dynamic_pointer(entry.d_un.d_ptr, &parsed_rela)) return;
                have_rela = true;
                break;
            case DT_RELASZ:
                if (entry.d_un.d_val > SIZE_MAX) return;
                parsed_relasz = static_cast<size_t>(entry.d_un.d_val);
                have_relasz = true;
                break;
            case DT_RELAENT:
                parsed_relaent = entry.d_un.d_val;
                have_relaent = true;
                break;
            default:
                break;
        }
    }
    if (!found_null || !have_symtab || !have_strtab || !have_strsz || !have_hash ||
        !have_syment || parsed_syment != sizeof(Elf64_Sym) || !have_relasz ||
        (parsed_relasz != 0 && (!have_rela || !have_relaent)) ||
        (have_relaent && parsed_relaent != sizeof(Elf64_Rela)) ||
        (parsed_relasz % sizeof(Elf64_Rela)) != 0 ||
        !contains(parsed_strtab, parsed_strsz, PF_R)) return;

    const auto* hash = reinterpret_cast<const uint32_t*>(parsed_hash);
    if (!contains(parsed_hash, 2 * sizeof(uint32_t), PF_R)) return;
    const uint32_t buckets = hash[0];
    const uint32_t chains = hash[1];
    if (buckets == 0 || chains == 0) return;
    size_t hash_words = 0;
    size_t hash_bytes = 0;
    if (static_cast<size_t>(buckets) > SIZE_MAX - 2 - static_cast<size_t>(chains)) return;
    hash_words = 2 + static_cast<size_t>(buckets) + static_cast<size_t>(chains);
    if (!checked_mul(hash_words, sizeof(uint32_t), &hash_bytes) ||
        !contains(parsed_hash, hash_bytes, PF_R)) return;

    size_t symtab_bytes = 0;
    if (!checked_mul(static_cast<size_t>(chains), sizeof(Elf64_Sym), &symtab_bytes) ||
        !contains(parsed_symtab, symtab_bytes, PF_R)) return;
    if (parsed_relasz != 0 && !contains(parsed_rela, parsed_relasz, PF_R)) return;

    const uint32_t* parsed_buckets = hash + 2;
    const uint32_t* parsed_chains = parsed_buckets + buckets;
    for (uint32_t i = 0; i < buckets; ++i) {
        if (parsed_buckets[i] >= chains) return;
    }
    for (uint32_t i = 0; i < chains; ++i) {
        if (parsed_chains[i] >= chains) return;
        const auto* symbol = reinterpret_cast<const Elf64_Sym*>(
            parsed_symtab + static_cast<uintptr_t>(i) * sizeof(Elf64_Sym));
        if (symbol->st_name >= parsed_strsz ||
            std::memchr(reinterpret_cast<const char*>(parsed_strtab + symbol->st_name), '\0',
                        parsed_strsz - symbol->st_name) == nullptr) return;
    }

    base_ = base;
    symtab_ = reinterpret_cast<const uint8_t*>(parsed_symtab);
    strtab_ = reinterpret_cast<const char*>(parsed_strtab);
    hash_bucket_ = hash;
    hash_chain_ = parsed_chains;
    hash_nbucket_ = buckets;
    hash_nchain_ = chains;
    sym_entsize_ = sizeof(Elf64_Sym);
    strtab_size_ = parsed_strsz;
    rela_ = parsed_relasz == 0 ? nullptr : reinterpret_cast<const void*>(parsed_rela);
    rela_count_ = parsed_relasz / sizeof(Elf64_Rela);
    attached_ = true;
}

uintptr_t SymbolResolver::resolveByName(const char* name) const {
    if (!attached_ || name == nullptr || name[0] == '\0') return 0;
    uint32_t index = hash_bucket_[2 + elf_hash(name) % hash_nbucket_];
    for (uint32_t steps = 0; index != 0 && index < hash_nchain_ && steps < hash_nchain_; ++steps) {
        const auto* symbol = reinterpret_cast<const Elf64_Sym*>(
            symtab_ + static_cast<uintptr_t>(index) * sym_entsize_);
        if (ELF64_ST_TYPE(symbol->st_info) != STT_TLS && symbol->st_shndx != SHN_UNDEF &&
            std::strcmp(strtab_ + symbol->st_name, name) == 0) {
            uintptr_t address = 0;
            if (checked_add(load_bias_, symbol->st_value, &address) && contains(address, 1, 0))
                return address - base_;
        }
        index = hash_chain_[index];
    }
    return 0;
}

uintptr_t SymbolResolver::resolveFunctionByName(const char* name) const {
    if (!attached_ || name == nullptr || name[0] == '\0') return 0;
    uint32_t index = hash_bucket_[2 + elf_hash(name) % hash_nbucket_];
    for (uint32_t steps = 0; index != 0 && index < hash_nchain_ && steps < hash_nchain_; ++steps) {
        const auto* symbol = reinterpret_cast<const Elf64_Sym*>(
            symtab_ + static_cast<uintptr_t>(index) * sym_entsize_);
        const unsigned char binding = ELF64_ST_BIND(symbol->st_info);
        const unsigned char visibility = symbol->st_other & 0x3u;
        if (symbol->st_shndx != SHN_UNDEF && ELF64_ST_TYPE(symbol->st_info) == STT_FUNC &&
            (binding == STB_GLOBAL || binding == STB_WEAK) &&
            (visibility == STV_DEFAULT || visibility == STV_PROTECTED) &&
            std::strcmp(strtab_ + symbol->st_name, name) == 0) {
            uintptr_t address = 0;
            if (checked_add(load_bias_, symbol->st_value, &address) &&
                contains(address, 1, PF_X)) return address - base_;
        }
        index = hash_chain_[index];
    }
    return 0;
}

uintptr_t SymbolResolver::resolveSlot(uintptr_t slot_vma) const {
    if (!attached_ || rela_ == nullptr || rela_count_ == 0) return 0;
    const auto* rela = static_cast<const Elf64_Rela*>(rela_);
    for (uint64_t i = 0; i < rela_count_; ++i) {
        if (ELF64_R_TYPE(rela[i].r_info) != R_AARCH64_RELATIVE) continue;
        if (rela[i].r_offset == slot_vma) {
            uintptr_t target = 0;
            if (checked_add(load_bias_, static_cast<uintptr_t>(rela[i].r_addend), &target) &&
                contains(target, 1, 0)) return target - base_;
            return 0;
        }
    }
    return 0;
}

ResolvedSymbol SymbolResolver::resolve(const char* name, uintptr_t vma) const {
    ResolvedSymbol result;
    result.offset = vma;
    if (name == nullptr || name[0] == '\0') {
        const uintptr_t slot = resolveSlot(vma);
        if (slot != 0) {
            result.offset = slot;
            result.source = SymbolSource::SLOT;
            ++slot_ok_;
        } else {
            result.source = SymbolSource::VMA;
            ++fallback_vma_;
        }
        result.has_name = false;
        return result;
    }
    result.has_name = true;
    const uintptr_t dynamic = resolveByName(name);
    if (dynamic != 0) {
        result.offset = dynamic;
        result.source = SymbolSource::ELF;
        ++elf_ok_;
    } else {
        result.source = SymbolSource::MISS;
        ++missed_;
    }
    return result;
}

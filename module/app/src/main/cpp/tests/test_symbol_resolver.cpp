#include "game/access/symbols.h"
#include "game/symbols/symbol_resolver.h"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <limits>

namespace {

enum class Malformation {
    kBadNameOffset,
    kCyclicChain,
    kOversizedHash,
    kNoNull,
    kBadSymEnt,
    kLocalFunction,
    kHiddenFunction,
    kIfunc,
};

struct alignas(16) ElfFixture {
    uint8_t bytes[4096]{};

    explicit ElfFixture(Malformation malformation) {
        constexpr size_t kDynamicOffset = 0x200;
        constexpr size_t kHashOffset = 0x400;
        constexpr size_t kSymbolOffset = 0x500;
        constexpr size_t kStringOffset = 0x600;
        constexpr size_t kRelaOffset = 0x700;
        auto* header = reinterpret_cast<Elf64_Ehdr*>(bytes);
        header->e_ident[EI_MAG0] = ELFMAG0;
        header->e_ident[EI_MAG1] = ELFMAG1;
        header->e_ident[EI_MAG2] = ELFMAG2;
        header->e_ident[EI_MAG3] = ELFMAG3;
        header->e_ident[EI_CLASS] = ELFCLASS64;
        header->e_phoff = sizeof(Elf64_Ehdr);
        header->e_phentsize = sizeof(Elf64_Phdr);
        header->e_phnum = 2;

        auto* program = reinterpret_cast<Elf64_Phdr*>(bytes + header->e_phoff);
        program[0].p_type = PT_LOAD;
        program[0].p_flags = PF_R | PF_X;
        program[0].p_memsz = sizeof(bytes);
        program[1].p_type = PT_DYNAMIC;
        program[1].p_flags = PF_R;
        program[1].p_vaddr = kDynamicOffset;
        program[1].p_memsz = 9 * sizeof(Elf64_Dyn);

        auto* dynamic = reinterpret_cast<Elf64_Dyn*>(bytes + kDynamicOffset);
        const auto set_pointer = [](Elf64_Dyn* entry, Elf64_Sxword tag, uint64_t value) {
            entry->d_tag = tag;
            entry->d_un.d_ptr = value;
        };
        set_pointer(&dynamic[0], DT_SYMTAB, kSymbolOffset);
        set_pointer(&dynamic[1], DT_STRTAB, kStringOffset);
        dynamic[2].d_tag = DT_STRSZ;
        dynamic[2].d_un.d_val = 16;
        set_pointer(&dynamic[3], DT_HASH, kHashOffset);
        dynamic[4].d_tag = DT_SYMENT;
        dynamic[4].d_un.d_val = malformation == Malformation::kBadSymEnt
                                    ? sizeof(Elf64_Sym) - 1
                                    : sizeof(Elf64_Sym);
        set_pointer(&dynamic[5], DT_RELA, kRelaOffset);
        dynamic[6].d_tag = DT_RELASZ;
        dynamic[6].d_un.d_val = 0;
        dynamic[7].d_tag = DT_RELAENT;
        dynamic[7].d_un.d_val = sizeof(Elf64_Rela);
        dynamic[8].d_tag = malformation == Malformation::kNoNull ? DT_DEBUG : DT_NULL;

        auto* hash = reinterpret_cast<uint32_t*>(bytes + kHashOffset);
        hash[0] = malformation == Malformation::kOversizedHash
                      ? std::numeric_limits<uint32_t>::max()
                      : 1;
        hash[1] = 2;
        hash[2] = 1;
        hash[3] = 0;
        hash[4] = malformation == Malformation::kCyclicChain ? 1 : 0;

        auto* symbols = reinterpret_cast<Elf64_Sym*>(bytes + kSymbolOffset);
        symbols[1].st_name = malformation == Malformation::kBadNameOffset ? 16 : 1;
        const unsigned char binding = malformation == Malformation::kLocalFunction
                                          ? STB_LOCAL
                                          : STB_GLOBAL;
        const unsigned char type = malformation == Malformation::kIfunc ? STT_GNU_IFUNC : STT_FUNC;
        symbols[1].st_info = ELF64_ST_INFO(binding, type);
        symbols[1].st_other = malformation == Malformation::kHiddenFunction ? STV_HIDDEN : STV_DEFAULT;
        symbols[1].st_shndx = 1;
        symbols[1].st_value = 0x900;
        std::memcpy(bytes + kStringOffset, "\0FixtureExport", 15);
    }
};

bool malformed_fixtures_are_rejected_or_bounded() {
    for (Malformation case_id : {Malformation::kBadNameOffset, Malformation::kOversizedHash,
                                 Malformation::kNoNull, Malformation::kBadSymEnt}) {
        ElfFixture fixture(case_id);
        SymbolResolver resolver;
        resolver.attach(reinterpret_cast<uintptr_t>(fixture.bytes));
        if (resolver.attached()) return false;
    }
    ElfFixture cyclic(Malformation::kCyclicChain);
    SymbolResolver resolver;
    resolver.attach(reinterpret_cast<uintptr_t>(cyclic.bytes));
    if (!resolver.attached() || resolver.resolveFunctionByName("NoSuchSymbol") != 0) return false;
    for (Malformation case_id : {Malformation::kLocalFunction, Malformation::kHiddenFunction,
                                 Malformation::kIfunc}) {
        ElfFixture fixture(case_id);
        SymbolResolver restricted;
        restricted.attach(reinterpret_cast<uintptr_t>(fixture.bytes));
        if (!restricted.attached() || restricted.resolveFunctionByName("FixtureExport") != 0)
            return false;
    }
    return true;
}

bool loaded_export_fixture_resolves() {
    void* library = dlopen(QOL_PLUGIN_EXPORT_FIXTURE, RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) return false;
    link_map* mapping = nullptr;
    if (dlinfo(library, RTLD_DI_LINKMAP, &mapping) != 0 || mapping == nullptr) return false;

    SymbolResolver resolver;
    resolver.attach(static_cast<uintptr_t>(mapping->l_addr));
    const char* symbol = qol::game::elf_export_name("elf-export:Fixture_Export_NotInAdapter");
    const char* rejected = qol::game::elf_export_name("elf-export:");
    const uintptr_t function = resolver.resolveFunctionByName(symbol);
    const uintptr_t data = resolver.resolveFunctionByName("Fixture_Data_NotFunction");
    const uintptr_t missing = resolver.resolveFunctionByName("Fixture_Missing");
    const ResolvedSymbol legacy_hit = resolver.resolve("Fixture_Export_NotInAdapter", 0x456);
    const ResolvedSymbol legacy_miss = resolver.resolve("Fixture_Missing", 0x789);
    const ResolvedSymbol legacy_slot = resolver.resolve(nullptr, 0x123);
    const bool passed = resolver.attached() && symbol != nullptr &&
                        std::strcmp(symbol, "Fixture_Export_NotInAdapter") == 0 &&
                        rejected != nullptr && rejected[0] == '\0' && function != 0 &&
                        data == 0 && missing == 0 && legacy_hit.source == SymbolSource::ELF &&
                        legacy_hit.offset == function && legacy_miss.source == SymbolSource::MISS &&
                        legacy_miss.offset == 0x789 && legacy_slot.source == SymbolSource::VMA &&
                        legacy_slot.offset == 0x123;
    dlclose(library);
    return passed;
}

}  // namespace

int main() {
    const bool normal = loaded_export_fixture_resolves();
    const bool malformed = malformed_fixtures_are_rejected_or_bounded();
    std::printf("symbol_resolver: %s\n", normal && malformed ? "pass" : "fail");
    return normal && malformed ? 0 : 1;
}

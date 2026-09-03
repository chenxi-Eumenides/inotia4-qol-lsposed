#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// ELF 内存符号解析器（v0.5.13，backlog P1 VMA 治理；v0.5.15 P2 RELATIVE 反查）：
// 从已加载 libgame.so 的 .dynsym 符号表按名字动态定位偏移，替代硬编码 VMA（换版本漂移免疫）。
// 不用 dlopen/dlsym：Android linker namespace 隔离下 dlopen 会加载独立副本；dlsym 有同名歧义。
// 已登记 VMA 作保底回退（GOT 槽/无名表项无符号名，恒走回退）。
// v0.5.15（P2）：无符号名 GOT 槽走 .rela.dyn RELATIVE 条目反查（r_offset=槽地址、r_addend=目标数据地址），
// 命中即得精确偏移；仅两者都失败才回退硬编码 VMA。

// 解析来源统计（g_symbol_report 用）
enum class SymbolSource : uint8_t {
    ELF = 0,   // 动态命中（.dynsym 找到且范围校验通过）
    SLOT = 1,  // RELATIVE 反查命中（无符号名 GOT 槽，.rela.dyn 命中）
    VMA = 2,   // 回退：无符号名且 RELATIVE 未命中（GOT 槽/表项）或名字未命中
    MISS = 3,  // 有符号名但解析失败（未命中/越界），已回退 VMA
};

struct ResolvedSymbol {
    uintptr_t offset = 0;   // 相对基址偏移（运行时地址 = g_base + offset）
    SymbolSource source = SymbolSource::VMA;
    bool has_name = false;  // 注册表声明了符号名（可动态化）？
};

class SymbolResolver {
public:
    // 解析 ELF 头/程序头/dynamic 段，构建 hash 表定位 + RELATIVE 反查表。幂等：重复 attach 直接复用。
    void attach(uintptr_t base);

    // 按符号名查偏移（SysV hash 查找，SHN_UNDEF/STT_TLS 过滤 + PT_LOAD 范围校验）。
    // 未 attach 或未命中返回 0。
    uintptr_t resolveByName(const char* name) const;

    // 无符号名 GOT 槽反查：.rela.dyn 中 R_AARCH64_RELATIVE 条目 r_offset==slot_vma 时返回
    // 其 r_addend（目标数据地址相对偏移）；未命中返回 0。
    uintptr_t resolveSlot(uintptr_t slot_vma) const;

    // 统一入口：声明了符号名 → 动态解析，未命中回退 vma 并标 MISS；
    // 无符号名（GOT 槽/表项）→ 先 RELATIVE 反查（标 SLOT），未命中回退 vma 标 VMA。
    ResolvedSymbol resolve(const char* name, uintptr_t vma) const;

    bool attached() const { return attached_; }
    int elfOk() const { return elf_ok_; }
    int slotOk() const { return slot_ok_; }
    int fallbackVma() const { return fallback_vma_; }
    int missed() const { return missed_; }

private:
    uintptr_t base_ = 0;
    const uint8_t* symtab_ = nullptr;   // DT_SYMTAB（load_bias + d_ptr）
    const char* strtab_ = nullptr;      // DT_STRTAB
    const uint32_t* hash_bucket_ = nullptr;  // SysV hash bucket
    const uint32_t* hash_chain_ = nullptr;
    uint32_t hash_nbucket_ = 0;
    uint32_t hash_nchain_ = 0;
    uint32_t sym_entsize_ = 24;         // DT_SYMENT（Elf64=24）
    const void* rela_ = nullptr;    // DT_RELA（RELATIVE 反查表，cpp 中转 Elf64_Rela*）
    uint64_t rela_count_ = 0;       // DT_RELASZ / DT_RELAENT（条目数）
    uintptr_t load_bias_ = 0;
    uintptr_t lo_ = 0, hi_ = 0;         // PT_LOAD 并集可读范围
    bool attached_ = false;

    mutable int elf_ok_ = 0, slot_ok_ = 0, fallback_vma_ = 0, missed_ = 0;
};

// 全局实例（game_access.cpp 使用）
extern SymbolResolver g_resolver;

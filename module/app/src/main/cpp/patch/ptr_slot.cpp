#include "patch/ptr_slot.h"

#include <sys/mman.h>

#include <cstdio>
#include <cstring>

#include "patch/claims.h"

namespace qol::patch {
namespace {

constexpr size_t kPageSize = 0x1000;

Outcome failure(const char* error) {
    return Outcome{1, error};
}

// 页的当前保护位（PROT_*）；地址不在任何映射内返回 -1。
int page_protection(uintptr_t page) {
    FILE* maps = fopen("/proc/self/maps", "r");
    if (maps == nullptr) return -1;
    char line[512];
    int protection = -1;
    while (fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long start = 0;
        unsigned long end = 0;
        char perms[8] = {};
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) continue;
        if (page < start || page >= end) continue;
        protection = 0;
        if (perms[0] == 'r') protection |= PROT_READ;
        if (perms[1] == 'w') protection |= PROT_WRITE;
        if (perms[2] == 'x') protection |= PROT_EXEC;
        break;
    }
    fclose(maps);
    return protection;
}

// 读槽当前值；槽地址由 game/access 解析（已做可读性检查），此处只做空地址防护。
void* slot_value(uintptr_t slot_addr) {
    return *reinterpret_cast<void**>(slot_addr);
}

}  // namespace

Outcome install_claim(const contracts::ResourceClaim& claim, const qol::game::PtrSlotSite& site) {
    const Primitives* table = primitives();
    if (table == nullptr) {
        return failure("primitives not set");
    }
    if (site.slot_addr == 0) {
        return failure("slot not found");
    }
    if (claim.handler == nullptr) {
        return failure("handler missing");
    }
    const void* current = slot_value(site.slot_addr);
    const char* owner = point_owner(site.slot_addr);
    if (claim.id != nullptr && owner != nullptr && std::strcmp(owner, claim.id) == 0) {
        // 本 claim 已占用：槽内仍为 handler → 幂等成功；否则视为被外部改写，fail-closed。
        return current == claim.handler ? Outcome{0, nullptr} : failure("slot value mismatch");
    }
    if (current != site.expected_value) {
        return failure("slot value mismatch");
    }
    if (!claim_point(site.slot_addr, claim.id)) {
        return failure("point already claimed");
    }
    if (table->write_ptr == nullptr || !table->write_ptr(site.slot_addr, claim.handler)) {
        // 未生效即撤销占用：否则失败安装会把槽位锁死给新所有者。
        release_point(site.slot_addr);
        return failure("primitive rejected");
    }
    return Outcome{0, nullptr};
}

Outcome release_claim(const contracts::ResourceClaim& claim, const qol::game::PtrSlotSite& site) {
    const Primitives* table = primitives();
    if (table == nullptr) {
        return failure("primitives not set");
    }
    if (site.slot_addr == 0) {
        return failure("slot not found");
    }
    const void* current = slot_value(site.slot_addr);
    if (current == site.expected_value) {
        // 槽已是原值：幂等归还。仅释放本 claim（或空闲）的占用，避免误删他 owner 的声明。
        const char* owner = point_owner(site.slot_addr);
        if (owner == nullptr || (claim.id != nullptr && std::strcmp(owner, claim.id) == 0)) {
            release_point(site.slot_addr);
        }
        return Outcome{0, nullptr};
    }
    if (claim.handler == nullptr || current != claim.handler) {
        // 槽值既非本 wrapper 也非原值：保留占用（§4.2 tombstone 语义），不冒险释放。
        return failure("slot value mismatch");
    }
    if (table->restore_ptr == nullptr ||
        !table->restore_ptr(site.slot_addr, site.expected_value)) {
        return failure("primitive rejected");
    }
    release_point(site.slot_addr);
    return Outcome{0, nullptr};
}

bool ptr_slot_write_raw(uintptr_t slot_addr, void* value) {
    if (slot_addr == 0) {
        return false;
    }
    const uintptr_t page = slot_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
    // 8 字节写必须落在同一页内：mprotect 只覆盖 slot_addr 所在页。
    if (slot_addr - page > kPageSize - sizeof(void*)) {
        return false;
    }
    const int original = page_protection(page);
    if (original < 0 || (original & PROT_READ) == 0) {
        return false;
    }
    // 指针槽不落在可执行页。写进代码页会把该页永久变成不可执行（原实现直接 mprotect 成
    // RW，丢掉 PROT_EXEC 且不还原，之后执行同页任何代码都会 SIGSEGV/SEGV_ACCERR），
    // 且槽址本就说明解析链把代码当成了控件数据，应 fail-closed 交给调用方拒绝。
    if ((original & PROT_EXEC) != 0) {
        return false;
    }
    const bool relaxed = (original & PROT_WRITE) == 0;
    if (relaxed &&
        mprotect(reinterpret_cast<void*>(page), kPageSize, original | PROT_WRITE) != 0) {
        return false;
    }
    *reinterpret_cast<void**>(slot_addr) = value;
    if (relaxed) {
        // 写穿 COW，值保留；只还原保护位，避免把只读页永久留成可写。
        (void)mprotect(reinterpret_cast<void*>(page), kPageSize, original);
    }
    return true;
}

bool ptr_slot_restore_raw(uintptr_t slot_addr, void* value) {
    return ptr_slot_write_raw(slot_addr, value);
}

}  // namespace qol::patch

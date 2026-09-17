#include "patch/ptr_slot.h"

#include <sys/mman.h>

#include <cstring>

#include "patch/claims.h"

namespace qol::patch {
namespace {

constexpr size_t kPageSize = 0x1000;

Outcome failure(const char* error) {
    return Outcome{1, error};
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
    if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE) != 0) {
        return false;
    }
    *reinterpret_cast<void**>(slot_addr) = value;
    return true;
}

bool ptr_slot_restore_raw(uintptr_t slot_addr, void* value) {
    return ptr_slot_write_raw(slot_addr, value);
}

}  // namespace qol::patch

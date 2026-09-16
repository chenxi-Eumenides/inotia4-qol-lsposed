#include "infra/capabilities.h"

#include <cstring>

namespace qol::infra {

namespace {

// 静态容量上限，避免动态分配。超限注册返回 false，调用方可感知并降级。
constexpr uint32_t kMaxCapabilities = 64;

// 直接复用冻结结构体作槽位；未注册槽位以 id == nullptr 标记。
CapabilityProvision g_slots[kMaxCapabilities];

// 按 id 查槽位；未注册返回 kMaxCapabilities。
uint32_t find_slot(CapabilityId id) {
    for (uint32_t i = 0; i < kMaxCapabilities; ++i) {
        // id 是编译期字符串常量：消费者与提供方各自持有同内容、不同地址的字面量，
        // 因此必须逐字节比较，比较指针会把合法查询误判为未注册。
        if (g_slots[i].id != nullptr && std::strcmp(g_slots[i].id, id) == 0) {
            return i;
        }
    }
    return kMaxCapabilities;
}

uint32_t used_count() {
    uint32_t count = 0;
    for (uint32_t i = 0; i < kMaxCapabilities; ++i) {
        if (g_slots[i].id != nullptr) {
            ++count;
        }
    }
    return count;
}

}  // namespace

bool register_capability(const CapabilityProvision& provision) {
    if (provision.id == nullptr || provision.iface == nullptr) {
        return false;
    }
    // 同一 id 在撤销前只允许一份实现，否则 require 结果取决于注册顺序。
    if (find_slot(provision.id) != kMaxCapabilities) {
        return false;
    }
    for (uint32_t i = 0; i < kMaxCapabilities; ++i) {
        if (g_slots[i].id == nullptr) {
            g_slots[i] = provision;
            return true;
        }
    }
    return false;
}

bool unregister_capability(CapabilityId id) {
    if (id == nullptr) {
        return false;
    }
    const uint32_t slot = find_slot(id);
    if (slot == kMaxCapabilities) {
        return false;
    }
    g_slots[slot] = CapabilityProvision{};
    return true;
}

const void* require_capability(CapabilityId id, QolVersionRange required) {
    if (id == nullptr) {
        return nullptr;
    }
    const uint32_t slot = find_slot(id);
    if (slot == kMaxCapabilities) {
        return nullptr;
    }
    // 版本不兼容与未注册同样返回 nullptr：调用方只走降级路径，不区分原因。
    if (!contracts::satisfies(g_slots[slot].version, required)) {
        return nullptr;
    }
    return g_slots[slot].iface;
}

uint32_t capability_count() { return used_count(); }

// 模块级重置：只清本注册表，供隔离断言使用；聚合重置见 infra/test_reset.cpp。
void capabilities_reset_for_test() {
    for (uint32_t i = 0; i < kMaxCapabilities; ++i) {
        g_slots[i] = CapabilityProvision{};
    }
}

}  // namespace qol::infra

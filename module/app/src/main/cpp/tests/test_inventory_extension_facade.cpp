// ITM-13：中立物品域扩展 provider facade host 单测。
// 覆盖 set-once 驻留、无 provider not-ready、resident/available 分离（gate off 仍命中）、
// 成员缺失拒绝与错误传播。
#include "host/inventory_extension_facade.h"

#include <cassert>
#include <cstdint>
#include <cstring>

#include "contracts/inventory_extension.h"

namespace {

int g_probe_calls = 0;
bool g_probe = false;
int32_t g_owns_calls = 0;

bool probe() {
    ++g_probe_calls;
    return g_probe;
}

int32_t owns_bag(int32_t bag) {
    ++g_owns_calls;
    return bag == 6 ? 1 : 0;
}

int32_t missing_move(int32_t, int32_t, int32_t, int32_t, char*, uint32_t, uint32_t*) {
    return 0;
}

const qol::contracts::QolInventoryExtensionProvider kProvider{
    &owns_bag, nullptr, nullptr, nullptr, nullptr, &missing_move, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr,
};

}  // namespace

int main() {
    using qol::contracts::QolInventoryExtensionAccess;

    // 无 provider 时清晰 not-ready（不冒充默认值）。
    assert(!qol::host::host_inventory_extension_resident());
    assert(!qol::host::host_inventory_extension_available());
    QolInventoryExtensionAccess access{};
    assert(qol::host::host_inventory_extension_get_access(&access).code == kQolErrNotReady);

    // 未绑定宿主前注册失败；绑定后成功（set-once）。
    assert(!qol::host::host_inventory_extension_set_provider(&kProvider, &probe));
    qol::host::host_inventory_extension_bind();
    assert(qol::host::host_inventory_extension_set_provider(&kProvider, &probe));

    // 注册后 resident=true；probe 与 available 联动（gate off 仍 resident、available=false）。
    assert(qol::host::host_inventory_extension_resident());
    assert(!qol::host::host_inventory_extension_available());
    assert(qol::host::host_inventory_extension_get_access(&access).ok == 0);
    assert(access.resident == 1 && access.available == 0 && access.provider == &kProvider);

    g_probe = true;
    assert(qol::host::host_inventory_extension_available());
    assert(qol::host::host_inventory_extension_get_access(&access).ok == 0);
    assert(access.resident == 1 && access.available == 1);

    // provider 内部谓词可达；get_access 不越权调用。
    const int owns_before = g_owns_calls;
    assert(access.provider->owns_bag(6) == 1 && access.provider->owns_bag(7) == 0);
    assert(g_owns_calls == owns_before + 2);

    // 成员缺失不伪装成功：move 回调自行返回 0（facade 只转发）。
    char buffer[8]{};
    uint32_t size = 0;
    assert(access.provider->move_item(6, 0, 7, 0, buffer, sizeof(buffer), &size) == 0);

    // set-once：重复注册同名表幂等；换表被拒绝。
    assert(qol::host::host_inventory_extension_set_provider(&kProvider, &probe));
    const qol::contracts::QolInventoryExtensionProvider other{};
    assert(!qol::host::host_inventory_extension_set_provider(&other, &probe));
    assert(qol::host::host_inventory_extension_get_access(nullptr).code ==
           kQolErrInvalidArgument);
    assert(qol::host::host_inventory_extension_provider_if_resident() == &kProvider);
    return 0;
}

#include "plugins/stack_limit/plugin.h"

#include <cassert>
#include <cstdint>
#include <cstring>

#include "contracts/result.h"
#include "contracts/stack_limit.h"
#include "host/item_count_facade.h"

uintptr_t g_base = 0;

uintptr_t fn_resolve(const char*, uintptr_t vma) { return vma; }

namespace {

bool g_effective = false;
bool g_gate = false;

QolResult config_get_bool(const char*, int32_t default_value, int32_t* out_value) {
    *out_value = g_gate ? 1 : default_value;
    return qol::contracts::ok();
}

const qol::contracts::QolHostSdk kSdk = {
    {sizeof(qol::contracts::QolHostSdk), qol::contracts::kHostSdkMajor,
     qol::contracts::kHostSdkMinor},
    {},
    {&config_get_bool, nullptr, nullptr, nullptr},
};

const qol::contracts::QolGameHost kGame = {
    {sizeof(qol::contracts::QolGameHost), qol::contracts::kGameHostMajor,
     qol::contracts::kGameHostMinor},
};

}  // namespace

bool stack_limit_enabled() { return g_effective; }
void stack_limit_set_effective(bool enabled) { g_effective = enabled; }

int main() {
    const qol::contracts::PluginDescriptor& descriptor =
        qol::features::stack_limit::plugin_descriptor();
    assert(std::strcmp(descriptor.id, "stack_limit") == 0);
    assert(descriptor.resources_count == 18);

    static const char* kIds[] = {
        "P_INVEN_MOVE_ITEM_CLAMP_150",
        "P_INVEN_MOVE_ITEM_CLAMP_158",
        "P_INVEN_SAVE_ITEM_DIRECT_CLAMP_DC",
        "P_INVEN_SAVE_ITEM_DIRECT_CLAMP_F0",
        "P_INVEN_SAVE_ITEM_DATA_CLAMP_7C",
        "P_INVEN_FIND_SAVE_SLOT_CLAMP_238",
        "P_SAVE_SAVE_INVENTORY_BITEND",
        "P_SAVE_LOAD_INVENTORY_BITEND",
        "P_UISTORE_BUTTON_SELL_EXE_D18D0_ARG",
        "P_UISTORE_BUTTON_SELL_EXE_D18D0_CALL",
        "P_UISTORE_BUTTON_SELL_EXE_D18E8_ARG",
        "P_UISTORE_BUTTON_SELL_EXE_D18E8_CALL",
        "P_UISTORE_SELL_ITEM_D26B8_ARG",
        "P_UISTORE_SELL_ITEM_D26B8_CALL",
        "P_ITEMSYSTEM_DIVIDE_108458_ARG",
        "P_ITEMSYSTEM_DIVIDE_108458_CALL",
        "P_ITEMSYSTEM_DIVIDE_10848C_ARG",
        "P_ITEMSYSTEM_DIVIDE_10848C_CALL",
    };
    for (uint32_t i = 0; i < descriptor.resources_count; ++i) {
        const qol::contracts::ResourceClaim& claim = descriptor.resources[i];
        assert(std::strcmp(claim.id, kIds[i]) == 0);
        assert(claim.mechanism == qol::contracts::ResourceMechanism::kCallPatch);
        assert(claim.requirement == qol::contracts::ResourceRequirement::kRequired);
        assert(claim.failure == qol::contracts::ResourceFailure::kFailClosed);
        assert(claim.resumable);
        assert(claim.atomic);
        assert(std::strcmp(claim.owner, "stack_limit") == 0);
        assert(std::strcmp(claim.gate, "plugin.stack_limit.enabled") == 0);
    }

    qol::contracts::PluginContext context{
        sizeof(qol::contracts::PluginContext),
        qol::contracts::kHostSdkMajor,
        qol::contracts::kHostSdkMinor,
        &kSdk,
        &kGame,
        "stack_limit",
    };
    assert(descriptor.install(context) == 0);
    uint32_t resident_count = 41;
    assert(qol::host::host_item_count_api().canonical(reinterpret_cast<void*>(1), &resident_count).code ==
           kQolErrUnsupported);
    assert(resident_count == 41);
    descriptor.on_enabled(context);
    assert(stack_limit_enabled());

    uint32_t count = 41;
    assert(qol::host::host_item_count_api().canonical(reinterpret_cast<void*>(1), &count).code ==
           kQolErrUnsupported);
    assert(count == 41);
    auto encoding = qol::contracts::QolItemCountEncoding::kEncoded;
    assert(qol::host::host_item_count_api().is_encoded(reinterpret_cast<void*>(1), &encoding).ok ==
           0);
    assert(encoding == qol::contracts::QolItemCountEncoding::kUnknown);
    assert(qol::host::host_item_count_api().write_effective(reinterpret_cast<void*>(1), 1).code ==
           kQolErrUnsupported);

    descriptor.on_disabled(context);
    descriptor.on_disabled(context);
    assert(!stack_limit_enabled());
    g_gate = true;
    count = 41;
    assert(qol::host::host_item_count_api().canonical(reinterpret_cast<void*>(1), &count).code ==
           kQolErrUnsupported);
    assert(count == 41);
    assert(qol::host::host_item_count_api().effective(reinterpret_cast<void*>(1), &count).code ==
           kQolErrNotReady);
    int32_t needed = 41;
    assert(qol::host::host_item_count_api().writeback_needed(reinterpret_cast<void*>(1), 1, &needed).code ==
           kQolErrNotReady);
    assert(needed == 41);
    g_gate = false;
    descriptor.deactivate(context);
    count = 41;
    assert(qol::host::host_item_count_api().canonical(reinterpret_cast<void*>(1), &count).code ==
           kQolErrNotReady);
    assert(count == 41);
    return 0;
}

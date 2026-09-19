#include "plugins/stack_limit/plugin.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "contracts/result.h"
#include "host/item_count_facade.h"

uintptr_t g_base = 0;

uintptr_t fn_resolve(const char*, uintptr_t vma) { return vma; }

namespace {

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

void assert_pure_conversion_api() {
    const qol::contracts::QolItemCountApi& api = qol::host::host_item_count_api();
    constexpr uint32_t kMaskA = 0x7u << 22;
    constexpr uint32_t kMaskB = 0x7fu << 25;
    const uint32_t field = 0x00123456u | (5u << 22) | (77u << 25);
    const uint32_t canonical_values[] = {99, 100, 127, 128, 199, 999};

    uint32_t canonical = 0;
    assert(api.canonical_from_field(field, &canonical).ok == 0);
    assert(canonical == 5u * 128u + 77u);

    for (uint32_t value : canonical_values) {
        for (int32_t extended : {0, 1, -1}) {
            uint32_t converted = 0;
            assert(api.field_from_canonical(field, value, extended, &converted).ok == 0);
            const uint32_t view = extended == 0 ? (value % 128u > 99u ? 99u : value % 128u)
                                                : (value > 999u ? 999u : value);
            const uint32_t expected = extended == 0
                                          ? (field & ~kMaskB) | ((view & 0x7fu) << 25)
                                          : (field & ~(kMaskA | kMaskB)) |
                                                (((view >> 7) & 0x7u) << 22) |
                                                ((view & 0x7fu) << 25);
            assert(converted == expected);
            if (extended == 0) assert((converted & ~kMaskB) == (field & ~kMaskB));

            uint32_t round_view = 0;
            assert(api.view_from_canonical(value, extended, &round_view).ok == 0);
            assert(round_view == view);
        }
    }

    uint32_t limit = 0;
    assert(api.view_limit(0, &limit).ok == 0);
    assert(limit == 99);
    assert(api.view_limit(1, &limit).ok == 0);
    assert(limit == 999);

    uint32_t untouched = 0xabcdef01u;
    assert(api.field_from_canonical(field, 99, 0, nullptr).code == kQolErrInvalidArgument);
    assert(api.canonical_from_field(field, nullptr).code == kQolErrInvalidArgument);
    assert(api.view_from_canonical(99, 0, nullptr).code == kQolErrInvalidArgument);
    assert(api.view_limit(0, nullptr).code == kQolErrInvalidArgument);
    assert(untouched == 0xabcdef01u);
}

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
    assert_pure_conversion_api();
    uint32_t resident_count = 41;
    assert(qol::host::host_item_count_api().canonical(reinterpret_cast<void*>(1), &resident_count).code ==
           kQolErrUnsupported);
    assert(resident_count == 41);
    descriptor.on_enabled(context);
    qol::contracts::PluginStatusOut status{};
    descriptor.fill_status(status);
    assert(status.enabled);

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
    status = {};
    descriptor.fill_status(status);
    assert(!status.enabled);
    g_gate = true;
    assert_pure_conversion_api();
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
    assert(qol::host::host_item_count_api().effective(reinterpret_cast<void*>(1), &count).code ==
           kQolErrNotReady);
    assert(qol::host::host_item_count_api().write_effective(reinterpret_cast<void*>(1), 1).code ==
           kQolErrNotReady);
    g_gate = false;
    descriptor.deactivate(context);
    count = 41;
    assert(qol::host::host_item_count_api().canonical(reinterpret_cast<void*>(1), &count).code ==
           kQolErrNotReady);
    assert(count == 41);
    uint32_t field = 41;
    assert(qol::host::host_item_count_api().field_from_canonical(0, 99, 0, &field).code ==
           kQolErrNotReady);
    assert(field == 41);
    return 0;
}

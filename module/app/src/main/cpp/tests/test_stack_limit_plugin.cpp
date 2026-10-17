// stack_limit 清单与生命周期边界：16 条无 gate 原子资源、单个 default-false feature、
// install/deactivate 只做自身初始化；S2 codec 与扩展模式策略归 Host 常驻 adapter。

#include "plugins/stack_limit/plugin.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>

#include "contracts/result.h"
#include "contracts/item_count.h"
#include "game/adapter_ports/item_count_facade.h"
#include "game/ops/game_ops_common.h"
#include "game/state/game_state.h"
#include "host/item_count_facade.h"

// game/access/patches.cpp 与 game/read/item_class.cpp 的链接依赖：本测试不进入编码路径。
uintptr_t g_base = 0;
bool game_in_world() { return false; }
std::string op_err(const char* msg) {
    return std::string("{\"ok\":false,\"error\":\"") + msg + "\"}";
}
bool inventory_item_ref_at(int, int, InventoryItemRef*) { return false; }
uintptr_t fn_resolve(const char*, uintptr_t vma) { return vma; }

namespace {

const qol::contracts::QolHostSdk kSdk = {
    {sizeof(qol::contracts::QolHostSdk), qol::contracts::kHostSdkMajor,
     qol::contracts::kHostSdkMinor},
    {},
    {},
};

const qol::contracts::QolGameHost kGame = {
    {sizeof(qol::contracts::QolGameHost), qol::contracts::kGameHostMajor,
     qol::contracts::kGameHostMinor},
};

}  // namespace

int main() {
    using qol::contracts::FeatureType;
    using qol::contracts::ResourceMechanism;
    using qol::contracts::ResourceRequirement;
    using qol::contracts::ResourceFailure;
    using qol::game::ItemCountExtendedMode;

    const qol::contracts::PluginDescriptor& descriptor =
        qol::features::stack_limit::plugin_descriptor();
    assert(std::strcmp(descriptor.id, "stack_limit") == 0);
    assert(descriptor.resources_count == 16);
    assert(descriptor.provides_count == 0 && descriptor.provides == nullptr);
    assert(descriptor.feature_count == 1 && descriptor.features != nullptr);
    assert(std::strcmp(descriptor.features[0].id, "stack_limit.limit_999") == 0);
    assert(descriptor.features[0].default_enabled == 0);
    assert(descriptor.features[0].type == FeatureType::kToggle);

    static const char* kIds[] = {
        "P_INVEN_MOVE_ITEM_CLAMP_150",
        "P_INVEN_MOVE_ITEM_CLAMP_158",
        "P_INVEN_SAVE_ITEM_DIRECT_CLAMP_DC",
        "P_INVEN_SAVE_ITEM_DIRECT_CLAMP_F0",
        "P_INVEN_SAVE_ITEM_DATA_CLAMP_7C",
        "P_INVEN_FIND_SAVE_SLOT_CLAMP_238",
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
    static const char* kCallArgPairs[][2] = {
        {"P_UISTORE_BUTTON_SELL_EXE_D18D0_CALL", "P_UISTORE_BUTTON_SELL_EXE_D18D0_ARG"},
        {"P_UISTORE_BUTTON_SELL_EXE_D18E8_CALL", "P_UISTORE_BUTTON_SELL_EXE_D18E8_ARG"},
        {"P_UISTORE_SELL_ITEM_D26B8_CALL", "P_UISTORE_SELL_ITEM_D26B8_ARG"},
        {"P_ITEMSYSTEM_DIVIDE_108458_CALL", "P_ITEMSYSTEM_DIVIDE_108458_ARG"},
        {"P_ITEMSYSTEM_DIVIDE_10848C_CALL", "P_ITEMSYSTEM_DIVIDE_10848C_ARG"},
    };
    for (uint32_t pair = 0; pair < sizeof(kCallArgPairs) / sizeof(kCallArgPairs[0]); ++pair) {
        bool has_call = false;
        bool has_arg = false;
        for (uint32_t i = 0; i < descriptor.resources_count; ++i) {
            const char* id = descriptor.resources[i].id;
            has_call = has_call || std::strcmp(id, kCallArgPairs[pair][0]) == 0;
            has_arg = has_arg || std::strcmp(id, kCallArgPairs[pair][1]) == 0;
        }
        assert(has_call && has_arg);
    }
    for (uint32_t i = 0; i < descriptor.resources_count; ++i) {
        const qol::contracts::ResourceClaim& claim = descriptor.resources[i];
        assert(std::strstr(claim.id, "BITEND") == nullptr);
        assert(std::strcmp(claim.id, kIds[i]) == 0);
        assert(claim.mechanism == ResourceMechanism::kCallPatch);
        assert(claim.policy == qol::contracts::ResourcePolicy::kPrivate);
        assert(claim.requirement == ResourceRequirement::kRequired);
        assert(claim.failure == ResourceFailure::kFailClosed);
        assert(claim.resumable);
        assert(claim.atomic);
        assert(std::strcmp(claim.owner, "stack_limit") == 0);
        // 旧跨 owner 门禁键已退役：资源由 feature 生命周期整组安装。
        assert(claim.gate == nullptr);
    }

    qol::contracts::PluginContext context{
        sizeof(qol::contracts::PluginContext),
        qol::contracts::kHostSdkMajor,
        qol::contracts::kHostSdkMinor,
        &kSdk,
        &kGame,
        "stack_limit",
    };

    // install/deactivate 不再注册 provider/probe；facade 恒为宿主常驻实现。
    assert(descriptor.install(context) == 0);
    qol::contracts::PluginStatusOut status{};
    descriptor.fill_status(status);
    assert(!status.enabled);
    descriptor.on_enabled(context);
    descriptor.fill_status(status);
    assert(status.enabled);
    assert(qol::game::item_count_api_if_resident() != nullptr);

    qol::game::item_count_set_extended_mode(ItemCountExtendedMode::kNotReady);
    uint8_t extended = 3;
    assert(qol::host::host_item_count_api().read_extended_mode(&extended).code == kQolErrNotReady);
    qol::game::item_count_set_extended_mode(ItemCountExtendedMode::kOff);
    assert(qol::host::host_item_count_api().read_extended_mode(&extended).ok == 0 && extended == 0);

    // 纯转换不依赖游戏对象仍在部署。
    uint32_t field = 0;
    assert(qol::host::host_item_count_api().field_from_canonical(0, 99, 0, &field).ok == 0);
    uint32_t limit = 0;
    assert(qol::host::host_item_count_api().view_limit(1, &limit).ok == 0 && limit == 999);

    descriptor.on_disabled(context);
    status = {};
    descriptor.fill_status(status);
    assert(!status.enabled);
    descriptor.deactivate(context);
    status = {};
    descriptor.fill_status(status);
    assert(!status.enabled);
    return 0;
}

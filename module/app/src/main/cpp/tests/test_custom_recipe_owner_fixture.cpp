// custom_recipe 单 owner 的**真实 descriptor + production feature-mode 装配** fixture。
//
// 目标（L4）：用生产的 plugins/custom_recipe/plugin.cpp 描述符驱动真实 feature registry /
// 资源事务，验证：
//   · 单 descriptor：13 条资源（12 UI hook + ownership 最后）、2 个写端口、0 个 provides、
//     1 个可见 feature（custom_recipe.recipes，默认 false）；
//   · 5 条核心 hook 逐个安装失败 → 逆序回滚、注册净额归零、表未发布、业务门禁保持关闭；
//   · 7 条装饰 hook 逐个失败 → 核心继续、失败资源保持 Free（fail-soft）、不回滚整插件；
//   · ownership acquire 失败 → 原表不发布、门禁保持关闭；
//   · off/on：清理订阅与 typed handler、还原表、门禁关闭，重新启用可再次就绪。
//
// 该 fixture 不替代 L3 的通用机制用例（真实 registry 动态循环）；这里绑定的是产品描述符。

#include "host/features/reconciler.h"
#include "host/core_settings_config_service.h"
#include "host/owner_identity.h"
#include "host/plugin_registry.h"
#include "host/save_route.h"
#include "infra/capabilities.h"
#include "infra/status.h"
#include "infra/store.h"
#include "hooks/registry.h"
#include "hooks/transaction.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "contracts/attribute_quality.h"
#include "contracts/craft_facts.h"
#include "contracts/craft_commands.h"
#include "contracts/game_host.h"
#include "contracts/game_ports/craft_ui_write_port.h"
#include "contracts/game_ports/custom_recipe_write_port.h"
#include "contracts/item_facts.h"
#include "contracts/plugin.h"
#include "contracts/text_layer.h"
#include "plugins/custom_recipe/feature.h"
#include "plugins/custom_recipe/plugin.h"
#include "plugins/custom_recipe/table.h"
#include "plugins/custom_recipe/ui/runtime.h"

namespace qol::host {
// 宿主运行时效果投影：fixture 只记录最近一次 actual。
std::string g_actual_plugin;
bool g_actual_value = false;
void runtime_effects_publish_actual(const char* plugin_id, bool enabled) {
    if (plugin_id != nullptr) {
        g_actual_plugin = plugin_id;
        g_actual_value = enabled;
    }
}
void runtime_effects_unsubscribe_plugin(const char*) {}

// test_game_port_stubs.cpp 的默认端口为空；本 fixture 注入「就绪」的生产形状端口。
int fixture_typed_registrations();
int fixture_typed_unregistrations();
int fixture_event_registrations();
int fixture_event_unregistrations();
}  // namespace qol::host

namespace qol::gametest {
// test_game_port_stubs.cpp 提供的端口注入点（未注入时保持空桩）。
void set_craft_ui_write_port_for_test(const qol::contracts::QolCraftUiWritePort* port);
void set_custom_recipe_write_port_for_test(const qol::contracts::QolCustomRecipeWritePort* port);
}  // namespace qol::gametest

namespace {

using namespace qol::contracts;
using qol::host::FeatureController;
using qol::host::FeatureReconciler;
using qol::host::HostDeps;

// ===========================================================================
// 表注入替身（host 上 table.cpp 只编译纯构造器）
// ===========================================================================
bool g_table_ensure_result = true;
int g_table_ensure_calls = 0;
int g_table_deactivate_calls = 0;
bool g_table_published = false;

// ===========================================================================
// 能力替身（provider 插件的 provides 目标）
// ===========================================================================
int g_text_subscribe_calls = 0;
int g_text_unsubscribe_calls = 0;
uint64_t g_last_text_consumer = 0;
const char* (*g_last_text_callback)(uint32_t, const char*, void*) = nullptr;

uint64_t fake_text_subscribe(int32_t, int32_t, const char* (*fn)(uint32_t, const char*, void*),
                             void*) {
    ++g_text_subscribe_calls;
    g_last_text_callback = fn;
    g_last_text_consumer = 900u + static_cast<uint64_t>(g_text_subscribe_calls);
    return g_last_text_consumer;
}
void fake_text_unsubscribe(uint64_t) { ++g_text_unsubscribe_calls; }
const TextLayerApi kTextLayerApi{&fake_text_subscribe, &fake_text_unsubscribe};

QolResult fake_category(QolItemHandle, int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out = 0;
    return qol::contracts::ok();
}
QolResult fake_is_real_equip(QolItemHandle, int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out = 1;
    return qol::contracts::ok();
}
QolResult fake_is_jewel_category(int32_t, int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out = 0;
    return qol::contracts::ok();
}
const ItemFactsApi kItemFactsApi{sizeof(ItemFactsApi), &fake_category, &fake_is_real_equip,
                                 &fake_is_jewel_category, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

const CraftFactsApi kCraftFactsApi{};

QolResult fake_query_item(QolItemHandle, const AttributeQualityOptions*,
                          AttributeQualityItemResult*) {
    return qol::contracts::error(kQolErrNotReady, nullptr, 0);
}
QolResult fake_query_option(QolItemHandle, int32_t, int32_t, const AttributeQualityOptions*,
                            AttributeQualityItem*) {
    return qol::contracts::error(kQolErrNotReady, nullptr, 0);
}
QolResult fake_evaluate(int32_t, AttributeRange, const AttributeQualityOptions*,
                        AttributeQuality*) {
    return qol::contracts::error(kQolErrNotReady, nullptr, 0);
}
const AttributeQualityApi kAttributeQualityApi{
    static_cast<uint32_t>(sizeof(AttributeQualityApi)), &fake_query_item, &fake_query_option,
    &fake_evaluate};

// ===========================================================================
// 两个写端口的就绪形状（函数指针非空即可；本 fixture 不执行业务写入）
// ===========================================================================
QolResult stub_ok() { return qol::contracts::ok(); }
QolResult stub_ok_i32(int32_t*) { return qol::contracts::ok(); }
QolResult stub_ok_int64(int64_t) { return qol::contracts::ok(); }
QolResult stub_ok_pair(int32_t, int64_t) { return qol::contracts::ok(); }
QolResult stub_ok_handle(QolItemHandle) { return qol::contracts::ok(); }
QolResult stub_ok_text(uint32_t) { return qol::contracts::ok(); }
QolResult stub_ok_yesno(uint32_t, QolCraftPopupCallback, QolCraftPopupCallback) {
    return qol::contracts::ok();
}
int g_fixture_needs_writes = 0;
int g_fixture_set_item_writes = 0;
QolResult stub_needs(uint32_t, int32_t, int32_t) {
    ++g_fixture_needs_writes;
    return qol::contracts::ok();
}
QolResult stub_set_item(uint32_t, QolItemHandle) {
    ++g_fixture_set_item_writes;
    return qol::contracts::ok();
}
QolResult stub_create(int32_t, QolItemHandle*) { return qol::contracts::ok(); }
QolResult stub_create4(int32_t, int32_t, int32_t, int32_t, QolItemHandle*) {
    return qol::contracts::ok();
}
QolResult stub_save(QolItemHandle, int32_t*) { return qol::contracts::ok(); }
QolResult stub_word_read(QolItemHandle, uint32_t*) { return qol::contracts::ok(); }
QolResult stub_word_write(QolItemHandle, uint32_t) { return qol::contracts::ok(); }
QolResult stub_random(int32_t lo, int32_t, int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out = lo;
    return qol::contracts::ok();
}

QolCraftUiWritePort make_ui_write_port() {
    QolCraftUiWritePort port{};
    port.abi_version = kQolPortCraftUiWriteVersion;
    port.init_mixing_state = &stub_ok;
    port.reset_stuff_item_control = &stub_ok;
    port.refresh_inventory_items = &stub_ok;
    port.select_first_empty_stuff_slot = &stub_ok_i32;
    port.set_ui_type = &stub_ok_int64;
    port.set_selected_recipe = &stub_ok_pair;
    port.show_text = &stub_ok_text;
    port.show_yesno_from_text = &stub_ok_yesno;
    port.set_target_slot_item = &stub_ok_handle;
    port.set_cost_zero = &stub_ok;
    return port;
}

QolResult stub_sound(int32_t) { return qol::contracts::ok(); }

QolCustomRecipeWritePort g_recipe_port{};
QolCraftUiWritePort g_ui_port{};

void install_ready_ports() {
    g_recipe_port = QolCustomRecipeWritePort{};
    g_recipe_port.abi_version = kQolPortCustomRecipeWriteVersion;
    g_recipe_port.write_stuff_need = &stub_needs;
    g_recipe_port.set_stuff_item = &stub_set_item;
    g_recipe_port.description_set_off = &stub_ok;
    g_recipe_port.create_perfect_item = &stub_create;
    g_recipe_port.create_item = &stub_create4;
    g_recipe_port.save_item = &stub_save;
    g_recipe_port.free_item = &stub_ok_handle;
    g_recipe_port.read_jewel_word = &stub_word_read;
    g_recipe_port.write_jewel_word = &stub_word_write;
    g_recipe_port.set_max_socket_enchant = &stub_ok_handle;
    g_recipe_port.random_inclusive = &stub_random;

    g_ui_port = make_ui_write_port();
    g_ui_port.play_sound = &stub_sound;
    qol::gametest::set_custom_recipe_write_port_for_test(&g_recipe_port);
    qol::gametest::set_craft_ui_write_port_for_test(&g_ui_port);
}

// ===========================================================================
// 资源安装注入：core / optional 符号地址与失败集合
// ===========================================================================
constexpr const char* kCoreSymbols[] = {
    "F_UIMIX_BUTTON_INVEN_ITEM_SELECT_EXE_VMA",
    "F_UIMIX_BUTTON_MIXING_EXE_VMA",
    "F_MAKE_MIX_VMA",
    "F_UIMIX_BUTTON_MENU_LIST_EXE_VMA",
    "F_UIMIX_BUTTON_RECIPE_EXE_VMA",
};
constexpr const char* kOptionalSymbols[] = {
    "F_XTEXTCTRL_SET_TEXT_CONTROL_VMA",
    "F_CONTROL_ITEM_DRAW_VMA",
    "F_ITEM_DRAW_PORTING_VMA",
    "F_UIMIX_BUTTON_RECIPE_DRAW_VMA",
    "F_UIMIX_DRAW_VMA",
    "F_UIMIX_BUTTON_MENU_LIST_DRAW_VMA",
    "F_UIMIX_RESET_STUFF_ITEM_CONTROL_VMA",
};
constexpr int kCoreSymbolCount = 5;
constexpr int kOptionalSymbolCount = 7;

const char* g_fail_symbol = nullptr;
int g_hook_calls = 0;
int g_unhook_calls = 0;

// 宿主机制分派替身：production 由 host/assembly.cpp 注入 dispatch_install/dispatch_release；
// fixture 只覆盖 custom_recipe 用到的 kOwnership（ownership_ops acquire/release）。
qol::hooks::TxDeps::MechanismOutcome fixture_install_other(const ResourceClaim& claim,
                                                           uintptr_t) {
    if (claim.mechanism != ResourceMechanism::kOwnership) {
        return {1, "mechanism unsupported"};
    }
    const OwnershipOps* ops = ownership_ops(claim.handler);
    if (ops == nullptr || ops->acquire == nullptr) return {1, "ownership ops missing"};
    if (ownership_acquire(ops, claim.id) != 0) return {1, "ownership acquire rejected"};
    return {0, nullptr};
}

qol::hooks::TxDeps::MechanismOutcome fixture_release_other(const ResourceClaim& claim,
                                                           uintptr_t) {
    if (claim.mechanism != ResourceMechanism::kOwnership) {
        return {1, "mechanism unsupported"};
    }
    ownership_release(ownership_ops(claim.handler), claim.id);
    return {0, nullptr};
}

uintptr_t resolve_symbol(const char* symbol, bool* out_followed) {
    if (out_followed != nullptr) *out_followed = false;
    if (symbol == nullptr) return 0;
    for (int i = 0; i < kCoreSymbolCount; ++i) {
        if (std::strcmp(symbol, kCoreSymbols[i]) == 0) {
            return 0x1000 + static_cast<uintptr_t>(i);
        }
    }
    for (int i = 0; i < kOptionalSymbolCount; ++i) {
        if (std::strcmp(symbol, kOptionalSymbols[i]) == 0) {
            return 0x2000 + static_cast<uintptr_t>(i);
        }
    }
    return 0x4000;  // 未知符号（fixture 不依赖）
}

int hook_symbol(void* target, void* replacement, void** backup) {
    (void)replacement;
    ++g_hook_calls;
    for (int i = 0; i < kCoreSymbolCount; ++i) {
        if (reinterpret_cast<uintptr_t>(target) == 0x1000 + static_cast<uintptr_t>(i) &&
            g_fail_symbol != nullptr && std::strcmp(g_fail_symbol, kCoreSymbols[i]) == 0) {
            return 1;
        }
    }
    for (int i = 0; i < kOptionalSymbolCount; ++i) {
        if (reinterpret_cast<uintptr_t>(target) == 0x2000 + static_cast<uintptr_t>(i) &&
            g_fail_symbol != nullptr && std::strcmp(g_fail_symbol, kOptionalSymbols[i]) == 0) {
            return 1;
        }
    }
    if (backup != nullptr) *backup = reinterpret_cast<void*>(0xB000 + g_hook_calls);
    return 0;
}
int unhook_symbol(void* target) {
    (void)target;
    ++g_unhook_calls;
    return 0;
}
bool gate_fn(const char*) { return true; }

// ===========================================================================
// provider 插件（4 个）
// ===========================================================================
int provider_install(PluginContext&) { return 0; }
void provider_noop(PluginContext&) {}
// Toggle feature 必须带 on_toggle（Host 的 feature 校验规则）。
QolResult provider_toggle(PluginContext&, uint8_t) { return qol::contracts::ok(); }

const CapabilityProvisionRef kItemFactsProvides[] = {
    {kItemFactsCapabilityId, {kItemFactsVersion.major, kItemFactsVersion.minor}, &kItemFactsApi}};
const CapabilityProvisionRef kCraftFactsProvides[] = {
    {kCraftFactsCapabilityId, {kCraftFactsVersion.major, kCraftFactsVersion.minor}, &kCraftFactsApi}};
const CapabilityProvisionRef kTextLayerProvides[] = {
    {kTextLayerCapabilityId,
     {static_cast<uint16_t>(kTextLayerVersionMajor), static_cast<uint16_t>(kTextLayerVersionMinor)},
     &kTextLayerApi}};
const CapabilityProvisionRef kAttributeQualityProvides[] = {
    {kAttributeQualityCapabilityId,
     {kAttributeQualityVersionMajor, kAttributeQualityVersionMinor}, &kAttributeQualityApi}};

// 零 feature 插件被禁止注册：每个 provider 声明一个 Hidden/默认开启的常驻 feature。
const FeatureDefinition kItemFactsFeatures[] = {
    {"item_facts.provider", 1, FeatureCategory::kHidden, FeatureType::kToggle, "item.facts", "",
     &provider_toggle, nullptr}};
const FeatureDefinition kCraftFactsFeatures[] = {
    {"craft_facts.provider", 1, FeatureCategory::kHidden, FeatureType::kToggle, "craft.facts", "",
     &provider_toggle, nullptr}};
const FeatureDefinition kTextLayerFeatures[] = {
    {"text_layer.provider", 1, FeatureCategory::kHidden, FeatureType::kToggle, "text_layer", "",
     &provider_toggle, nullptr}};
const FeatureDefinition kAttributeQualityFeatures[] = {
    {"attribute_quality.provider", 1, FeatureCategory::kHidden, FeatureType::kToggle,
     "attribute.quality", "", &provider_toggle, nullptr}};

PluginDescriptor provider_descriptor(const char* id, const CapabilityProvisionRef* provides,
                                     const FeatureDefinition* features) {
    PluginDescriptor descriptor{};
    descriptor.id = id;
    descriptor.version = "0.1.0";
    descriptor.install = &provider_install;
    descriptor.deactivate = &provider_noop;
    descriptor.on_enabled = &provider_noop;
    descriptor.on_disabled = &provider_noop;
    descriptor.provides = provides;
    descriptor.provides_count = 1;
    descriptor.features = features;
    descriptor.feature_count = 1;
    return descriptor;
}

// ===========================================================================
// fixture 生命周期
// ===========================================================================
FeatureController* g_controller = nullptr;
FeatureReconciler* g_reconciler = nullptr;
std::thread::id g_main_thread;

void frames(int count = 24) {
    for (int i = 0; i < count; ++i) g_reconciler->step();
}

FeatureReceipt await_receipt(FeatureReceipt receipt) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        assert(succeeded(g_controller->receipt(receipt.request_id, &receipt)));
        if (receipt.persistence != FeaturePersistence::kPending) return receipt;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::fprintf(stderr, "receipt timed out\n");
    std::abort();
}

void set_recipes(bool value) {
    FeatureReceipt receipt{};
    assert(succeeded(
        g_controller->request_set_feature("custom_recipe", "custom_recipe.recipes",
                                          value ? 1 : 0, &receipt)));
    receipt = await_receipt(receipt);
    assert(receipt.persistence == FeaturePersistence::kPersisted);
}

int resource_state_code(const char* id) { return static_cast<int>(qol::hooks::state_of(id)); }
bool resource_free(const char* id) {
    return qol::hooks::state_of(id) == ResourceState::kFree;
}
bool resource_live(const char* id) {
    return qol::hooks::state_of(id) == ResourceState::kLive;
}
// 13 条资源 id（与 ui/runtime.cpp / plugin.cpp 声明一致）。
constexpr const char* kResourceIds[] = {
    "hook:UIMix_ButtonInvenItemSelectExe",
    "hook:UIMix_ButtonMixingExe",
    "hook:MIXSYSTEM_MakeItem",
    "hook:UIMix_ButtonMenuListExe",
    "hook:UIMix_ButtonRecipeExe",
    "hook:X_TEXTCTRL_SetTextControl",
    "hook:ControlItem_Draw",
    "hook:ITEM_DrawPorting",
    "hook:UIMix_ButtonRecipeDraw",
    "hook:UIMix_Draw",
    "hook:UIMix_ButtonMenuListDraw",
    "hook:UIMix_ResetStuffItemControl",
    "own:RECIPEBASE_MIXTUREBASE",
};

void reset_gate_and_table() {
    g_table_ensure_result = true;
    g_table_published = false;
    g_fail_symbol = nullptr;
}

void assert_all_resources_free() {
    for (const char* id : kResourceIds) {
        assert(resource_free(id));
    }
}

void assert_core_resources_live() {
    for (int i = 0; i < kCoreSymbolCount; ++i) {
        assert(resource_live(kResourceIds[i]));
    }
}

void assert_no_public_craft_result_capability() {
    assert(qol::infra::require_capability("craft_result", {1, 1}) == nullptr);
}

void assert_registrations_net_zero() {
    assert(qol::host::fixture_typed_registrations() == qol::host::fixture_typed_unregistrations());
    assert(qol::host::fixture_event_registrations() == qol::host::fixture_event_unregistrations());
    assert(g_text_subscribe_calls == g_text_unsubscribe_calls);
}

}  // namespace

// host 上 table.cpp 只编译纯构造器；表注入行为由 fixture 替身提供（可注入失败）。
namespace custom_recipe {

void set_context(const qol::contracts::QolGameHost*, const qol::contracts::QolHostSdk*) {}

bool custom_recipe_table_ensure() {
    ++g_table_ensure_calls;
    if (!g_table_ensure_result) return false;
    g_table_published = true;
    ::custom_recipe::bind_base_record_count(69);
    return true;
}

void custom_recipe_table_deactivate() {
    ++g_table_deactivate_calls;
    g_table_published = false;
}

}  // namespace custom_recipe

int main(int argc, char** argv) {
    assert(argc == 2);
    g_main_thread = std::this_thread::get_id();
    install_ready_ports();

    qol::host::plugin_registry_reset_for_test();
    qol::hooks::registry_reset_for_test();
    qol::hooks::tx_reset_for_test();
    qol::hooks::tx_set_deps(qol::hooks::TxDeps{&resolve_symbol, &hook_symbol, &unhook_symbol,
                                               &gate_fn, &fixture_install_other,
                                               &fixture_release_other});

    PluginDescriptor item_facts =
        provider_descriptor("item_facts", kItemFactsProvides, kItemFactsFeatures);
    PluginDescriptor craft_facts =
        provider_descriptor("craft_facts", kCraftFactsProvides, kCraftFactsFeatures);
    PluginDescriptor text_layer =
        provider_descriptor("text_layer", kTextLayerProvides, kTextLayerFeatures);
    PluginDescriptor attribute_quality = provider_descriptor(
        "attribute_quality", kAttributeQualityProvides, kAttributeQualityFeatures);
    const PluginDescriptor& custom_recipe = qol::features::custom_recipe::plugin_descriptor();

    // ---- 单 descriptor 形状 ----
    assert(std::strcmp(custom_recipe.id, "custom_recipe") == 0);
    assert(custom_recipe.provides_count == 0 && custom_recipe.provides == nullptr);
    assert(custom_recipe.requires_count == 3);
    assert(custom_recipe.optional_count == 1);
    assert(custom_recipe.jni_count == 0);
    assert(custom_recipe.port_count == 2);
    assert(custom_recipe.ports[0].id == QolPortId::QOL_PORT_CUSTOM_RECIPE_WRITE);
    assert(custom_recipe.ports[1].id == QolPortId::QOL_PORT_CRAFT_UI_WRITE);
    assert(custom_recipe.feature_count == 1);
    assert(std::strcmp(custom_recipe.features[0].id, "custom_recipe.recipes") == 0);
    assert(custom_recipe.features[0].default_enabled == 0);
    assert(custom_recipe.resources_count == 13);
    for (int i = 0; i < 12; ++i) {
        assert(std::strcmp(custom_recipe.resources[i].id, kResourceIds[i]) == 0);
        assert(custom_recipe.resources[i].mechanism == ResourceMechanism::kEntryHook);
        const bool required = i < kCoreSymbolCount;
        assert((custom_recipe.resources[i].requirement == ResourceRequirement::kRequired) ==
               required);
        assert((custom_recipe.resources[i].failure == ResourceFailure::kFailClosed) == required);
    }
    // ownership 必须在最后（宿主逆序释放 → 表先还原，再释放 hook）。
    assert(std::strcmp(custom_recipe.resources[12].id, kResourceIds[12]) == 0);
    assert(custom_recipe.resources[12].mechanism == ResourceMechanism::kOwnership);
    assert(custom_recipe.resources[12].failure == ResourceFailure::kFailSoft);
    // descriptor 不声明 craft_result provides（能力已内部化）。
    assert(!qol::features::custom_recipe::enabled());
    assert(!qol::features::custom_recipe::ownership_ready());

    for (const PluginDescriptor* provider : {&item_facts, &craft_facts, &text_layer,
                                             &attribute_quality}) {
        assert(qol::host::register_plugin(*provider));
    }
    assert(qol::host::register_plugin(custom_recipe));

    qol::host::HostDeps deps{&resolve_symbol, &hook_symbol, &unhook_symbol};
    qol::host::FeatureDirectory directory;
    for (const PluginDescriptor* provider : {&item_facts, &craft_facts, &text_layer,
                                             &attribute_quality}) {
        assert(directory.add(*provider).ok());
    }
    assert(directory.add(custom_recipe).ok());

    std::string path = std::string(argv[1]) + "/custom-recipe-owner-XXXXXX";
    assert(::mkdtemp(path.data()) != nullptr);
    qol::host::CoreSettingsConfigService service;
    FeatureController control(service);
    FeatureReconciler reconcile(control);
    g_controller = &control;
    g_reconciler = &reconcile;
    assert(control.initialize(directory, path, &FeatureReconciler::pending_desired, &reconcile)
               .ok());
    assert(reconcile.bind({&resolve_symbol, &hook_symbol, &unhook_symbol}));

    // ---- 门禁开：真实描述符装配成功 ----
    set_recipes(true);
    frames();
    assert(qol::host::feature_registry_active("custom_recipe"));
    assert(qol::features::custom_recipe::enabled());
    assert(qol::features::custom_recipe::ownership_ready());
    assert(g_table_published && g_table_ensure_calls >= 1);
    assert_core_resources_live();
    for (int i = kCoreSymbolCount; i < 12; ++i) assert(resource_live(kResourceIds[i]));
    assert(resource_live(kResourceIds[12]));
    assert_no_public_craft_result_capability();
    assert(qol::features::custom_recipe::ui::core_backups_ready());
    assert(qol::host::fixture_typed_registrations() == 2);
    assert(g_text_subscribe_calls >= 1);
    std::puts("owner_fixture: single descriptor installs (13 live resources, gate open, no public craft_result)");

    // ---- 7 条装饰 hook 逐个失败：核心继续，失败资源 Free，插件保持 active ----
    for (int optional = 0; optional < kOptionalSymbolCount; ++optional) {
        set_recipes(false);
        frames();
        reset_gate_and_table();
        g_table_published = false;
        const int ensure_before = g_table_ensure_calls;
        g_fail_symbol = kOptionalSymbols[optional];
        set_recipes(true);
        frames();
        assert(qol::host::feature_registry_active("custom_recipe"));
        assert(qol::features::custom_recipe::enabled());
        assert_core_resources_live();
        assert(resource_free(kResourceIds[kCoreSymbolCount + optional]));
        assert(g_table_published && g_table_ensure_calls == ensure_before + 1);
        assert_no_public_craft_result_capability();
    }
    std::puts("owner_fixture: optional hook failures degrade only that resource (core stays live, fail-soft Free)");

    // ---- 5 条核心 hook 逐个失败：整插件回滚、注册净零、表未发布、门禁关闭 ----
    for (int core = 0; core < kCoreSymbolCount; ++core) {
        set_recipes(false);
        frames();
        reset_gate_and_table();
        const int ensure_before = g_table_ensure_calls;
        const int deactivate_before = g_table_deactivate_calls;
        const int typed_before = qol::host::fixture_typed_registrations();
        const int typed_unreg_before = qol::host::fixture_typed_unregistrations();
        const int event_before = qol::host::fixture_event_registrations();
        const int event_unreg_before = qol::host::fixture_event_unregistrations();
        const int text_sub_before = g_text_subscribe_calls;
        const int text_unsub_before = g_text_unsubscribe_calls;

        g_fail_symbol = kCoreSymbols[core];
        set_recipes(true);
        frames();
        assert(!qol::host::feature_registry_active("custom_recipe"));
        assert(!qol::features::custom_recipe::enabled());
        assert(!qol::features::custom_recipe::ownership_ready());
        assert_all_resources_free();
        assert(!g_table_published);
        assert(g_table_ensure_calls == ensure_before);  // 核心失败→表未开始注入
        // ownership 从未 publish；安装失败清理只允许调用一次 deactivate（绝不残留已发布表）。
        assert(g_table_deactivate_calls <= deactivate_before + 1);
        // 本次 install 的注册必须净额归零。
        assert(qol::host::fixture_typed_registrations() == typed_before + 2);
        assert(qol::host::fixture_typed_unregistrations() == typed_unreg_before + 2);
        assert(qol::host::fixture_event_registrations() == event_before + 1);
        assert(qol::host::fixture_event_unregistrations() == event_unreg_before + 1);
        assert(g_text_subscribe_calls == text_sub_before + 1);
        assert(g_text_unsubscribe_calls == text_unsub_before + 1);
        assert(!qol::features::custom_recipe::ui::core_backups_ready());
        assert_no_public_craft_result_capability();
    }
    assert_registrations_net_zero();
    std::puts("owner_fixture: each core hook failure rolls back to Free with net-zero registrations and no table publish");

    // ---- ownership acquire 失败：claim 为 required×fail-soft，插件保持装配但门禁必须关闭 ----
    reset_gate_and_table();
    g_table_ensure_result = false;
    set_recipes(true);
    frames();
    assert(qol::host::feature_registry_active("custom_recipe"));  // fail-soft 不级联停用
    assert(!qol::features::custom_recipe::enabled());
    assert(!qol::features::custom_recipe::ownership_ready());
    assert(g_table_ensure_calls >= 1);
    assert(!g_table_published);
    assert(resource_free(kResourceIds[12]));  // 失败资源保持 Free（可重试），不 tombstone
    assert_core_resources_live();
    assert_no_public_craft_result_capability();

    // gate=false（ownership 未就绪）：旧队列里排队的 typed 写命令不得写回。
    {
        const qol::contracts::CraftWriteStuffNeedPayload need{1, 15, 3};
        const qol::contracts::CraftSetStuffItemPayload set{2, qol::contracts::QolItemHandle{1, 1}};
        const auto* need_bytes = reinterpret_cast<const uint8_t*>(&need);
        const auto* set_bytes = reinterpret_cast<const uint8_t*>(&set);
        const int needs_before = g_fixture_needs_writes;
        const int set_before = g_fixture_set_item_writes;
        assert(!qol::features::custom_recipe::enabled());
        assert(!qol::features::custom_recipe::handle_write_stuff_need_command(need_bytes,
                                                                             sizeof(need)));
        assert(!qol::features::custom_recipe::handle_set_stuff_item_command(set_bytes,
                                                                           sizeof(set)));
        assert(g_fixture_needs_writes == needs_before && g_fixture_set_item_writes == set_before);

        // 恢复就绪后同一批 payload 才允许写回。
        reset_gate_and_table();
        set_recipes(false);
        frames();
        set_recipes(true);
        frames();
        assert(qol::features::custom_recipe::enabled());
        assert(qol::features::custom_recipe::handle_write_stuff_need_command(need_bytes,
                                                                            sizeof(need)));
        assert(qol::features::custom_recipe::handle_set_stuff_item_command(set_bytes, sizeof(set)));
        assert(g_fixture_needs_writes == needs_before + 1 &&
               g_fixture_set_item_writes == set_before + 1);
    }
    std::puts("owner_fixture: ownership acquire failure keeps the gate closed and publishes no table");

    // ---- 恢复：停用后表可用，再次启用应重新就绪 ----
    set_recipes(false);
    frames();
    reset_gate_and_table();
    set_recipes(true);
    frames();
    assert(qol::host::feature_registry_active("custom_recipe"));
    assert(qol::features::custom_recipe::enabled());
    assert(qol::features::custom_recipe::ownership_ready());
    assert(g_table_published);

    // ---- off：清理订阅/typed handler、还原表、门禁关闭；再 on 可恢复 ----
    set_recipes(false);
    frames();
    assert(!qol::features::custom_recipe::enabled());
    assert(!qol::features::custom_recipe::ownership_ready());
    assert(g_table_deactivate_calls >= 1 && !g_table_published);
    assert_registrations_net_zero();
    assert_all_resources_free();
    assert(!qol::features::custom_recipe::ui::core_backups_ready());
    assert_no_public_craft_result_capability();

    set_recipes(true);
    frames();
    assert(qol::host::feature_registry_active("custom_recipe"));
    assert(qol::features::custom_recipe::enabled());
    assert(qol::features::custom_recipe::ownership_ready());
    assert_core_resources_live();

    std::puts("custom_recipe_owner_fixture_tests: pass (single descriptor, per-hook rollback/degrade, ownership gate, off/on cleanup)");
    return 0;
}

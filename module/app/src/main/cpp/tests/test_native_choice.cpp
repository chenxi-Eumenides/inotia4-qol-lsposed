// core.custom_ui host 测试：
// 选择框规格边界/重复打开/关闭项/地址归属（native_choice_detail），以及插件清单
// （ui_choice 能力提供 + 2 条声明式资源的机制/handler/门禁）和 feature.cpp 运行时选择流程。

#include "plugins/core.custom_ui/feature.h"
#include "plugins/core.custom_ui/plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/ui_choice.h"
#include "contracts/game_ports/choice_port.h"
#include "contracts/ui.h"

namespace {

using qol::contracts::ResourceFailure;
using qol::contracts::ResourceMechanism;
using qol::contracts::ResourceRequirement;
using qol::contracts::UiChoiceApi;
using qol::contracts::UiChoiceSpec;
using qol::contracts::QolChoicePort;
using qol::features::core_ui::native_choice_detail::address_in_range;
using qol::features::core_ui::native_choice_detail::can_open;
using qol::features::core_ui::native_choice_detail::is_close_index;
using qol::features::core_ui::native_choice_detail::valid_spec;

int g_pass = 0;
int g_fail = 0;
bool g_push_succeeds = true;
bool g_push_initialized = false;
bool g_close_during_prepare = false;
int g_push_calls = 0;
int g_close_calls = 0;
int32_t g_state_id = 1;
char g_written_title[64]{};
const char* g_title_pointer = nullptr;

QolChoicePort g_choice_port{};

QolResult choice_state_id(int32_t* out_state_id) {
    *out_state_id = g_state_id;
    return qol::contracts::ok();
}

QolResult choice_write_items(const char* const*, uint32_t) {
    if (g_close_during_prepare) {
        g_close_during_prepare = false;
        qol::features::core_ui::native_choice_close();
    }
    return qol::contracts::ok();
}
QolResult choice_write_focus(int32_t) { return qol::contracts::ok(); }
QolResult choice_write_title(const char* title) {
    g_title_pointer = title;  // 游戏保存指针而非复制标题。
    std::snprintf(g_written_title, sizeof(g_written_title), "%s", title);
    return qol::contracts::ok();
}
QolResult choice_push() {
    ++g_push_calls;
    qol::features::core_ui::choice_init_wrapper(nullptr);
    g_push_initialized = qol::features::core_ui::native_choice_active();
    return g_push_succeeds ? qol::contracts::ok()
                           : qol::contracts::error(kQolErrInternal, "push failed", 11);
}
QolResult choice_close_popup() {
    ++g_close_calls;
    return qol::contracts::ok();
}

bool setup_choice_feature() {
    static qol::contracts::QolUiAdapter ui{};
    static qol::contracts::QolHostSdk sdk{};
    static const void* ports[] = {&g_choice_port};
    ui.size = sizeof(ui);
    ui.major = qol::contracts::kUiContractMajor;
    ui.minor = qol::contracts::kUiAdapterMinor;
    sdk.ui = &ui;
    qol::contracts::PluginContext context{};
    context.sdk = &sdk;
    context.ports = ports;
    context.port_count = 1;
    const bool installed = qol::features::core_ui::plugin_descriptor().install(context) == 0;
    qol::features::core_ui::native_choice_set_installed(true);
    return installed;
}

void reset_fake_choice() {
    g_choice_port = {};
    g_choice_port.read.state_id = &choice_state_id;
    g_choice_port.write.write_items = &choice_write_items;
    g_choice_port.write.write_title = &choice_write_title;
    g_choice_port.write.write_focus = &choice_write_focus;
    g_choice_port.write.push = &choice_push;
    g_choice_port.write.close_popup = &choice_close_popup;
    g_push_succeeds = true;
    g_push_initialized = false;
    g_close_during_prepare = false;
    g_push_calls = 0;
    g_close_calls = 0;
    g_written_title[0] = '\0';
    g_title_pointer = nullptr;
}

#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (0)

void on_select(int, void*) {}

UiChoiceSpec valid_spec_of(int count = 2, int close_index = 1) {
    static const char* const items[] = {"one", "two", "three", "four", "five", "six"};
    return {items, count, "title", close_index, &on_select, nullptr};
}

void test_spec_bounds() {
    CHECK(!valid_spec(valid_spec_of(0)));
    CHECK(valid_spec(valid_spec_of(1, -1)));
    CHECK(valid_spec(valid_spec_of(6, 5)));
    CHECK(!valid_spec(valid_spec_of(7)));
    CHECK(!valid_spec(valid_spec_of(2, 2)));
}

void test_repeated_open() {
    const UiChoiceSpec spec = valid_spec_of();
    CHECK(can_open(false, spec));
    CHECK(!can_open(true, spec));
}

void test_close_index() {
    CHECK(is_close_index(1, 1));
    CHECK(!is_close_index(0, 1));
    CHECK(!is_close_index(0, -1));
    CHECK(valid_spec(valid_spec_of(1, 0)));
}

void test_address_ownership_range() {
    constexpr uintptr_t begin = 0x1000;
    CHECK(address_in_range(begin, begin, 256));
    CHECK(address_in_range(begin + 255, begin, 256));
    CHECK(!address_in_range(begin + 256, begin, 256));
    CHECK(!address_in_range(begin - 1, begin, 256));
    CHECK(!address_in_range(begin, begin, 0));
}

// 插件清单：provides `ui_choice` v1.0；2 条资源各自机制/handler/门禁正确；读写各一端口。
void test_plugin_manifest() {
    const qol::contracts::PluginDescriptor& d =
        qol::features::core_ui::plugin_descriptor();
    CHECK(std::strcmp(d.id, "core.custom_ui") == 0);
    CHECK(d.requires_count == 0 && d.optional_count == 0);  // 工具插件无 requires
    CHECK(d.provides_count == 4);  // ui_components + ui_panels + ui_choice + text_layer
    CHECK(std::strcmp(d.provides[2].id, qol::contracts::kUiChoiceCapabilityId) == 0);
    CHECK(d.provides[2].version.major == qol::contracts::kUiChoiceVersionMajor);
    CHECK(d.provides[2].version.minor == qol::contracts::kUiChoiceVersionMinor);
    CHECK(d.provides[2].iface != nullptr);
    CHECK(d.port_count == 1 && d.ports != nullptr);  // 仅 QOL_PORT_CHOICE
    CHECK(d.ports[0].id == qol::contracts::QolPortId::QOL_PORT_CHOICE);
    CHECK(d.ports[0].range.major == qol::contracts::kQolPortChoiceVersion.major);
    CHECK(d.ports[0].range.min_minor == qol::contracts::kQolPortChoiceVersion.minor);
    const auto* api = static_cast<const UiChoiceApi*>(d.provides[2].iface);
    CHECK(api->open != nullptr && api->active != nullptr && api->close != nullptr);
    CHECK(!api->active());

    CHECK(d.resources_count == 3);  // 选择控件 2 条 + 文本 GetText 共享资源
    const qol::contracts::ResourceClaim& init = d.resources[0];
    CHECK(std::strcmp(init.id, "hook:UIChoice_Init") == 0);
    CHECK(std::strcmp(init.symbol, "F_UI_CHOICE_INIT_VMA") == 0);
    CHECK(init.mechanism == ResourceMechanism::kEntryHook);
    CHECK(init.handler != nullptr);
    CHECK(init.gate == nullptr);
    CHECK(init.resumable);
    CHECK(init.requirement == ResourceRequirement::kRequired);
    CHECK(init.failure == ResourceFailure::kFailClosed);

    const qol::contracts::ResourceClaim& button = d.resources[1];
    CHECK(std::strcmp(button.id, "ptrslot:g_uichoice_button_list_exe_got") == 0);
    CHECK(std::strcmp(button.symbol, "ptrslot:g_uichoice_button_list_exe_got") == 0);
    CHECK(button.mechanism == ResourceMechanism::kPtrSlot);
    CHECK(button.handler != nullptr);
    CHECK(button.gate == nullptr);
    CHECK(button.resumable);
    CHECK(button.requirement == ResourceRequirement::kRequired);
    CHECK(button.failure == ResourceFailure::kFailClosed);

    CHECK(d.on_resource_ready != nullptr);
}

void test_synchronous_push_initialization() {
    const UiChoiceSpec spec = valid_spec_of();
    CHECK(qol::features::core_ui::native_choice_open(spec));
    CHECK(g_push_calls == 1);
    CHECK(g_push_initialized);
    CHECK(std::strcmp(g_written_title, "title") == 0);
    CHECK(g_title_pointer != nullptr);
    CHECK(g_title_pointer != spec.title);  // 模块缓冲独立于调用方标题。
    CHECK(std::strcmp(g_title_pointer, "title") == 0);
    CHECK(qol::features::core_ui::native_choice_active());
    CHECK(!qol::features::core_ui::native_choice_open(spec));
    qol::features::core_ui::native_choice_close();
    CHECK(!qol::features::core_ui::native_choice_active());
    CHECK(g_close_calls == 1);
}

void test_push_failure_rolls_back() {
    g_push_succeeds = false;
    CHECK(!qol::features::core_ui::native_choice_open(valid_spec_of()));
    CHECK(!qol::features::core_ui::native_choice_active());
    g_push_succeeds = true;
    CHECK(qol::features::core_ui::native_choice_open(valid_spec_of()));
    CHECK(qol::features::core_ui::native_choice_active());
    qol::features::core_ui::native_choice_close();
}

void test_close_and_reopen_gate() {
    const UiChoiceSpec spec = valid_spec_of();
    CHECK(qol::features::core_ui::native_choice_open(spec));
    qol::features::core_ui::native_choice_close();
    CHECK(!qol::features::core_ui::native_choice_active());
    CHECK(qol::features::core_ui::native_choice_open(spec));
    CHECK(qol::features::core_ui::native_choice_active());
    qol::features::core_ui::native_choice_set_installed(false);
    CHECK(!qol::features::core_ui::native_choice_active());
    CHECK(!qol::features::core_ui::native_choice_open(spec));
    qol::features::core_ui::native_choice_set_installed(true);
}

void test_close_during_prepare_releases_open_gate() {
    const UiChoiceSpec spec = valid_spec_of();
    g_close_during_prepare = true;
    CHECK(!qol::features::core_ui::native_choice_open(spec));
    CHECK(!qol::features::core_ui::native_choice_active());
    CHECK(qol::features::core_ui::native_choice_open(spec));
    CHECK(qol::features::core_ui::native_choice_active());
    qol::features::core_ui::native_choice_close();
}

}  // namespace

int main() {
    reset_fake_choice();
    CHECK(setup_choice_feature());
    test_spec_bounds();
    test_repeated_open();
    test_close_index();
    test_address_ownership_range();
    test_plugin_manifest();
    test_synchronous_push_initialization();
    test_push_failure_rolls_back();
    test_close_and_reopen_gate();
    test_close_during_prepare_releases_open_gate();
    std::printf("native_choice_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

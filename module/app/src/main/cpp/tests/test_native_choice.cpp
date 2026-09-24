// plugins/native_choice 纯逻辑 host 测试：
// 选择框规格边界/重复打开/关闭项/地址归属（native_choice_detail），以及插件清单
// （ui_choice 能力提供 + 2 条声明式资源的机制/handler/门禁）。
//
// 本 target 只链接 plugin.cpp，feature.cpp 的运行时实现以桩替代（真机验证）；
// 纯逻辑走 feature.h 的 header-inline，不触碰游戏内存。

#include "plugins/core.custom_ui/feature.h"
#include "plugins/core.custom_ui/plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/ui_choice.h"
#include "contracts/game_ports/choice_port.h"

namespace {

using qol::contracts::ResourceFailure;
using qol::contracts::ResourceMechanism;
using qol::contracts::ResourceRequirement;
using qol::contracts::UiChoiceApi;
using qol::contracts::UiChoiceSpec;
using qol::features::core_ui::native_choice_detail::address_in_range;
using qol::features::core_ui::native_choice_detail::can_open;
using qol::features::core_ui::native_choice_detail::is_close_index;
using qol::features::core_ui::native_choice_detail::valid_spec;

int g_pass = 0;
int g_fail = 0;

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
    CHECK(d.provides_count == 3);  // ui_components + ui_panels + ui_choice
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

    CHECK(d.resources_count == 2);
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

}  // namespace

int main() {
    test_spec_bounds();
    test_repeated_open();
    test_close_index();
    test_address_ownership_range();
    test_plugin_manifest();
    std::printf("native_choice_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

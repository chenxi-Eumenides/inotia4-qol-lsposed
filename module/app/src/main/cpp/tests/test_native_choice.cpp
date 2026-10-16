// core.custom_ui host 测试：
// 选择框规格边界/重复打开/关闭项/地址归属（native_choice_detail），以及插件清单
// （ui_choice 能力提供 + 2 条声明式资源的机制/handler/门禁）和 feature.cpp 运行时选择流程。

#include "plugins/core.custom_ui/feature.h"
#include "plugins/core.custom_ui/plugin.h"

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <type_traits>

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
using qol::contracts::kUiChoiceVersionMinor;
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
bool g_state_api_available = true;
int g_dialog_publish_calls = 0;
int g_dialog_clear_calls = 0;
int g_select_calls = 0;
int g_select_index = -1;
void* g_select_user = nullptr;
int32_t g_selected_index = 0;
uint64_t g_dialog_generation = 0;
char g_dialog_type[128]{};
char g_dialog_title[64]{};
char g_dialog_labels[6][64]{};
uint32_t g_dialog_option_count = 0;

QolChoicePort g_choice_port{};
qol::contracts::QolHostSdk g_sdk{};

QolResult state_publish_panels(const qol::contracts::UiCustomPanelsSnapshot*) {
    return qol::contracts::ok();
}
QolResult state_publish_dialog(const qol::contracts::UiCustomDialogSnapshot* snapshot) {
    ++g_dialog_publish_calls;
    g_dialog_generation = snapshot->generation;
    std::snprintf(g_dialog_type, sizeof(g_dialog_type), "%s", snapshot->type_id);
    std::snprintf(g_dialog_title, sizeof(g_dialog_title), "%s",
                  snapshot->title == nullptr ? "" : snapshot->title);
    g_dialog_option_count = snapshot->option_count;
    for (uint32_t i = 0; i < snapshot->option_count; ++i) {
        std::snprintf(g_dialog_labels[i], sizeof(g_dialog_labels[i]), "%s",
                      snapshot->option_labels[i]);
    }
    return qol::contracts::ok();
}
QolResult state_clear_dialog(uint64_t generation, uint64_t) {
    ++g_dialog_clear_calls;
    g_dialog_generation = generation;
    return qol::contracts::ok();
}
const qol::contracts::UiStateApi kUiStateApi{
    &state_publish_panels, &state_publish_dialog, &state_clear_dialog};

QolResult capability_require(const char* id, QolVersionRange, const void** out_iface) {
    if (id == nullptr || out_iface == nullptr || !g_state_api_available ||
        std::strcmp(id, qol::contracts::kUiStateCapabilityId) != 0) {
        return qol::contracts::error(kQolErrNotFound, "not found", 9);
    }
    *out_iface = &kUiStateApi;
    return qol::contracts::ok();
}

// ---- 真实 Scene 顺序模型 ----
// Scene_Init_POPUP_SC_CHOICE：先 UIChoice_Init（重置 pItemText[]/count/focus/title），
// 再 UIChoice_CreateControl（只消费同一组槽，不改）。fake port 与 fake original Init 写同一组槽。
const char* const* g_game_items = nullptr;
uint32_t g_game_count = 0;
int32_t g_game_focus = -1;
const char* g_game_title = nullptr;
int g_init_original_calls = 0;
int g_game_items_writes = 0;
int g_game_focus_writes = 0;
int g_game_title_writes = 0;
int g_items_write_fail_at = -1;  // 1-based：第 N 次 write_items 失败（-1 = 不失败）
int g_port_calls = 0;            // 任何 choice port 读/写/关闭调用计数（证明零调用边界）
bool g_selected_index_fail = false;
bool g_focus_fail = false;
bool g_text_owns_fail = false;

// 事件轨迹（W=write_items, F=write_focus, T=write_title, I=原 Init, C=CreateControl）：
// 用于证明重写发生在原 Init 之后、CreateControl 之前。
char g_scene_events[64]{};

void scene_event(char event) {
    const size_t used = std::strlen(g_scene_events);
    if (used + 2 < sizeof(g_scene_events)) {
        g_scene_events[used] = event;
        g_scene_events[used + 1] = '\0';
    }
}

void fake_init_original(void* /*control*/) {
    ++g_init_original_calls;
    scene_event('I');
    // 原版 Init 的重置语义：清空 items/count/focus/title（这就是冲掉模块预写数据的那一步）。
    g_game_items = nullptr;
    g_game_count = 0;
    g_game_focus = -1;
    g_game_title = nullptr;
}

// 观察 UIChoice_CreateControl 的最终消费结果。
int g_create_calls = 0;
uint32_t g_create_count = 0;
char g_create_label0[64]{};
const char* g_create_title = nullptr;

void fake_scene_create_control() {
    ++g_create_calls;
    scene_event('C');
    g_create_count = g_game_count;
    g_create_label0[0] = '\0';
    if (g_game_items != nullptr && g_game_count > 0 && g_game_items[0] != nullptr) {
        std::snprintf(g_create_label0, sizeof(g_create_label0), "%s", g_game_items[0]);
    }
    g_create_title = g_game_title;
}

// text_owns 收到的 items 指针（用于逐指针一致性断言）。
const char* const* g_text_owns_items = nullptr;
uint32_t g_text_owns_count = 0;

QolResult choice_state_id(int32_t* out_state_id) {
    ++g_port_calls;
    *out_state_id = g_state_id;
    return qol::contracts::ok();
}
QolResult choice_selected_index(int32_t* out_index) {
    ++g_port_calls;
    *out_index = g_selected_index;
    return g_selected_index_fail ? qol::contracts::error(kQolErrInternal, "index read failed", 16)
                                 : qol::contracts::ok();
}
int32_t g_text_owned = 1;  // 0 = 文本不属于本选择框（foreign_text 分支）
QolResult choice_text_owns(const char* const* items, uint32_t count, int32_t* out_owned) {
    ++g_port_calls;
    g_text_owns_items = items;
    g_text_owns_count = count;
    *out_owned = g_text_owned;
    return g_text_owns_fail ? qol::contracts::error(kQolErrInternal, "owns read failed", 16)
                            : qol::contracts::ok();
}

QolResult choice_write_items(const char* const* items, uint32_t count) {
    ++g_port_calls;
    ++g_game_items_writes;
    scene_event('W');
    if (g_close_during_prepare) {
        g_close_during_prepare = false;
        qol::features::core_ui::native_choice_close();
    }
    if (g_items_write_fail_at > 0 && g_game_items_writes == g_items_write_fail_at) {
        return qol::contracts::error(kQolErrNotReady, "items write failed", 17);
    }
    g_game_items = items;
    g_game_count = count;
    return qol::contracts::ok();
}
QolResult choice_write_focus(int32_t index) {
    ++g_port_calls;
    ++g_game_focus_writes;
    scene_event('F');
    if (g_focus_fail) return qol::contracts::error(kQolErrInternal, "focus write failed", 17);
    g_game_focus = index;
    return qol::contracts::ok();
}
QolResult choice_write_title(const char* title) {
    ++g_port_calls;
    ++g_game_title_writes;
    scene_event('T');
    g_title_pointer = title;  // 游戏保存指针而非复制标题。
    std::snprintf(g_written_title, sizeof(g_written_title), "%s", title);
    g_game_title = title;
    return qol::contracts::ok();
}
QolResult choice_push() {
    ++g_port_calls;
    ++g_push_calls;
    qol::features::core_ui::choice_init_wrapper(nullptr);
    g_push_initialized = qol::features::core_ui::native_choice_active();
    return g_push_succeeds ? qol::contracts::ok()
                           : qol::contracts::error(kQolErrInternal, "push failed", 11);
}
QolResult choice_close_popup() {
    ++g_port_calls;
    ++g_close_calls;
    return qol::contracts::ok();
}

// ---- 日志桩：观察失败分支 warn / 成功 debug（不引入新日志框架，仅记录 SDK 回调内容）----
int g_log_calls = 0;
int32_t g_log_level = -1;
int32_t g_log_domain = -1;
// 多条日志按行追加（同一用例可能同时有 fallback 原因与 original=null 两条）。
char g_log_message[1024]{};
const char* g_log_source = nullptr;

QolResult fake_log_write(int32_t level, int32_t domain, const char* source, const char* message,
                         uint32_t message_len) {
    ++g_log_calls;
    g_log_level = level;
    g_log_domain = domain;
    g_log_source = source;
    const size_t used = std::strlen(g_log_message);
    if (message != nullptr && used + message_len + 2 < sizeof(g_log_message)) {
        const size_t offset = used == 0 ? 0 : used + 1;
        if (used != 0) g_log_message[used] = '\n';
        std::memcpy(g_log_message + offset, message, message_len);
        g_log_message[offset + message_len] = '\0';
    }
    return qol::contracts::ok();
}

bool log_contains(const char* needle) { return std::strstr(g_log_message, needle) != nullptr; }

void reset_log() {
    g_log_calls = 0;
    g_log_level = -1;
    g_log_domain = -1;
    g_log_source = nullptr;
    g_log_message[0] = '\0';
}

// ---- 原生按钮 ExecuteProc 替身：必须收到与调用方相同的控件指针 ----
int g_original_calls = 0;
void* g_original_control = nullptr;

void fake_button_original(void* control) {
    ++g_original_calls;
    g_original_control = control;
}

bool setup_choice_feature() {
    static qol::contracts::QolUiAdapter ui{};
    static const void* ports[] = {&g_choice_port};
    ui.size = sizeof(ui);
    ui.major = qol::contracts::kUiContractMajor;
    ui.minor = qol::contracts::kUiAdapterMinor;
    g_sdk.ui = &ui;
    g_sdk.capabilities.require = &capability_require;
    g_sdk.log.write = &fake_log_write;
    qol::contracts::PluginContext context{};
    context.sdk = &g_sdk;
    context.ports = ports;
    context.port_count = 1;
    const bool installed = qol::features::core_ui::plugin_descriptor().install(context) == 0;
    qol::features::core_ui::native_choice_set_installed(true);
    return installed;
}

void reset_fake_choice() {
    g_choice_port = {};
    g_choice_port.read.state_id = &choice_state_id;
    g_choice_port.read.selected_index = &choice_selected_index;
    g_choice_port.read.text_owns = &choice_text_owns;
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
    g_state_api_available = true;
    g_dialog_publish_calls = 0;
    g_dialog_clear_calls = 0;
    g_select_calls = 0;
    g_selected_index = 0;
    g_dialog_generation = 0;
    g_dialog_type[0] = '\0';
    g_dialog_title[0] = '\0';
    g_dialog_option_count = 0;
    for (auto& label : g_dialog_labels) label[0] = '\0';
    g_text_owned = 1;
    g_original_calls = 0;
    g_original_control = nullptr;
    g_select_index = -1;
    reset_log();
    g_game_items = nullptr;
    g_game_count = 0;
    g_game_focus = -1;
    g_game_title = nullptr;
    g_init_original_calls = 0;
    g_game_items_writes = 0;
    g_game_focus_writes = 0;
    g_game_title_writes = 0;
    g_items_write_fail_at = -1;
    g_port_calls = 0;
    g_selected_index_fail = false;
    g_focus_fail = false;
    g_text_owns_fail = false;
    g_create_calls = 0;
    g_create_count = 0;
    g_create_label0[0] = '\0';
    g_create_title = nullptr;
    g_text_owns_items = nullptr;
    g_text_owns_count = 0;
    g_select_user = nullptr;
    g_scene_events[0] = '\0';
}

#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (0)

void on_select(int index, void* user) {
    ++g_select_calls;
    g_select_index = index;
    g_select_user = user;
}

UiChoiceSpec valid_spec_of(int count = 2, int close_index = 1) {
    static const char* const items[] = {"one", "two", "three", "four", "five", "six"};
    return {items, count, "title", close_index, &on_select, nullptr, "test.choice"};
}

static_assert(offsetof(UiChoiceSpec, dialog_type_id) ==
                  offsetof(UiChoiceSpec, user) + sizeof(UiChoiceSpec::user),
              "dialog_type_id must be appended after the frozen UiChoiceSpec prefix");
static_assert(offsetof(UiChoiceSpec, items) == 0 &&
                  offsetof(UiChoiceSpec, items) < offsetof(UiChoiceSpec, count) &&
                  offsetof(UiChoiceSpec, count) < offsetof(UiChoiceSpec, title) &&
                  offsetof(UiChoiceSpec, title) < offsetof(UiChoiceSpec, close_index) &&
                  offsetof(UiChoiceSpec, close_index) < offsetof(UiChoiceSpec, on_select) &&
                  offsetof(UiChoiceSpec, on_select) < offsetof(UiChoiceSpec, user),
              "v1.0 UiChoiceSpec field prefix must retain order");
static_assert(std::is_standard_layout<UiChoiceSpec>::value &&
                  std::is_trivially_copyable<UiChoiceSpec>::value);

void test_spec_bounds() {
    CHECK(!valid_spec(valid_spec_of(0)));
    CHECK(valid_spec(valid_spec_of(1, -1)));
    CHECK(valid_spec(valid_spec_of(6, 5)));
    CHECK(!valid_spec(valid_spec_of(7)));
    CHECK(!valid_spec(valid_spec_of(2, 2)));
    UiChoiceSpec missing_id = valid_spec_of();
    missing_id.dialog_type_id = nullptr;
    CHECK(!qol::features::core_ui::native_choice_open(missing_id));
    UiChoiceSpec unstable_id = valid_spec_of();
    unstable_id.dialog_type_id = "world teleport";
    CHECK(!qol::features::core_ui::native_choice_open(unstable_id));
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
#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS
    CHECK(d.requires_count == 0 && d.optional_count == 3);
#else
    CHECK(d.requires_count == 0 && d.optional_count == 1);  // ui.state 为单向可选依赖
#endif
    CHECK(std::strcmp(d.optional_requires[0].id, qol::contracts::kUiStateCapabilityId) == 0);
    CHECK(d.optional_requires[0].range.major == qol::contracts::kUiStateVersionMajor &&
          d.optional_requires[0].range.min_minor == qol::contracts::kUiStateVersionMinor);
#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS
    CHECK(std::strcmp(d.optional_requires[1].id, qol::contracts::kHttpRegistryCapabilityId) == 0);
    CHECK(std::strcmp(d.optional_requires[2].id, qol::contracts::kHttpDispatchCapabilityId) == 0);
    CHECK(d.http_endpoint_count == 3 && d.http_endpoints != nullptr);
    const char* const debug_paths[] = {
        "/api/debug/custom-ui/components", "/api/debug/custom-ui/scroll",
        "/api/debug/custom-ui/help",
    };
    for (uint32_t index = 0; index < d.http_endpoint_count; ++index) {
        CHECK(d.http_endpoints[index].method == qol::contracts::HttpMethod::kPost);
        CHECK(std::strcmp(d.http_endpoints[index].path, debug_paths[index]) == 0);
        CHECK(std::strcmp(d.http_endpoints[index].route_owner, "core.custom_ui") == 0);
        CHECK(d.http_endpoints[index].kind == qol::contracts::HttpHandlerKind::kCommand);
        CHECK(d.http_endpoints[index].command_handler != nullptr);
    }
#else
    CHECK(d.http_endpoint_count == 0 && d.http_endpoints == nullptr);
#endif
    CHECK(d.http_query_count == 0 && d.http_queries == nullptr);
    CHECK(d.http_route_metadata_count == 0 && d.http_route_metadata == nullptr);
    CHECK(d.provides_count == 4);  // ui_components + ui_panels + ui_choice + text_layer
    CHECK(std::strcmp(d.provides[2].id, qol::contracts::kUiChoiceCapabilityId) == 0);
    CHECK(d.provides[2].version.major == qol::contracts::kUiChoiceVersionMajor);
    CHECK(d.provides[2].version.minor == qol::contracts::kUiChoiceVersionMinor);
    CHECK(kUiChoiceVersionMinor == 1);
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

// ---- 绑定/解绑原生按钮原函数（经 production on_resource_ready，与宿主交还 backup 同路径）----
qol::contracts::PluginContext& fixture_context() {
    static qol::contracts::PluginContext context{};
    static bool initialized = false;
    if (!initialized) {
        static const void* ports[] = {&g_choice_port};  // 对象地址稳定，reset 只改字段
        context.sdk = &g_sdk;
        context.ports = ports;
        context.port_count = 1;
        initialized = true;
    }
    return context;
}

void bind_button_original(void* backup) {
    CHECK(qol::features::core_ui::on_resource_ready(
              fixture_context(), "ptrslot:g_uichoice_button_list_exe_got", backup) == 0);
}

void bind_init_original(void* backup) {
    CHECK(qol::features::core_ui::on_resource_ready(fixture_context(), "hook:UIChoice_Init",
                                                    backup) == 0);
}

// 真实时序：Scene_Init → push（内部原 Init 重置 → 包装重写）→ Scene CreateControl 消费 globals。
bool scene_open_and_create(const UiChoiceSpec& spec) {
    const bool opened = qol::features::core_ui::native_choice_open(spec);
    fake_scene_create_control();
    return opened;
}

// 冻结1：无 module active → 原样透传原函数，零 choice port 调用、不调模块回调。
void test_native_inactive_passthrough_zero_port_calls() {
    bind_button_original(reinterpret_cast<void*>(&fake_button_original));
    qol::features::core_ui::native_choice_close();
    CHECK(!qol::features::core_ui::native_choice_active());
    reset_log();
    g_original_calls = 0;
    g_port_calls = 0;
    const int select_before = g_select_calls;
    void* const control = reinterpret_cast<void*>(0x6000);
    qol::features::core_ui::choice_button_execute(control);
    CHECK(g_original_calls == 1 && g_original_control == control);  // 原样透传
    CHECK(g_port_calls == 0);                                      // 零 port 读/写/关闭
    CHECK(g_select_calls == select_before);                        // 不调模块回调
    CHECK(!log_contains("module blocked") && !log_contains("foreign passthrough"));
    bind_button_original(nullptr);
}

// 冻结2：module active 但 text_owns 明确 false（foreign 旁路）→ 清本地状态后透传原函数，
// 不写原生 items/focus、不关原生 popup、不调模块回调。
void test_foreign_text_passthrough_no_game_write() {
    bind_button_original(reinterpret_cast<void*>(&fake_button_original));
    CHECK(qol::features::core_ui::native_choice_open(valid_spec_of(5, 1)));
    g_text_owned = 0;
    const int items_before = g_game_items_writes;
    const int focus_before = g_game_focus_writes;
    const int close_before = g_close_calls;
    const int select_before = g_select_calls;
    reset_log();
    g_original_calls = 0;
    void* const control = reinterpret_cast<void*>(0x6100);
    qol::features::core_ui::choice_button_execute(control);
    CHECK(g_original_calls == 1 && g_original_control == control);  // 透传原函数
    CHECK(g_game_items_writes == items_before);                    // 不写原生 items
    CHECK(g_game_focus_writes == focus_before);                    // 不写原生 focus
    CHECK(g_close_calls == close_before);                          // 不关原生 popup
    CHECK(g_select_calls == select_before);                        // 不调模块回调
    CHECK(!qol::features::core_ui::native_choice_active());         // 本地状态已清
    CHECK(log_contains("foreign passthrough"));
    g_text_owned = 1;
    bind_button_original(nullptr);
}

// 冻结3：已确认 module owned → 任何情况都不调用原生原函数；失败保留模块状态供下一次点击。
void test_owned_never_original_and_failures_keep_state() {
    bind_button_original(reinterpret_cast<void*>(&fake_button_original));

    // owned 成功（非 close）
    CHECK(qol::features::core_ui::native_choice_open(valid_spec_of(5, 1)));
    g_selected_index = 2;
    reset_log();
    g_original_calls = 0;
    int select_before = g_select_calls;
    qol::features::core_ui::choice_button_execute(reinterpret_cast<void*>(0x6200));
    CHECK(g_original_calls == 0);                                   // 绝不回落原生
    CHECK(g_select_calls == select_before + 1 && g_select_index == 2);
    CHECK(!qol::features::core_ui::native_choice_active());

    // owned close 索引：只清自身状态 + sound/close，不 on_select、不 original
    CHECK(qol::features::core_ui::native_choice_open(valid_spec_of(5, 1)));
    g_selected_index = 1;  // close_index
    reset_log();
    g_original_calls = 0;
    select_before = g_select_calls;
    const int clear_before = g_dialog_clear_calls;
    qol::features::core_ui::choice_button_execute(reinterpret_cast<void*>(0x6201));
    CHECK(g_original_calls == 0);
    CHECK(g_select_calls == select_before);                         // close 不 on_select
    CHECK(!qol::features::core_ui::native_choice_active());
    CHECK(g_dialog_clear_calls == clear_before + 1);
    CHECK(log_contains("module close index=1"));

    // owned 读 index 失败 → 保 active、不 original、不关 Game
    CHECK(qol::features::core_ui::native_choice_open(valid_spec_of(5, 1)));
    g_selected_index_fail = true;
    reset_log();
    g_original_calls = 0;
    const int close_before_read_fail = g_close_calls;
    qol::features::core_ui::choice_button_execute(reinterpret_cast<void*>(0x6202));
    CHECK(g_original_calls == 0);
    CHECK(qol::features::core_ui::native_choice_active());
    CHECK(g_close_calls == close_before_read_fail);
    CHECK(log_contains("module blocked reason=selected_index"));
    g_selected_index_fail = false;

    // owned index 越界 → 保 active、不 original
    g_selected_index = -1;
    reset_log();
    g_original_calls = 0;
    qol::features::core_ui::choice_button_execute(reinterpret_cast<void*>(0x6203));
    CHECK(g_original_calls == 0);
    CHECK(qol::features::core_ui::native_choice_active());
    CHECK(log_contains("module blocked reason=index"));
    g_selected_index = 0;

    // owned focus 写失败 → 保 active、不 original、不关 Game
    g_focus_fail = true;
    reset_log();
    g_original_calls = 0;
    qol::features::core_ui::choice_button_execute(reinterpret_cast<void*>(0x6204));
    CHECK(g_original_calls == 0);
    CHECK(qol::features::core_ui::native_choice_active());
    CHECK(g_close_calls == close_before_read_fail);
    CHECK(log_contains("module blocked reason=focus"));
    g_focus_fail = false;

    // ownership 读取失败 → 不猜 owner、不写/关 Game、不 callback、不 original，保 active
    g_text_owns_fail = true;
    reset_log();
    g_original_calls = 0;
    const int focus_before_owns_fail = g_game_focus_writes;
    qol::features::core_ui::choice_button_execute(reinterpret_cast<void*>(0x6205));
    CHECK(g_original_calls == 0);
    CHECK(qol::features::core_ui::native_choice_active());
    CHECK(g_game_focus_writes == focus_before_owns_fail);
    CHECK(g_close_calls == close_before_read_fail);
    CHECK(log_contains("module blocked reason=owner_read_failed"));
    g_text_owns_fail = false;

    // ownership 读面不可用（无 text_owns）→ 同上，保 active
    g_choice_port.read.text_owns = nullptr;
    reset_log();
    g_original_calls = 0;
    const int focus_before_unavailable = g_game_focus_writes;
    qol::features::core_ui::choice_button_execute(reinterpret_cast<void*>(0x6206));
    CHECK(g_original_calls == 0);
    CHECK(qol::features::core_ui::native_choice_active());
    CHECK(g_game_focus_writes == focus_before_unavailable);
    CHECK(g_close_calls == close_before_read_fail);
    CHECK(log_contains("module blocked reason=owner_unavailable"));
    g_choice_port.read.text_owns = &choice_text_owns;

    // 缺模块写/关闭函数 → fail module 路径，不借 original，保 active
    g_choice_port.write.close_popup = nullptr;
    reset_log();
    g_original_calls = 0;
    qol::features::core_ui::choice_button_execute(reinterpret_cast<void*>(0x6207));
    CHECK(g_original_calls == 0);
    CHECK(qol::features::core_ui::native_choice_active());
    CHECK(log_contains("module blocked reason=missing_module_fn"));
    g_choice_port.write.close_popup = &choice_close_popup;

    qol::features::core_ui::native_choice_close();
    bind_button_original(nullptr);
}

void test_module_select_path_skips_original_and_logs_index() {
    bind_button_original(reinterpret_cast<void*>(&fake_button_original));  // 有 backup 也不该走原函数
    CHECK(qol::features::core_ui::native_choice_open(valid_spec_of()));
    g_selected_index = 0;  // 有效且非 close（close_index=1）→ 模块选择路径
    const int select_before = g_select_calls;
    const int close_before = g_close_calls;
    reset_log();
    g_original_calls = 0;
    qol::features::core_ui::choice_button_execute(reinterpret_cast<void*>(0x4000));
    CHECK(g_select_calls == select_before + 1);
    CHECK(g_select_index == 0);                                     // 模块回调收到选中索引
    CHECK(g_original_calls == 0);                                   // 成功路径不回落原函数
    CHECK(g_close_calls == close_before + 1);                       // close_popup 调用不变
    CHECK(log_contains("module select index=0"));
    CHECK(g_log_level == 0);                                        // 成功路径极简 debug
    bind_button_original(nullptr);
}

// 真实时序：Init 冲掉预写数据后，模块在 Init/CreateControl 之间重写 items/count/focus(+标题)，
// CreateControl 因此看到 module 的 5 项与正确标题；选择非 close 索引调用 module 回调而非原函数。
void test_real_scene_sequence_restores_module_items_after_init() {
    bind_init_original(reinterpret_cast<void*>(&fake_init_original));
    UiChoiceSpec spec = valid_spec_of(5, 1);
    static int user_marker = 0;
    spec.user = &user_marker;
    g_scene_events[0] = '\0';
    reset_log();
    CHECK(scene_open_and_create(spec));
    CHECK(g_init_original_calls == 1);                       // 原 Init 只调一次
    CHECK(std::strcmp(g_scene_events, "WFIWFTC") == 0);      // 重写在 Init 之后、CreateControl 之前
    CHECK(g_create_count == 5);                              // CreateControl 看到 module 5 项
    CHECK(std::strcmp(g_create_label0, "one") == 0);
    CHECK(g_game_title != nullptr && std::strcmp(g_game_title, "title") == 0);
    CHECK(g_create_title == g_game_title);                   // 标题来自 module 静态缓冲
    CHECK(g_game_focus == 0);                                // Init 的 focus 重置被恢复

    const int select_before = g_select_calls;
    g_selected_index = 2;                                    // 非 close 索引（close_index=1）
    qol::features::core_ui::choice_button_execute(reinterpret_cast<void*>(0x5000));
    CHECK(g_select_calls == select_before + 1);
    CHECK(g_select_index == 2);                              // 正确的 on_select(index, ...)
    CHECK(g_select_user == &user_marker);                    // user 原样透传
    CHECK(g_original_calls == 0);                            // 不回落原函数
    CHECK(g_text_owns_items == g_game_items && g_text_owns_count == 5);  // 逐指针相同
    CHECK(g_text_owns_items[0] == g_game_items[0] &&
          std::strcmp(g_text_owns_items[0], "one") == 0);
    bind_init_original(nullptr);
}

// 无标题 spec：同样恢复 items（不因缺 write_title 门禁跳过），且不调用 write_title。
void test_no_title_spec_restores_items_without_write_title() {
    bind_init_original(reinterpret_cast<void*>(&fake_init_original));
    UiChoiceSpec spec = valid_spec_of(5, 1);
    spec.title = nullptr;
    g_scene_events[0] = '\0';
    CHECK(scene_open_and_create(spec));
    CHECK(std::strcmp(g_scene_events, "WFIWFC") == 0);       // 无 T（未调用 write_title）
    CHECK(g_create_count == 5);                              // items 仍被恢复
    CHECK(std::strcmp(g_create_label0, "one") == 0);
    CHECK(g_create_title == nullptr);
    qol::features::core_ui::native_choice_close();
    bind_init_original(nullptr);
}

// 非 module 的 Init：只透传原函数，不覆写游戏槽位。
void test_non_module_init_pass_through_does_not_overwrite() {
    bind_init_original(reinterpret_cast<void*>(&fake_init_original));
    qol::features::core_ui::native_choice_close();           // 无 pending module 状态
    const char* const* const sentinel_items = reinterpret_cast<const char* const*>(0x1);
    const char* const sentinel_title = reinterpret_cast<const char*>(0x2);
    g_game_items = sentinel_items;
    g_game_count = 42;
    g_game_focus = 7;
    g_game_title = sentinel_title;
    const int items_writes_before = g_game_items_writes;
    const int focus_writes_before = g_game_focus_writes;
    const int title_writes_before = g_game_title_writes;
    const int init_before = g_init_original_calls;
    qol::features::core_ui::choice_init_wrapper(nullptr);
    CHECK(g_init_original_calls == init_before + 1);         // 原函数只透传
    // 非 module：只有原 Init 自身的重置生效（哨兵被 Init 清掉），模块侧不得再写回任何数据。
    CHECK(g_game_items == nullptr && g_game_count == 0);
    CHECK(g_game_focus == -1 && g_game_title == nullptr);
    CHECK(g_game_items_writes == items_writes_before);       // 零 port 写（未覆写）
    CHECK(sentinel_items != nullptr && sentinel_title != nullptr);
    CHECK(g_game_focus_writes == focus_writes_before);
    CHECK(g_game_title_writes == title_writes_before);
    bind_init_original(nullptr);
}

// post-Init 重写失败：不报告模块 open 成功、不调用 module 回调，并按同 generation 收口。
void test_post_init_write_failure_not_reported_ready() {
    bind_init_original(reinterpret_cast<void*>(&fake_init_original));
    g_items_write_fail_at = g_game_items_writes + 2;         // 预检写成功，紧接的重写失败
    const int select_before = g_select_calls;
    const int publish_before = g_dialog_publish_calls;
    reset_log();
    CHECK(!qol::features::core_ui::native_choice_open(valid_spec_of(5, 1)));  // 不报告成功
    CHECK(!qol::features::core_ui::native_choice_active());                    // 同 generation 清 active
    CHECK(g_dialog_publish_calls == publish_before);                           // 未发布模块快照
    CHECK(g_select_calls == select_before);                                    // 未调用 module 回调
    CHECK(log_contains("reassert failed") && log_contains("stage=items"));
    CHECK(log_contains("backup=1") || log_contains("backup=0"));
    g_items_write_fail_at = -1;
    bind_init_original(nullptr);
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
    CHECK(g_dialog_publish_calls == 1 && g_dialog_option_count == 2);
    CHECK(std::strcmp(g_dialog_type, "test.choice") == 0);
    CHECK(std::strcmp(g_dialog_title, "title") == 0);
    CHECK(std::strcmp(g_dialog_labels[0], "one") == 0 &&
          std::strcmp(g_dialog_labels[1], "two") == 0);
    CHECK(!qol::features::core_ui::native_choice_open(spec));
    qol::features::core_ui::native_choice_close();
    CHECK(!qol::features::core_ui::native_choice_active());
    CHECK(g_close_calls == 1);
    CHECK(g_dialog_clear_calls == 1 && g_dialog_generation != 0);
}

void test_optional_registry_failure_keeps_native_choice() {
    g_state_api_available = false;
    const int published_before = g_dialog_publish_calls;
    CHECK(qol::features::core_ui::native_choice_open(valid_spec_of()));
    CHECK(g_dialog_publish_calls == published_before);
    CHECK(qol::features::core_ui::native_choice_active());
    qol::features::core_ui::native_choice_close();
    g_state_api_available = true;
}

void test_select_clears_matching_dialog() {
    const int clears_before = g_dialog_clear_calls;
    CHECK(qol::features::core_ui::native_choice_open(valid_spec_of()));
    const uint64_t generation = g_dialog_generation;
    CHECK(generation != 0);
    qol::features::core_ui::choice_button_execute(nullptr);
    CHECK(g_select_calls == 1);
    CHECK(!qol::features::core_ui::native_choice_active());
    CHECK(g_dialog_clear_calls == clears_before + 1);
    CHECK(g_dialog_generation == generation);
}

void test_push_failure_rolls_back() {
    const int publish_count_before = g_dialog_publish_calls;
    g_push_succeeds = false;
    CHECK(!qol::features::core_ui::native_choice_open(valid_spec_of()));
    CHECK(!qol::features::core_ui::native_choice_active());
    CHECK(g_dialog_publish_calls == publish_count_before);
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
    const int clears_before_deactivate = g_dialog_clear_calls;
    qol::features::core_ui::native_choice_set_installed(false);
    CHECK(!qol::features::core_ui::native_choice_active());
    CHECK(g_dialog_clear_calls == clears_before_deactivate + 1);
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
    test_optional_registry_failure_keeps_native_choice();
    test_select_clears_matching_dialog();
    test_push_failure_rolls_back();
    test_close_and_reopen_gate();
    test_close_during_prepare_releases_open_gate();
    test_native_inactive_passthrough_zero_port_calls();
    test_foreign_text_passthrough_no_game_write();
    test_owned_never_original_and_failures_keep_state();
    test_module_select_path_skips_original_and_logs_index();
    test_real_scene_sequence_restores_module_items_after_init();
    test_no_title_spec_restores_items_without_write_title();
    test_non_module_init_pass_through_does_not_overwrite();
    test_post_init_write_failure_not_reported_ready();
    std::printf("native_choice_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

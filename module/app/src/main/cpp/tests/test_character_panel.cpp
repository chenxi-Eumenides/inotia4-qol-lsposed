// plugins/character_panel 插件 host 单测：
//   1. 清单：requires/ports/resources/features 与两条资源声明；
//   2. 汉化：标签表（连续性 + 35188 占位）、backup 转发、作用域 push/pop、消费者注册失败；
//   3. 属性总览纯逻辑：分区行数与分页映射、千分比格式化、内容区命中判定。
// 文本机制本身归 core.custom_ui（其清单由 native_choice_tests 断言）；游戏内存读取与
// 真机行为不在本测试覆盖范围。

#include "plugins/character_panel/plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/host_sdk.h"
#include "contracts/panels.h"
#include "contracts/plugin.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/character_panel/charinfo_scope.h"
#include "plugins/character_panel/charinfo_watch.h"
#include "plugins/character_panel/feature.h"
#include "plugins/character_panel/overview.h"
#include "plugins/character_panel/overview_model.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

using qol::contracts::ResourceFailure;
using qol::contracts::ResourceMechanism;
using qol::contracts::ResourcePolicy;
using qol::contracts::ResourceRequirement;
namespace i18n = qol::features::character_panel;

constexpr int32_t kPanel = qol::contracts::kTextScopeCharacterPanel;

// ---- 属性总览点击链路 stub：假 Host SDK / PopupState / ui_panels / ui 适配表 ----
int g_watch_resolve_calls = 0;
int g_watch_acquire_calls = 0;
int g_watch_restore_calls = 0;
int g_watch_original_enter_calls = 0;
int g_watch_original_event_calls = 0;
uint32_t g_watch_original_event_result = 7;
bool g_watch_hooks_enter_present = false;
bool g_watch_hooks_event_present = false;
bool g_watch_hooks_partial_wrap = false;
int g_watch_open_calls = 0;
const char* g_watch_last_open_id = nullptr;
uint8_t g_watch_slot_storage = 0;

void watch_original_enter() { ++g_watch_original_enter_calls; }

uint32_t watch_original_event(uint64_t, uint64_t, uint64_t) {
    ++g_watch_original_event_calls;
    return g_watch_original_event_result;
}

QolResult watch_popup_resolve(const char* symbol, qol::contracts::QolPopupStateEntry* entry) {
    ++g_watch_resolve_calls;
    if (symbol == nullptr || entry == nullptr ||
        std::strcmp(symbol, "F_PANEL_CHARACTER_INFO_ENTER") != 0) {
        return qol::contracts::error(kQolErrNotFound, "missing symbol", 14);
    }
    std::memset(entry, 0, sizeof(*entry));
    entry->slot = &g_watch_slot_storage;
    entry->state_id = 42;
    // 宿主快照：backup 中保留原 enter/event 入口（wrapper 从这里转发）。
    const auto enter = &watch_original_enter;
    const auto event = &watch_original_event;
    std::memcpy(entry->backup + ::kPopupStateEntryEnterOffset, &enter,
                sizeof(enter));
    std::memcpy(entry->backup + ::kPopupStateEntryEventOffset, &event,
                sizeof(event));
    return qol::contracts::ok();
}

QolResult watch_popup_acquire(qol::contracts::QolPopupStateEntry* entry,
                              const qol::contracts::QolPopupStateHooks* hooks) {
    ++g_watch_acquire_calls;
    if (entry == nullptr || entry->slot == nullptr || hooks == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad acquire", 11);
    }
    g_watch_hooks_enter_present = hooks->enter != nullptr;
    g_watch_hooks_event_present = hooks->event != nullptr;
    g_watch_hooks_partial_wrap =
        hooks->process == nullptr && hooks->f3 == nullptr && hooks->f4 == nullptr;
    return qol::contracts::ok();
}

QolResult watch_popup_restore(qol::contracts::QolPopupStateEntry* entry) {
    ++g_watch_restore_calls;
    if (entry == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad restore", 11);
    return qol::contracts::ok();
}

bool watch_panel_open(const char* panel_id) {
    ++g_watch_open_calls;
    g_watch_last_open_id = panel_id;
    return panel_id != nullptr;
}
void watch_panel_close(const char*) {}
bool watch_panel_active(const char*) { return false; }
const char* watch_panel_block_reason() { return nullptr; }
bool watch_panel_deactivate_owner(const char*) { return true; }
bool watch_panel_back() { return true; }

const qol::contracts::UiPanelsApi g_watch_panels_api{
    &watch_panel_open, &watch_panel_close, &watch_panel_active,
    &watch_panel_block_reason, &watch_panel_deactivate_owner, &watch_panel_back};

QolResult watch_require(const char* id, QolVersionRange range, const void** out_iface) {
    if (out_iface == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "out", 3);
    if (id != nullptr && std::strcmp(id, qol::contracts::kUiPanelsCapabilityId) == 0) {
        *out_iface = &g_watch_panels_api;
        return qol::contracts::ok();
    }
    (void)range;
    *out_iface = nullptr;
    return qol::contracts::error(kQolErrNotFound, "unsupported", 11);
}

int64_t g_watch_touch_x = 0;
int64_t g_watch_touch_y = 0;
qol::contracts::UiRect g_watch_content_rect{100, 200, 300, 400};

QolResult watch_touch_coordinates(uint64_t param, int64_t* x, int64_t* y) {
    if (param == 0 || x == nullptr || y == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "touch", 5);
    }
    *x = g_watch_touch_x;
    *y = g_watch_touch_y;
    return qol::contracts::ok();
}

QolResult watch_content_rect(qol::contracts::UiRect* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "rect", 4);
    *out = g_watch_content_rect;
    return qol::contracts::ok();
}

qol::contracts::QolUiAdapter g_watch_ui{};

void bind_watch_stub_sdk() {
    static qol::contracts::QolHostSdk sdk{};
    sdk.hooks.popup_resolve = &watch_popup_resolve;
    sdk.hooks.popup_acquire = &watch_popup_acquire;
    sdk.hooks.popup_restore = &watch_popup_restore;
    sdk.capabilities.require = &watch_require;
    g_watch_ui.size = sizeof(g_watch_ui);
    g_watch_ui.major = qol::contracts::kUiAdapterMajor;
    g_watch_ui.minor = qol::contracts::kUiAdapterMinor;
    g_watch_ui.touch_coordinates = &watch_touch_coordinates;
    g_watch_ui.character_info_content_rect = &watch_content_rect;
    sdk.ui = &g_watch_ui;
    i18n::bind_sdk(&sdk);
}

void reset_watch_stub() {
    i18n::character_info_test_reset();
    i18n::set_overview_enabled(true);
    g_watch_resolve_calls = 0;
    g_watch_acquire_calls = 0;
    g_watch_restore_calls = 0;
    g_watch_original_enter_calls = 0;
    g_watch_original_event_calls = 0;
    g_watch_original_event_result = 7;
    g_watch_hooks_enter_present = false;
    g_watch_hooks_event_present = false;
    g_watch_hooks_partial_wrap = false;
    g_watch_open_calls = 0;
    g_watch_last_open_id = nullptr;
    g_watch_touch_x = 0;
    g_watch_touch_y = 0;
    g_watch_content_rect = {100, 200, 300, 400};
}


int g_push_count = 0;
int g_pop_count = 0;
int32_t g_last_push_scope = -1;
bool g_push_ok = true;

QolResult stub_push(int32_t scope) {
    ++g_push_count;
    g_last_push_scope = scope;
    if (!g_push_ok) return qol::contracts::error(kQolErrBlocked, "blocked", 7);
    return qol::contracts::ok();
}

QolResult stub_pop() {
    ++g_pop_count;
    return qol::contracts::ok();
}

bool g_scene_draw_called = false;
bool g_allow_nested = false;
bool g_nested_done = false;

// 原绘制函数替身；g_allow_nested 时在原调用内部再次进入同一 wrapper，验证作用域嵌套恢复。
void fake_scene_draw() {
    g_scene_draw_called = true;
    if (g_allow_nested && !g_nested_done) {
        g_nested_done = true;
        i18n::scene_draw_charinfo_wrapper();
    }
}

void bind_stub_sdk() {
    static qol::contracts::QolHostSdk sdk{};
    sdk.text_scope.push = &stub_push;
    sdk.text_scope.pop = &stub_pop;
    i18n::bind_sdk(&sdk);
}

void reset_state() {
    g_push_count = 0;
    g_pop_count = 0;
    g_last_push_scope = -1;
    g_push_ok = true;
    g_scene_draw_called = false;
    g_allow_nested = false;
    g_nested_done = false;
    i18n::set_active_scope_value(qol::contracts::kTextScopeNone);
}

void test_manifest() {
    const qol::contracts::PluginDescriptor& d = i18n::plugin_descriptor();
    CHECK(std::strcmp(d.id, "character_panel") == 0);
    // requires = text_layer + ui_components + ui_panels；optional 为空。
    CHECK(d.requires_count == 3 && d.optional_count == 0);
    CHECK(d.provides_count == 0);  // 纯消费者
    CHECK(d.resources_count == 2);
    CHECK(d.on_resource_ready != nullptr);
    CHECK(d.port_count == 1 && d.ports != nullptr &&
          d.ports[0].id == qol::contracts::QolPortId::QOL_PORT_CHARACTER_EXTRAS);
    CHECK(d.feature_count == 2);

    const qol::contracts::ResourceClaim& charinfo = d.resources[0];
    CHECK(std::strcmp(charinfo.id, "hook:Scene_Draw_Popup_SC_CharacterInfo") == 0);
    CHECK(std::strcmp(charinfo.symbol, "F_SCENE_DRAW_POPUP_SC_CHARACTER_INFO_VMA") == 0);
    CHECK(charinfo.mechanism == ResourceMechanism::kEntryHook);
    CHECK(charinfo.policy == ResourcePolicy::kPrivate);
    CHECK(charinfo.requirement == ResourceRequirement::kRequired);
    CHECK(charinfo.failure == ResourceFailure::kFailSoft);
    CHECK(charinfo.resumable);
    CHECK(std::strcmp(charinfo.owner, "character_panel") == 0);
    CHECK(charinfo.gate == nullptr);
    CHECK(charinfo.step_budget == 120);
    CHECK(charinfo.handler == reinterpret_cast<void*>(&i18n::scene_draw_charinfo_wrapper));

    // 人物属性面板 PopupState 所有权：id/symbol 同为 own:popup.character_info。
    const qol::contracts::ResourceClaim& ownership = d.resources[1];
    CHECK(std::strcmp(ownership.id, "own:popup.character_info") == 0);
    CHECK(std::strcmp(ownership.symbol, "own:popup.character_info") == 0);
    CHECK(ownership.mechanism == ResourceMechanism::kOwnership);
    CHECK(ownership.policy == ResourcePolicy::kPrivate);
    CHECK(ownership.requirement == ResourceRequirement::kRequired);
    CHECK(ownership.failure == ResourceFailure::kFailSoft);
    CHECK(ownership.resumable);
    CHECK(std::strcmp(ownership.owner, "character_panel") == 0);
    CHECK(ownership.gate == nullptr);
    CHECK(ownership.handler ==
          reinterpret_cast<void*>(const_cast<qol::contracts::OwnershipOps*>(
              &i18n::kCharacterInfoPopupOps)));
    CHECK(i18n::kCharacterInfoPopupOps.acquire == &i18n::character_info_popup_acquire);
    CHECK(i18n::kCharacterInfoPopupOps.release == &i18n::character_info_popup_release);
}

// requires/feature 的 id 与数量：防止重命名后 descriptor 漏改。
void test_manifest_ids() {
    const qol::contracts::PluginDescriptor& d = i18n::plugin_descriptor();
    CHECK(d.requires_count == 3);
    CHECK(std::strcmp(d.requires[0].id, qol::contracts::kTextLayerCapabilityId) == 0);
    CHECK(std::strcmp(d.requires[1].id, qol::contracts::kUiComponentsCapabilityId) == 0);
    CHECK(std::strcmp(d.requires[2].id, qol::contracts::kUiPanelsCapabilityId) == 0);
    CHECK(d.requires[0].range.min_minor == qol::contracts::kTextLayerVersionMinor);
    CHECK(d.requires[1].range.min_minor == qol::contracts::kUiContractMinor);
    CHECK(d.requires[2].range.min_minor == qol::contracts::kUiPanelsVersionMinor);
    CHECK(d.ports != nullptr && d.port_count == 1);
    CHECK(d.ports[0].range.major == qol::contracts::kQolPortCharacterExtrasVersion.major);
    CHECK(d.ports[0].range.min_minor == qol::contracts::kQolPortCharacterExtrasVersion.minor);

    bool has_ownership = false;
    for (uint32_t i = 0; i < d.resources_count; ++i) {
        if (std::strcmp(d.resources[i].id, "own:popup.character_info") == 0 &&
            d.resources[i].mechanism == ResourceMechanism::kOwnership) {
            has_ownership = true;
        }
    }
    CHECK(has_ownership);

    CHECK(d.feature_count == 2 && d.features != nullptr);
    CHECK(std::strcmp(d.features[0].id, "character_panel.i18n") == 0);
    CHECK(std::strcmp(d.features[1].id, "character_panel.overview") == 0);
    CHECK(d.features[0].default_enabled == 1);
    CHECK(d.features[1].default_enabled == 1);
}

// 分区行数 + 分页映射：61 行 → 8 页，逐页校验分区/页内序号/首行/行数。
void test_overview_row_model() {
    CHECK(i18n::kSectionCount == 4);
    CHECK(i18n::kPanelRowCount == 16);
    CHECK(i18n::kHiddenRowCount == 22);
    CHECK(i18n::kElementRowCount == 19);
    CHECK(i18n::kGlobalRowCount == 4);
    CHECK(i18n::kTotalRowCount == 61);
    CHECK(i18n::kMaxRowsPerPage == 10);
    CHECK(i18n::total_page_count() == 8);

    // 每页：分区 / 分区内页序号 / 首行 / 行数。
    struct ExpectedPage {
        uint32_t section;
        uint32_t local_page;
        uint32_t first_row;
        uint32_t row_count;
    };
    const ExpectedPage expected[8] = {
        {0, 0, 0, 10},   // 主属性与面板属性 (1/2)
        {0, 1, 10, 6},   // 主属性与面板属性 (2/2)
        {1, 0, 0, 10},   // 隐藏属性 (1/3)
        {1, 1, 10, 10},  // 隐藏属性 (2/3)
        {1, 2, 20, 2},   // 隐藏属性 (3/3)
        {2, 0, 0, 10},   // 元素与抗性 (1/2)
        {2, 1, 10, 9},   // 元素与抗性 (2/2)
        {3, 0, 0, 4},    // 全局加成 (1/1)
    };
    for (uint32_t page = 0; page < 8; ++page) {
        i18n::PageRef ref{};
        CHECK(i18n::locate_page(page, &ref));
        CHECK(ref.section == expected[page].section);
        CHECK(ref.local_page == expected[page].local_page);
        CHECK(ref.first_row == expected[page].first_row);
        CHECK(ref.row_count == expected[page].row_count);
        // 页内行必须落在分区范围内。
        CHECK(ref.first_row + ref.row_count <= i18n::kSections[ref.section].count);
    }

    // 边界页首尾：P2 首行 10、末行 15；隐藏属性末页首行 20、末行 22；元素末页末行 18。
    i18n::PageRef ref{};
    CHECK(i18n::locate_page(1, &ref) && ref.first_row + ref.row_count == 16);
    CHECK(i18n::locate_page(4, &ref) && ref.first_row == 20 && ref.first_row + ref.row_count == 22);
    CHECK(i18n::locate_page(6, &ref) && ref.first_row == 10 && ref.first_row + ref.row_count == 19);

    // 越界页码不产生页引用，且不越界写 out。
    i18n::PageRef untouched{7, 7, 7, 7};
    CHECK(!i18n::locate_page(8, &untouched));
    CHECK(untouched.section == 7 && untouched.local_page == 7 && untouched.first_row == 7 &&
          untouched.row_count == 7);
    CHECK(!i18n::locate_page(100, nullptr));
    CHECK(i18n::locate_page(0, nullptr));
}

// 格式化：千分比 raw/10 保留 1 位小数 + %；整数；失败 `--`；说明行带注记。
void test_overview_format_value() {
    char buffer[40];
    const i18n::RowSpec& crit = i18n::kPanelRows[8];      // 暴击率（attr 千分比）
    const i18n::RowSpec& hit = i18n::kPanelRows[9];       // 命中率（hit_rate_1000）
    const i18n::RowSpec& defense = i18n::kPanelRows[7];   // 防御力（整数）
    const i18n::RowSpec& hidden = i18n::kHiddenRows[0];   // HP吸收（attr 原值）
    const i18n::RowSpec& global = i18n::kGlobalRows[0];   // 全局加成说明行

    i18n::format_value(crit, true, 1234, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "123.4%") == 0);
    i18n::format_value(crit, true, 0, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "0.0%") == 0);
    i18n::format_value(crit, true, 5, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "0.5%") == 0);
    i18n::format_value(hit, true, 999, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "99.9%") == 0);
    i18n::format_value(crit, true, -5, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "-0.5%") == 0);

    i18n::format_value(defense, true, 4321, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "4321") == 0);
    i18n::format_value(hidden, true, -7, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "-7") == 0);

    i18n::format_value(crit, false, 1234, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "--") == 0);
    i18n::format_value(defense, false, 0, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "--") == 0);

    char note_buffer[128];
    i18n::format_value(global, false, 0, note_buffer, sizeof(note_buffer));
    CHECK(std::strcmp(note_buffer, i18n::kGlobalNote) == 0);
    // 缓冲不足时不得越界写：只校验首字符被写入且仍是 NUL 结尾片段的合法前缀。
    char tiny[4] = {'x', 'x', 'x', 'x'};
    i18n::format_value(crit, true, 1234, tiny, sizeof(tiny));
    CHECK(tiny[sizeof(tiny) - 1] == '\0');
}

// PopupState 装配：acquire 必须传非空 enter/event；enter wrapper 转发原 enter；
// release 调 popup_restore 并清空缓存的原始入口。
void test_popup_assembly() {
    reset_watch_stub();
    bind_watch_stub_sdk();

    CHECK(i18n::character_info_popup_acquire(nullptr) == 0);
    CHECK(g_watch_resolve_calls == 1);
    CHECK(g_watch_acquire_calls == 1);
    CHECK(g_watch_hooks_enter_present);
    CHECK(g_watch_hooks_event_present);
    CHECK(g_watch_hooks_partial_wrap);  // process/f3/f4 保持原函数

    const int enter_before = g_watch_original_enter_calls;
    i18n::character_info_enter_wrapper();
    CHECK(g_watch_original_enter_calls == enter_before + 1);

    // 幂等：重复 acquire 直接成功且不再调用宿主。
    CHECK(i18n::character_info_popup_acquire(nullptr) == 0);
    CHECK(g_watch_acquire_calls == 1);

    i18n::character_info_popup_release(nullptr);
    CHECK(g_watch_restore_calls == 1);
    // release 后缓存的原始 enter/event 已清空：wrapper 不再转发、返回 0。
    const int enter_after_release = g_watch_original_enter_calls;
    i18n::character_info_enter_wrapper();
    CHECK(g_watch_original_enter_calls == enter_after_release);
    CHECK(i18n::character_info_event_wrapper(0x17, 1, 0) == 0);

    i18n::character_info_test_reset();
    i18n::bind_sdk(nullptr);
}

// 点击链路：按下/抬起同区 → pending → tick 打开一次；不同区/移动/开关关闭不打开；
// event 始终返回原函数返回值。
void test_overview_click_flow() {
    reset_watch_stub();
    bind_watch_stub_sdk();
    CHECK(i18n::character_info_popup_acquire(nullptr) == 0);

    // 1) 按下/抬起都在内容区 → 打开一次，tick 两次都保持常驻。
    g_watch_touch_x = 250;
    g_watch_touch_y = 300;
    CHECK(i18n::character_info_event_wrapper(0x17, 1, 0) == g_watch_original_event_result);
    CHECK(i18n::character_info_event_wrapper(0x18, 1, 0) == g_watch_original_event_result);
    CHECK(g_watch_original_event_calls == 2);  // 未被吞掉
    const int opens_after_first_click = g_watch_open_calls;
    CHECK(i18n::character_info_open_tick(0, nullptr) != 0);
    CHECK(g_watch_open_calls == opens_after_first_click + 1);
    CHECK(g_watch_last_open_id != nullptr &&
          std::strcmp(g_watch_last_open_id, "character_panel.overview") == 0);
    CHECK(i18n::character_info_open_tick(1, nullptr) != 0);
    CHECK(g_watch_open_calls == opens_after_first_click + 1);  // 第二次不再打开

    // 2) 按下在内容区外、抬起在内容区内 → 不打开。
    i18n::character_info_reset();
    const int opens_before_outside = g_watch_open_calls;
    g_watch_touch_x = 50;
    g_watch_touch_y = 50;
    CHECK(i18n::character_info_event_wrapper(0x17, 1, 0) == g_watch_original_event_result);
    g_watch_touch_x = 250;
    g_watch_touch_y = 300;
    CHECK(i18n::character_info_event_wrapper(0x18, 1, 0) == g_watch_original_event_result);
    CHECK(i18n::character_info_open_tick(2, nullptr) != 0);
    CHECK(g_watch_open_calls == opens_before_outside);

    // 3) 按下在内容区、移动出内容区、抬起回到内容区 → 按下点失效，不打开。
    i18n::character_info_reset();
    const int opens_before_move = g_watch_open_calls;
    g_watch_touch_x = 250;
    g_watch_touch_y = 300;
    CHECK(i18n::character_info_event_wrapper(0x17, 1, 0) == g_watch_original_event_result);
    g_watch_touch_x = 50;
    g_watch_touch_y = 50;
    CHECK(i18n::character_info_event_wrapper(0x19, 1, 0) == g_watch_original_event_result);
    g_watch_touch_x = 250;
    g_watch_touch_y = 300;
    CHECK(i18n::character_info_event_wrapper(0x18, 1, 0) == g_watch_original_event_result);
    CHECK(i18n::character_info_open_tick(3, nullptr) != 0);
    CHECK(g_watch_open_calls == opens_before_move);

    // 3b) 按下/移动/抬起都在内容区（触屏轻微抖动）→ 仍打开。
    i18n::character_info_reset();
    const int opens_before_jitter = g_watch_open_calls;
    g_watch_touch_x = 250;
    g_watch_touch_y = 300;
    CHECK(i18n::character_info_event_wrapper(0x17, 1, 0) == g_watch_original_event_result);
    g_watch_touch_x = 252;
    g_watch_touch_y = 302;
    CHECK(i18n::character_info_event_wrapper(0x19, 1, 0) == g_watch_original_event_result);
    g_watch_touch_x = 250;
    g_watch_touch_y = 300;
    CHECK(i18n::character_info_event_wrapper(0x18, 1, 0) == g_watch_original_event_result);
    CHECK(i18n::character_info_open_tick(4, nullptr) != 0);
    CHECK(g_watch_open_calls == opens_before_jitter + 1);

    // 4) 总览开关关闭：点击不置 pending，tick 不打开。
    i18n::character_info_reset();
    i18n::set_overview_enabled(false);
    const int opens_before_gate = g_watch_open_calls;
    g_watch_touch_x = 250;
    g_watch_touch_y = 300;
    CHECK(i18n::character_info_event_wrapper(0x17, 1, 0) == g_watch_original_event_result);
    CHECK(i18n::character_info_event_wrapper(0x18, 1, 0) == g_watch_original_event_result);
    CHECK(i18n::character_info_open_tick(4, nullptr) != 0);
    CHECK(g_watch_open_calls == opens_before_gate);
    i18n::set_overview_enabled(true);

    i18n::character_info_test_reset();
    i18n::bind_sdk(nullptr);
}


// 内容区命中：左上包含、右下不包含；空/负尺寸恒不命中。
void test_character_info_tap_inside() {
    const qol::contracts::UiRect rect{100, 200, 300, 400};
    CHECK(i18n::character_info_tap_inside(rect, 250, 300));
    CHECK(i18n::character_info_tap_inside(rect, 100, 200));   // 左上角包含
    CHECK(i18n::character_info_tap_inside(rect, 399, 599));   // 右下内最后一像素
    CHECK(!i18n::character_info_tap_inside(rect, 400, 300));  // 右边界不包含
    CHECK(!i18n::character_info_tap_inside(rect, 300, 600));  // 下边界不包含
    CHECK(!i18n::character_info_tap_inside(rect, 99, 300));
    CHECK(!i18n::character_info_tap_inside(rect, 300, 199));

    CHECK(!i18n::character_info_tap_inside({100, 200, 0, 400}, 100, 200));
    CHECK(!i18n::character_info_tap_inside({100, 200, 300, 0}, 100, 200));
    CHECK(!i18n::character_info_tap_inside({100, 200, -10, 400}, 100, 200));
}

// 标签表：覆盖 35185..35196 连续升序；35188 为 nullptr 占位；未命中/占位返回 nullptr。
void test_table() {
    size_t count = 0;
    const auto* entries = i18n::entries(&count);
    CHECK(count == 12);
    CHECK(entries != nullptr);
    for (size_t i = 0; i < count; ++i) {
        CHECK(entries[i].text_id == static_cast<uint16_t>(35185 + i));
        CHECK(entries[i].scope == kPanel);
    }
    CHECK(std::strcmp(i18n::lookup(35185, kPanel), "物攻") == 0);
    // 占位：35188 显式 nullptr（保持原文），不得替换
    CHECK(i18n::lookup(35188, kPanel) == nullptr);
    CHECK(i18n::entry_by_key("charinfo.magic_def") != nullptr);
    // 作用域不匹配（非面板窗口）不替换
    CHECK(i18n::lookup(35185, qol::contracts::kTextScopeNone) == nullptr);
    CHECK(i18n::lookup(35185, qol::contracts::kTextScopeUimixRecipeButton) == nullptr);
    // 分派阶段/权限数值来自 contracts（不再依赖 hooks 私有枚举）
    CHECK(i18n::kOwnerTablePhase == qol::contracts::kTextPhaseAfter);
    CHECK(i18n::kOwnerTableKind == qol::contracts::kTextConsumerModify);
}

// 消费者回调：按当前线程作用域查表；超 uint16 的 text_id 不得回绕折叠命中面板 id。
void test_owner_table_modify_guard() {
    i18n::set_active_scope_value(kPanel);
    CHECK(std::strcmp(i18n::owner_table_modify(35185u, nullptr, nullptr), "物攻") == 0);
    CHECK(i18n::owner_table_modify(35188u, nullptr, nullptr) == nullptr);  // 占位保持原文
    CHECK(i18n::owner_table_modify(65536u, nullptr, nullptr) == nullptr);  // 超 uint16 不折叠
    CHECK(i18n::owner_table_modify(0x10000u + 35185u, nullptr, nullptr) == nullptr);
    i18n::set_active_scope_value(qol::contracts::kTextScopeNone);
    CHECK(i18n::owner_table_modify(35185u, nullptr, nullptr) == nullptr);  // 非面板作用域
}

// 用户开关门禁：feature on_toggle 立即改写运行时标志，关闭后即使消费者仍在册也不替换文本。
void test_translation_gate() {
    qol::contracts::PluginContext ctx{};
    const qol::contracts::PluginDescriptor& d = i18n::plugin_descriptor();
    CHECK(d.feature_count == 2);
    CHECK(std::strcmp(d.features[0].id, "character_panel.i18n") == 0);
    CHECK(d.features[0].type == qol::contracts::FeatureType::kToggle);
    CHECK(d.features[0].on_toggle != nullptr);

    i18n::set_active_scope_value(kPanel);
    i18n::set_translation_enabled(true);
    CHECK(std::strcmp(i18n::owner_table_modify(35185u, nullptr, nullptr), "物攻") == 0);

    CHECK(d.features[0].on_toggle(ctx, 0).ok == kQolOk);
    CHECK(!i18n::translation_enabled());
    CHECK(i18n::owner_table_modify(35185u, nullptr, nullptr) == nullptr);

    CHECK(d.features[0].on_toggle(ctx, 1).ok == kQolOk);
    CHECK(i18n::translation_enabled());
    CHECK(std::strcmp(i18n::owner_table_modify(35185u, nullptr, nullptr), "物攻") == 0);
    i18n::set_active_scope_value(qol::contracts::kTextScopeNone);
}

// backup 交付与作用域：空 backup 拒绝；非空 → wrapper push(面板)→转发→pop，嵌套恢复。
void test_backup_delivery_and_scope() {
    reset_state();
    bind_stub_sdk();
    qol::contracts::PluginContext ctx{};
    const qol::contracts::PluginDescriptor& d = i18n::plugin_descriptor();

    CHECK(d.on_resource_ready(ctx, "hook:Scene_Draw_Popup_SC_CharacterInfo", nullptr) != 0);
    reset_state();
    CHECK(d.on_resource_ready(ctx, "hook:Scene_Draw_Popup_SC_CharacterInfo",
                              reinterpret_cast<void*>(&fake_scene_draw)) == 0);

    g_allow_nested = true;
    i18n::scene_draw_charinfo_wrapper();
    CHECK(g_scene_draw_called);
    CHECK(g_nested_done);
    CHECK(g_push_count == 2);   // 外层 + 嵌套
    CHECK(g_pop_count == 2);
    CHECK(g_last_push_scope == kPanel);
    CHECK(i18n::active_scope_value() == qol::contracts::kTextScopeNone);  // 恢复上一层
    i18n::bind_sdk(nullptr);
}

// push 失败：不写作用域、不 pop，但仍转发原绘制（fail-soft 透传）。
void test_push_failure_passthrough() {
    reset_state();
    bind_stub_sdk();
    qol::contracts::PluginContext ctx{};
    const qol::contracts::PluginDescriptor& d = i18n::plugin_descriptor();
    CHECK(d.on_resource_ready(ctx, "hook:Scene_Draw_Popup_SC_CharacterInfo",
                              reinterpret_cast<void*>(&fake_scene_draw)) == 0);

    g_push_ok = false;
    i18n::scene_draw_charinfo_wrapper();
    CHECK(g_scene_draw_called);
    CHECK(g_push_count == 1);
    CHECK(g_pop_count == 0);
    CHECK(i18n::active_scope_value() == qol::contracts::kTextScopeNone);
    i18n::bind_sdk(nullptr);
}

// 无 SDK 时 on_enabled 注册失败，fill_status 如实报告（不再恒报 enabled）。
void test_registration_failure_observable() {
    i18n::bind_sdk(nullptr);
    qol::contracts::PluginContext ctx{};
    const qol::contracts::PluginDescriptor& d = i18n::plugin_descriptor();
    d.on_enabled(ctx);
    qol::contracts::PluginStatusOut out{};
    out.enabled = true;
    out.error = nullptr;
    d.fill_status(out);
    CHECK(!out.enabled);
    CHECK(out.error != nullptr);
}

}  // namespace

int main() {
    test_manifest();
    test_table();
    test_owner_table_modify_guard();
    test_translation_gate();
    test_backup_delivery_and_scope();
    test_push_failure_passthrough();
    test_registration_failure_observable();
    test_manifest_ids();
    test_overview_row_model();
    test_overview_format_value();
    test_character_info_tap_inside();
    test_popup_assembly();
    test_overview_click_flow();
    std::printf("character_panel: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

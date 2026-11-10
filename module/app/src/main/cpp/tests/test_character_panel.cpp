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

#include "contracts/character_attributes.h"
#include "contracts/game_ports/character_extras_port.h"
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

// ---- 选中角色解析 stub：menu_character → party_index → party_member 指针比对 ----
void* g_menu_character = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000));
void* g_party_character = reinterpret_cast<void*>(static_cast<uintptr_t>(0x2000));
void* g_party_members[3] = {g_party_character, nullptr, nullptr};
int g_menu_character_calls = 0;
int g_party_member_calls = 0;
int g_party_index_calls = 0;
bool g_menu_character_ok = true;
bool g_menu_character_null = false;
bool g_party_member_ok = true;
bool g_party_index_ok = true;
int32_t g_party_index_value = 0;

QolResult stub_menu_character(void** out) {
    ++g_menu_character_calls;
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "out", 3);
    *out = g_menu_character_null ? nullptr : g_menu_character;
    return g_menu_character_ok ? qol::contracts::ok()
                               : qol::contracts::error(kQolErrNotReady, "menu", 4);
}

QolResult stub_party_index(const void*, int32_t* out) {
    ++g_party_index_calls;
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "out", 3);
    if (!g_party_index_ok) return qol::contracts::error(kQolErrNotReady, "index", 5);
    *out = g_party_index_value;
    return qol::contracts::ok();
}

QolResult stub_party_member(int32_t role, void** out) {
    ++g_party_member_calls;
    if (out == nullptr || role < 0 || role >= 3) {
        return qol::contracts::error(kQolErrInvalidArgument, "role", 4);
    }
    if (!g_party_member_ok || g_party_members[role] == nullptr) {
        return qol::contracts::error(kQolErrNotReady, "party", 5);
    }
    *out = g_party_members[role];
    return qol::contracts::ok();
}

void reset_character_stub() {
    g_menu_character_calls = 0;
    g_party_member_calls = 0;
    g_party_index_calls = 0;
    g_menu_character_ok = true;
    g_menu_character_null = false;
    g_party_member_ok = true;
    g_party_index_ok = true;
    g_party_index_value = 0;
    g_party_members[0] = g_party_character;
    g_party_members[1] = nullptr;
    g_party_members[2] = nullptr;
}

// ---- character.attributes stub：snapshot/derived/attr 驱动页面数据 ----
bool g_attributes_snapshot_ok = true;
bool g_attributes_present = true;
int32_t g_attributes_main_stats[5] = {1, 2, 3, 4, 5};
int32_t g_attributes_values[qol::contracts::kQolCharacterAttrSlotCount] = {};
uint8_t g_attributes_valid[qol::contracts::kQolCharacterAttrSlotCount] = {};
bool g_attributes_derived_ok = true;
int32_t g_attributes_derived_base = 100;
bool g_attributes_attr_ok = true;
bool g_attributes_attr_notfound[3] = {false, false, false};
int32_t g_attributes_team_values[3] = {0, 0, 0};
int32_t g_attributes_snapshot_role = -1;

QolResult stub_attributes_snapshot(qol::contracts::QolCharacterRef ref,
                                   qol::contracts::QolCharacterAttributeSnapshot* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "out", 3);
    if (!g_attributes_snapshot_ok) {
        return qol::contracts::error(kQolErrNotReady, "snapshot", 8);
    }
    *out = {};
    out->struct_size = sizeof(*out);
    out->ref = ref;
    out->present = g_attributes_present ? 1 : 0;
    g_attributes_snapshot_role = ref.index;
    for (int32_t i = 0; i < qol::contracts::kQolCharacterMainStatCount; ++i) {
        out->main_stats[i] = g_attributes_main_stats[i];
    }
    for (int32_t id = 0; id < qol::contracts::kQolCharacterAttrSlotCount; ++id) {
        out->attrs[id] = g_attributes_values[id];
        out->attr_valid[id] = g_attributes_valid[id];
    }
    return qol::contracts::ok();
}

QolResult stub_attributes_derived(qol::contracts::QolCharacterRef, int32_t kind, int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "out", 3);
    if (!g_attributes_derived_ok) {
        return qol::contracts::error(kQolErrNotReady, "derived", 7);
    }
    *out = g_attributes_derived_base + kind;
    return qol::contracts::ok();
}

QolResult stub_attributes_attr(qol::contracts::QolCharacterRef ref, int32_t attr_id,
                               int32_t* out) {
    if (out == nullptr || ref.index < 0 || ref.index >= 3) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad ref", 7);
    }
    if (!g_attributes_attr_ok) return qol::contracts::error(kQolErrNotReady, "attr", 4);
    if (g_attributes_attr_notfound[ref.index]) {
        return qol::contracts::error(kQolErrNotFound, "empty slot", 10);
    }
    *out = attr_id == 92 ? g_attributes_team_values[ref.index] : 0;
    return qol::contracts::ok();
}

void reset_attributes_stub() {
    g_attributes_snapshot_ok = true;
    g_attributes_present = true;
    g_attributes_derived_ok = true;
    g_attributes_derived_base = 100;
    g_attributes_attr_ok = true;
    g_attributes_snapshot_role = -1;
    for (int32_t i = 0; i < 5; ++i) g_attributes_main_stats[i] = i + 1;
    for (int32_t id = 0; id < qol::contracts::kQolCharacterAttrSlotCount; ++id) {
        g_attributes_values[id] = 0;
        g_attributes_valid[id] = 0;
    }
    for (int32_t role = 0; role < 3; ++role) {
        g_attributes_attr_notfound[role] = false;
        g_attributes_team_values[role] = 0;
    }
}

QolResult stub_group_skill_learned(int32_t act_id, int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "out", 3);
    *out = act_id == 130 ? 1 : 0;
    return qol::contracts::ok();
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
    CHECK(d.requires_count == 4 && d.optional_count == 0);
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
    CHECK(d.requires_count == 4);
    CHECK(std::strcmp(d.requires[0].id, qol::contracts::kTextLayerCapabilityId) == 0);
    CHECK(std::strcmp(d.requires[1].id, qol::contracts::kUiComponentsCapabilityId) == 0);
    CHECK(std::strcmp(d.requires[2].id, qol::contracts::kUiPanelsCapabilityId) == 0);
    CHECK(std::strcmp(d.requires[3].id, qol::contracts::kCharacterAttributesCapabilityId) == 0);
    CHECK(d.requires[0].range.min_minor == qol::contracts::kTextLayerVersionMinor);
    CHECK(d.requires[1].range.min_minor == qol::contracts::kUiContractMinor);
    CHECK(d.requires[2].range.min_minor == qol::contracts::kUiPanelsVersionMinor);
    CHECK(d.requires[3].range.major == qol::contracts::kCharacterAttributesVersion.major);
    CHECK(d.requires[3].range.min_minor == qol::contracts::kCharacterAttributesVersion.minor);
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

// 分区行数 + 分页映射：143 行 → 12 页双列（每列最多 11 行），校验左右列与行覆盖。
void test_overview_row_model() {
    CHECK(i18n::kSectionCount == 4);
    CHECK(i18n::kMainStatRowCount == 5);
    CHECK(i18n::kDerivedRowCount == 11);
    CHECK(i18n::kAttrRowCount == 115);
    CHECK(i18n::kTeamRowCount == 12);
    CHECK(i18n::kTotalRowCount == 143);
    CHECK(i18n::kMaxRowsPerColumn == 11);
    CHECK(i18n::kPageCount == 12);

    struct ExpectedColumn {
        uint32_t section;
        uint32_t first_row;
        uint32_t row_count;
    };
    struct ExpectedPage {
        ExpectedColumn left;
        ExpectedColumn right;
    };
    ExpectedPage expected[12] = {};
    expected[0] = {{0, 0, 5}, {1, 0, 11}};  // P1 主属性 / 面板派生
    for (uint32_t page = 1; page <= 9; ++page) {
        const uint32_t first = (page - 1) * 12;
        expected[page] = {{2, first, 6}, {2, first + 6, 6}};
    }
    expected[10] = {{2, 108, 6}, {2, 114, 1}};  // attr 末页 7 行
    expected[11] = {{3, 0, 6}, {3, 6, 6}};      // 全队共享

    uint32_t covered_rows = 0;
    for (uint32_t page = 0; page < i18n::kPageCount; ++page) {
        const i18n::PageSpec* spec = nullptr;
        CHECK(i18n::page_at(page, &spec));
        CHECK(spec != nullptr && spec->title != nullptr);
        const i18n::ColumnSpec* columns[2] = {&spec->left, &spec->right};
        const ExpectedColumn* expected_columns[2] = {&expected[page].left,
                                                    &expected[page].right};
        for (int column = 0; column < 2; ++column) {
            const i18n::ColumnSpec& actual = *columns[column];
            const ExpectedColumn& want = *expected_columns[column];
            CHECK(actual.section == want.section);
            CHECK(actual.first_row == want.first_row);
            CHECK(actual.row_count == want.row_count);
            CHECK(actual.row_count <= i18n::kMaxRowsPerColumn);
            CHECK(actual.first_row + actual.row_count <=
                  i18n::kSections[actual.section].count);
            covered_rows += actual.row_count;
        }
    }
    CHECK(covered_rows == i18n::kTotalRowCount);
    // 主属性页：左 5 维、右面板派生 11 项。
    CHECK(i18n::kPages[0].left.row_count == 5);
    CHECK(i18n::kPages[0].right.row_count == i18n::kDerivedRowCount);
    // attr 页：P2..P11 共 10 页，前 9 页 6/6、末页 6/1。
    CHECK(i18n::kPages[10].left.first_row == 108);
    CHECK(i18n::kPages[10].right.first_row == 114);
    CHECK(i18n::kPages[10].left.row_count + i18n::kPages[10].right.row_count == 7);
    // 全队共享页 6/6。
    CHECK(i18n::kPages[11].left.row_count == 6);
    CHECK(i18n::kPages[11].right.first_row == i18n::kPages[11].left.row_count);
    CHECK(i18n::kPages[11].left.row_count + i18n::kPages[11].right.row_count ==
          i18n::kTeamRowCount);

    // 越界页码返回 false，且不写 out。
    CHECK(!i18n::page_at(i18n::kPageCount, nullptr));
    CHECK(!i18n::page_at(100, nullptr));
    CHECK(i18n::page_at(0, nullptr));
    const i18n::PageSpec* untouched = reinterpret_cast<const i18n::PageSpec*>(
        static_cast<uintptr_t>(0x1234));
    CHECK(!i18n::page_at(12, &untouched));
    CHECK(reinterpret_cast<uintptr_t>(untouched) == 0x1234);
}

// attr 名称：72 项确认名称，其余 id 一律 attr<id>（不得编造）。
void test_attr_names() {
    struct ExpectedName {
        int32_t id;
        const char* name;
    };
    const ExpectedName expected[] = {
        {0, "暴击率"}, {1, "暴击抵抗率"}, {2, "暴击伤害抵抗率"}, {3, "暴击伤害"},
        {4, "物理攻击"}, {5, "普攻伤害增幅"}, {7, "物理防御基础"}, {8, "魔法攻击"},
        {9, "魔法增伤"}, {10, "专属增伤开关"}, {11, "魔法抵抗"}, {12, "闪避"},
        {13, "总敏捷"}, {14, "命中率基数"}, {15, "命中率百分比"}, {17, "防御"},
        {18, "防御增幅系数"}, {19, "武器格挡"}, {20, "盾牌格挡"}, {22, "火"},
        {23, "风"}, {24, "寒气"}, {25, "神圣"}, {26, "黑暗"}, {27, "毒"},
        {28, "等级驱动属性"}, {29, "MP恢复"}, {30, "HP上限"}, {31, "MP上限"},
        {32, "蓝耗增减"}, {33, "HP吸收"}, {36, "固定吸血"}, {40, "友军受击回血"},
        {42, "治疗增幅"}, {44, "魔法伤害削减"}, {52, "双倍攻击概率"}, {58, "命中生成护盾"},
        {60, "高额吸血触发概率"}, {61, "高额吸血数值"}, {63, "命中回蓝"}, {66, "重击概率"},
        {69, "命中持续状态层数"}, {70, "重置召唤物CD概率"}, {71, "增伤阶段"},
        {72, "命中嘲讽"}, {75, "行为距离加成"}, {76, "反伤"}, {78, "重力摆"},
        {79, "冰霜"}, {80, "治愈气息"}, {81, "狂战士"}, {82, "瞬间恢复"},
        {83, "魔力专家"}, {84, "减少敌意值"}, {85, "失明抗性"}, {86, "睡眠抗性"},
        {87, "眩晕抗性"}, {88, "恐惧抗性"}, {89, "沉默抗性"}, {90, "减速抗性"},
        {91, "移动速度"}, {92, "掉宝率"}, {93, "武器攻击帧"}, {94, "仇恨系数"},
        {98, "嘲讽概率"}, {106, "额外普攻开关"}, {107, "受击获得持续增益概率"},
        {109, "额外普攻增幅"}, {110, "反击减益"}, {111, "命中沉默"},
        {112, "命中固定回蓝"}, {113, "总攻击"},
    };
    CHECK(i18n::kAttrNameCount == 72);
    CHECK(sizeof(expected) / sizeof(expected[0]) == i18n::kAttrNameCount);
    for (const ExpectedName& entry : expected) {
        const char* name = i18n::attr_display_name(entry.id);
        CHECK(name != nullptr && std::strcmp(name, entry.name) == 0);
    }

    const int32_t unknown[] = {
        6, 16, 21, 34, 35, 37, 38, 39, 41, 43, 45, 46, 47, 48, 49, 50, 51,
        53, 54, 55, 56, 57, 59, 62, 64, 65, 67, 68, 73, 74, 77, 95, 96, 97,
        99, 100, 101, 102, 103, 104, 105, 108, 114,
    };
    CHECK(sizeof(unknown) / sizeof(unknown[0]) ==
          i18n::kAttrRowCount - i18n::kAttrNameCount);
    for (int32_t id : unknown) CHECK(i18n::attr_display_name(id) == nullptr);

    char label[24];
    const i18n::RowSpec known{nullptr, i18n::RowKind::kAttrInt, 33};
    i18n::format_label(known, label, sizeof(label));
    CHECK(std::strcmp(label, "HP吸收") == 0);
    const i18n::RowSpec fallback{nullptr, i18n::RowKind::kAttrInt, 34};
    i18n::format_label(fallback, label, sizeof(label));
    CHECK(std::strcmp(label, "attr34") == 0);
    const i18n::RowSpec labeled{"物攻", i18n::RowKind::kNormalDamage, 0};
    i18n::format_label(labeled, label, sizeof(label));
    CHECK(std::strcmp(label, "物攻") == 0);
}

// 格式化：千分比 raw/10 保留 1 位小数 + %；整数；失败 `--`；佣兵团技能已学/未学；全队掉宝倍率。
void test_overview_format_value() {
    char buffer[40];
    const i18n::RowSpec& crit = i18n::kDerivedRows[3];    // 暴击率（attr 千分比）
    const i18n::RowSpec& hit = i18n::kDerivedRows[4];     // 命中率（hit_rate_1000）
    const i18n::RowSpec& defense = i18n::kDerivedRows[2]; // 防御力（整数）
    const i18n::RowSpec& hidden = i18n::kAttrRowTable.rows[33];  // HP吸收（attr 原值）
    const i18n::RowSpec& multiplier = i18n::kTeamRows[0];    // 全队掉宝倍率
    const i18n::RowSpec& scroll_drop = i18n::kTeamRows[1];   // 卷轴掉率 act 130
    const i18n::RowSpec& legendary = i18n::kTeamRows[4];     // 传说装备率 act 122
    const i18n::RowSpec& potion_cooldown = i18n::kTeamRows[11];  // 药水冷却 act 141

    i18n::format_value(crit, true, false, 1234, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "123.4%") == 0);
    i18n::format_value(crit, true, false, 0, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "0.0%") == 0);
    i18n::format_value(crit, true, false, 5, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "0.5%") == 0);
    i18n::format_value(hit, true, false, 999, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "99.9%") == 0);
    i18n::format_value(crit, true, false, -5, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "-0.5%") == 0);

    i18n::format_value(defense, true, false, 4321, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "4321") == 0);
    i18n::format_value(hidden, true, false, -7, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "-7") == 0);

    i18n::format_value(crit, false, false, 1234, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "--") == 0);
    i18n::format_value(defense, false, false, 0, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "--") == 0);

    // kPartyDropMultiplier：sum=0 → ×1.00（+0）；sum=300 → ×1.30（+300）；失败 → `--`。
    i18n::format_value(multiplier, true, false, 0, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "×1.00（+0）") == 0);
    i18n::format_value(multiplier, true, false, 300, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "×1.30（+300）") == 0);
    i18n::format_value(multiplier, false, false, 0, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "--") == 0);

    // kGroupSkill：已学 → learned_text；未学 → missing_text；读取失败 → `--`。
    i18n::format_value(scroll_drop, true, true, 1, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "+0.2pp") == 0);
    i18n::format_value(scroll_drop, true, false, 0, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "未获得") == 0);
    i18n::format_value(legendary, true, true, 1, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "+3%") == 0);
    i18n::format_value(potion_cooldown, true, true, 1, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "-3秒") == 0);
    i18n::format_value(scroll_drop, false, false, 0, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "--") == 0);
    i18n::format_value(scroll_drop, false, true, 1, buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "--") == 0);

    // 缓冲不足时不得越界写：只校验首字符被写入且仍是 NUL 结尾片段的合法前缀。
    char tiny[4] = {'x', 'x', 'x', 'x'};
    i18n::format_value(crit, true, false, 1234, tiny, sizeof(tiny));
    CHECK(tiny[sizeof(tiny) - 1] == '\0');
}

// 选中角色优先级：menu_character（原版面板同源）优先；失败/空指针回退 party_member(0)。
void test_selected_character_priority() {
    qol::contracts::QolCharacterExtrasPort port{};
    port.read.menu_character = &stub_menu_character;
    port.read.party_index = &stub_party_index;
    port.read.party_member = &stub_party_member;
    i18n::overview_bind_context(nullptr, &port);

    // menu_character + party_index 成功：直接采用 party_index。
    reset_character_stub();
    g_party_index_value = 1;
    CHECK(i18n::overview_resolve_party_role() == 1);
    CHECK(g_menu_character_calls == 1 && g_party_index_calls == 1);
    CHECK(g_party_member_calls == 0);

    // party_index 失败：party_member 指针比对回退。
    reset_character_stub();
    g_party_index_ok = false;
    g_party_members[2] = g_menu_character;
    CHECK(i18n::overview_resolve_party_role() == 2);
    CHECK(g_party_member_calls == 3);

    // menu_character 空/失败：role 0。
    reset_character_stub();
    g_menu_character_null = true;
    CHECK(i18n::overview_resolve_party_role() == 0);
    reset_character_stub();
    g_menu_character_ok = false;
    CHECK(i18n::overview_resolve_party_role() == 0);

    // party_index 越界且指针不匹配：role 0。
    reset_character_stub();
    g_party_index_value = 9;
    CHECK(i18n::overview_resolve_party_role() == 0);

    i18n::overview_bind_context(nullptr, nullptr);
    CHECK(i18n::overview_resolve_party_role() == 0);
}

// 页面数据由 character.attributes 驱动：snapshot/derived/attr，attr_valid=0 → `--`。
void test_attributes_driven_rows() {
    qol::contracts::QolCharacterExtrasPort port{};
    port.read.menu_character = &stub_menu_character;
    port.read.party_index = &stub_party_index;
    port.read.party_member = &stub_party_member;
    qol::contracts::QolGameHost game{};
    game.character_read.group_skill_learned = &stub_group_skill_learned;
    qol::contracts::CharacterAttributesApi api{};
    api.struct_size = sizeof(api);
    api.snapshot = &stub_attributes_snapshot;
    api.derived = &stub_attributes_derived;
    api.attr = &stub_attributes_attr;
    i18n::overview_bind_context(&game, &port);
    i18n::overview_bind_attributes(&api);

    reset_character_stub();
    reset_attributes_stub();
    g_party_index_value = 0;
    g_attributes_main_stats[0] = 55;
    g_attributes_values[42] = 4242;
    g_attributes_valid[42] = 1;
    g_attributes_values[43] = 0;
    g_attributes_valid[43] = 0;
    g_attributes_team_values[0] = 100;
    g_attributes_attr_notfound[1] = true;  // role1 空槽跳过
    g_attributes_team_values[2] = 50;

    i18n::overview_refresh_snapshot();
    i18n::OverviewRowProbe probe{};
    CHECK(g_attributes_snapshot_role == 0);
    CHECK(i18n::overview_probe_row(0, 0, &probe) && probe.ok == 1 && probe.value == 55);
    CHECK(i18n::overview_probe_row(1, 0, &probe) && probe.ok == 1 && probe.value == 100);
    CHECK(i18n::overview_probe_row(2, 42, &probe) && probe.ok == 1 && probe.value == 4242);
    CHECK(i18n::overview_probe_row(2, 43, &probe) && probe.ok == 0);
    CHECK(i18n::overview_probe_row(3, 0, &probe) && probe.ok == 1 && probe.value == 150);
    CHECK(i18n::overview_probe_row(3, 1, &probe) && probe.ok == 1 && probe.value == 1 &&
          probe.learned == 1);

    // present=0：角色行全部 `--`，全队行仍可用。
    g_attributes_present = false;
    i18n::overview_refresh_snapshot();
    CHECK(i18n::overview_probe_row(0, 0, &probe) && probe.ok == 0);
    CHECK(i18n::overview_probe_row(2, 42, &probe) && probe.ok == 0);
    CHECK(i18n::overview_probe_row(3, 0, &probe) && probe.ok == 1 && probe.value == 150);

    // 快照失败：同样只降级角色行。
    g_attributes_present = true;
    g_attributes_snapshot_ok = false;
    i18n::overview_refresh_snapshot();
    CHECK(i18n::overview_probe_row(0, 0, &probe) && probe.ok == 0);
    CHECK(i18n::overview_probe_row(1, 0, &probe) && probe.ok == 0);

    // 全队倍率非 NotFound 失败：整行 `--`。
    g_attributes_snapshot_ok = true;
    g_attributes_attr_ok = false;
    i18n::overview_refresh_snapshot();
    CHECK(i18n::overview_probe_row(3, 0, &probe) && probe.ok == 0);

    i18n::overview_bind_attributes(nullptr);
    i18n::overview_bind_context(nullptr, nullptr);
    reset_attributes_stub();
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
    test_attr_names();
    test_selected_character_priority();
    test_attributes_driven_rows();
    test_overview_format_value();
    test_character_info_tap_inside();
    test_popup_assembly();
    test_overview_click_flow();
    std::printf("character_panel: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

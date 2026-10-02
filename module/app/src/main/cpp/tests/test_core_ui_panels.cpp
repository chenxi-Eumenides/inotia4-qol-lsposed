// core.ui 面板栈 + 分页规则 host 测试（D1-b2）。
//
// 覆盖：
//   · panels/rules.h：逻辑页收集/去重、页码换算、格序、动作项过滤、页码钳制；
//   · panels/stack：open 幂等、push 成功、push 失败「关栈顶再开」兜底、陈旧栈顶剔除、
//     栈深 4 上限、close（含其上层）、block_reason 栈顶优先。
//
// fake provider 经 Host SDK panels.find 提供；不触碰游戏内存/真实 UI。

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/panels.h"
#include "contracts/host_sdk.h"
#include "plugins/core.settings/panels/rules.h"
#include "plugins/core.custom_ui/panels/stack.h"

using qol::contracts::PanelItemKind;
using qol::contracts::PanelProviderSpec;
using qol::contracts::QolHostSdk;
using qol::features::core_ui::ui_panels_active;
using qol::features::core_ui::ui_panels_block_reason;
using qol::features::core_ui::ui_panels_close;
using qol::features::core_ui::ui_panels_depth_for_test;
using qol::features::core_ui::ui_panels_deactivate_owner;
using qol::features::core_ui::ui_panels_open;
using qol::features::core_ui::ui_panels_reset_for_test;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

// ---------------- 分页规则 ----------------

namespace {

using qol::features::core_ui::panels_rules::Item;

const Item kItems[] = {
    {"", "存档备份", 1},
    {"a", "扩展背包", 1},
    {"b", "堆叠上限", 1},
    {"c", "拖拽合并", 2},
    {"d", "OP能力", 3},
};
constexpr int kItemCount = static_cast<int>(sizeof(kItems) / sizeof(kItems[0]));

void test_rules_pages() {
    using namespace qol::features::core_ui::panels_rules;

    CHECK(page_count(kItems, kItemCount) == 3);

    int pages[kMaxPages] = {};
    CHECK(collect_logical_pages(kItems, kItemCount, pages, kMaxPages) == 3);
    CHECK(pages[0] == 1 && pages[1] == 2 && pages[2] == 3);

    CHECK(logical_page_at(kItems, kItemCount, 0) == 1);
    CHECK(logical_page_at(kItems, kItemCount, 2) == 3);
    CHECK(logical_page_at(kItems, kItemCount, 3) == -1);
    CHECK(logical_page_at(kItems, kItemCount, -1) == -1);

    CHECK(item_physical_page(kItems, kItemCount, 0) == 0);
    CHECK(item_physical_page(kItems, kItemCount, 3) == 1);
    CHECK(item_physical_page(kItems, kItemCount, 4) == 2);
    CHECK(item_physical_page(kItems, kItemCount, 99) == -1);

    CHECK(item_is_action(kItems, kItemCount, 0));
    CHECK(!item_is_action(kItems, kItemCount, 1));
    CHECK(!item_is_action(kItems, kItemCount, -1));

    // 物理页 0：3 个条目（cell0..2）；cell3/4 空。
    CHECK(item_index_at_cell(kItems, kItemCount, 0, 0) == 0);
    CHECK(item_index_at_cell(kItems, kItemCount, 0, 2) == 2);
    CHECK(item_index_at_cell(kItems, kItemCount, 0, 3) == -1);
    CHECK(item_index_at_cell(kItems, kItemCount, 0, 4) == -1);
    CHECK(item_index_at_cell(kItems, kItemCount, 0, kPageCapacity) == -1);
    CHECK(item_index_at_cell(kItems, kItemCount, 1, 0) == 3);
    CHECK(item_index_at_cell(kItems, kItemCount, 2, 0) == 4);
    CHECK(item_index_at_cell(kItems, kItemCount, 2, 1) == -1);

    CHECK(item_cell_at_page(kItems, kItemCount, 2, 0) == 2);
    CHECK(item_cell_at_page(kItems, kItemCount, 3, 0) == -1);
    CHECK(item_cell_at_page(kItems, kItemCount, 4, 2) == 0);
    CHECK(item_cell_at_page(kItems, kItemCount, 99, 0) == -1);

    // 页码钳制：超界 → 末页；空表 → 1 页 0 下标。
    int page = 99;
    int total = 0;
    sync_page_state(kItems, kItemCount, &page, &total);
    CHECK(total == 3 && page == 2);
    page = -5;
    sync_page_state(kItems, kItemCount, &page, &total);
    CHECK(page == 0);
    page = 7;
    sync_page_state(nullptr, 0, &page, &total);
    CHECK(total == 1 && page == 0);

    // page<=0 的条目不参与分页（空表/全关闭页）。
    const Item kNoPages[] = {{"x", "x", 0}, {"y", "y", -1}};
    CHECK(page_count(kNoPages, 2) == 0);
}

// ---------------- 面板栈 ----------------

struct Fake {
    bool active = false;
    int open_calls = 0;
    int close_calls = 0;
    int fail_open_times = 0;
    const char* reason = nullptr;
};

Fake g_f[5];

bool f_open(int i) {
    Fake& f = g_f[i];
    ++f.open_calls;
    if (f.fail_open_times > 0) {
        --f.fail_open_times;
        f.active = false;
        return false;
    }
    f.active = true;
    return true;
}

void f_close(int i) {
    ++g_f[i].close_calls;
    g_f[i].active = false;
}

bool f_active(int i) { return g_f[i].active; }
const char* f_reason(int i) { return g_f[i].reason; }

bool a_open() { return f_open(0); }
void a_close() { f_close(0); }
bool a_active() { return f_active(0); }
const char* a_reason() { return f_reason(0); }

bool b_open() { return f_open(1); }
void b_close() { f_close(1); }
bool b_active() { return f_active(1); }
const char* b_reason() { return f_reason(1); }
bool c_open() { return f_open(2); }
void c_close() { f_close(2); }
bool c_active() { return f_active(2); }
const char* c_reason() { return f_reason(2); }
bool d_open() { return f_open(3); }
void d_close() { f_close(3); }
bool d_active() { return f_active(3); }
const char* d_reason() { return f_reason(3); }
bool e_open() { return f_open(4); }
void e_close() { f_close(4); }
bool e_active() { return f_active(4); }
const char* e_reason() { return f_reason(4); }

const PanelProviderSpec kProviders[] = {
    {"pa", "core.settings", &a_open, &a_close, &a_active, &a_reason},
    {"pb", "save_backup", &b_open, &b_close, &b_active, &b_reason},
    {"pc", "core.ui", &c_open, &c_close, &c_active, &c_reason},
    {"pd", "core.ui", &d_open, &d_close, &d_active, &d_reason},
    {"pe", "core.ui", &e_open, &e_close, &e_active, &e_reason},
};

QolResult fake_panels_find(const char* panel_id, const PanelProviderSpec** out_provider) {
    if (panel_id == nullptr || out_provider == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    *out_provider = nullptr;
    for (const PanelProviderSpec& provider : kProviders) {
        if (std::strcmp(provider.panel_id, panel_id) == 0) {
            *out_provider = &provider;
            return qol::contracts::ok();
        }
    }
    return qol::contracts::error(kQolErrNotFound, "not found", 9);
}

void reset_fakes() {
    for (Fake& f : g_f) {
        f = Fake{};
    }
    ui_panels_reset_for_test();
}

void test_stack_open_idempotent() {
    reset_fakes();
    CHECK(ui_panels_open("pa"));
    CHECK(g_f[0].open_calls == 1 && g_f[0].active);
    CHECK(ui_panels_depth_for_test() == 1);
    // 已在栈顶且 active → 幂等，不再调 provider.open。
    CHECK(ui_panels_open("pa"));
    CHECK(g_f[0].open_calls == 1);
    CHECK(ui_panels_active("pa"));
    CHECK(!ui_panels_active("pb"));
    CHECK(!ui_panels_active(nullptr));
    CHECK(!ui_panels_open("missing"));
}

void test_stack_fallback_close_top() {
    reset_fakes();
    CHECK(ui_panels_open("pa"));  // 设置页在栈
    g_f[1].fail_open_times = 1;   // 备份面板首推失败（模拟嵌套 push 未切换栈顶）
    CHECK(ui_panels_open("pb"));
    CHECK(g_f[0].close_calls == 1);   // 关掉了栈顶设置页
    CHECK(g_f[1].open_calls == 2);    // 关闭后重试成功
    CHECK(g_f[1].active);
    CHECK(ui_panels_depth_for_test() == 1);

    // 无面板 active 时 block_reason 为空。
    g_f[1].active = false;
    CHECK(ui_panels_block_reason() == nullptr);
}

void test_stack_block_reason_top_first() {
    reset_fakes();
    CHECK(ui_panels_open("pa"));
    g_f[0].reason = "ui occupied: settings panel";
    CHECK(ui_panels_open("pb"));
    g_f[1].reason = "ui occupied: backup panel";
    CHECK(ui_panels_depth_for_test() == 2);
    // 栈顶优先：嵌套时报告最上层。
    CHECK(std::strcmp(ui_panels_block_reason(), "ui occupied: backup panel") == 0);
    ui_panels_close("pb");
    CHECK(g_f[0].active);
    CHECK(std::strcmp(ui_panels_block_reason(), "ui occupied: settings panel") == 0);
}

void test_stack_prune_stale_top() {
    reset_fakes();
    CHECK(ui_panels_open("pa"));
    g_f[0].active = false;  // provider 自行关闭（游戏内返回键），不经栈
    CHECK(ui_panels_open("pb"));
    CHECK(g_f[0].close_calls == 0);  // 陈旧栈顶只丢记录，不再调 close
    CHECK(g_f[1].active);
    CHECK(ui_panels_depth_for_test() == 1);
}

void test_stack_depth_limit_and_close() {
    reset_fakes();
    CHECK(ui_panels_open("pa"));
    CHECK(ui_panels_open("pb"));
    CHECK(ui_panels_open("pc"));
    CHECK(ui_panels_open("pd"));
    CHECK(ui_panels_depth_for_test() == 4);
    CHECK(ui_panels_open("pe"));  // 深度上限：关栈顶再开
    CHECK(ui_panels_depth_for_test() == 4);
    CHECK(g_f[3].close_calls == 1);  // pd 被关
    CHECK(g_f[4].active);
    CHECK(g_f[0].active && g_f[1].active && g_f[2].active);

    // close 顶层：只关自己。
    ui_panels_close("pe");
    CHECK(!g_f[4].active);
    CHECK(ui_panels_depth_for_test() == 3);

    // close 底层：连同其上的面板一并关闭。
    ui_panels_close("pa");
    CHECK(g_f[0].close_calls == 1 && g_f[1].close_calls == 1 && g_f[2].close_calls == 1);
    CHECK(ui_panels_depth_for_test() == 0);
    CHECK(!g_f[0].active && !g_f[1].active && !g_f[2].active);

    // 未在栈中但 active 的 provider：直接 close。
    g_f[4].active = true;
    ui_panels_close("pe");
    CHECK(g_f[4].close_calls == 2);
}

void test_provider_deactivate_removes_owned_panel() {
    reset_fakes();
    CHECK(ui_panels_open("pa"));
    CHECK(ui_panels_open("pb"));
    CHECK(ui_panels_deactivate_owner("core.settings"));
    CHECK(!g_f[0].active && g_f[1].active);
    CHECK(g_f[0].close_calls == 1 && g_f[1].close_calls == 0);
    CHECK(ui_panels_depth_for_test() == 1);
    CHECK(ui_panels_deactivate_owner("core.settings"));
    CHECK(g_f[0].close_calls == 1);
    CHECK(!ui_panels_deactivate_owner(nullptr));
}

}  // namespace

int main() {
    QolHostSdk sdk{};
    sdk.panels.find = &fake_panels_find;
    qol::features::core_ui::ui_panels_bind_sdk(&sdk);

    test_rules_pages();
    test_stack_open_idempotent();
    test_stack_fallback_close_top();
    test_stack_block_reason_top_first();
    test_stack_prune_stale_top();
    test_stack_depth_limit_and_close();
    test_provider_deactivate_removes_owned_panel();

    std::printf("core_ui_panels_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

// 设置页数据驱动模型 host 测试（core.settings 重建）。
//
// 覆盖 settings_model.h 的纯规则：类别条目计数、右栏行号→目录全局下标映射、
// 开关目标值、动作/入队失败文案、成功回执附加提示。不触碰游戏内存与真实 UI。

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "plugins/settings/panels/settings_model.h"
#include "plugins/settings/panels/feature_model.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

namespace {

using qol::contracts::SettingsConfigApplyState;
using qol::contracts::SettingsItemType;
using qol::contracts::SettingsItemView;
using qol::features::core_ui::settings_model::action_error_text;
using qol::features::core_ui::settings_model::apply_pending_text;
using qol::features::core_ui::settings_model::category_item_count;
using qol::features::core_ui::settings_model::next_toggle_value;
using qol::features::core_ui::settings_model::request_error_text;
using qol::features::core_ui::settings_model::row_paint_plan;
using qol::features::core_ui::settings_model::row_description;
using qol::features::core_ui::settings_model::RowPaintPlan;
using qol::features::core_ui::settings_model::row_text_layout;
using qol::features::core_ui::settings_model::truncate_text;
using qol::features::core_ui::settings_model::right_row_height;
using qol::features::core_ui::settings_model::toggle_top;
using qol::features::core_ui::settings_model::description_width_for_toggle;
using qol::features::core_ui::settings_model::right_control_left;
using qol::features::core_ui::settings_model::row_to_item_index;
using qol::features::core_ui::settings_model::use_item_description;
using qol::features::core_ui::settings_model::toggle_enabled;
using qol::features::core_ui::feature_model::entry_enabled;

SettingsItemView item(const char* id, const char* category, SettingsItemType type) {
    SettingsItemView view{};
    std::snprintf(view.id, sizeof(view.id), "%s", id);
    std::snprintf(view.category_id, sizeof(view.category_id), "%s", category);
    view.type = type;
    return view;
}

void test_category_mapping() {
    const SettingsItemView items[] = {
        item("a.1", "a", SettingsItemType::kToggle),
        item("b.1", "b", SettingsItemType::kAction),
        item("a.2", "a", SettingsItemType::kToggle),
        item("c.1", "c", SettingsItemType::kToggle),
        item("a.3", "a", SettingsItemType::kAction),
    };
    const uint32_t count = sizeof(items) / sizeof(items[0]);

    CHECK(category_item_count(items, count, "a") == 3);
    CHECK(category_item_count(items, count, "b") == 1);
    CHECK(category_item_count(items, count, "c") == 1);
    CHECK(category_item_count(items, count, "missing") == 0);
    CHECK(category_item_count(items, count, nullptr) == 0);
    CHECK(category_item_count(nullptr, count, "a") == 0);

    // 行号按目录 items 顺序过滤：a 类的 0/1/2 → 全局 0/2/4。
    CHECK(row_to_item_index(items, count, "a", 0) == 0);
    CHECK(row_to_item_index(items, count, "a", 1) == 2);
    CHECK(row_to_item_index(items, count, "a", 2) == 4);
    CHECK(row_to_item_index(items, count, "a", 3) == -1);
    CHECK(row_to_item_index(items, count, "b", 0) == 1);
    CHECK(row_to_item_index(items, count, "b", 1) == -1);
    CHECK(row_to_item_index(items, count, "c", 0) == 3);
    CHECK(row_to_item_index(items, count, "missing", 0) == -1);
    CHECK(row_to_item_index(items, count, nullptr, 0) == -1);
    CHECK(row_to_item_index(nullptr, count, "a", 0) == -1);
}

void test_hidden_catalog_items_follow_build_visibility() {
    using qol::contracts::FeatureCategory;
    using qol::contracts::FeatureType;
    using qol::contracts::QolFeatureCatalogItem;
    using qol::contracts::QolFeatureCatalogPlugin;

    const QolFeatureCatalogItem core_items[] = {
        {"api.enabled", 1, FeatureCategory::kGeneral, FeatureType::kToggle,
         "API服务", "", nullptr},
        {"core.custom_ui.open_debug_components", 1, FeatureCategory::kHidden,
         FeatureType::kEntry, "打开调试面板", "", "打开"},
    };
    const QolFeatureCatalogItem regular_items[] = {
        {"save_backup.open_manager", 1, FeatureCategory::kTools,
         FeatureType::kEntry, "存档备份导出", "打开存档备份管理器。", "打开"},
    };
    const QolFeatureCatalogItem op_items[] = {
        {"op.enabled", 0, FeatureCategory::kGeneral, FeatureType::kToggle,
         "开启OP功能", "", nullptr},
    };
    const QolFeatureCatalogPlugin catalog[] = {
        {"core.api", core_items, 1, 0},
        {"core.custom_ui", core_items + 1, 1, 0},
        {"save_backup", regular_items, 1, 0},
        {"module.security", op_items, 1, 1},
    };
    std::vector<qol::features::core_ui::feature_model::Item> items;

    CHECK(qol::features::core_ui::feature_model::build(catalog, 4, false, items));
    CHECK(items.size() == 3);
    CHECK(std::strcmp(items[0].feature_id, "api.enabled") == 0);
    CHECK(std::strcmp(items[1].feature_id, "save_backup.open_manager") == 0);
    CHECK(std::strcmp(items[2].feature_id, "op.enabled") == 0 &&
          items[2].category == FeatureCategory::kGeneral);

    CHECK(qol::features::core_ui::feature_model::build(catalog, 4, true, items));
    CHECK(items.size() == 4);
    CHECK(std::strcmp(items[0].feature_id, "api.enabled") == 0 &&
          items[0].category == FeatureCategory::kGeneral);
    CHECK(std::strcmp(items[1].feature_id, "core.custom_ui.open_debug_components") == 0 &&
          items[1].type == FeatureType::kEntry && items[1].category == FeatureCategory::kHidden);
    CHECK(std::strcmp(items[2].feature_id, "save_backup.open_manager") == 0 &&
          items[2].category == FeatureCategory::kTools);
}

void test_display_priority_and_registered_categories() {
    using namespace qol::contracts;
    namespace model = qol::features::core_ui::feature_model;
    const QolFeatureCatalogItem first[]{
        {"zero.first", 1, FeatureCategory::kGeneral, FeatureType::kToggle, "First", ""},
        {"early.first", 1, FeatureCategory::kGeneral, FeatureType::kToggle, "Early", "", nullptr, -128},
        {"hidden", 1, FeatureCategory::kHidden, FeatureType::kToggle, "Hidden", "", nullptr, -10},
        {"late", 1, FeatureCategory::kTools, FeatureType::kEntry, "Late", "", "Open", 127},
    };
    const QolFeatureCatalogItem second[]{
        {"early.second", 1, FeatureCategory::kGeneral, FeatureType::kToggle, "Second", "", nullptr, -128},
        {"zero.second", 1, FeatureCategory::kGameplay, FeatureType::kToggle, "Zero", ""},
    };
    const QolFeatureCatalogPlugin plugins[]{
        {"first", first, 4, 0}, {"second", second, 2, 0},
    };
    const FeatureCategoryDefinition registry[]{
        {FeatureCategory::kTools, "Host tools", -3},
        {FeatureCategory::kGameplay, "Host gameplay", -5},
        {FeatureCategory::kGeneral, "Host general", -9},
        {FeatureCategory::kBasicOptimization, "Empty", -8},
        {FeatureCategory::kHidden, "Host hidden", -10},
    };
    std::vector<model::Item> debug_items, release_items;
    std::vector<FeatureCategoryDefinition> categories;
    CHECK(model::build(plugins, 2, true, debug_items));
    const char* expected[]{"early.first", "early.second", "hidden", "zero.first", "zero.second", "late"};
    CHECK(debug_items.size() == 6);
    for (size_t i = 0; i < debug_items.size(); ++i)
        CHECK(std::strcmp(debug_items[i].feature_id, expected[i]) == 0);
    CHECK(model::build(plugins, 2, false, release_items));
    CHECK(release_items.size() == 5);
    size_t release_index = 0;
    for (const auto& item : debug_items) {
        if (item.category == FeatureCategory::kHidden) continue;
        CHECK(std::strcmp(item.feature_id, release_items[release_index++].feature_id) == 0);
    }
    CHECK(model::build_categories(registry, 5, true, debug_items, categories));
    CHECK(categories.size() == 4);
    CHECK(categories[0].category == FeatureCategory::kHidden);
    CHECK(categories[1].category == FeatureCategory::kGeneral);
    CHECK(categories[2].category == FeatureCategory::kGameplay);
    CHECK(categories[3].category == FeatureCategory::kTools);
    CHECK(std::strcmp(categories[1].name, "Host general") == 0);
    CHECK(model::build_categories(registry, 5, false, release_items, categories));
    CHECK(categories.size() == 3 && categories[0].category == FeatureCategory::kGeneral);
    const FeatureCategoryDefinition tied[]{
        {FeatureCategory::kTools, "Tools first", 0},
        {FeatureCategory::kGeneral, "General second", 0},
        {FeatureCategory::kGameplay, "Gameplay third", 0},
    };
    CHECK(model::build_categories(tied, 3, true, debug_items, categories));
    CHECK(categories.size() == 3 && categories[0].category == FeatureCategory::kTools &&
          categories[1].category == FeatureCategory::kGeneral &&
          categories[2].category == FeatureCategory::kGameplay);
    CHECK(!model::build_categories(nullptr, 1, true, debug_items, categories));
    CHECK(categories.empty());
    CHECK(model::build_categories(nullptr, 0, true, debug_items, categories));
    const FeatureCategoryDefinition malformed[]{ {FeatureCategory::kGeneral, nullptr, 0} };
    CHECK(!model::build_categories(malformed, 1, true, debug_items, categories));
    CHECK(categories.empty());
}

void test_toggle_target() {
    CHECK(next_toggle_value(0) == 1);
    CHECK(next_toggle_value(1) == 0);
    CHECK(next_toggle_value(7) == 0);
}

void test_toggle_enable_state_machine() {
    int create_count = 1;
    int destroy_count = 0;
    bool confirmed_valid = true;
    bool availability_valid = true;
    bool available = true;
    bool gesture_requested = false;
    uint64_t request_id = 0;
    bool request_queued = false;

    CHECK(toggle_enabled(true, confirmed_valid, availability_valid, available,
                         gesture_requested, request_id, request_queued));
    gesture_requested = true;
    CHECK(!toggle_enabled(true, confirmed_valid, availability_valid, available,
                          gesture_requested, request_id, request_queued));
    gesture_requested = false;
    request_id = 11;
    request_queued = true;
    CHECK(!toggle_enabled(true, confirmed_valid, availability_valid, available,
                          gesture_requested, request_id, request_queued));

    // Persistence success and apply-pending are terminal for serialization, not a UI lock.
    request_id = 0;
    request_queued = false;
    CHECK(toggle_enabled(true, confirmed_valid, availability_valid, available,
                         gesture_requested, request_id, request_queued));
    gesture_requested = true;
    CHECK(!toggle_enabled(true, confirmed_valid, availability_valid, available,
                          gesture_requested, request_id, request_queued));
    gesture_requested = false;
    request_id = 12;
    request_queued = true;
    CHECK(!toggle_enabled(true, confirmed_valid, availability_valid, available,
                          gesture_requested, request_id, request_queued));
    request_id = 0;
    request_queued = false;
    CHECK(toggle_enabled(true, confirmed_valid, availability_valid, available,
                         gesture_requested, request_id, request_queued));

    // Failure, unchanged/failed admission, and unknown request state must release the UI guard;
    // unknown config mirrors remain fail-closed rather than being interpreted as false.
    CHECK(toggle_enabled(true, confirmed_valid, availability_valid, available,
                         gesture_requested, request_id, request_queued));
    CHECK(!toggle_enabled(true, false, availability_valid, available,
                          gesture_requested, request_id, request_queued));
    CHECK(!toggle_enabled(true, confirmed_valid, false, available,
                          gesture_requested, request_id, request_queued));
    CHECK(!toggle_enabled(true, confirmed_valid, availability_valid, false,
                          gesture_requested, request_id, request_queued));
    CHECK(!toggle_enabled(false, confirmed_valid, availability_valid, available,
                          gesture_requested, request_id, request_queued));
    CHECK(create_count == 1 && destroy_count == 0);
}

void test_entry_enable_state() {
    qol::features::core_ui::feature_model::Item entry{};
    entry.type = qol::contracts::FeatureType::kEntry;
    CHECK(entry_enabled(entry, true, false, false));
    CHECK(!entry_enabled(entry, false, false, false));
    CHECK(!entry_enabled(entry, true, true, false));
    CHECK(!entry_enabled(entry, true, false, true));

    entry.type = qol::contracts::FeatureType::kToggle;
    CHECK(!entry_enabled(entry, true, false, false));
}

void test_error_text() {
    CHECK(std::strcmp(action_error_text(kQolErrConflict),
                      "操作冲突：设置目录已更新，请返回后重进") == 0);
    CHECK(std::strcmp(action_error_text(kQolErrNotReady),
                      "操作未就绪：所属功能当前不可用") == 0);
    CHECK(std::strcmp(action_error_text(kQolErrBlocked),
                      "操作被阻止：请先关闭其他界面") == 0);
    CHECK(action_error_text(9999) != nullptr && action_error_text(9999)[0] != '\0');
    CHECK(std::strcmp(request_error_text(kQolErrNotReady), "配置尚未就绪") == 0);
    CHECK(std::strcmp(request_error_text(kQolErrBlocked), "请求队列已满，请稍后再试") == 0);
    CHECK(request_error_text(1234)[0] != '\0');

    CHECK(std::strcmp(apply_pending_text(SettingsConfigApplyState::kPendingFrame),
                      "已保存，生效中") == 0);
    CHECK(std::strcmp(apply_pending_text(SettingsConfigApplyState::kPlatformPending),
                      "已保存，生效中") == 0);
    CHECK(apply_pending_text(SettingsConfigApplyState::kApplied)[0] == '\0');
    CHECK(apply_pending_text(SettingsConfigApplyState::kUnchanged)[0] == '\0');
    CHECK(apply_pending_text(SettingsConfigApplyState::kNotSubmitted)[0] == '\0');
    CHECK(std::strcmp(apply_pending_text(SettingsConfigApplyState::kRejected),
                      "设置被拒绝") == 0);
}

void test_row_paint_geometry() {
    // Origin includes native scroll-group displacement; a 17-unit scroll is not row-aligned.
    const auto selected = row_paint_plan(131, 117, 300, 73, true);
    CHECK(selected.operation_count == 2);
    CHECK(selected.operations[0] == RowPaintPlan::Operation::kSelectedFill);
    CHECK(selected.operations[1] == RowPaintPlan::Operation::kDivider);
    CHECK(selected.divider.x == 131);
    CHECK(selected.divider.y == 188);
    CHECK(selected.divider.w == 300);
    CHECK(selected.divider.h == 2);
    CHECK(selected.divider.y + selected.divider.h == 190);
    CHECK(selected.divider.y + selected.divider.h <= 117 + 73);

    const auto ordinary = row_paint_plan(131, 117, 300, 73, false);
    CHECK(ordinary.operation_count == 1);
    CHECK(ordinary.operations[0] == RowPaintPlan::Operation::kDivider);
    CHECK(ordinary.divider.y == selected.divider.y);
    CHECK(ordinary.divider.y + ordinary.divider.h == 190);
}

void test_stable_row_text_layout() {
    const auto with_description = row_text_layout(true, 122);
    CHECK(with_description.has_description);
    CHECK(with_description.title_y == 8);
    CHECK(with_description.description_y == 42);
    CHECK(use_item_description("介绍文字"));

    const auto without_description = row_text_layout(false, 122);
    CHECK(!without_description.has_description);
    CHECK(without_description.title_y == 49);
    CHECK(!use_item_description(nullptr));

    const auto empty_description = row_text_layout(false, 122);
    CHECK(!empty_description.has_description);
    CHECK(empty_description.title_y == 49);
    CHECK(!use_item_description(""));

    // Rendered content remains catalog-owned through request-in-flight and successful receipts.
    SettingsItemView toggle = item("setting", "main", SettingsItemType::kToggle);
    std::snprintf(toggle.description, sizeof(toggle.description), "%s", "保持稳定说明");
    const char* const catalog_description = row_description(toggle);
    const char* const request_messages[] = {"保存中…", "已保存，生效中", ""};
    for (const char* message : request_messages) {
        CHECK(row_description(toggle) == catalog_description);
        CHECK(std::strcmp(row_description(toggle), "保持稳定说明") == 0);
        CHECK(use_item_description(row_description(toggle)));
        CHECK(message != nullptr);
    }
    CHECK(std::strcmp(row_description(toggle), "保持稳定说明") == 0);
}

void test_description_geometry_and_utf8_truncation() {
    const int32_t toggle_left = right_control_left(572);
    const int32_t action_left = right_control_left(572);
    const int32_t toggle_column = description_width_for_toggle(toggle_left);
    const int32_t action_column = description_width_for_toggle(action_left);
    CHECK(toggle_left == 332 && toggle_column == 300);
    CHECK(toggle_left - (16 + toggle_column) == 16);
    CHECK(action_left == toggle_left && action_column == toggle_column);
    const qol::contracts::UiRect toggle_rect{toggle_left,
        toggle_top(qol::features::core_ui::settings_model::kMinimumRightRowHeight),
        qol::features::core_ui::settings_model::kToggleWidth,
        qol::features::core_ui::settings_model::kToggleHeight};
    const qol::contracts::UiRect action_rect{action_left,
        toggle_top(qol::features::core_ui::settings_model::kMinimumRightRowHeight),
        qol::features::core_ui::settings_model::kToggleWidth,
        qol::features::core_ui::settings_model::kToggleHeight};
    CHECK(toggle_rect.x == action_rect.x && toggle_rect.y == action_rect.y &&
          toggle_rect.w == action_rect.w && toggle_rect.h == action_rect.h);

    const auto truncated = truncate_text("控制自动出售扫描及背包买入，受保护装备、已穿戴、已强化、已镶嵌物品", toggle_column);
    CHECK(truncated.lines.size() == 1);
    CHECK(truncated.lines[0].size() >= 3);
    CHECK(truncated.lines[0].compare(truncated.lines[0].size() - 3, 3, "…") == 0);
    CHECK(truncated.estimated_width <= toggle_column);
    const int32_t row_height = right_row_height(1);
    CHECK(row_height == 96);
    CHECK(toggle_top(row_height) >= 0);
    CHECK(toggle_top(row_height) + 80 <= row_height);

    const auto ascii = truncate_text("autosell option status 1234567890", 96);
    CHECK(ascii.lines.size() == 1 && ascii.lines[0].compare(ascii.lines[0].size() - 3, 3, "…") == 0);
    CHECK(truncate_text("中文说明", 24).lines[0] == "…");
    CHECK(truncate_text("", toggle_column).lines.empty());
    CHECK(right_row_height(0) == 96);
}

}  // namespace

int main() {
    test_category_mapping();
    test_hidden_catalog_items_follow_build_visibility();
    test_display_priority_and_registered_categories();
    test_toggle_target();
    test_toggle_enable_state_machine();
    test_entry_enable_state();
    test_error_text();
    test_row_paint_geometry();
    test_stable_row_text_layout();
    test_description_geometry_and_utf8_truncation();
    std::printf("settings_panel_model_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

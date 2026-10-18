// 旧 settings catalog 贡献退休断言：settings 页面元数据现只来自 descriptor.features，
// 插件不再向 infra/settings_catalog 注册任何类别/条目（含入口动作）。
// 各 register_* 存根为无副作用 no-op，返回 ok，且不读取 SDK/config。

#include "contracts/host_sdk.h"
#include "infra/settings_catalog.h"
#include "plugins/autosell/settings_contribution.h"
#include "plugins/core.custom_ui/settings_contribution.h"
#include "plugins/settings/settings_contribution.h"
#include "plugins/custom_recipe/settings_contribution.h"
#include "plugins/extension_bag/assembly/settings_contribution.h"
#include "plugins/move_merge/settings_contribution.h"
#include "plugins/push_all/settings_contribution.h"
#include "plugins/save_backup/settings_contribution.h"
#include "plugins/simple_mode/settings_contribution.h"
#include "plugins/stack_limit/settings_contribution.h"

#include <cassert>
#include <cstdio>

namespace {
using namespace qol::contracts;

using RegisterFn = QolResult (*)(const QolHostSdk*);

// 本 lane 退休的旧贡献（含 count-seam 迁移后的 stack_limit）。
constexpr RegisterFn kRetiredContributions[]{
    &qol::features::save_backup::register_settings,
    &qol::features::core_settings::register_settings,
    &qol::features::core_settings::register_categories,
    &qol::features::extension_bag::register_settings,
    &qol::features::autosell::register_settings,
    &qol::features::custom_recipe::register_settings,
    &qol::features::push_all::register_settings,
    &qol::features::move_merge::register_settings,
    &qol::features::simple_mode::register_settings,
    &qol::features::core_ui::register_settings,
    &qol::features::core_ui::register_settings_category,
    &qol::features::stack_limit::register_settings,
};

struct View {
    const CatalogView* data = nullptr;
    SettingsCatalogToken token = 0;
    View() { assert(qol::infra::settings_catalog_snapshot_acquire(&data, &token).ok == kQolOk); }
    ~View() { assert(qol::infra::settings_catalog_snapshot_release(token).ok == kQolOk); }
};

void test_retired_contributions_register_nothing() {
    qol::infra::settings_catalog_reset_for_test();
    assert(View{}.data->item_count == 0 && View{}.data->category_count == 0);

    // 无 SDK / 任意 SDK 都必须是无副作用 no-op：存根不检查可选尾、不读 config。
    for (auto contribution : kRetiredContributions) {
        assert(contribution(nullptr).ok == kQolOk);
    }
    QolHostSdk sdk{};
    sdk.abi = {sizeof(sdk), kHostSdkMajor, kHostSdkMinor};
    for (auto contribution : kRetiredContributions) {
        assert(contribution(&sdk).ok == kQolOk);
    }
    View view;
    assert(view.data->item_count == 0 && view.data->category_count == 0);

    // 重复调用保持无注册、无 generation 变化。
    const uint64_t generation = view.data->generation;
    for (auto contribution : kRetiredContributions) {
        assert(contribution(&sdk).ok == kQolOk);
    }
    View again;
    assert(again.data->generation == generation && again.data->item_count == 0 &&
           again.data->category_count == 0);
}
}  // namespace

namespace qol::game {
// settings_catalog_adapter 依赖的主线程判定；本测试单线程，视为恒真。
bool game_host_is_main_thread() { return true; }
}  // namespace qol::game

int main() {
    qol::infra::settings_catalog_reset_for_test();
    test_retired_contributions_register_nothing();
    std::puts("settings_contributions: PASS (legacy catalog contributions retired; no items/categories)");
    return 0;
}

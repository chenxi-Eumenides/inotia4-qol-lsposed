#include "contracts/host_sdk.h"
#include "infra/settings_catalog.h"

#include <cassert>
#include <cstring>
#include <array>
#include <string>
#include <thread>
#include <type_traits>

using namespace qol::contracts;

static_assert(std::is_standard_layout_v<SettingsCategorySpec>);
static_assert(std::is_trivial_v<SettingsItemSpec>);

namespace {
using namespace qol::infra;
int g_callbacks = 0;
QolResult open(void*) { ++g_callbacks; return ok(); }

SettingsCategorySpec category_of(const char* id = "sample", const char* owner = "sample.owner") {
    return {sizeof(SettingsCategorySpec), id, owner, "Sample", "Description"};
}
SettingsItemSpec item_of(const char* id = "sample.enable", const char* category = "sample",
                         const char* owner = "sample.owner") {
    return {sizeof(SettingsItemSpec), id, category, owner, "Enable", "Feature gate",
            SettingsItemType::kToggle, "sampleEnabled", 1, nullptr, nullptr, nullptr};
}
struct View {
    const CatalogView* data = nullptr;
    SettingsCatalogToken token = 0;
    View() { assert(settings_catalog_snapshot_acquire(&data, &token).ok == kQolOk); }
    ~View() { assert(settings_catalog_snapshot_release(token).ok == kQolOk); }
};

void test_atomic_conflict_idempotence() {
    settings_catalog_reset_for_test();
    auto category = category_of();
    auto item = item_of();
    const uint64_t initial_generation = View{}.data->generation;
    assert(settings_catalog_register_category(&category, &item, 1).ok == kQolOk);
    View before;
    assert(before.data->generation != initial_generation);
    assert(settings_catalog_register_category(&category, &item, 1).ok == kQolOk);
    assert(View{}.data->generation == before.data->generation);
    category.title = "different";
    assert(settings_catalog_register_category(&category, &item, 1).code == kQolErrConflict);
    category = category_of();
    item.description = "different";
    assert(settings_catalog_register_category(&category, &item, 1).code == kQolErrConflict);
    item = item_of();
    category.owner = item.owner = "other.owner";
    assert(settings_catalog_register_category(&category, &item, 1).code == kQolErrConflict);
    category = category_of("other", "other.owner");
    std::array<SettingsItemSpec, 2> bundle{{item_of("new", "other", "other.owner"),
                                         item_of("sample.enable", "other", "other.owner")}};
    assert(settings_catalog_register_category(&category, bundle.data(), 2).code == kQolErrConflict);
    View after;
    assert(after.data->category_count == 1 && after.data->item_count == 1);
    assert(after.data->generation == before.data->generation);
    bundle[1] = bundle[0];
    assert(settings_catalog_register_category(&category, bundle.data(), 2).code == kQolErrConflict);
    bundle[1] = item_of("new2", "other", "other.owner");
    bundle[1].label = "";
    assert(settings_catalog_register_category(&category, bundle.data(), 2).code == kQolErrInvalidArgument);
    assert(View{}.data->category_count == 1);
}

void test_deep_copy_snapshot_and_generation() {
    settings_catalog_reset_for_test();
    std::string id = "sample", owner = "sample.owner", title = "Sample", desc = "Description";
    std::string iid = "sample.enable", label = "Enable", item_desc = "Feature gate", key = "sampleEnabled";
    SettingsCategorySpec category{sizeof(SettingsCategorySpec), id.c_str(), owner.c_str(),
                                  title.c_str(), desc.c_str()};
    SettingsItemSpec item{sizeof(SettingsItemSpec), iid.c_str(), id.c_str(), owner.c_str(),
                          label.c_str(), item_desc.c_str(), SettingsItemType::kToggle, key.c_str(),
                          1, nullptr, nullptr, nullptr};
    assert(settings_catalog_register_category(&category, &item, 1).ok == kQolOk);
    id = owner = title = desc = iid = label = item_desc = key = "mutated";
    View old;
    assert(std::strcmp(old.data->categories[0].id, "sample") == 0);
    assert(std::strcmp(old.data->categories[0].owner, "sample.owner") == 0);
    assert(std::strcmp(old.data->categories[0].title, "Sample") == 0);
    assert(std::strcmp(old.data->categories[0].description, "Description") == 0);
    assert(std::strcmp(old.data->items[0].id, "sample.enable") == 0);
    assert(std::strcmp(old.data->items[0].category_id, "sample") == 0);
    assert(std::strcmp(old.data->items[0].owner, "sample.owner") == 0);
    assert(std::strcmp(old.data->items[0].label, "Enable") == 0);
    assert(std::strcmp(old.data->items[0].description, "Feature gate") == 0);
    assert(std::strcmp(old.data->items[0].config_key, "sampleEnabled") == 0);
    assert(settings_catalog_unregister_owner("sample.owner").ok == kQolOk);
    View current;
    assert(current.data->item_count == 0 && current.data->category_count == 0);
    assert(current.data->generation != old.data->generation);
    assert(std::strcmp(old.data->items[0].label, "Enable") == 0);
    SettingsActionRecord record{};
    assert(settings_catalog_resolve_action("missing", current.data->generation, &record).code == kQolErrNotFound);
    assert(settings_catalog_resolve_action("sample.enable", old.data->generation, &record).code == kQolErrConflict);
    assert(settings_catalog_unregister_owner("missing").ok == kQolOk);
    assert(View{}.data->generation == current.data->generation);
}

void test_shared_category_owners_and_registration_order() {
    settings_catalog_reset_for_test();
    auto category = category_of("shared", "category.author");
    auto author_item = item_of("author.item", "shared", "category.author");
    assert(settings_catalog_register_category(&category, &author_item, 1).ok == kQolOk);

    auto foreign = item_of("foreign.item", "shared", "foreign.owner");
    const uint64_t before_join = View{}.data->generation;
    assert(settings_catalog_register_items("foreign.owner", "shared", &foreign, 1).ok == kQolOk);
    View joined;
    assert(joined.data->category_count == 1 && joined.data->item_count == 2);
    assert(joined.data->generation != before_join);
    assert(std::strcmp(joined.data->categories[0].owner, "category.author") == 0);
    assert(std::strcmp(joined.data->categories[0].title, "Sample") == 0);
    assert(std::strcmp(joined.data->items[1].owner, "foreign.owner") == 0);

    const uint64_t before_duplicate = joined.data->generation;
    assert(settings_catalog_register_items("foreign.owner", "shared", &foreign, 1).ok == kQolOk);
    assert(View{}.data->generation == before_duplicate);
    auto conflict = foreign;
    conflict.label = "Different";
    assert(settings_catalog_register_items("foreign.owner", "shared", &conflict, 1).code ==
           kQolErrConflict);
    auto other_owner = item_of("foreign.item", "shared", "other.owner");
    assert(settings_catalog_register_items("other.owner", "shared", &other_owner, 1).code ==
           kQolErrConflict);
    assert(View{}.data->generation == before_duplicate);

    std::array<SettingsItemSpec, 2> atomic{{item_of("new.item", "shared", "foreign.owner"),
                                           item_of("author.item", "shared", "foreign.owner")}};
    assert(settings_catalog_register_items("foreign.owner", "shared", atomic.data(), atomic.size()).code ==
           kQolErrConflict);
    View unchanged;
    assert(unchanged.data->item_count == 2 && unchanged.data->generation == before_duplicate);

    assert(settings_catalog_unregister_owner("category.author").ok == kQolOk);
    View after_author_exit;
    assert(after_author_exit.data->category_count == 1 && after_author_exit.data->item_count == 1);
    assert(std::strcmp(after_author_exit.data->categories[0].owner, "category.author") == 0);
    assert(std::strcmp(after_author_exit.data->items[0].owner, "foreign.owner") == 0);
    assert(std::strcmp(joined.data->items[1].id, "foreign.item") == 0);
    assert(settings_catalog_unregister_owner("foreign.owner").ok == kQolOk);
    assert(View{}.data->category_count == 0 && View{}.data->item_count == 0);

    settings_catalog_reset_for_test();
    auto first = category_of("custom.first", "one");
    auto second = category_of("custom.second", "two");
    auto first_item = item_of("first.item", "custom.first", "one");
    auto second_item = item_of("second.item", "custom.second", "two");
    assert(settings_catalog_register_category(&first, &first_item, 1).ok == kQolOk);
    assert(settings_catalog_register_category(&second, &second_item, 1).ok == kQolOk);
    View sequence;
    assert(std::strcmp(sequence.data->categories[0].id, "custom.first") == 0);
    assert(std::strcmp(sequence.data->categories[1].id, "custom.second") == 0);
}

void test_metadata_bootstrap_join_and_owner_removal() {
    settings_catalog_reset_for_test();
    auto basic = category_of(kSettingsBasicCategoryId, "core.settings");
    basic.title = "基础设置";
    auto optimization = category_of(kSettingsOptimizationCategoryId, "core.settings");
    optimization.title = "优化功能";
    assert(settings_catalog_register_category_metadata(&basic).ok == kQolOk);
    assert(settings_catalog_register_category_metadata(&optimization).ok == kQolOk);
    const uint64_t boot_generation = View{}.data->generation;
    assert(settings_catalog_register_category_metadata(&basic).ok == kQolOk);
    assert(View{}.data->generation == boot_generation);
    auto conflict = basic;
    conflict.title = "different";
    assert(settings_catalog_register_category_metadata(&conflict).code == kQolErrConflict);

    auto custom = category_of("custom.third", "custom.owner");
    assert(settings_catalog_register_category_metadata(&custom).ok == kQolOk);
    auto joined = item_of("custom.item", "custom.third", "custom.item.owner");
    assert(settings_catalog_register_items("custom.item.owner", "custom.third", &joined, 1).ok == kQolOk);
    View with_custom;
    assert(with_custom.data->category_count == 3 && with_custom.data->item_count == 1);
    assert(std::strcmp(with_custom.data->categories[0].id, kSettingsBasicCategoryId) == 0);
    assert(std::strcmp(with_custom.data->categories[1].id, kSettingsOptimizationCategoryId) == 0);
    assert(std::strcmp(with_custom.data->categories[2].id, "custom.third") == 0);

    auto foreign = item_of("foreign.basic", kSettingsBasicCategoryId, "foreign.owner");
    assert(settings_catalog_register_items("foreign.owner", kSettingsBasicCategoryId, &foreign, 1).ok == kQolOk);
    auto other_empty = category_of("other.empty", "other.author");
    assert(settings_catalog_register_category_metadata(&other_empty).ok == kQolOk);
    assert(settings_catalog_unregister_owner("core.settings").ok == kQolOk);
    View after_author;
    assert(after_author.data->category_count == 3 && after_author.data->item_count == 2);
    assert(std::strcmp(after_author.data->categories[0].id, kSettingsBasicCategoryId) == 0);
    assert(std::strcmp(after_author.data->categories[1].id, "custom.third") == 0);
    assert(std::strcmp(after_author.data->categories[2].id, "other.empty") == 0);
    assert(std::strcmp(after_author.data->categories[0].owner, "core.settings") == 0);
    assert(settings_catalog_unregister_owner("other.author").ok == kQolOk);
    View after_empty_author;
    assert(after_empty_author.data->category_count == 2);

    settings_catalog_reset_for_test();
    std::array<std::string, kSettingsMaxCategories> ids;
    for (uint32_t i = 0; i < kSettingsMaxCategories; ++i) {
        ids[i] = "metadata." + std::to_string(i);
        auto entry = category_of(ids[i].c_str(), "metadata.owner");
        assert(settings_catalog_register_category_metadata(&entry).ok == kQolOk);
    }
    const uint64_t full_generation = View{}.data->generation;
    auto overflow = category_of("metadata.overflow", "metadata.owner");
    assert(settings_catalog_register_category_metadata(&overflow).code == kQolErrBlocked);
    View full;
    assert(full.data->category_count == kSettingsMaxCategories &&
           full.data->generation == full_generation);
}

void test_invalid_fields_and_text() {
    settings_catalog_reset_for_test();
    auto category = category_of();
    auto item = item_of();
    assert(settings_catalog_register_category(nullptr, &item, 1).code == kQolErrInvalidArgument);
    assert(settings_catalog_register_category(&category, nullptr, 1).code == kQolErrInvalidArgument);
    assert(settings_catalog_register_category(&category, &item, 0).code == kQolErrInvalidArgument);
    auto bad_category = category;
    bad_category.size = sizeof(SettingsCategorySpec) - 1;
    assert(settings_catalog_register_category(&bad_category, &item, 1).code == kQolErrInvalidArgument);
    for (auto member : {&SettingsCategorySpec::id, &SettingsCategorySpec::owner, &SettingsCategorySpec::title}) {
        for (const char* value : {static_cast<const char*>(nullptr), ""}) {
            bad_category = category;
            bad_category.*member = value;
            assert(settings_catalog_register_category(&bad_category, &item, 1).code == kQolErrInvalidArgument);
        }
    }
    auto reject = [&](SettingsItemSpec bad) {
        assert(settings_catalog_register_category(&category, &bad, 1).code == kQolErrInvalidArgument);
    };
    for (auto member : {&SettingsItemSpec::id, &SettingsItemSpec::category_id, &SettingsItemSpec::owner,
                        &SettingsItemSpec::label, &SettingsItemSpec::config_key}) {
        auto bad = item;
        bad.*member = nullptr;
        reject(bad);
        bad.*member = "";
        reject(bad);
    }
    auto bad = item;
    bad.size = 0; reject(bad);
    bad = item; bad.category_id = "different"; reject(bad);
    bad = item; bad.owner = "different"; reject(bad);
    bad = item; bad.owner_enable = 2; reject(bad);
    bad = item; bad.open = &open; reject(bad);
    bad = item; bad.type = static_cast<SettingsItemType>(99); reject(bad);
    bad = item; bad.type = SettingsItemType::kAction; reject(bad);
    bad.config_key = nullptr; reject(bad);
    bad.open = &open; reject(bad);  // action cannot be an owner-enable toggle
    bad.owner_enable = 0;
    assert(settings_catalog_register_category(&category, &bad, 1).ok == kQolOk);
    assert(settings_catalog_register_category(&category, &bad, 1).ok == kQolOk);
    int other_context = 0;
    bad.user = &other_context;
    assert(settings_catalog_register_category(&category, &bad, 1).code == kQolErrConflict);
    bad.user = nullptr;
    bad.open = +[](void*) { return ok(); };
    assert(settings_catalog_register_category(&category, &bad, 1).code == kQolErrConflict);
    assert(g_callbacks == 0);
    settings_catalog_reset_for_test();
    std::string oversized(kSettingsDescriptionCapacity, 'x');
    bad_category = category; bad_category.description = oversized.c_str();
    assert(settings_catalog_register_category(&bad_category, &item, 1).code == kQolErrInvalidArgument);
    bad = item; bad.description = oversized.c_str(); reject(bad);
    oversized.assign(kSettingsIdCapacity, 'x');
    bad = item; bad.id = oversized.c_str(); reject(bad);
    oversized.assign(kSettingsTitleCapacity, 'x');
    bad = item; bad.label = oversized.c_str(); reject(bad);
    oversized.assign(kSettingsConfigKeyCapacity, 'x');
    bad = item; bad.config_key = oversized.c_str(); reject(bad);
    assert(settings_catalog_unregister_owner("").code == kQolErrInvalidArgument);
    assert(View{}.data->category_count == 0);
    // Largest permitted description is copied intact; optional descriptions normalize to empty.
    oversized.assign(kSettingsDescriptionCapacity - 1, 'x');
    item.description = oversized.c_str(); category.description = nullptr;
    assert(settings_catalog_register_category(&category, &item, 1).ok == kQolOk);
    View view;
    assert(std::strlen(view.data->items[0].description) == kSettingsDescriptionCapacity - 1);
    assert(view.data->categories[0].description[0] == '\0');
}

void test_capacity_and_snapshot_tokens() {
    settings_catalog_reset_for_test();
    std::array<std::string, kSettingsMaxItems> ids;
    std::array<SettingsItemSpec, kSettingsMaxItems> items;
    for (uint32_t i = 0; i < items.size(); ++i) {
        ids[i] = "item." + std::to_string(i);
        items[i] = item_of(ids[i].c_str());
    }
    auto category = category_of();
    assert(settings_catalog_register_category(&category, items.data(), kSettingsMaxItems + 1).code == kQolErrBlocked);
    assert(settings_catalog_register_category(&category, items.data(), items.size()).ok == kQolOk);
    assert(settings_catalog_register_category(&category, items.data(), items.size()).ok == kQolOk);
    auto extra_category = category_of("extra");
    auto extra = item_of("extra.item", "extra");
    assert(settings_catalog_register_category(&extra_category, &extra, 1).code == kQolErrBlocked);
    assert(View{}.data->category_count == 1);
    settings_catalog_reset_for_test();
    for (uint32_t i = 0; i < kSettingsMaxCategories; ++i) {
        std::string category_id = "category." + std::to_string(i);
        category = category_of(category_id.c_str());
        items[i].category_id = category_id.c_str();
        assert(settings_catalog_register_category(&category, &items[i], 1).ok == kQolOk);
    }
    assert(settings_catalog_register_category(&extra_category, &extra, 1).code == kQolErrBlocked);
    assert(View{}.data->item_count == kSettingsMaxCategories);
    std::array<SettingsCatalogToken, kSettingsMaxSnapshots> tokens{};
    const CatalogView* view = nullptr;
    for (auto& token : tokens) assert(settings_catalog_snapshot_acquire(&view, &token).ok == kQolOk);
    SettingsCatalogToken overflow = 123;
    assert(settings_catalog_snapshot_acquire(&view, &overflow).code == kQolErrBlocked);
    assert(view == nullptr && overflow == 0);
    assert(settings_catalog_snapshot_release(0).code == kQolErrNotFound);
    for (auto token : tokens) {
        assert(settings_catalog_snapshot_release(token).ok == kQolOk);
        assert(settings_catalog_snapshot_release(token).code == kQolErrNotFound);
    }
    assert(settings_catalog_snapshot_acquire(nullptr, &overflow).code == kQolErrInvalidArgument);
    assert(settings_catalog_snapshot_acquire(&view, nullptr).code == kQolErrInvalidArgument);
    std::thread reader([] {
        View snapshot;
        assert(snapshot.data->category_count == kSettingsMaxCategories);
    });
    reader.join();
}
}  // namespace

int main() {
    test_atomic_conflict_idempotence();
    test_deep_copy_snapshot_and_generation();
    test_shared_category_owners_and_registration_order();
    test_metadata_bootstrap_join_and_owner_removal();
    test_invalid_fields_and_text();
    test_capacity_and_snapshot_tokens();
    qol::infra::settings_catalog_reset_for_test();
    SettingsCategorySpec category{sizeof(SettingsCategorySpec), "sample", "sample.owner",
                                  "Sample", "Description"};
    SettingsItemSpec item{sizeof(SettingsItemSpec), "sample.enable", "sample", "sample.owner",
                          "Enable", "Feature gate", SettingsItemType::kToggle, "sampleEnabled",
                          1, nullptr, nullptr, nullptr};
    assert(qol::infra::settings_catalog_register_category(&category, &item, 1).ok == kQolOk);
    const CatalogView* view = nullptr;
    SettingsCatalogToken token = 0;
    assert(qol::infra::settings_catalog_snapshot_acquire(&view, &token).ok == kQolOk);
    assert(view->category_count == 1 && view->item_count == 1);
    assert(std::strcmp(view->items[0].label, "Enable") == 0);
    assert(qol::infra::settings_catalog_snapshot_release(token).ok == kQolOk);
}

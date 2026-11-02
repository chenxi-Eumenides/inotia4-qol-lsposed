#include "plugins/extension_bag/model/item_handle_custody.h"
#include "game/access/item_handle_registry.h"

#include <cstdio>

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (condition) {                                                                  \
            ++g_pass;                                                                     \
        } else {                                                                          \
            ++g_fail;                                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);             \
        }                                                                                 \
    } while (false)

bool test_main_thread() { return true; }

void test_create_move_release_and_release_failure() {
    using qol::contracts::QolItemHandle;
    using qol::features::extension_bag::model::ItemHandleCustody;
    ItemHandleCustody custody;
    int item = 1;
    void* resolved = nullptr;
    const QolItemHandle created{1, 1};

    CHECK(custody.bind(&item, created));
    CHECK(custody.lookup(created, &resolved) && resolved == &item);
    CHECK(custody.lookup(created, &resolved) && resolved == &item);  // move keeps identity

    CHECK(custody.begin_release(created));
    CHECK(!custody.lookup(created, &resolved));
    CHECK(custody.finish_release(created, false));                    // release failed; restore
    CHECK(custody.lookup(created, &resolved) && resolved == &item);
    CHECK(custody.begin_release(created));
    CHECK(custody.finish_release(created, true));                     // release committed
    CHECK(!custody.lookup(created, &resolved));
    CHECK(!custody.finish_release(created, true));                    // double release rejected
}

void test_replace_merge_adopt_rollback_and_load() {
    using qol::contracts::QolItemHandle;
    using qol::features::extension_bag::model::ItemHandleCustody;
    ItemHandleCustody custody;
    int old_item = 1;
    int replacement_item = 2;
    int survivor_item = 3;
    void* resolved = nullptr;

    const QolItemHandle old_handle{7, 4};
    const QolItemHandle replacement{8, 1};
    CHECK(custody.bind(&old_item, old_handle));
    CHECK(custody.bind(&replacement_item, replacement));                 // replace is new identity
    CHECK(custody.lookup(old_handle, &resolved) && resolved == &old_item);

    const QolItemHandle merge_source{9, 1};
    const QolItemHandle merge_survivor{10, 1};
    CHECK(custody.bind(&old_item, merge_source));
    CHECK(custody.bind(&survivor_item, merge_survivor));
    CHECK(custody.begin_release(merge_source));
    CHECK(custody.finish_release(merge_source, true));                  // consumed source removed
    CHECK(!custody.lookup(merge_source, &resolved));
    CHECK(custody.lookup(merge_survivor, &resolved) && resolved == &survivor_item);

    const QolItemHandle adopted_old{11, 3};
    const QolItemHandle adopted_new{11, 4};
    CHECK(custody.bind(&old_item, adopted_old));
    CHECK(custody.rebind(&old_item, adopted_old, adopted_new));         // adoption gets new gen
    CHECK(!custody.lookup(adopted_old, &resolved));
    CHECK(custody.lookup(adopted_new, &resolved) && resolved == &old_item);

    const QolItemHandle rollback{12, 2};
    CHECK(custody.bind(&old_item, rollback));
    CHECK(custody.begin_release(rollback));
    CHECK(custody.finish_release(rollback, false));                     // uncertain rollback stays old
    CHECK(custody.lookup(rollback, &resolved) && resolved == &old_item);

    custody.invalidate_all();                                           // load/rebuild invalidates old set
    CHECK(!custody.lookup(rollback, &resolved));
    CHECK(custody.bind(&old_item, QolItemHandle{12, 3}));              // new load identity
    CHECK(!custody.lookup(rollback, &resolved));
}

void test_register_failure_and_address_reuse() {
    using qol::contracts::QolItemHandle;
    using qol::features::extension_bag::model::ItemHandleCustody;
    ItemHandleCustody custody;
    int item = 1;
    void* resolved = nullptr;

    CHECK(!custody.bind(&item, QolItemHandle{0, 1}));                    // registration failed
    const QolItemHandle before_reuse{4, 2};
    const QolItemHandle after_reuse{4, 3};
    CHECK(custody.bind(&item, before_reuse));
    CHECK(custody.rebind(&item, before_reuse, after_reuse));
    CHECK(!custody.lookup(before_reuse, &resolved));
    CHECK(custody.lookup(after_reuse, &resolved) && resolved == &item);
}

void test_original_insert_handle_resolution_and_rollback_order() {
    using qol::contracts::QolItemHandle;
    using qol::features::extension_bag::model::ItemHandleCustody;
    using qol::game::ItemHandleError;
    using qol::game::ItemHandleRegistry;

    ItemHandleRegistry registry(&test_main_thread);
    ItemHandleCustody custody;
    int original_item = 41;
    ItemHandleError error{};
    const QolItemHandle game_owned = registry.register_item(
        &original_item, qol::contracts::kQolItemHandleOwnerGame, &error);
    CHECK(error == ItemHandleError::kNone);
    void* resolved = nullptr;
    CHECK(!custody.lookup(game_owned, &resolved));
    CHECK(registry.invalidate_item(game_owned));

    const QolItemHandle inserted = registry.register_item(
        &original_item, qol::contracts::kQolItemHandleOwnerProvider, &error);
    CHECK(error == ItemHandleError::kNone);
    CHECK(inserted.slot != 0 && inserted.generation != 0);
    CHECK(custody.bind(&original_item, inserted));

    QolItemHandle before[3]{};
    CHECK(qol::features::extension_bag::model::find_handle_slot(inserted, before, 3) == -1);
    QolItemHandle after[3]{};
    after[2] = inserted;
    CHECK(qol::features::extension_bag::model::find_handle_slot(inserted, after, 3) == 2);
    CHECK(custody.lookup(after[2], &resolved) && resolved == &original_item);

    // A rollback first removes the visible original-inventory entry, then invalidates
    // the provider handle and releases private custody.
    after[2] = {};
    CHECK(qol::features::extension_bag::model::find_handle_slot(inserted, after, 3) == -1);
    CHECK(custody.begin_release(inserted));
    CHECK(registry.invalidate_item(inserted));
    CHECK(custody.finish_release(inserted, true));
    CHECK(!custody.lookup(inserted, &resolved));
    const auto stale = registry.resolve_item(
        inserted, qol::contracts::kQolItemHandleOwnerProvider);
    CHECK(stale.error == ItemHandleError::kStale);
}

}  // namespace

namespace qol::game {
bool game_host_is_main_thread() { return true; }
}  // namespace qol::game

int main() {
    test_create_move_release_and_release_failure();
    test_replace_merge_adopt_rollback_and_load();
    test_register_failure_and_address_reuse();
    test_original_insert_handle_resolution_and_rollback_order();
    std::printf("item_handle_custody: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

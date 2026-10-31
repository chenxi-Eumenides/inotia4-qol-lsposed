#include "contracts/inventory_access.h"
#include "plugins/core.inventory/api/access.h"
#include "plugins/core.inventory/plugin.h"
#include "plugins/core.inventory/store/slot_table.h"

#include <cassert>
#include <cstring>
#include <memory>
#include <vector>

namespace {

using qol::contracts::QolInventoryItem;
using qol::contracts::QolInventoryItemRef;
using qol::contracts::QolInventorySlotRef;
using qol::contracts::QolItemHandle;
using qol::features::core_inventory::inventory_access_api;
using qol::features::core_inventory::inventory_store;
using qol::features::core_inventory::store::MemorySlotTable;

QolInventorySlotRef ref(int32_t space_id, int32_t bag, int32_t slot) {
    return QolInventorySlotRef{space_id, bag, slot};
}

QolInventoryItem item(uint32_t handle_slot, uint32_t generation, int32_t category,
                      int32_t count) {
    return QolInventoryItem{{handle_slot, generation}, category, count};
}

void assert_ok(const QolResult& result) { assert(result.ok == kQolOk); }

void assert_error(const QolResult& result, int32_t code) {
    assert(result.ok != kQolOk);
    assert(result.code == code);
}

void test_descriptor() {
    const auto& descriptor = qol::features::core_inventory::plugin_descriptor();
    assert(std::strcmp(descriptor.id, "core.inventory") == 0);
    assert(descriptor.provides_count == 2);
    assert(std::strcmp(descriptor.provides[0].id, "inventory.access") == 0);
    assert(std::strcmp(descriptor.provides[1].id, "inventory.bag_source") == 0);
    assert(descriptor.feature_count == 1);
    assert(descriptor.port_count == 1);
    assert(descriptor.ports[0].id == qol::contracts::QolPortId::QOL_PORT_ITEM);
    assert(descriptor.resources_count == 12);
    for (uint32_t i = 0; i < descriptor.resources_count; ++i) {
        assert(std::strcmp(descriptor.resources[i].owner, "core.inventory") == 0);
    }
    assert(std::strcmp(descriptor.resources[0].id, "hook:INVEN_FindItem") == 0);
}

void test_store() {
    auto& store = inventory_store();
    store.clear();
    const auto& api = inventory_access_api();
    assert(qol::contracts::qol_inventory_access_api_valid(&api));

    assert(store.add_table(std::make_unique<MemorySlotTable>(2, std::vector<int32_t>{2, 3})));
    assert(!store.add_table(std::make_unique<MemorySlotTable>(2, std::vector<int32_t>{1})));

    // 空槽查询与首次落位。
    QolInventorySlotRef empty{};
    assert_ok(api.find_empty(api.context, 2, &empty));
    assert(empty.space_id == 2 && empty.bag == 0 && empty.slot == 0);

    const QolInventoryItem first = item(1, 1, 10, 5);
    assert_ok(api.place(api.context, &empty, &first));
    QolInventoryItemRef view{};
    assert_ok(api.item_at(api.context, &empty, &view));
    assert(view.space_id == 2 && view.bag == 0 && view.slot == 0);
    assert(view.category == 10 && view.count == 5);
    assert(view.kind == static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider));

    QolItemHandle handle{1, 1};
    assert_ok(api.locate(api.context, handle, &view));
    assert(view.bag == 0 && view.slot == 0);

    int32_t count = 0;
    assert_ok(api.count_item(api.context, 10, &count));
    assert(count == 5);
    assert_ok(api.count_item(api.context, 11, &count));
    assert(count == 0);

    // 移动到空槽；源清空，目标可读。
    const QolInventorySlotRef target = ref(2, 1, 2);
    assert_ok(api.move(api.context, &empty, &target));
    assert_error(api.item_at(api.context, &empty, &view), kQolErrNotFound);
    assert_ok(api.item_at(api.context, &target, &view));
    assert(view.count == 5);

    // 部分移除只改数量，不清槽。
    assert_ok(api.remove_count(api.context, &target, 2));
    assert_ok(api.item_at(api.context, &target, &view));
    assert(view.count == 3);
    assert_error(api.remove_count(api.context, &target, 4), kQolErrConflict);
    assert_ok(api.item_at(api.context, &target, &view));
    assert(view.count == 3);

    // 目标已占用时拒绝移动；源槽不变。
    const QolInventorySlotRef occupied = ref(2, 0, 1);
    const QolInventoryItem second = item(2, 1, 10, 1);
    assert_ok(api.place(api.context, &occupied, &second));
    assert_error(api.move(api.context, &target, &occupied), kQolErrConflict);
    assert_ok(api.item_at(api.context, &target, &view));
    assert(view.count == 3);

    // 整件取出后槽为空。
    QolInventoryItem taken{};
    assert_ok(api.take(api.context, &target, &taken));
    assert(taken.item.slot == 1 && taken.item.generation == 1);
    assert(taken.category == 10 && taken.count == 3);
    assert_error(api.item_at(api.context, &target, &view), kQolErrNotFound);

    // 非法参数与未知空间。
    assert_error(api.item_at(api.context, nullptr, &view), kQolErrInvalidArgument);
    const QolInventorySlotRef missing = ref(9, 0, 0);
    assert_error(api.item_at(api.context, &missing, &view), kQolErrNotFound);
    assert_error(api.find_empty(api.context, 9, &empty), kQolErrNotFound);

    // 任务袋只读：可读、不可写、不可作为移动源/目标。
    assert(store.add_table(std::make_unique<MemorySlotTable>(
        0, std::vector<int32_t>{3, 3, 3, 3, 3, 3})));
    auto* task_table = store.find_table(0);
    assert(task_table != nullptr);
    assert(task_table->set(qol::contracts::kQolTaskBagIndex, 0, item(3, 1, 20, 1)));
    const QolInventorySlotRef task = ref(0, qol::contracts::kQolTaskBagIndex, 0);
    assert_ok(api.item_at(api.context, &task, &view));
    assert_error(api.take(api.context, &task, &taken), kQolErrBlocked);
    const QolInventoryItem blocked = item(4, 1, 20, 1);
    assert_error(api.place(api.context, &task, &blocked), kQolErrBlocked);
    assert_error(api.move(api.context, &task, &empty), kQolErrBlocked);
}

}  // namespace

int main() {
    test_descriptor();
    test_store();
    inventory_store().clear();
    return 0;
}

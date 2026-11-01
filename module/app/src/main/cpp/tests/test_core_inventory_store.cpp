#include "contracts/inventory_access.h"
#include "contracts/inventory_transfer.h"
#include "plugins/core.inventory/api/access.h"
#include "plugins/core.inventory/api/bag_source.h"
#include "plugins/core.inventory/plugin.h"
#include "plugins/core.inventory/store/slot_table.h"
#include "plugins/core.inventory/transfer/move_item.h"

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
    assert(descriptor.provides_count == 3);
    assert(std::strcmp(descriptor.provides[0].id, "inventory.access") == 0);
    assert(std::strcmp(descriptor.provides[1].id, "inventory.bag_source") == 0);
    assert(std::strcmp(descriptor.provides[2].id, "inventory.transfer") == 0);
    assert(descriptor.provides[2].iface != nullptr);
    assert(qol::contracts::qol_inventory_transfer_api_valid(
        static_cast<const qol::contracts::QolInventoryTransferApi*>(
            descriptor.provides[2].iface)));
    assert(descriptor.feature_count == 1);
    assert(descriptor.port_count == 3);
    assert(descriptor.ports[0].id == qol::contracts::QolPortId::QOL_PORT_ITEM);
    assert(descriptor.ports[1].id == qol::contracts::QolPortId::QOL_PORT_ITEM_MONEY_WRITE);
    assert(descriptor.ports[2].id ==
           qol::contracts::QolPortId::QOL_PORT_ITEM_INVENTORY_STATE_WRITE);
    assert(descriptor.resources_count == 12);
    for (uint32_t i = 0; i < descriptor.resources_count; ++i) {
        assert(std::strcmp(descriptor.resources[i].owner, "core.inventory") == 0);
    }
    assert(std::strcmp(descriptor.resources[0].id, "hook:INVEN_FindItem") == 0);
    // 库存 HTTP 读侧 owner 迁移：5 条 GET 的 optional http.registry/http.dispatch + endpoint 表。
    assert(descriptor.optional_count == 2 && descriptor.optional_requires != nullptr);
    assert(std::strcmp(descriptor.optional_requires[0].id,
                       qol::contracts::kHttpRegistryCapabilityId) == 0);
    assert(std::strcmp(descriptor.optional_requires[1].id,
                       qol::contracts::kHttpDispatchCapabilityId) == 0);
    assert(descriptor.http_query_count == 0 && descriptor.http_queries == nullptr);
    // 读侧 5 条 GET + 写侧 2 条 POST（move/money）；metadata 1 条（set-slot）。
    assert(descriptor.http_endpoint_count == 7 && descriptor.http_endpoints != nullptr);
    assert(descriptor.http_route_metadata_count == 1 &&
           descriptor.http_route_metadata != nullptr);
    assert(std::strcmp(descriptor.http_route_metadata[0].path,
                       "/api/op/inventory/set-slot") == 0);
    assert(descriptor.http_route_metadata[0].method == qol::contracts::HttpMethod::kPost);
    assert(descriptor.http_route_metadata[0].op_required == 1);
    assert(descriptor.http_route_metadata[0].lease_required == 1);
    for (uint32_t i = 0; i < descriptor.http_endpoint_count; ++i) {
        const auto& endpoint = descriptor.http_endpoints[i];
        assert(endpoint.route_owner != nullptr &&
               std::strcmp(endpoint.route_owner, "core.inventory") == 0);
        assert(endpoint.lease_required == 1);
        if (i < 5) {
            assert(endpoint.method == qol::contracts::HttpMethod::kGet);
            assert(endpoint.kind == qol::contracts::HttpHandlerKind::kQuery);
            assert(endpoint.op_required == 0);
            assert(endpoint.query_handler != nullptr && endpoint.command_handler == nullptr);
        } else {
            assert(endpoint.method == qol::contracts::HttpMethod::kPost);
            assert(endpoint.kind == qol::contracts::HttpHandlerKind::kCommand);
            assert(endpoint.query_handler == nullptr && endpoint.command_handler != nullptr);
        }
        assert(endpoint.ctx != nullptr);
    }
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

// transfer 超量预检夹具：单袋注册源，bag 0 slot 0 固定 count=3 物品（handle {1,1}）。
constexpr int32_t kTransferTestSpace = qol::contracts::kQolBagSpaceFirstRegistered;
int g_transfer_export_calls = 0;
int g_transfer_import_calls = 0;
int g_transfer_remove_calls = 0;

int32_t transfer_source_count() { return 1; }

QolResult transfer_source_info(int32_t bag, qol::contracts::QolBagInfo* out) {
    if (out == nullptr || bag != 0) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad source bag", 14);
    }
    *out = qol::contracts::QolBagInfo{16, 1};
    return qol::contracts::ok();
}

QolResult transfer_source_item_at(int32_t bag, int32_t slot,
                                  qol::contracts::QolInventoryItemRef* out) {
    if (out == nullptr || bag != 0 || slot < 0 || slot >= 16) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad source slot", 15);
    }
    if (slot != 0) {
        return qol::contracts::error(kQolErrNotFound, "empty source slot", 17);
    }
    *out = qol::contracts::QolInventoryItemRef{
        static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider), 0, 0, 10, 3,
        qol::contracts::QolItemHandle{1, 1}, kTransferTestSpace};
    return qol::contracts::ok();
}

QolResult transfer_source_for_each(qol::contracts::QolInventoryBagSlotFn callback, void* ctx) {
    if (callback == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad callback", 12);
    }
    qol::contracts::QolInventoryItemRef item{};
    const QolResult result = transfer_source_item_at(0, 0, &item);
    if (result.code == kQolErrNotFound) return qol::contracts::ok();
    if (!qol::contracts::succeeded(result)) return result;
    (void)callback(&item, ctx);
    return qol::contracts::ok();
}

QolResult transfer_source_export(qol::contracts::QolItemHandle, int32_t count,
                                 qol::contracts::QolBagTransfer* out) {
    ++g_transfer_export_calls;
    if (out == nullptr || count <= 0) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad transfer", 12);
    }
    out->category = 10;
    out->count = count;
    out->payload_size = 1;
    out->payload[0] = 0;
    return qol::contracts::ok();
}

QolResult transfer_source_import(const qol::contracts::QolBagTransfer* transfer, int32_t bag,
                                 int32_t slot, qol::contracts::QolBagMutationResult* out) {
    ++g_transfer_import_calls;
    if (transfer == nullptr || out == nullptr || bag != 0 || slot < 0 || slot >= 16) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad transfer target", 19);
    }
    out->ref = qol::contracts::QolBagRef{kTransferTestSpace, bag};
    out->item = qol::contracts::QolItemHandle{2, 1};
    out->count = transfer->count;
    return qol::contracts::ok();
}

QolResult transfer_source_remove(qol::contracts::QolItemHandle, int32_t,
                                 qol::contracts::QolBagMutationResult*) {
    ++g_transfer_remove_calls;
    return qol::contracts::ok();
}

void test_transfer_move_count_precheck() {
    qol::contracts::QolBagSource source{};
    source.version = qol::contracts::kBagSourceVersion;
    source.space_id = kTransferTestSpace;
    source.count = &transfer_source_count;
    source.info = &transfer_source_info;
    source.item_at = &transfer_source_item_at;
    source.for_each_item = &transfer_source_for_each;
    source.export_item = &transfer_source_export;
    source.import_item = &transfer_source_import;
    source.remove = &transfer_source_remove;
    assert_ok(qol::features::core_inventory::kBagSourceRegistryApi.register_source(&source));

    // 注册源 count=3、move count=4：导出前预检拒绝，源/目标槽不变。
    const qol::contracts::QolBagRef from{kTransferTestSpace, 0};
    const qol::contracts::QolBagRef to{kTransferTestSpace, 0};
    qol::contracts::QolBagMutationResult target{};
    const QolResult result = qol::features::core_inventory::transfer::move_item::move_item(
        from, 0, 4, to, 2, &target);
    assert_error(result, kQolErrInvalidArgument);
    assert(std::strcmp(result.message, "invalid item transfer request") == 0);
    assert(g_transfer_export_calls == 0);
    assert(g_transfer_import_calls == 0);
    assert(g_transfer_remove_calls == 0);

    qol::contracts::QolInventoryItemRef view{};
    assert_ok(transfer_source_item_at(0, 0, &view));
    assert(view.count == 3 && view.item.slot == 1 && view.item.generation == 1);
    assert_error(transfer_source_item_at(0, 2, &view), kQolErrNotFound);

    assert_ok(qol::features::core_inventory::kBagSourceRegistryApi.unregister_source(
        kTransferTestSpace));
}

}  // namespace

int main() {
    test_descriptor();
    test_store();
    test_transfer_move_count_precheck();
    inventory_store().clear();
    qol::features::core_inventory::reset_bag_sources();
    return 0;
}

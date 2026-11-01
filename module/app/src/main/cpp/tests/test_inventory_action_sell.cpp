#include "contracts/game_ports/item_inventory_state_write_port.h"
#include "contracts/bag.h"
#include "contracts/inventory_action.h"
#include "contracts/item_facts.h"
#include "contracts/world_facts.h"
#include "plugins/core.item/runtime/context.h"
#include "plugins/core.item/usecases/item_action.h"
#include "plugins/core.item/usecases/sell_item.h"
#include "infra/log/qol_log.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;
#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

using namespace qol::contracts;

QolInventoryItemRef g_item{};
QolGameHost g_game{};
QolItemPort g_port{};
QolItemInventoryStateWritePort g_write{};
ItemFactsApi g_facts{};
WorldFactsApi g_world{};
QolHostSdk g_sdk{};
QolBagSource g_source{};
bool g_credit_fail = false;
bool g_refund_fail = false;
bool g_remove_fail = false;
bool g_extension_import_fail = false;
int g_credit_calls = 0;
int g_refund_calls = 0;
int g_native_remove_calls = 0;
int g_extension_remove_calls = 0;
int g_extension_import_calls = 0;
int g_extension_use_calls = 0;
int g_native_use_calls = 0;
int g_native_move_calls = 0;
int g_native_equip_calls = 0;
int g_native_unequip_calls = 0;
int g_native_jewel_calls = 0;
int g_last_write_bag = -1;
int g_last_to_bag = -1;
bool g_force_empty = false;
bool g_invalid_import_ref = false;

QolResult fail(int code, const char* message) {
    return error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult fake_in_world(int32_t* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument, "out required");
    *out = 1;
    return ok();
}

QolResult fake_world_state(int32_t* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument, "out required");
    *out = 5;
    return ok();
}

QolResult fake_item_at(const QolBagRef* bag, int32_t slot, QolInventoryItemRef* out) {
    if (bag == nullptr || out == nullptr) return fail(kQolErrInvalidArgument, "bad ref");
    if (g_force_empty || bag->space_id != g_item.space_id || bag->bag != g_item.bag ||
        slot != g_item.slot) {
        return fail(kQolErrNotFound, "slot empty");
    }
    *out = g_item;
    return ok();
}

QolResult fake_extension_item_at(int32_t bag, int32_t slot, QolInventoryItemRef* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument, "out required");
    if (g_force_empty || bag != g_item.bag || slot != g_item.slot) {
        return fail(kQolErrNotFound, "slot empty");
    }
    *out = g_item;
    return ok();
}

QolResult fake_extension_info(int32_t bag, QolBagInfo* out) {
    if (out == nullptr || bag != g_item.bag) return fail(kQolErrInvalidArgument, "bad bag");
    *out = {16, 1};
    return ok();
}

QolResult fake_no_sell(int32_t, int32_t* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument, "out required");
    *out = 0;
    return ok();
}

QolResult fake_quote(QolItemHandle, uint32_t, QolSaleQuote* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument, "out required");
    *out = {sizeof(*out), 20, g_item.count, 20LL * g_item.count * 7 / 10, 7, 10};
    return ok();
}

QolResult fake_add_money(int64_t) {
    ++g_credit_calls;
    return g_credit_fail ? fail(kQolErrBlocked, "credit failed") : ok();
}

QolResult fake_minus_money(int64_t) {
    ++g_refund_calls;
    return g_refund_fail ? fail(kQolErrBlocked, "refund failed") : ok();
}

QolResult fake_game_remove(QolItemHandle) {
    ++g_native_remove_calls;
    return g_remove_fail ? fail(kQolErrNotFound, "remove failed") : ok();
}

QolResult fake_native_use(int32_t bag, int32_t slot,
                          QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument, "out required");
    ++g_native_use_calls;
    g_last_write_bag = bag;
    if (g_item.bag != bag || g_item.slot != slot) return fail(kQolErrNotFound, "stale");
    if (g_item.count > 1) --g_item.count;
    else g_force_empty = true;
    return ok();
}

QolResult fake_native_move(int32_t bag, int32_t slot, int32_t count, int32_t to_bag,
                           int32_t to_slot, QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument, "out required");
    ++g_native_move_calls;
    g_last_write_bag = bag;
    g_last_to_bag = to_bag;
    if (g_item.bag != bag || g_item.slot != slot || count <= 0)
        return fail(kQolErrNotFound, "stale");
    g_item.bag = to_bag;
    g_item.slot = to_slot;
    return ok();
}

QolResult fake_native_equip(int32_t, int32_t bag, int32_t slot,
                            QolItemInventoryStateWriteResult* out) {
    ++g_native_equip_calls;
    g_last_write_bag = bag;
    int32_t local_bag = bag;
    if (bag >= qol::contracts::kQolBagFlatRegisteredBase) {
        int32_t space_id = -1;
        if (!qol::contracts::qol_bag_flat_decode(bag, &space_id, &local_bag)) {
            return fail(kQolErrInvalidArgument, "bad flat bag");
        }
    }
    if (out == nullptr || g_item.bag != local_bag || g_item.slot != slot)
        return fail(kQolErrNotFound, "stale");
    return ok();
}

QolResult fake_native_unequip(int32_t, int32_t,
                              QolItemInventoryStateWriteResult* out) {
    ++g_native_unequip_calls;
    return out != nullptr ? ok() : fail(kQolErrInvalidArgument, "out required");
}

QolResult fake_native_jewel(int32_t, int32_t bag, int32_t slot, int32_t,
                            QolItemInventoryStateWriteResult* out) {
    ++g_native_jewel_calls;
    g_last_write_bag = bag;
    if (out == nullptr || g_item.bag != bag || g_item.slot != slot)
        return fail(kQolErrNotFound, "stale");
    return ok();
}

QolResult fake_native_remove(QolItemHandle expected, int32_t count,
                             QolItemInventoryStateWriteResult* out) {
    ++g_native_remove_calls;
    if (out == nullptr || expected.slot != g_item.item.slot ||
        expected.generation != g_item.item.generation || count <= 0)
        return fail(kQolErrConflict, "stale");
    if (g_remove_fail) return fail(kQolErrConflict, "remove failed");
    if (count >= g_item.count) g_force_empty = true;
    else g_item.count -= count;
    return ok();
}

QolResult fake_extension_export(QolItemHandle, int32_t count, QolBagTransfer* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument, "out required");
    *out = {g_item.category, count, 1, {1}};
    return ok();
}

QolResult fake_extension_import(const QolBagTransfer* transfer, int32_t bag, int32_t slot,
                                QolBagMutationResult* out) {
    ++g_extension_import_calls;
    if (g_extension_import_fail) return fail(kQolErrInternal, "extension rollback failed");
    if (out == nullptr) return fail(kQolErrInvalidArgument, "out required");
    const int32_t actual_slot = slot == -1 ? 4 : slot;
    out->ref = {g_item.space_id, g_invalid_import_ref ? bag + 1 : bag};
    out->item = g_item.item;
    out->count = transfer != nullptr ? transfer->count : g_item.count;
    g_item.bag = bag;
    g_item.slot = actual_slot;
    return ok();
}

QolResult fake_extension_remove(QolItemHandle, int32_t, QolBagMutationResult* out) {
    ++g_extension_remove_calls;
    if (out != nullptr) {
        out->ref = {g_item.space_id, g_item.bag};
        out->item = g_item.item;
        out->count = g_item.count;
    }
    return g_remove_fail ? fail(kQolErrConflict, "extension remove failed") : ok();
}

QolResult fake_extension_use(QolBagRef, int32_t) {
    ++g_extension_use_calls;
    return ok();
}

QolResult fake_capability_require(const char* id, QolVersionRange, const void** out) {
    if (id == nullptr || out == nullptr) return fail(kQolErrInvalidArgument, "bad require");
    if (std::strcmp(id, kWorldFactsCapabilityId) == 0) {
        *out = &g_world;
        return ok();
    }
    *out = nullptr;
    return fail(kQolErrNotFound, "capability missing");
}

void reset_fixture() {
    g_item = {static_cast<int32_t>(QolInventoryItemKind::kOriginal), 0, 0, 42, 3, {12, 3}, 0};
    g_game = {};
    g_port = {};
    g_write = {};
    g_facts = {};
    g_world = {};
    g_sdk = {};
    g_source = {};
    g_credit_fail = false;
    g_refund_fail = false;
    g_remove_fail = false;
    g_extension_import_fail = false;
    g_credit_calls = 0;
    g_refund_calls = 0;
    g_native_remove_calls = 0;
    g_extension_remove_calls = 0;
    g_extension_import_calls = 0;
    g_extension_use_calls = 0;
    g_native_use_calls = 0;
    g_native_move_calls = 0;
    g_native_equip_calls = 0;
    g_native_unequip_calls = 0;
    g_native_jewel_calls = 0;
    g_last_write_bag = -1;
    g_last_to_bag = -1;
    g_force_empty = false;
    g_invalid_import_ref = false;

    g_game.world_action.add_money = &fake_add_money;
    g_game.world_action.minus_money = &fake_minus_money;
    g_game.inventory_write.remove_item = &fake_game_remove;
    g_port.abi_version = {kQolPortItemVersion.major, kQolPortItemVersion.minor};
    g_port.struct_size = sizeof(g_port);
    g_port.read.in_world = &fake_in_world;
    g_port.read.item_at = &fake_item_at;
    g_write.use_item = &fake_native_use;
    g_write.move_item = &fake_native_move;
    g_write.equip_item = &fake_native_equip;
    g_write.unequip_item = &fake_native_unequip;
    g_write.put_jewel = &fake_native_jewel;
    g_write.remove_item = &fake_native_remove;
    g_facts.struct_size = sizeof(g_facts);
    g_facts.is_no_sell = &fake_no_sell;
    g_facts.quote = &fake_quote;
    g_world.state = &fake_world_state;
    g_sdk.abi = {sizeof(g_sdk), kHostSdkMajor, kHostSdkMinor};
    g_sdk.capabilities.require = &fake_capability_require;

    g_source.version = kBagSourceVersion;
    g_source.space_id = kQolBagSpaceFirstRegistered;
    g_source.info = &fake_extension_info;
    g_source.item_at = &fake_extension_item_at;
    g_source.export_item = &fake_extension_export;
    g_source.import_item = &fake_extension_import;
    g_source.remove = &fake_extension_remove;
    g_source.use = &fake_extension_use;

    qol::features::core_item::g_port = &g_port;
    qol::features::core_item::g_item_state_write = &g_write;
    CHECK(qol::features::inventory_hooks::attach(&g_sdk, &g_game, &g_facts, &g_port));
}

void test_api_v3_and_native_sell() {
    reset_fixture();
    const auto& api = qol::features::core_item::kInventoryActionApi;
    CHECK(inventory_action_api_valid(&api));
    CHECK(api.struct_size == sizeof(InventoryActionApi));
    CHECK(api.version_major == 3 && api.version_minor == 3);

    const InventorySellResult sold = api.sell({0, 0, 0}, g_item.item, 2, "test");
    CHECK(sold.status == InventorySellStatus::kOk);
    CHECK(sold.price == 28 && sold.count == 2 && sold.category == 42);
    CHECK(g_credit_calls == 1 && g_native_remove_calls == 1);
    CHECK(g_extension_remove_calls == 0 && g_extension_import_calls == 0);
}

void test_native_sell_failure_and_identity() {
    reset_fixture();
    g_credit_fail = true;
    CHECK(qol::features::core_item::kInventoryActionApi.sell({0, 0, 0}, g_item.item, 1,
                                                              "test").status ==
          InventorySellStatus::kCreditFailed);
    CHECK(g_native_remove_calls == 0);

    reset_fixture();
    g_remove_fail = true;
    CHECK(qol::features::core_item::kInventoryActionApi.sell({0, 0, 0}, g_item.item, 1,
                                                              "test").status ==
          InventorySellStatus::kRemoveFailed);
    CHECK(g_credit_calls == 1 && g_native_remove_calls == 1 && g_refund_calls == 1);

    reset_fixture();
    g_remove_fail = true;
    g_refund_fail = true;
    CHECK(qol::features::core_item::kInventoryActionApi.sell({0, 0, 0}, g_item.item, 1,
                                                              "test").status ==
          InventorySellStatus::kRollbackFailed);

    reset_fixture();
    CHECK(qol::features::core_item::kInventoryActionApi.sell({0, 0, 0}, {99, 99}, 1,
                                                              "test").status ==
          InventorySellStatus::kStale);
    CHECK(g_credit_calls == 0);
    CHECK(qol::features::core_item::kInventoryActionApi.sell({0, 5, 0}, g_item.item, 1,
                                                              "test").status ==
          InventorySellStatus::kTaskBagExcluded);
}

void test_extension_sale_and_native_only_operations() {
    reset_fixture();
    g_item.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item.space_id = 2;
    g_item.bag = 1;
    g_item.slot = 2;
    const auto extension_sold =
        qol::features::core_item::kInventoryActionApi.sell({2, 1, 2}, g_item.item, 2,
                                                            "extension");
    CHECK(extension_sold.status == InventorySellStatus::kOk);
    CHECK(g_extension_remove_calls == 1 && g_credit_calls == 1);

    reset_fixture();
    g_item.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item.space_id = 2;
    g_item.bag = 1;
    g_item.slot = 2;
    g_credit_fail = true;
    const auto extension_credit_failed =
        qol::features::core_item::kInventoryActionApi.sell({2, 1, 2}, g_item.item, 2,
                                                            "extension");
    CHECK(extension_credit_failed.status == InventorySellStatus::kCreditFailed);
    CHECK(g_extension_remove_calls == 1 && g_extension_import_calls == 1);

    reset_fixture();
    g_item.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item.space_id = 2;
    g_item.bag = 1;
    g_item.slot = 2;
    g_credit_fail = true;
    g_extension_import_fail = true;
    CHECK(qol::features::core_item::kInventoryActionApi.sell({2, 1, 2}, g_item.item, 2,
                                                              "extension").status ==
          InventorySellStatus::kRollbackFailed);

    reset_fixture();
    g_item.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item.space_id = 2;
    g_item.bag = 1;
    g_item.slot = 2;
    CHECK(qol::features::core_item::kInventoryActionApi.equip(0, {2, 1, 2}, 0).status ==
          InventoryActionStatus::kOk);
    CHECK(g_native_equip_calls == 1);
    CHECK(g_last_write_bag == qol::contracts::qol_bag_flat_id(2, 1));
    CHECK(qol::features::core_item::kInventoryActionApi.put_jewel(0, {2, 1, 2}, 0).status ==
          InventoryActionStatus::kUnsupported);
}

void test_extension_use_and_removed_same_space_move() {
    reset_fixture();
    g_item.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item.space_id = 2;
    g_item.bag = 1;
    g_item.slot = 2;
    const InventoryUseOutcome used =
        qol::features::core_item::kInventoryActionApi.use({2, 1, 2});
    CHECK(used.status == InventoryActionStatus::kOk && used.consumed_count == 0);
    CHECK(g_extension_use_calls == 1);

    // 注册源同空间移动：export -> import -> remove。
    const InventoryMoveOutcome moved =
        qol::features::core_item::kInventoryActionApi.move({2, 1, 2}, 2, {2, 1, 3});
    CHECK(moved.status == InventoryActionStatus::kOk);
    CHECK(moved.moved_count == 2);
    CHECK(g_item.slot == 3);
    CHECK(g_extension_import_calls == 1 && g_extension_remove_calls == 1);
}

void test_auto_slot_move_uses_registered_source() {
    reset_fixture();
    g_item.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item.space_id = 2;
    g_item.bag = 1;
    g_item.slot = 2;

    const InventoryMoveOutcome moved =
        qol::features::core_item::kInventoryActionApi.move({2, 1, 2}, 2, {2, 1, -1});
    CHECK(moved.status == InventoryActionStatus::kOk);
    CHECK(moved.moved_count == 2);
    CHECK(g_extension_import_calls == 1 && g_extension_remove_calls == 1);
}

void test_auto_slot_move_source_path_mismatch_is_internal() {
    reset_fixture();
    g_item.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item.space_id = 2;
    g_item.bag = 1;
    g_item.slot = 2;
    g_invalid_import_ref = true;

    const InventoryMoveOutcome moved =
        qol::features::core_item::kInventoryActionApi.move({2, 1, 2}, 2, {2, 1, -1});
    CHECK(moved.status == InventoryActionStatus::kInternal);
    CHECK(moved.moved_count == 0);
}

void test_native_move_use_discard_and_equipment() {
    reset_fixture();
    const InventoryMoveOutcome auto_slot_move =
        qol::features::core_item::kInventoryActionApi.move({0, 0, 0}, 2, {0, 0, -1});
    CHECK(auto_slot_move.status == InventoryActionStatus::kInvalidRef);
    CHECK(g_native_move_calls == 0);

    const InventoryMoveOutcome moved =
        qol::features::core_item::kInventoryActionApi.move({0, 0, 0}, 2, {0, 0, 1});
    CHECK(moved.status == InventoryActionStatus::kOk && moved.moved_count == 2);
    CHECK(moved.target.space_id == 0 && moved.target.bag == 0 && moved.target.slot == 1);
    CHECK(moved.target_item.slot == g_item.item.slot && g_native_move_calls == 1);
    CHECK(g_last_write_bag == 0 && g_last_to_bag == 0);

    reset_fixture();
    const InventoryUseOutcome used =
        qol::features::core_item::kInventoryActionApi.use({0, 0, 0});
    CHECK(used.status == InventoryActionStatus::kOk && used.consumed_count == 1);
    CHECK(g_native_use_calls == 1);

    reset_fixture();
    const InventoryUseOutcome discarded =
        qol::features::core_item::kInventoryActionApi.discard({0, 0, 0}, 2);
    CHECK(discarded.status == InventoryActionStatus::kOk && discarded.consumed_count == 2);
    CHECK(g_native_remove_calls == 1);

    reset_fixture();
    CHECK(qol::features::core_item::kInventoryActionApi.equip(0, {0, 0, 0}, 1).status ==
          InventoryActionStatus::kOk);
    CHECK(qol::features::core_item::kInventoryActionApi.unequip(0, 1).status ==
          InventoryActionStatus::kOk);
    CHECK(qol::features::core_item::kInventoryActionApi.put_jewel(0, {0, 0, 0}, 1).status ==
          InventoryActionStatus::kOk);
    CHECK(g_native_equip_calls == 1 && g_native_unequip_calls == 1 && g_native_jewel_calls == 1);
}

}  // namespace

int main() {
    test_api_v3_and_native_sell();
    test_native_sell_failure_and_identity();
    test_extension_sale_and_native_only_operations();
    test_extension_use_and_removed_same_space_move();
    test_auto_slot_move_uses_registered_source();
    test_auto_slot_move_source_path_mismatch_is_internal();
    test_native_move_use_discard_and_equipment();
    std::printf("inventory_action_sell_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

namespace qol::features::core_item {

const contracts::QolItemPort* g_port = nullptr;
const contracts::QolItemInventoryStateWritePort* g_item_state_write = nullptr;
const contracts::QolCharacterExtrasPort* g_character_extras_port = nullptr;
// sell 路径不消费 inventory.transfer；置空即可（capability 缺失按 Unsupported 处理）。
const contracts::QolHostSdk* g_sdk = nullptr;

const contracts::QolBagSource* find_bag_source(int32_t space_id) {
    return space_id == 2 ? &g_source : nullptr;
}

uint32_t registered_bag_source_count() { return g_source.space_id != 0 ? 1u : 0u; }

const contracts::QolBagSource* registered_bag_source_at(uint32_t index) {
    return index == 0 && g_source.space_id != 0 ? &g_source : nullptr;
}

void request_inventory_refresh() {}

}  // namespace qol::features::core_item

namespace qol::features::inventory_hooks {
void inventory_slot_bind(const contracts::QolHostSdk*, const contracts::QolGameHost*,
                         const contracts::ItemFactsApi*, const contracts::QolItemPort*) {}

// 本用例不覆盖 create 的 count>1 路径；生命周期用例单独验证该访问器。
QolResult write_detached_item_count(const contracts::QolItemHandle&, uint32_t) {
    return contracts::error(kQolErrUnsupported, "not used", 8);
}
}  // namespace qol::features::inventory_hooks

bool inventory_native_hook_bind_sdk(const qol::contracts::QolHostSdk*) { return true; }
bool inventory_native_hook_bind_game(const qol::contracts::QolGameHost*) { return true; }
bool inventory_native_hook_bind_item_facts(const qol::contracts::ItemFactsApi*) { return true; }
void inventory_native_hook_set_support(bool) {}
bool inventory_native_hook_support_enabled() { return true; }
bool inventory_native_hook_call_refresh_item_area_original() { return true; }
void inventory_native_hook_bind_player_update_shortcut(void (*)(int32_t)) {}
void inventory_native_hook_update_shortcut_category(int32_t) {}
void inventory_native_hook_update_shortcut() {}
const qol::contracts::ResourceClaim* inventory_hooks_claims(uint32_t* out_count) {
    if (out_count != nullptr) *out_count = 0;
    return nullptr;
}
int32_t inventory_hooks_bind_backup(const char*, void*) { return 0; }
void inventory_log_bind_sdk(const qol::contracts::QolHostSdk*) {}
bool inventory_log_debug_enabled() { return false; }
bool inventory_log_debug_due(int64_t*, int64_t) { return false; }
void inventory_log_write(int32_t, const char*, int, const char*, ...) {}

void qol_log_write(QolLogLevel, QolDomain, const char*, int, const char*, ...) {}

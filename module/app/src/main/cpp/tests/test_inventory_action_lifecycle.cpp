#include "contracts/bag.h"
#include "contracts/game_ports/item_inventory_state_write_port.h"
#include "contracts/inventory_action.h"
#include "contracts/item_facts.h"
#include "infra/log/qol_log.h"
#include "plugins/core.item/runtime/context.h"
#include "plugins/core.item/usecases/item_action.h"

#include <cstddef>
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

constexpr int32_t kExtensionSpace = kQolBagSpaceFirstRegistered;
constexpr uint32_t kSerializedSize = 20;

QolItemHandle g_created{21, 1};
QolItemHandle g_imported{23, 1};
QolItemHandle g_bag_item{31, 1};
QolItemHandle g_last_freed{};
QolItemHandle g_last_count_handle{};
uint32_t g_last_count = 0;
QolResult g_count_write_result{};
QolResult g_create_result{};
QolResult g_place_result{};
QolResult g_import_result{};
QolResult g_free_result{};
QolResult g_save_result{};
int32_t g_saved = 1;
bool g_bag_item_present = false;
int32_t g_bag_item_slot = 2;
int32_t g_bag_item_count = 5;
int32_t g_native_bag_count = 0;
int32_t g_extension_bag_count = 0;
int g_load_calls = 0;
int g_create_calls = 0;
int g_create_perfect_calls = 0;
int g_free_calls = 0;
int g_place_calls = 0;
int g_remove_calls = 0;
int g_import_calls = 0;
int g_save_calls = 0;
QolItemHandle g_last_saved{};
int g_place_slot = -99;
int32_t g_place_bag = -99;
int32_t g_last_create_category = -1;
uint32_t g_last_payload_size = 0;

QolItemPort g_port{};
QolItemInventoryStateWritePort g_write{};
QolBagSource g_source{};

QolResult fail(int code, const char* message) {
    return error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult fake_in_world(int32_t* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument, "out required");
    *out = 1;
    return ok();
}

QolResult fake_category(QolItemHandle, int32_t* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument, "out required");
    *out = 42;
    return ok();
}

QolResult fake_effective_cumulate_count(QolItemHandle item, int32_t* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument, "out required");
    *out = item.slot == g_bag_item.slot ? g_bag_item_count : 3;
    return ok();
}

QolResult fake_cumulate_count(QolItemHandle item, int32_t* out) {
    return fake_effective_cumulate_count(item, out);
}

QolResult fake_bag_count(int32_t space_id, int32_t* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument, "out required");
    *out = space_id == kQolBagSpaceOriginal ? g_native_bag_count : 0;
    return ok();
}

QolResult fake_bag_info(const QolBagRef* ref, QolBagInfo* out) {
    if (ref == nullptr || out == nullptr) return fail(kQolErrInvalidArgument, "bad bag");
    *out = {16, 1};
    return ok();
}

QolResult fake_item_at(const QolBagRef* ref, int32_t slot, QolInventoryItemRef* out) {
    if (ref == nullptr || out == nullptr) return fail(kQolErrInvalidArgument, "bad ref");
    if (!g_bag_item_present || ref->space_id != kQolBagSpaceOriginal || ref->bag != 0 ||
        slot != g_bag_item_slot) {
        return fail(kQolErrNotFound, "slot empty");
    }
    *out = {static_cast<int32_t>(QolInventoryItemKind::kOriginal), ref->bag, slot, 42,
            g_bag_item_count, g_bag_item, 0};
    return ok();
}

QolResult fake_serialize_item(QolItemHandle item, uint8_t* out, uint32_t capacity,
                              uint32_t* out_size) {
    if (item.slot == 0 || out_size == nullptr) {
        return fail(kQolErrInvalidArgument, "bad item");
    }
    if (out == nullptr) {
        *out_size = kSerializedSize;
        return ok();
    }
    if (capacity < kSerializedSize) {
        *out_size = kSerializedSize;
        return fail(kQolErrInvalidArgument, "capacity");
    }
    std::memset(out, 0, kSerializedSize);
    out[0] = static_cast<uint8_t>(kSerializedSize - 1);
    *out_size = kSerializedSize;
    return ok();
}

QolResult fake_load_item(const uint8_t* payload, uint32_t size, QolItemHandle* out_item) {
    ++g_load_calls;
    if (payload == nullptr || out_item == nullptr || size == 0) {
        return fail(kQolErrInvalidArgument, "bad payload");
    }
    g_last_payload_size = size;
    *out_item = g_created;
    return ok();
}

QolResult fake_create_item(int32_t category, int32_t, int32_t, int32_t,
                           QolItemHandle* out_item) {
    ++g_create_calls;
    g_last_create_category = category;
    if (out_item == nullptr) return fail(kQolErrInvalidArgument, "out required");
    *out_item = g_created;
    return g_create_result;
}

QolResult fake_create_perfect_item(int32_t category, QolItemHandle* out_item) {
    ++g_create_perfect_calls;
    g_last_create_category = category;
    if (out_item == nullptr) return fail(kQolErrInvalidArgument, "out required");
    *out_item = g_created;
    return g_create_result;
}

QolResult fake_free_item(QolItemHandle item) {
    ++g_free_calls;
    if (item.slot == 0 || item.generation == 0) {
        return fail(kQolErrInvalidArgument, "bad handle");
    }
    if (!succeeded(g_free_result)) return g_free_result;
    g_last_freed = item;
    return ok();
}

QolResult fake_save_item(QolItemHandle item, int32_t* out_saved) {
    ++g_save_calls;
    if (out_saved == nullptr) return fail(kQolErrInvalidArgument, "out required");
    g_last_saved = item;
    *out_saved = g_saved;
    return g_save_result;
}

QolResult fake_place_item(const QolBagRef* ref, int32_t slot, QolItemHandle item,
                          QolItemInventoryStateWriteResult* out) {
    ++g_place_calls;
    if (out == nullptr) return fail(kQolErrInvalidArgument, "out required");
    if (!succeeded(g_place_result)) return g_place_result;
    g_place_bag = ref != nullptr ? ref->bag : -99;
    g_place_slot = slot >= 0 ? slot : 4;
    out->gained[0] = {ref != nullptr ? ref->bag : 0, g_place_slot, 42, 1};
    out->gained_count = 1;
    out->flags |= kQolItemInventoryStateHasGained;
    g_bag_item_present = false;
    (void)item;
    return ok();
}

QolResult fake_remove_item(QolItemHandle item, int32_t count,
                           QolItemInventoryStateWriteResult* out) {
    ++g_remove_calls;
    if (out == nullptr) return fail(kQolErrInvalidArgument, "out required");
    if (item.slot != g_bag_item.slot || item.generation != g_bag_item.generation || count <= 0) {
        return fail(kQolErrInvalidArgument, "bad removal");
    }
    if (count > g_bag_item_count) return fail(kQolErrInvalidArgument, "count exceeds stack");
    g_bag_item_count -= count;
    out->value = count;
    return ok();
}

int32_t fake_source_count() { return g_extension_bag_count; }

QolResult fake_source_info(int32_t bag, QolBagInfo* out) {
    if (out == nullptr || bag != 0) return fail(kQolErrInvalidArgument, "bad bag");
    *out = {16, 0};
    return ok();
}

QolResult fake_source_item_at(int32_t, int32_t, QolInventoryItemRef* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument, "out required");
    return fail(kQolErrNotFound, "slot empty");
}

QolResult fake_source_import(const QolBagTransfer* transfer, int32_t bag, int32_t slot,
                             QolBagMutationResult* out) {
    ++g_import_calls;
    if (out == nullptr || transfer == nullptr) return fail(kQolErrInvalidArgument, "bad import");
    if (!succeeded(g_import_result)) return g_import_result;
    const int32_t actual_slot = slot < 0 ? 4 : slot;
    out->ref = {kExtensionSpace, bag < 0 ? 0 : bag};
    out->item = g_imported;
    out->count = transfer->count;
    return ok();
}

QolResult fake_source_remove(QolItemHandle, int32_t, QolBagMutationResult* out) {
    if (out != nullptr) out->item = g_imported;
    return fail(kQolErrNotFound, "not used");
}

void reset_fixture() {
    g_created = {21, 1};
    g_imported = {23, 1};
    g_bag_item = {31, 1};
    g_last_freed = kInvalidItemHandle;
    g_last_count_handle = kInvalidItemHandle;
    g_last_count = 0;
    g_count_write_result = ok();
    g_create_result = ok();
    g_place_result = ok();
    g_import_result = ok();
    g_free_result = ok();
    g_save_result = ok();
    g_saved = 1;
    g_bag_item_present = false;
    g_bag_item_slot = 2;
    g_bag_item_count = 5;
    g_native_bag_count = 0;
    g_extension_bag_count = 0;
    g_load_calls = 0;
    g_create_calls = 0;
    g_create_perfect_calls = 0;
    g_free_calls = 0;
    g_place_calls = 0;
    g_remove_calls = 0;
    g_import_calls = 0;
    g_place_slot = -99;
    g_place_bag = -99;
    g_last_create_category = -1;
    g_last_payload_size = 0;

    g_port = {};
    g_port.abi_version = {kQolPortItemVersion.major, kQolPortItemVersion.minor};
    g_port.struct_size = sizeof(g_port);
    g_port.read.in_world = &fake_in_world;
    g_port.read.category = &fake_category;
    g_port.read.cumulate_count = &fake_cumulate_count;
    g_port.read.effective_cumulate_count = &fake_effective_cumulate_count;
    g_port.read.bag_count = &fake_bag_count;
    g_port.read.bag_info = &fake_bag_info;
    g_port.read.item_at = &fake_item_at;
    g_port.read.serialize_item = &fake_serialize_item;

    g_write = {};
    g_write.load_item = &fake_load_item;
    g_write.create_item = &fake_create_item;
    g_write.create_perfect_item = &fake_create_perfect_item;
    g_write.place_item = &fake_place_item;
    g_write.remove_item = &fake_remove_item;
    g_write.free_item = &fake_free_item;
    g_write.save_item = &fake_save_item;

    g_source = {};
    g_source.version = kBagSourceVersion;
    g_source.space_id = kExtensionSpace;
    g_source.count = &fake_source_count;
    g_source.info = &fake_source_info;
    g_source.item_at = &fake_source_item_at;
    g_source.import_item = &fake_source_import;
    g_source.remove = &fake_source_remove;

    qol::features::core_item::g_port = &g_port;
    qol::features::core_item::g_item_state_write = &g_write;
}

const QolItemCreateSpec make_create_spec(int32_t category, int32_t count, uint32_t flags,
                                         const uint8_t* payload, uint32_t payload_size) {
    return {sizeof(QolItemCreateSpec), category, count, flags, payload, payload_size};
}

QolItemPlaceTarget make_place_target(int32_t space_id, int32_t bag, int32_t slot) {
    QolItemPlaceTarget target{};
    target.struct_size = sizeof(QolItemPlaceTarget);
    target.ref = {space_id, bag};
    target.slot = slot;
    return target;
}

void test_create_from_payload() {
    reset_fixture();
    uint8_t payload[kSerializedSize] = {};
    payload[0] = static_cast<uint8_t>(kSerializedSize - 1);
    const QolItemCreateSpec spec =
        make_create_spec(0, 1, kQolItemCreateFlagPerfect, payload, kSerializedSize);
    const QolItemCreateOutcome outcome =
        qol::features::core_item::kInventoryActionApi.create(&spec);
    CHECK(outcome.status == InventoryActionStatus::kOk);
    CHECK(outcome.item.slot == g_created.slot && outcome.item.generation == g_created.generation);
    CHECK(g_load_calls == 1 && g_last_payload_size == kSerializedSize);
    CHECK(g_create_calls == 0 && g_create_perfect_calls == 0 && g_free_calls == 0);
}

void test_create_from_category() {
    reset_fixture();
    const QolItemCreateSpec plain = make_create_spec(77, 0, 0, nullptr, 0);
    QolItemCreateOutcome outcome = qol::features::core_item::kInventoryActionApi.create(&plain);
    CHECK(outcome.status == InventoryActionStatus::kOk);
    CHECK(g_create_calls == 1 && g_create_perfect_calls == 0);
    CHECK(g_last_create_category == 77 && g_load_calls == 0);

    reset_fixture();
    const QolItemCreateSpec perfect =
        make_create_spec(78, 1, kQolItemCreateFlagPerfect, nullptr, 0);
    outcome = qol::features::core_item::kInventoryActionApi.create(&perfect);
    CHECK(outcome.status == InventoryActionStatus::kOk);
    CHECK(g_create_calls == 0 && g_create_perfect_calls == 1);
    CHECK(g_last_create_category == 78);
}

void test_create_count_and_failure_cleanup() {
    reset_fixture();
    const QolItemCreateSpec stacked = make_create_spec(77, 3, 0, nullptr, 0);
    QolItemCreateOutcome outcome = qol::features::core_item::kInventoryActionApi.create(&stacked);
    CHECK(outcome.status == InventoryActionStatus::kOk);
    CHECK(g_last_count == 3u && g_last_count_handle.slot == g_created.slot);
    CHECK(g_free_calls == 0);

    reset_fixture();
    g_count_write_result = fail(kQolErrUnsupported, "item not stackable");
    outcome = qol::features::core_item::kInventoryActionApi.create(&stacked);
    CHECK(outcome.status == InventoryActionStatus::kUnsupported);
    CHECK(outcome.item.slot == 0 && outcome.item.generation == 0);
    CHECK(g_free_calls == 1 && g_last_freed.slot == g_created.slot);

    reset_fixture();
    g_create_result = fail(kQolErrNotReady, "create item failed");
    const QolItemCreateSpec failing = make_create_spec(77, 1, 0, nullptr, 0);
    outcome = qol::features::core_item::kInventoryActionApi.create(&failing);
    CHECK(outcome.status != InventoryActionStatus::kOk);
    CHECK(outcome.item.slot == 0);
}

void test_create_rejects_invalid_spec() {
    reset_fixture();
    CHECK(qol::features::core_item::kInventoryActionApi.create(nullptr).status ==
          InventoryActionStatus::kInvalidRef);
    QolItemCreateSpec spec = make_create_spec(77, 1, 0, nullptr, 0);
    spec.struct_size = 4;
    CHECK(qol::features::core_item::kInventoryActionApi.create(&spec).status ==
          InventoryActionStatus::kInvalidRef);
    spec = make_create_spec(0, 1, 0, nullptr, 0);
    CHECK(qol::features::core_item::kInventoryActionApi.create(&spec).status ==
          InventoryActionStatus::kInvalidRef);
}

void test_place_native_auto_slot() {
    reset_fixture();
    const QolItemPlaceTarget target = make_place_target(kQolBagSpaceOriginal, 0, -1);
    const QolItemPlaceOutcome outcome = qol::features::core_item::kInventoryActionApi.place(
        g_created, &target);
    CHECK(outcome.status == InventoryActionStatus::kOk);
    CHECK(g_place_calls == 1 && g_place_slot == 4 && g_place_bag == 0);
    CHECK(outcome.ref.space_id == kQolBagSpaceOriginal && outcome.ref.bag == 0);
    CHECK(outcome.item.slot == g_created.slot && outcome.count == 1);

    reset_fixture();
    g_place_result = fail(kQolErrBlocked, "native bag full");
    const QolItemPlaceOutcome failed = qol::features::core_item::kInventoryActionApi.place(
        g_created, &target);
    CHECK(failed.status == InventoryActionStatus::kFull);
    CHECK(failed.item.slot == 0);
}

void test_place_auto_save() {
    reset_fixture();
    const QolItemPlaceTarget target = make_place_target(kQolBagSpaceOriginal, -1, -1);
    QolItemPlaceOutcome outcome =
        qol::features::core_item::kInventoryActionApi.place(g_created, &target);
    CHECK(outcome.status == InventoryActionStatus::kOk);
    CHECK(g_save_calls == 1 && g_last_saved.slot == g_created.slot);
    CHECK(g_place_calls == 0);
    CHECK(outcome.ref.space_id == kQolBagSpaceOriginal && outcome.ref.bag == -1);
    CHECK(outcome.item.slot == g_created.slot && outcome.count == 3);

    // 自动保存未落地（saved==0）→ 失败且不释放；句柄仍可 discard_handle。
    reset_fixture();
    g_saved = 0;
    outcome = qol::features::core_item::kInventoryActionApi.place(g_created, &target);
    CHECK(outcome.status != InventoryActionStatus::kOk);
    CHECK(g_free_calls == 0);
    const InventoryUseOutcome discarded =
        qol::features::core_item::kInventoryActionApi.discard_handle(g_created, 1);
    CHECK(discarded.status == InventoryActionStatus::kOk);
    CHECK(g_last_freed.slot == g_created.slot);

    // 保存入口报错同样保持 detached。
    reset_fixture();
    g_save_result = fail(kQolErrBlocked, "inventory full");
    outcome = qol::features::core_item::kInventoryActionApi.place(g_created, &target);
    CHECK(outcome.status != InventoryActionStatus::kOk);
    CHECK(g_free_calls == 0);

    // 沿用迁移前的低 8 位判定：saved==256 视为未落地。
    reset_fixture();
    g_saved = 256;
    outcome = qol::features::core_item::kInventoryActionApi.place(g_created, &target);
    CHECK(outcome.status != InventoryActionStatus::kOk);
}

void test_place_extension_import() {
    reset_fixture();
    const QolItemPlaceTarget target = make_place_target(kExtensionSpace, 0, 3);
    const QolItemPlaceOutcome outcome = qol::features::core_item::kInventoryActionApi.place(
        g_created, &target);
    CHECK(outcome.status == InventoryActionStatus::kOk);
    CHECK(g_import_calls == 1);
    CHECK(outcome.ref.space_id == kExtensionSpace && outcome.ref.bag == 0);
    CHECK(outcome.item.slot == g_imported.slot && outcome.count == 3);
    CHECK(g_free_calls == 1 && g_last_freed.slot == g_created.slot);

    reset_fixture();
    g_import_result = fail(kQolErrBlocked, "extension bag full");
    const QolItemPlaceOutcome failed = qol::features::core_item::kInventoryActionApi.place(
        g_created, &target);
    CHECK(failed.status != InventoryActionStatus::kOk);
    CHECK(g_free_calls == 0);
    // 导入失败后句柄保持 detached，可由 discard_handle 释放。
    const InventoryUseOutcome discarded =
        qol::features::core_item::kInventoryActionApi.discard_handle(g_created, 1);
    CHECK(discarded.status == InventoryActionStatus::kOk);
    CHECK(g_free_calls == 1 && g_last_freed.slot == g_created.slot);
}

void test_discard_handle() {
    reset_fixture();
    InventoryUseOutcome outcome =
        qol::features::core_item::kInventoryActionApi.discard_handle(g_created, 1);
    CHECK(outcome.status == InventoryActionStatus::kOk);
    CHECK(g_free_calls == 1 && g_last_freed.slot == g_created.slot);
    CHECK(g_remove_calls == 0);

    reset_fixture();
    g_bag_item_present = true;
    g_native_bag_count = 1;
    outcome = qol::features::core_item::kInventoryActionApi.discard_handle(g_bag_item, 2);
    CHECK(outcome.status == InventoryActionStatus::kOk);
    CHECK(outcome.consumed_count == 2 && g_remove_calls == 1 && g_bag_item_count == 3);
    CHECK(g_free_calls == 0);

    reset_fixture();
    CHECK(qol::features::core_item::kInventoryActionApi.discard_handle(kInvalidItemHandle, 1)
              .status == InventoryActionStatus::kInvalidRef);
    CHECK(qol::features::core_item::kInventoryActionApi.discard_handle(g_created, 0).status ==
          InventoryActionStatus::kInvalidRef);

    // 既不在背包、也无法作为 detached 解析时按无效引用报告。
    reset_fixture();
    g_free_result = fail(kQolErrNotFound, "stale item handle");
    CHECK(qol::features::core_item::kInventoryActionApi.discard_handle(g_created, 1).status ==
          InventoryActionStatus::kInvalidRef);
}

void test_api_size_compatibility() {
    constexpr uint32_t kLegacySize = static_cast<uint32_t>(
        offsetof(InventoryActionApi, put_jewel) + sizeof(InventoryActionApi::put_jewel));
    const InventoryActionApi& current = qol::features::core_item::kInventoryActionApi;
    CHECK(current.struct_size == sizeof(InventoryActionApi));
    CHECK(current.version_major == kInventoryActionVersionMajor);
    CHECK(current.version_minor == kInventoryActionVersionMinor);
    CHECK(inventory_action_api_valid(&current));

    InventoryActionApi legacy = current;
    legacy.struct_size = kLegacySize;
    legacy.version_minor = 2;
    CHECK(inventory_action_api_valid(&legacy));

    InventoryActionApi truncated = current;
    truncated.create = nullptr;
    truncated.place = nullptr;
    truncated.discard_handle = nullptr;
    truncated.struct_size = kLegacySize;
    CHECK(inventory_action_api_valid(&truncated));

    truncated.struct_size = sizeof(InventoryActionApi);
    CHECK(!inventory_action_api_valid(&truncated));

    InventoryActionApi missing_place = current;
    missing_place.place = nullptr;
    CHECK(!inventory_action_api_valid(&missing_place));
}

}  // namespace

int main() {
    test_create_from_payload();
    test_create_from_category();
    test_create_count_and_failure_cleanup();
    test_create_rejects_invalid_spec();
    test_place_native_auto_slot();
    test_place_auto_save();
    test_place_extension_import();
    test_discard_handle();
    test_api_size_compatibility();
    std::printf("inventory_action_lifecycle_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

namespace qol::features::core_item {

const contracts::QolItemPort* g_port = nullptr;
const contracts::QolItemInventoryStateWritePort* g_item_state_write = nullptr;

const contracts::QolBagSource* find_bag_source(int32_t space_id) {
    return space_id == kExtensionSpace ? &g_source : nullptr;
}

uint32_t registered_bag_source_count() { return 1; }

const contracts::QolBagSource* registered_bag_source_at(uint32_t index) {
    return index == 0 ? &g_source : nullptr;
}

void request_inventory_refresh() {}

namespace usecases {
QolResult move_item(const contracts::QolBagRef&, int32_t, int32_t, const contracts::QolBagRef&,
                    int32_t, contracts::QolBagMutationResult*) {
    return contracts::error(kQolErrUnsupported, "not used", 8);
}
}  // namespace usecases

}  // namespace qol::features::core_item

namespace qol::features::inventory_hooks {

contracts::InventorySellResult inventory_action_sell(contracts::QolActionRef,
                                                     contracts::QolItemHandle, int32_t,
                                                     const char*) {
    contracts::InventorySellResult result{};
    result.status = contracts::InventorySellStatus::kSymbolsUnresolved;
    return result;
}

QolResult write_detached_item_count(const contracts::QolItemHandle& item, uint32_t count) {
    g_last_count_handle = item;
    g_last_count = count;
    return g_count_write_result;
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
void inventory_log_bind_sdk(const qol::contracts::QolHostSdk*) {}
bool inventory_log_debug_enabled() { return false; }
bool inventory_log_debug_due(int64_t*, int64_t) { return false; }
void inventory_log_write(int32_t, const char*, int, const char*, ...) {}

void qol_log_write(QolLogLevel, QolDomain, const char*, int, const char*, ...) {}

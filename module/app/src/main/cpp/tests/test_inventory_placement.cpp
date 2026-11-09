// 统一自动落槽顺序（placement resolver）与 FindSaveSlot 托管路径的 host 测试：
// 自动落槽顺序（v603 关闭 space1）、payload 身份门、堆叠上限预检、scope 限定、任务袋排除、
// wrapper 的扩展堆优先与"构造失败/无目标不回退原生"，以及 move_item 自动目标
// 优先已有堆。原生 space0/space1 与注册源 space2 均为 fake 读面。

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/bag.h"
#include "contracts/game_host.h"
#include "contracts/game_ports/item_port.h"
#include "contracts/game_ports/ports.h"
#include "contracts/host_sdk.h"
#include "contracts/inventory_identity.h"
#include "plugins/core.inventory/api/bag_source.h"
#include "plugins/core.inventory/hooks/placement.h"
#include "plugins/core.inventory/hooks/storage_hooks.cpp"
#include "plugins/core.inventory/runtime/context.h"
#include "plugins/core.inventory/transfer/move_item.h"

namespace {

using qol::contracts::QolBagMutationResult;
using qol::contracts::QolBagRef;
using qol::contracts::QolBagSource;
using qol::contracts::QolBagTransfer;
using qol::contracts::QolInventoryItemRef;
using qol::contracts::QolItemHandle;
namespace placement = qol::features::core_inventory::hooks::placement;

// ---------------------------------------------------------------------------
// fake item 网格：原生 space0（6 袋）/ space1 C18（2 袋）+ 注册源 space2（2 袋）。
// ---------------------------------------------------------------------------

constexpr int kSlotsPerBag = 16;
constexpr int32_t kRegSpace = qol::contracts::kQolBagSpaceFirstRegistered;
constexpr int kRegBags = 2;

struct FakeItem {
    bool occupied = false;
    int32_t category = 0;
    int32_t count = 0;
    uint32_t count_limit = 999;
    uint8_t payload[32]{};
    uint32_t payload_size = 0;
};

FakeItem g_native[2][8][kSlotsPerBag];
FakeItem g_reg[kRegBags][kSlotsPerBag];

void reset_grid() {
    std::memset(g_native, 0, sizeof(g_native));
    std::memset(g_reg, 0, sizeof(g_reg));
}

QolResult ok_result() { return qol::contracts::ok(); }

QolResult not_found() {
    return qol::contracts::error(kQolErrNotFound, "empty slot", 10);
}

// 原生句柄：slot = 1 + (space+1)*1000 + bag*20 + slot，generation 7。
// 注册源句柄：slot = 3001 + bag*20 + slot，generation 9。
QolItemHandle native_handle(int space, int bag, int slot) {
    return QolItemHandle{static_cast<uint32_t>(1 + (space + 1) * 1000 + bag * 20 + slot), 7};
}

QolItemHandle reg_handle(int bag, int slot) {
    return QolItemHandle{static_cast<uint32_t>(3001 + bag * 20 + slot), 9};
}

struct ItemCell {
    FakeItem* item = nullptr;
};

bool decode_handle(QolItemHandle handle, ItemCell* out) {
    if (out == nullptr) return false;
    out->item = nullptr;
    if (handle.generation == 7 && handle.slot >= 1001 && handle.slot < 5000) {
        const uint32_t raw = handle.slot - 1;
        const int32_t space = static_cast<int32_t>(raw / 1000) - 1;
        const uint32_t rest = raw % 1000;
        out->item = &g_native[space][rest / 20][rest % 20];
        return out->item->occupied;
    }
    if (handle.generation == 9 && handle.slot >= 3001 &&
        handle.slot < static_cast<uint32_t>(3001 + kRegBags * 20)) {
        const uint32_t raw = handle.slot - 3001;
        out->item = &g_reg[raw / 20][raw % 20];
        return out->item->occupied;
    }
    return false;
}

void place_native(int space, int bag, int slot, int32_t category, int32_t count,
                  const uint8_t* payload, uint32_t payload_size, uint32_t count_limit) {
    FakeItem& cell = g_native[space][bag][slot];
    cell = FakeItem{};
    cell.occupied = true;
    cell.category = category;
    cell.count = count;
    cell.count_limit = count_limit;
    cell.payload_size = payload_size;
    std::memcpy(cell.payload, payload, payload_size);
}

void place_reg(int bag, int slot, int32_t category, int32_t count, const uint8_t* payload,
               uint32_t payload_size, uint32_t count_limit) {
    FakeItem& cell = g_reg[bag][slot];
    cell = FakeItem{};
    cell.occupied = true;
    cell.category = category;
    cell.count = count;
    cell.count_limit = count_limit;
    cell.payload_size = payload_size;
    std::memcpy(cell.payload, payload, payload_size);
}

// payload 布局：[0]=len-1、[1,9)=UID、[9,11)=I_TYPE、[11,15)=数量位、[15..)=身份尾。
uint32_t make_payload(uint8_t* buf, uint32_t size, uint8_t type_hi, uint8_t tail) {
    std::memset(buf, 0, size);
    buf[0] = static_cast<uint8_t>(size - 1);
    for (int i = 1; i < 9; ++i) buf[i] = static_cast<uint8_t>(0x30 + i);
    buf[9] = 0x10;
    buf[10] = type_hi;
    buf[11] = 0x11;
    buf[12] = 0x22;
    buf[13] = 0x33;
    buf[14] = 0x44;
    if (size > 15) buf[15] = tail;
    return size;
}

constexpr uint32_t kPayloadSize = 19;

// transfer 侧身份：type_hi=1, tail=7。
void make_transfer(QolBagTransfer* transfer, int32_t category, int32_t count, uint8_t type_hi,
                   uint8_t tail) {
    std::memset(transfer, 0, sizeof(*transfer));
    transfer->category = category;
    transfer->count = count;
    transfer->payload_size = make_payload(transfer->payload, kPayloadSize, type_hi, tail);
}

// 占位填充身份（tail=9，与 transfer 身份不同）：只占槽，不参与合并。
void make_filler_payload(uint8_t* buf) {
    std::memset(buf, 0, kPayloadSize);
    (void)make_payload(buf, kPayloadSize, 1, 9);
}

void fill_native_bag(int space, int bag) {
    uint8_t payload[kPayloadSize];
    make_filler_payload(payload);
    for (int slot = 0; slot < kSlotsPerBag; ++slot) {
        place_native(space, bag, slot, 10, 1, payload, kPayloadSize, 999);
    }
}

// ---------------------------------------------------------------------------
// fake item port
// ---------------------------------------------------------------------------

QolResult port_native_bag_layout(qol::contracts::QolNativeBagLayout* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    *out = {8, 1};
    return ok_result();
}

QolResult port_bag_capacity(const QolBagRef* ref, int32_t* out) {
    if (ref == nullptr || out == nullptr || ref->space_id < 0 || ref->space_id > 1 ||
        ref->bag < 0 || ref->bag >= (ref->space_id == 0 ? 6 : 2)) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad bag ref", 11);
    }
    *out = kSlotsPerBag;
    return ok_result();
}

QolResult port_slot_occupied(const QolBagRef* ref, int32_t slot, int32_t* out) {
    if (ref == nullptr || out == nullptr || ref->space_id < 0 || ref->space_id > 1 ||
        ref->bag < 0 || ref->bag >= (ref->space_id == 0 ? 6 : 2) || slot < 0 ||
        slot >= kSlotsPerBag) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad slot", 9);
    }
    *out = g_native[ref->space_id][ref->bag][slot].occupied ? 1 : 0;
    return ok_result();
}

QolResult port_item_ref_at(int32_t flat_bag, int32_t slot, QolInventoryItemRef* out) {
    if (out == nullptr || flat_bag < 0 || flat_bag >= 8 || slot < 0 || slot >= kSlotsPerBag) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad slot", 9);
    }
    const int32_t space = flat_bag < 6 ? 0 : 1;
    const int32_t bag = flat_bag < 6 ? flat_bag : flat_bag - 6;
    const FakeItem& cell = g_native[space][bag][slot];
    if (!cell.occupied) return not_found();
    *out = QolInventoryItemRef{0, bag, slot, cell.category, cell.count,
                               native_handle(space, bag, slot), space};
    return ok_result();
}

QolResult port_serialize(qol::contracts::QolItemHandle handle, uint8_t* out, uint32_t capacity,
                         uint32_t* out_size) {
    ItemCell cell{};
    if (out_size == nullptr || !decode_handle(handle, &cell)) {
        return qol::contracts::error(kQolErrNotFound, "stale item handle", 18);
    }
    *out_size = cell.item->payload_size;
    if (out == nullptr) return ok_result();
    if (capacity < cell.item->payload_size) {
        return qol::contracts::error(kQolErrInvalidArgument, "insufficient capacity", 21);
    }
    std::memcpy(out, cell.item->payload, cell.item->payload_size);
    return ok_result();
}

QolResult port_category(qol::contracts::QolItemHandle handle, int32_t* out) {
    ItemCell cell{};
    if (out == nullptr || !decode_handle(handle, &cell)) {
        return qol::contracts::error(kQolErrNotFound, "stale item handle", 18);
    }
    *out = cell.item->category;
    return ok_result();
}

QolResult port_cumulate_count(qol::contracts::QolItemHandle handle, int32_t* out) {
    ItemCell cell{};
    if (out == nullptr || !decode_handle(handle, &cell)) {
        return qol::contracts::error(kQolErrNotFound, "stale item handle", 18);
    }
    *out = cell.item->count;
    return ok_result();
}

QolResult port_count_encoding(qol::contracts::QolItemHandle,
                              qol::contracts::QolItemCountEncoding* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    *out = qol::contracts::QolItemCountEncoding::kEncoded;
    return ok_result();
}

QolResult port_count_limit(qol::contracts::QolItemHandle handle, uint32_t* out) {
    ItemCell cell{};
    if (out == nullptr || !decode_handle(handle, &cell)) {
        return qol::contracts::error(kQolErrNotFound, "stale item handle", 18);
    }
    *out = cell.item->count_limit;
    return ok_result();
}

qol::contracts::QolItemPort make_port() {
    qol::contracts::QolItemPort port{};
    port.abi_version = qol::contracts::kQolPortItemVersion;
    port.struct_size = sizeof(qol::contracts::QolItemPort);
    port.read.native_bag_layout = &port_native_bag_layout;
    port.read.bag_capacity = &port_bag_capacity;
    port.read.slot_occupied = &port_slot_occupied;
    port.read.item_ref_at = &port_item_ref_at;
    port.read.serialize_item = &port_serialize;
    port.read.category = &port_category;
    port.read.cumulate_count = &port_cumulate_count;
    port.read.count_encoding = &port_count_encoding;
    port.read.count_limit = &port_count_limit;
    return port;
}

// ---------------------------------------------------------------------------
// fake 注册源（space2，2 袋）
// ---------------------------------------------------------------------------

int32_t reg_count() { return kRegBags; }

QolResult reg_info(int32_t bag, qol::contracts::QolBagInfo* out) {
    if (out == nullptr || bag < 0 || bag >= kRegBags) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad source bag", 14);
    }
    *out = qol::contracts::QolBagInfo{kSlotsPerBag, 0};
    return ok_result();
}

QolResult reg_item_at(int32_t bag, int32_t slot, QolInventoryItemRef* out) {
    if (out == nullptr || bag < 0 || bag >= kRegBags || slot < 0 || slot >= kSlotsPerBag) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad source slot", 15);
    }
    const FakeItem& cell = g_reg[bag][slot];
    if (!cell.occupied) return not_found();
    *out = QolInventoryItemRef{
        static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider), bag, slot,
        cell.category, cell.count, reg_handle(bag, slot), kRegSpace};
    return ok_result();
}

QolResult reg_export(qol::contracts::QolItemHandle item, int32_t count, QolBagTransfer* out) {
    ItemCell cell{};
    if (out == nullptr || count <= 0 || !decode_handle(item, &cell)) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad export item", 14);
    }
    out->category = cell.item->category;
    out->count = count;
    out->payload_size = cell.item->payload_size;
    std::memcpy(out->payload, cell.item->payload, cell.item->payload_size);
    return ok_result();
}

int g_reg_import_calls = 0;
int32_t g_reg_import_bag = -1;
int32_t g_reg_import_slot = -1;

QolResult reg_import(const QolBagTransfer* transfer, int32_t bag, int32_t slot,
                     QolBagMutationResult* out) {
    ++g_reg_import_calls;
    g_reg_import_bag = bag;
    g_reg_import_slot = slot;
    if (transfer == nullptr || out == nullptr || bag < 0 || bag >= kRegBags || slot < 0 ||
        slot >= kSlotsPerBag) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad transfer target", 19);
    }
    out->ref = QolBagRef{kRegSpace, bag};
    out->item = QolItemHandle{2, 1};
    out->count = transfer->count;
    return ok_result();
}

QolResult reg_remove(qol::contracts::QolItemHandle, int32_t, QolBagMutationResult*) {
    return ok_result();
}

QolResult reg_for_each(qol::contracts::QolInventoryBagSlotFn callback, void* ctx) {
    if (callback == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad callback", 12);
    }
    for (int bag = 0; bag < kRegBags; ++bag) {
        for (int slot = 0; slot < kSlotsPerBag; ++slot) {
            QolInventoryItemRef item{};
            const QolResult result = reg_item_at(bag, slot, &item);
            if (result.code == kQolErrNotFound) continue;
            if (!qol::contracts::succeeded(result)) return result;
            if (callback(&item, ctx) != 0) return ok_result();
        }
    }
    return ok_result();
}

QolResult reg_identify(qol::contracts::QolItemHandle, QolBagRef*, int32_t*) {
    return not_found();
}

QolResult reg_replace(qol::contracts::QolItemHandle,
                      const qol::contracts::QolBagTransfer*,
                      qol::contracts::QolBagMutationResult*) {
    return not_found();
}

void register_reg_source() {
    static QolBagSource source = [&] {
        QolBagSource s{};
        s.version = qol::contracts::kBagSourceVersion;
        s.space_id = kRegSpace;
        s.count = &reg_count;
        s.info = &reg_info;
        s.item_at = &reg_item_at;
        s.for_each_item = &reg_for_each;
        s.export_item = &reg_export;
        s.import_item = &reg_import;
        s.remove = &reg_remove;
        s.identify = &reg_identify;
        s.replace_item = &reg_replace;
        return s;
    }();
    assert(qol::contracts::succeeded(
        qol::features::core_inventory::kBagSourceRegistryApi.register_source(&source)));
}

// ---------------------------------------------------------------------------
// fake Host SDK / Game Host（FindSaveSlot wrapper 路径）
// ---------------------------------------------------------------------------

int32_t fake_debug_disabled() { return 0; }
int32_t fake_intercept_blocks(const char*, void*) { return 0; }

void* g_hook_item = nullptr;
QolItemHandle g_hook_handle{};

int32_t registry_lookup(void* item, QolItemHandle* out_handle, uint32_t* out_provider) {
    if (item != g_hook_item || out_handle == nullptr) {
        return qol::contracts::kQolItemHandleStatusNotFound;
    }
    if (out_provider != nullptr) *out_provider = 0;
    *out_handle = g_hook_handle;
    return qol::contracts::kQolItemHandleStatusOk;
}

int32_t registry_register(void* item, uint32_t, QolItemHandle* out_handle) {
    if (item != g_hook_item || out_handle == nullptr) {
        return qol::contracts::kQolItemHandleStatusInvalidArgument;
    }
    *out_handle = g_hook_handle;
    return qol::contracts::kQolItemHandleStatusOk;
}

int32_t registry_invalidate(void*, uint32_t) { return qol::contracts::kQolItemHandleStatusOk; }
int32_t registry_rebind(void*, uint32_t) { return qol::contracts::kQolItemHandleStatusOk; }
int32_t registry_begin_invalidation(void*, uint32_t, QolItemHandle*) {
    return qol::contracts::kQolItemHandleStatusOk;
}
int32_t registry_finish_invalidation(QolItemHandle, int32_t) {
    return qol::contracts::kQolItemHandleStatusOk;
}

qol::contracts::QolHostSdk make_sdk() {
    qol::contracts::QolHostSdk sdk{};
    sdk.abi.size = static_cast<uint32_t>(offsetof(qol::contracts::QolHostSdk, item_handle) +
                                         sizeof(qol::contracts::QolItemHandleApi));
    sdk.abi.major = 1;
    sdk.abi.minor = 29;
    sdk.log.debug_enabled = &fake_debug_disabled;
    sdk.hooks.intercept_blocks = &fake_intercept_blocks;
    sdk.item_handle = {&registry_register, &registry_lookup, &registry_invalidate,
                       &registry_rebind, &registry_begin_invalidation,
                       &registry_finish_invalidation};
    return sdk;
}

QolResult fake_native_save_domain(int32_t, int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    *out = 0;
    return ok_result();
}

QolResult bind_find_item_backup(qol::contracts::QolFindItemAbiFn) { return ok_result(); }

const char* g_fake_series_name = "c18";
const char* g_fake_version_label = "v589";

QolResult fake_game_identity(qol::contracts::QolGameIdentity* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    *out = {};
    out->series_name = g_fake_series_name;
    out->version_label = g_fake_version_label;
    out->known = 1;
    return ok_result();
}

qol::contracts::QolGameHost make_game() {
    qol::contracts::QolGameHost game{};
    game.abi.major = qol::contracts::kGameHostMajor;
    game.abi.minor = qol::contracts::kGameHostSavedBagMinor;
    game.read_only.item.native_save_domain = &fake_native_save_domain;
    game.item_abi.bind_find_item_backup = &bind_find_item_backup;
    game.game_identity.read = &fake_game_identity;
    return game;
}

const qol::contracts::QolGameHost& placement_test_game() {
    static const qol::contracts::QolGameHost game = make_game();
    return game;
}

int g_native_find_save_slot_calls = 0;
int fake_native_find_save_slot(void*, int8_t*) {
    ++g_native_find_save_slot_calls;
    return 0;
}

void attach_hooks(const qol::contracts::QolHostSdk& sdk, const qol::contracts::QolItemPort& port) {
    assert(qol::features::core_inventory::hooks::attach(&sdk, &placement_test_game(), &port));
    assert(qol::features::core_inventory::hooks::bind_backup(
               "hook:INVEN_FindSaveSlot",
               reinterpret_cast<void*>(&fake_native_find_save_slot)) == 0);
}

void test_attach_maps_c18_v603_identity(const qol::contracts::QolHostSdk& sdk,
                                        const qol::contracts::QolItemPort& port) {
    const auto auto_save_for = [&](const char* series, const char* version) {
        g_fake_series_name = series;
        g_fake_version_label = version;
        assert(qol::features::core_inventory::hooks::attach(&sdk, &placement_test_game(),
                                                            &port));
        return qol::features::core_inventory::g_c18_native_bag_auto_save;
    };
    // 只有 c18 + v603 关闭；其他 C18 版本、未知版本与其它变体都必须保持旧行为。
    assert(!auto_save_for("c18", "v603"));
    assert(auto_save_for("c18", "v589"));
    assert(auto_save_for("c18", "v520"));
    assert(auto_save_for("c18", ""));
    assert(auto_save_for("c18", "v999"));
    assert(auto_save_for("monster", "v29"));
    assert(auto_save_for("original", "v1.3.2"));
    assert(auto_save_for("overhaul", "20260810"));
    g_fake_series_name = "c18";
    g_fake_version_label = "v589";
}

// ---------------------------------------------------------------------------
// 断言辅助
// ---------------------------------------------------------------------------

void expect_target(const placement::PlacementTarget& target, int32_t space, int32_t bag,
                   int32_t slot, bool merge) {
    if (target.space_id != space || target.bag != bag || target.slot != slot ||
        target.merge != merge) {
        std::fprintf(stderr,
                     "expect_target failed: got space=%d bag=%d slot=%d merge=%d, "
                     "want space=%d bag=%d slot=%d merge=%d\n",
                    target.space_id, target.bag, target.slot, target.merge ? 1 : 0, space, bag,
                    slot, merge ? 1 : 0);
    }
    assert(target.space_id == space);
    assert(target.bag == bag);
    assert(target.slot == slot);
    assert(target.merge == merge);
}

// ---------------------------------------------------------------------------
// 测试
// ---------------------------------------------------------------------------

void test_identity_helper() {
    uint8_t a[kPayloadSize];
    uint8_t b[kPayloadSize];
    make_payload(a, kPayloadSize, 1, 7);
    make_payload(b, kPayloadSize, 1, 7);
    // UID 与数量位不参与：改 UID 与 count 位后仍同身份。
    for (int i = 1; i < 9; ++i) b[i] = static_cast<uint8_t>(0x70 + i);
    b[11] = 0xEE;
    assert(qol::contracts::qol_inventory_payload_identity_equal(a, kPayloadSize, b,
                                                                kPayloadSize));
    // I_TYPE 不同 → 身份不同。
    uint8_t c[kPayloadSize];
    make_payload(c, kPayloadSize, 2, 7);
    assert(!qol::contracts::qol_inventory_payload_identity_equal(a, kPayloadSize, c,
                                                                 kPayloadSize));
    // 身份尾不同 → 身份不同。
    uint8_t d[kPayloadSize];
    make_payload(d, kPayloadSize, 1, 8);
    assert(!qol::contracts::qol_inventory_payload_identity_equal(a, kPayloadSize, d,
                                                                 kPayloadSize));
    // 尾部长度不同 → false。
    uint8_t e[20];
    make_payload(e, 20, 1, 7);
    assert(!qol::contracts::qol_inventory_payload_identity_equal(a, kPayloadSize, e, 20));
    // 长度 < 19 → false。
    assert(!qol::contracts::qol_inventory_payload_identity_equal(a, 18, b, 18));
    // 同长度同身份（更长尾部）→ true。
    uint8_t f[20];
    uint8_t g[20];
    make_payload(f, 20, 1, 7);
    make_payload(g, 20, 1, 7);
    assert(qol::contracts::qol_inventory_payload_identity_equal(f, 20, g, 20));
}

void test_global_order_extension_merge_beats_native_empty() {
    reset_grid();
    uint8_t payload[kPayloadSize];
    make_payload(payload, kPayloadSize, 1, 7);
    place_reg(0, 0, 10, 50, payload, kPayloadSize, 999);
    // 原生全空（空槽充足）+ 扩展已有同身份堆 → 选扩展堆合并（步骤 2 先于步骤 4）。
    QolBagTransfer transfer{};
    make_transfer(&transfer, 10, 3, 1, 7);
    placement::PlacementTarget target{};
    assert(placement::resolve_placement(transfer, true, -1, -1, &target));
    expect_target(target, kRegSpace, 0, 0, true);
}

void test_global_order_native_merge_first() {
    reset_grid();
    uint8_t payload[kPayloadSize];
    make_payload(payload, kPayloadSize, 1, 7);
    place_native(0, 2, 5, 10, 10, payload, kPayloadSize, 999);
    place_reg(0, 0, 10, 50, payload, kPayloadSize, 999);
    QolBagTransfer transfer{};
    make_transfer(&transfer, 10, 3, 1, 7);
    placement::PlacementTarget target{};
    assert(placement::resolve_placement(transfer, true, -1, -1, &target));
    expect_target(target, qol::contracts::kQolBagSpaceOriginal, 2, 5, true);
}

void test_global_order_c18_merge_beats_native_empty() {
    reset_grid();
    uint8_t payload[kPayloadSize];
    make_payload(payload, kPayloadSize, 1, 7);
    place_native(1, 0, 3, 10, 7, payload, kPayloadSize, 999);
    QolBagTransfer transfer{};
    make_transfer(&transfer, 10, 3, 1, 7);
    placement::PlacementTarget target{};
    // 变体仍提供 C18 背包时：C18 合并（步骤 3）先于原生空槽（步骤 4）。
    assert(placement::resolve_placement(transfer, true, -1, -1, &target));
    expect_target(target, qol::contracts::kQolBagSpaceVariant, 0, 3, true);
}

void test_c18_bag_removed_skips_auto_placement() {
    reset_grid();
    uint8_t payload[kPayloadSize];
    make_payload(payload, kPayloadSize, 1, 7);
    place_native(1, 0, 3, 10, 7, payload, kPayloadSize, 999);
    QolBagTransfer transfer{};
    make_transfer(&transfer, 10, 3, 1, 7);
    placement::PlacementTarget target{};
    // C18L v603：C18 背包已移除 → C18 合并不参与，落原生空槽。
    qol::features::core_inventory::g_c18_native_bag_auto_save = false;
    assert(placement::resolve_placement(transfer, true, -1, -1, &target));
    expect_target(target, qol::contracts::kQolBagSpaceOriginal, 0, 0, false);
    // 原生 0..4 + 注册源全满、C18 仍空 → 无自动目标（保持背包已满语义）。
    for (int bag = 0; bag < 5; ++bag) fill_native_bag(0, bag);
    uint8_t filler[kPayloadSize];
    make_filler_payload(filler);
    for (int bag = 0; bag < kRegBags; ++bag) {
        for (int slot = 0; slot < kSlotsPerBag; ++slot) {
            place_reg(bag, slot, 10, 1, filler, kPayloadSize, 999);
        }
    }
    assert(!placement::resolve_placement(transfer, true, -1, -1, &target));
    qol::features::core_inventory::g_c18_native_bag_auto_save = true;
}

void test_explicit_c18_scope_still_supported_when_removed() {
    reset_grid();
    // 显式目标 space=1 不受“C18 背包已移除”影响：用户主动移动仍按原语义落槽，
    // 否则已在追加袋内的数据会变成不可移动的孤儿。
    uint8_t payload[kPayloadSize];
    make_payload(payload, kPayloadSize, 1, 7);
    place_native(1, 0, 3, 10, 7, payload, kPayloadSize, 999);
    QolBagTransfer transfer{};
    make_transfer(&transfer, 10, 3, 1, 7);
    placement::PlacementTarget target{};
    qol::features::core_inventory::g_c18_native_bag_auto_save = false;
    assert(placement::resolve_placement(transfer, true,
                                        qol::contracts::kQolBagSpaceVariant, -1, &target));
    expect_target(target, qol::contracts::kQolBagSpaceVariant, 0, 3, true);
    reset_grid();
    assert(placement::resolve_placement(transfer, true,
                                        qol::contracts::kQolBagSpaceVariant, -1, &target));
    expect_target(target, qol::contracts::kQolBagSpaceVariant, 0, 0, false);
    qol::features::core_inventory::g_c18_native_bag_auto_save = true;
}

void test_global_order_native_empty_before_registered_empty() {
    reset_grid();
    QolBagTransfer transfer{};
    make_transfer(&transfer, 10, 3, 1, 7);
    placement::PlacementTarget target{};
    assert(placement::resolve_placement(transfer, true, -1, -1, &target));
    expect_target(target, qol::contracts::kQolBagSpaceOriginal, 0, 0, false);
    // 原生 0..4 袋全满（任务袋留空，填充身份与 transfer 不同）→
    // 注册源空槽（步骤 5）先于 C18 空槽（步骤 6）。
    for (int bag = 0; bag < 5; ++bag) fill_native_bag(0, bag);
    assert(placement::resolve_placement(transfer, true, -1, -1, &target));
    expect_target(target, kRegSpace, 0, 0, false);
    // 注册源也满 → C18 空槽兜底（变体仍提供 C18 背包的旧行为）。
    uint8_t payload[kPayloadSize];
    make_filler_payload(payload);
    for (int bag = 0; bag < kRegBags; ++bag) {
        for (int slot = 0; slot < kSlotsPerBag; ++slot) {
            place_reg(bag, slot, 10, 1, payload, kPayloadSize, 999);
        }
    }
    assert(placement::resolve_placement(transfer, true, -1, -1, &target));
    expect_target(target, qol::contracts::kQolBagSpaceVariant, 0, 0, false);
}

void test_same_category_different_payload_never_merges() {
    reset_grid();
    uint8_t payload[kPayloadSize];
    make_payload(payload, kPayloadSize, 1, 9);  // 身份尾不同
    place_reg(0, 0, 10, 50, payload, kPayloadSize, 999);
    QolBagTransfer transfer{};
    make_transfer(&transfer, 10, 3, 1, 7);
    placement::PlacementTarget target{};
    assert(placement::resolve_placement(transfer, true, -1, -1, &target));
    expect_target(target, qol::contracts::kQolBagSpaceOriginal, 0, 0, false);
}

void test_stackable_false_skips_all_merge_steps() {
    reset_grid();
    uint8_t payload[kPayloadSize];
    make_payload(payload, kPayloadSize, 1, 7);
    place_reg(0, 0, 10, 50, payload, kPayloadSize, 999);
    place_native(0, 0, 0, 10, 5, payload, kPayloadSize, 999);
    QolBagTransfer transfer{};
    make_transfer(&transfer, 10, 3, 1, 7);
    placement::PlacementTarget target{};
    assert(placement::resolve_placement(transfer, false, -1, -1, &target));
    expect_target(target, qol::contracts::kQolBagSpaceOriginal, 0, 1, false);
}

void test_stack_limit_uses_count_limit() {
    reset_grid();
    uint8_t payload[kPayloadSize];
    make_payload(payload, kPayloadSize, 1, 7);
    // count_limit=100：99+5=104 超预检上限 → 跳过合并，落原生空槽。
    place_reg(0, 0, 10, 99, payload, kPayloadSize, 100);
    QolBagTransfer transfer{};
    make_transfer(&transfer, 10, 5, 1, 7);
    placement::PlacementTarget target{};
    assert(placement::resolve_placement(transfer, true, -1, -1, &target));
    expect_target(target, qol::contracts::kQolBagSpaceOriginal, 0, 0, false);
    // 99+1=100 不超上限 → 合并。
    make_transfer(&transfer, 10, 1, 1, 7);
    assert(placement::resolve_placement(transfer, true, -1, -1, &target));
    expect_target(target, kRegSpace, 0, 0, true);
}

void test_task_bag_never_returned() {
    reset_grid();
    // 原生 0..4 袋全满（占位身份不同于 transfer）、任务袋 5 空、注册源与 C18 空
    // → 不得返回 bag5。
    for (int bag = 0; bag < 5; ++bag) fill_native_bag(0, bag);
    QolBagTransfer transfer{};
    make_transfer(&transfer, 10, 3, 1, 7);
    placement::PlacementTarget target{};
    assert(placement::resolve_placement(transfer, true, -1, -1, &target));
    expect_target(target, kRegSpace, 0, 0, false);
}

void test_scope_resolution() {
    reset_grid();
    uint8_t payload[kPayloadSize];
    make_payload(payload, kPayloadSize, 1, 7);
    place_reg(1, 0, 10, 50, payload, kPayloadSize, 999);
    QolBagTransfer transfer{};
    make_transfer(&transfer, 10, 3, 1, 7);
    placement::PlacementTarget target{};
    // scope 全注册源：bag1 合并堆命中。
    assert(placement::resolve_placement(transfer, true, kRegSpace, -1, &target));
    expect_target(target, kRegSpace, 1, 0, true);
    // scope 限定 bag0：无合并 → bag0 空槽。
    assert(placement::resolve_placement(transfer, true, kRegSpace, 0, &target));
    expect_target(target, kRegSpace, 0, 0, false);
    // scope 限定原生 space0：忽略注册源 → 原生空槽。
    assert(placement::resolve_placement(transfer, true, qol::contracts::kQolBagSpaceOriginal,
                                        -1, &target));
    expect_target(target, qol::contracts::kQolBagSpaceOriginal, 0, 0, false);
    // scope_bag 无 scope_space → 非法。
    assert(!placement::resolve_placement(transfer, true, -1, 0, &target));
    // category 非法 → false。
    make_transfer(&transfer, 0, 3, 1, 7);
    assert(!placement::resolve_placement(transfer, true, -1, -1, &target));
}

void test_wrapper_prefers_extension_merge() {
    reset_grid();
    uint8_t payload[kPayloadSize];
    make_payload(payload, kPayloadSize, 1, 7);
    place_reg(0, 0, 10, 50, payload, kPayloadSize, 999);
    // 原生全空 + 扩展已有同类同身份堆 → wrapper 选扩展堆（out_slot=0），
    // 且启用态不再调原生 backup。
    static int hook_item = 0;
    g_hook_item = &hook_item;
    g_hook_handle = reg_handle(0, 0);
    g_native_find_save_slot_calls = 0;
    int8_t out_slot = -1;
    const int result = qol::features::core_inventory::hooks::find_save_slot_wrapper(
        &hook_item, &out_slot);
    assert(result == 1);
    assert(out_slot == 0);
    assert(g_native_find_save_slot_calls == 0);
}

void test_wrapper_no_target_returns_zero_without_native_fallback() {
    reset_grid();
    // C18L v603 场景：C18 背包已移除，即使 space1 全空也不能成为目标。
    qol::features::core_inventory::g_c18_native_bag_auto_save = false;
    // 托管域物品（注册源身份 A 堆，99+99 超自身 count_limit=100 → 无合并候选），
    // 其余全部槽位被不同身份占位填满 → resolver 无目标 → 返回 0，
    // 且不回退原生 FindSaveSlot。
    uint8_t hook_payload[kPayloadSize];
    make_payload(hook_payload, kPayloadSize, 1, 7);
    place_reg(0, 0, 10, 99, hook_payload, kPayloadSize, 100);
    uint8_t filler[kPayloadSize];
    make_filler_payload(filler);
    for (int bag = 0; bag < 5; ++bag) fill_native_bag(0, bag);
    // space=1（C18 追加袋）不参与自动落槽；即使全空也不得成为目标。
    for (int bag = 0; bag < kRegBags; ++bag) {
        for (int slot = 0; slot < kSlotsPerBag; ++slot) {
            if (bag == 0 && slot == 0) continue;
            place_reg(bag, slot, 10, 1, filler, kPayloadSize, 999);
        }
    }
    static int hook_item = 0;
    g_hook_item = &hook_item;
    g_hook_handle = reg_handle(0, 0);
    g_native_find_save_slot_calls = 0;
    int8_t out_slot = -1;
    const int result = qol::features::core_inventory::hooks::find_save_slot_wrapper(
        &hook_item, &out_slot);
    assert(result == 0);
    assert(g_native_find_save_slot_calls == 0);
    qol::features::core_inventory::g_c18_native_bag_auto_save = true;
}

void test_move_item_auto_target_prefers_merge() {
    reset_grid();
    uint8_t payload[kPayloadSize];
    make_payload(payload, kPayloadSize, 1, 7);
    place_reg(0, 0, 10, 50, payload, kPayloadSize, 999);
    place_reg(0, 3, 10, 5, payload, kPayloadSize, 999);
    g_reg_import_calls = 0;
    g_reg_import_bag = -1;
    g_reg_import_slot = -1;
    QolBagMutationResult target{};
    const QolResult result = qol::features::core_inventory::transfer::move_item::move_item(
        QolBagRef{kRegSpace, 0}, 3, 2, QolBagRef{kRegSpace, 0}, -1, &target);
    assert(qol::contracts::succeeded(result));
    // 自动目标命中已有同身份堆 slot0，而不是空槽 slot1。
    assert(g_reg_import_calls == 1);
    assert(g_reg_import_bag == 0);
    assert(g_reg_import_slot == 0);
    assert(target.ref.space_id == kRegSpace && target.ref.bag == 0);
    assert(target.count == 2);
}

void test_move_item_auto_target_falls_back_to_empty() {
    reset_grid();
    // bag0 只有源堆：resolver 会命中源槽本身（同槽身份恒等），move_item 的自并
    // 防护回退纯空槽扫描 → 落 slot0（第一个空槽）。
    uint8_t payload[kPayloadSize];
    make_payload(payload, kPayloadSize, 1, 7);
    place_reg(0, 3, 10, 5, payload, kPayloadSize, 999);
    g_reg_import_calls = 0;
    g_reg_import_slot = -1;
    QolBagMutationResult target{};
    const QolResult result = qol::features::core_inventory::transfer::move_item::move_item(
        QolBagRef{kRegSpace, 0}, 3, 1, QolBagRef{kRegSpace, 0}, -1, &target);
    assert(qol::contracts::succeeded(result));
    assert(g_reg_import_calls == 1);
    assert(g_reg_import_slot == 0);
}

void test_move_item_explicit_slot_passthrough() {
    reset_grid();
    uint8_t payload[kPayloadSize];
    make_payload(payload, kPayloadSize, 1, 7);
    place_reg(0, 3, 10, 5, payload, kPayloadSize, 999);
    g_reg_import_calls = 0;
    g_reg_import_slot = -1;
    QolBagMutationResult target{};
    const QolResult result = qol::features::core_inventory::transfer::move_item::move_item(
        QolBagRef{kRegSpace, 0}, 3, 1, QolBagRef{kRegSpace, 0}, 6, &target);
    assert(qol::contracts::succeeded(result));
    assert(g_reg_import_calls == 1);
    assert(g_reg_import_slot == 6);
}

}  // namespace

int main() {
    static qol::contracts::QolItemPort port = make_port();
    qol::features::core_inventory::g_port = &port;

    test_identity_helper();

    static qol::contracts::QolHostSdk sdk = make_sdk();
    attach_hooks(sdk, port);
    test_attach_maps_c18_v603_identity(sdk, port);
    register_reg_source();

    test_global_order_extension_merge_beats_native_empty();
    test_global_order_native_merge_first();
    test_global_order_c18_merge_beats_native_empty();
    test_c18_bag_removed_skips_auto_placement();
    test_explicit_c18_scope_still_supported_when_removed();
    test_global_order_native_empty_before_registered_empty();
    test_same_category_different_payload_never_merges();
    test_stackable_false_skips_all_merge_steps();
    test_stack_limit_uses_count_limit();
    test_task_bag_never_returned();
    test_scope_resolution();
    test_wrapper_prefers_extension_merge();
    test_wrapper_no_target_returns_zero_without_native_fallback();
    test_move_item_auto_target_prefers_merge();
    test_move_item_auto_target_falls_back_to_empty();
    test_move_item_explicit_slot_passthrough();

    qol::features::core_inventory::hooks::detach();
    qol::features::core_inventory::g_port = nullptr;
    qol::features::core_inventory::reset_bag_sources();
    return 0;
}

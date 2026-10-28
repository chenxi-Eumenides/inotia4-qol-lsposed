#include "game/access/inventory.h"

#include "game/state/game_state.h"
#include "game/access/game_access.h"
#include "game/access/item_class.h"
#include "game/access/item_handle_registry.h"
#include "game/access/qol_log_seam.h"
#include "game/variants/game_variant.h"

#include <chrono>
#include <cstring>
#include <mutex>

namespace qol::game {
namespace {

constexpr const char* kInvalidArgument = "invalid argument";
constexpr const char* kNotFound = "inventory slot is empty";
constexpr const char* kNotReady = "inventory not ready";

// 原生袋布局探测会扫描 C18 GOT 袋表并对每个候选袋做 game_memory_accessible；
// set_current_bag / inventory_bag_info 每帧多次调用，必须缓存。布局只在读档/
// 袋数变化时改变，1 秒 TTL 足以覆盖 UI 刷新，同时避免跨存档长期陈旧。
NativeBagLayout g_native_bag_layout_cache{};
bool g_native_bag_layout_valid = false;
std::chrono::steady_clock::time_point g_native_bag_layout_at{};
constexpr auto kNativeBagLayoutTtl = std::chrono::seconds(1);

QolResult error_result(int32_t code, const char* message);
ItemHandleError copy_ref(const InventoryItemRef& source,
                         contracts::QolInventoryItemRef* target);
QolResult item_handle_error(ItemHandleError error);

contracts::QolFindItemAbiFn g_find_item_backup = nullptr;

// C18 改版客户端（v507..v514 同布局）的袋对象表与袋槽表地址；两者相邻，
// 袋对象表 0x735000 起、槽表 0x735100 起（每袋 0x80）。原版客户端这两处
// 仍是未使用的 bss；C18 探测会校验表项对象的可读性、类别和容量。
constexpr uintptr_t G_C18_BAG_TABLE_VMA = 0x735000;
constexpr uintptr_t G_C18_SLOT_TABLE_VMA = 0x735100;
#if __SIZEOF_POINTER__ == 8
static_assert((G_C18_SLOT_TABLE_VMA - G_C18_BAG_TABLE_VMA) / sizeof(void*) ==
                  contracts::kQolMaxNativeBagCount,
              "C18 bag table capacity changed");
#endif
// C18's native tables keep the original six-bag prefix before added bags.
constexpr int32_t kC18FirstNativeBag = contracts::kQolOriginalBagCount;

bool is_c18_bag_entry(void* candidate) {
    if (candidate == nullptr || !game_memory_accessible(candidate, 0x20, 'r')) return false;
    const int category = item_class_category(candidate);
    if (category < 1 || category > 4) return false;
    const auto* bytes = reinterpret_cast<const uint8_t*>(candidate);
    const uint32_t capacity =
        *reinterpret_cast<const uint32_t*>(bytes + 0x10) & 0x1FFFFFFu;
    return capacity == 4 || capacity == 8 || capacity == 12 || capacity == 16;
}

bool probe_c18_bag_count(int32_t* out_count) {
    if (out_count == nullptr || g_base == 0) return false;

    // C18 布局标记：0x735000 处保留原版 6 个袋的镜像；首项为空即不是 C18 布局。
    const uintptr_t layout_probe = g_base + G_C18_BAG_TABLE_VMA;
    if (!game_memory_accessible(reinterpret_cast<void*>(layout_probe), sizeof(void*), 'r') ||
        *reinterpret_cast<void* const*>(layout_probe) == nullptr) {
        return false;
    }

    // 真实袋对象表由游戏自己的 GOT 指向（C18 v520 起不再固定在 0x735000），
    // 逐项用 INVEN_GetBagSize 校验：非袋/越界索引返回 0。
    auto* got = reinterpret_cast<void* const*>(g_base + G_BAG_TABLE_VMA);
    if (!game_memory_accessible(got, sizeof(void*), 'r')) return false;
    auto* bag_table = reinterpret_cast<void* const*>(*got);
    if (bag_table == nullptr ||
        !game_memory_accessible(bag_table,
                                sizeof(void*) * contracts::kQolMaxNativeBagCount, 'r')) {
        return false;
    }

    // 表尾可能残留非袋指针（索引 18 即此类），且 INVEN_GetBagSize 在索引 18 会解引用
    // 该指针导致崩溃。因此只校验袋对象本身：可读、类别 1..4、容量位域为已知值。
    int32_t highest = -1;
    for (int32_t index = 0; index < contracts::kQolMaxNativeBagCount; ++index) {
        if (!game_memory_accessible(bag_table + index, sizeof(void*), 'r')) break;
        if (is_c18_bag_entry(bag_table[index])) highest = index;
    }
    int32_t count = highest + 1;
    if (count < contracts::kQolOriginalBagCount) count = contracts::kQolOriginalBagCount;
    if (count > contracts::kQolMaxNativeBagCount) count = contracts::kQolMaxNativeBagCount;
    *out_count = count;
    return true;
}

NativeBagLayout probe_native_bag_layout() {
    int32_t count = 0;
    if (probe_c18_bag_count(&count)) {
        return {reinterpret_cast<uint8_t*>(g_base + G_C18_SLOT_TABLE_VMA), count};
    }
    return {reinterpret_cast<uint8_t*>(g_inven), contracts::kQolOriginalBagCount};
}

QolResult unsupported_space() {
    return error_result(kQolErrUnsupported, "bag space unsupported");
}

QolResult native_item_ref_at_impl(const NativeBagLayout& layout, int32_t space_id, int32_t bag,
                                  int32_t slot, contracts::QolInventoryItemRef* out,
                                  bool validate_slot) {
    if (layout.slots == nullptr) return error_result(kQolErrNotReady, kNotReady);
    const int32_t native_bag = bag +
        (space_id == contracts::kQolBagSpaceVariant ? kC18FirstNativeBag : 0);
    auto* slot_address = reinterpret_cast<void**>(layout.slots +
        static_cast<size_t>(native_bag) * 0x80 + static_cast<size_t>(slot) * sizeof(void*));
    if (validate_slot && !game_memory_accessible(slot_address, sizeof(*slot_address), 'r')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    void* item = *slot_address;
    if (item == nullptr) return error_result(kQolErrNotFound, kNotFound);
    if (!game_memory_accessible(item, I_TYPE + sizeof(uint16_t), 'r')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    const int category = item_class_category(item);
    if (category < 0) return error_result(kQolErrNotReady, kNotReady);
    const InventoryItemRef source{
        InventoryItemKind::kOriginal, bag, slot, category,
        fn_get_cumulate_count != nullptr ? fn_get_cumulate_count(item) : 1, item};
    const ItemHandleError handle_error = copy_ref(source, out);
    if (handle_error != ItemHandleError::kNone) return item_handle_error(handle_error);
    out->space_id = space_id;
    return contracts::ok();
}

QolResult native_item_ref_at(const NativeBagLayout& layout, int32_t space_id, int32_t bag,
                             int32_t slot, contracts::QolInventoryItemRef* out) {
    return native_item_ref_at_impl(layout, space_id, bag, slot, out, true);
}

QolResult native_bag_capacity(int32_t space_id, int32_t bag, int32_t* out_capacity) {
    if (out_capacity == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    // 容量以原版 INVEN_GetBagSize 为准：未装备的袋没有袋对象，返回 0，
    // 不能按固定 16 格上报，否则空槽扫描会把不可写袋当成可用目标。
    if (fn_get_bag_size == nullptr) return error_result(kQolErrNotReady, kNotReady);
    const int32_t native_bag = bag +
        (space_id == contracts::kQolBagSpaceVariant ? kC18FirstNativeBag : 0);
    *out_capacity = fn_get_bag_size(native_bag);
    return contracts::ok();
}

thread_local uint32_t g_original_only_depth = 0;

void log_find_item_debug(int32_t category, const char* source, void* item,
                         int32_t result_code, uint32_t handle_slot = 0,
                         uint32_t handle_generation = 0, int32_t resolve_error = 0) {
    if (!qol_seam_debug_enabled()) return;
    qol_seam_write(kQolLogLevelDebug, kQolLogDomainInventory, __FILE__, __LINE__,
                   "INVEN_FindItem category=%d source=%s item=%p result_code=%d "
                   "handle=%u/%u resolve_error=%d original_only=%u",
                   category, source, item, result_code, handle_slot, handle_generation,
                   resolve_error, g_original_only_depth != 0 ? 1u : 0u);
}

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

ItemHandleError handle_for_item(void* item, contracts::QolItemHandle* out_handle) {
    if (item == nullptr || out_handle == nullptr) return ItemHandleError::kInvalidArgument;
    *out_handle = lookup_item(item);
    if (out_handle->slot != 0 && out_handle->generation != 0) return ItemHandleError::kNone;
    ItemHandleError error = ItemHandleError::kNone;
    *out_handle = register_item(item, contracts::kQolItemHandleOwnerGame, &error);
    return out_handle->slot != 0 && out_handle->generation != 0 ? ItemHandleError::kNone : error;
}

ItemHandleError copy_ref(const InventoryItemRef& source,
                         contracts::QolInventoryItemRef* target) {
    const ItemHandleError error = handle_for_item(source.native_item, &target->item);
    if (error != ItemHandleError::kNone) return error;
    target->kind = static_cast<int32_t>(source.kind);
    target->bag = source.bag;
    target->slot = source.slot;
    target->category = source.category;
    target->count = source.count;
    return ItemHandleError::kNone;
}

QolResult item_handle_error(ItemHandleError error) {
    switch (error) {
        case ItemHandleError::kNone: return contracts::ok();
        case ItemHandleError::kInvalidArgument:
            return error_result(kQolErrInvalidArgument, kInvalidArgument);
        case ItemHandleError::kWrongThread:
            return error_result(kQolErrBlocked, "wrong thread");
        case ItemHandleError::kFull:
            return error_result(kQolErrNotReady, "item handle registry full");
        case ItemHandleError::kType:
            return error_result(kQolErrConflict, "item handle owner conflict");
        case ItemHandleError::kTransition:
            return error_result(kQolErrBlocked, "item handle transition");
        case ItemHandleError::kStale:
            return error_result(kQolErrNotFound, kNotFound);
    }
    return error_result(kQolErrInternal, "item handle registration failed");
}

struct BagSlotContext {
    contracts::QolInventoryBagSlotFn callback;
    void* callback_context;
};

bool bridge_bag_slot(void* item, int bag, int slot, void* context) {
    auto* state = static_cast<BagSlotContext*>(context);
    contracts::QolInventoryItemRef ref{
        static_cast<int32_t>(contracts::QolInventoryItemKind::kOriginal),
        bag,
        slot,
        item_class_category(item),
        fn_get_cumulate_count != nullptr ? fn_get_cumulate_count(item) : 1,
        {},
    };
    if (handle_for_item(item, &ref.item) != ItemHandleError::kNone) return false;
    return state->callback(&ref, state->callback_context) != 0;
}

}  // namespace

void* game_inventory_find_item_hook(int32_t category) {
    const contracts::QolFindItemAbiFn backup = g_find_item_backup;
    if (backup == nullptr) {
        log_find_item_debug(category, "backup_missing", nullptr, 0);
        return nullptr;
    }
    void* original = backup(category);
    if (original != nullptr) {
        // The pointer remains inside this ABI wrapper and is returned only to the game caller.
        (void)register_item(original, contracts::kQolItemHandleOwnerGame);
        log_find_item_debug(category, "original", original, 0);
        return original;
    }
    if (g_original_only_depth != 0) {
        log_find_item_debug(category, "original_only_miss", nullptr, 0);
        return nullptr;
    }
    log_find_item_debug(category, "native_miss", nullptr, 0);
    return nullptr;
}

QolResult game_inventory_bind_find_item_backup(contracts::QolFindItemAbiFn backup) {
    if (backup == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    g_find_item_backup = backup;
    return contracts::ok();
}

QolResult game_original_find_item_slot(contracts::QolItemHandle handle, int32_t* out_bag,
                                       int32_t* out_slot) {
    if (handle.slot == 0 || handle.generation == 0 || out_bag == nullptr || out_slot == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_inven_find_item_slot == nullptr) return error_result(kQolErrNotReady, kNotReady);
    const ItemHandleResolution resolved =
        item_handle_registry().resolve_item(handle, contracts::kQolItemHandleOwnerAny);
    if (resolved.error == ItemHandleError::kWrongThread) {
        return error_result(kQolErrBlocked, "wrong thread");
    }
    if (resolved.error != ItemHandleError::kNone || resolved.item == nullptr) {
        return error_result(kQolErrNotFound, "stale item handle");
    }
    int8_t encoded = -1;
    if (fn_inven_find_item_slot(resolved.item, &encoded) == 0) {
        return error_result(kQolErrNotFound, kNotFound);
    }
    const uint8_t value = static_cast<uint8_t>(encoded);
    *out_bag = value >> 5;
    *out_slot = value & 0x1F;
    return contracts::ok();
}

QolResult game_item_identity_at(int32_t bag, int32_t slot, contracts::QolItemHandle* out_item) {
    if (out_item == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out_item = contracts::kInvalidItemHandle;
    contracts::QolInventoryItemRef ref{};
    const QolResult result = inventory_item_ref_at(bag, slot, &ref);
    if (!contracts::succeeded(result)) return result;
    *out_item = ref.item;
    return contracts::ok();
}

void game_begin_original_only() { ++g_original_only_depth; }

void game_end_original_only() {
    if (g_original_only_depth != 0) --g_original_only_depth;
}

QolResult game_native_item_at(int32_t bag, int32_t slot, void** out_item) {
    return physical_item_at(bag, slot, out_item);
}

QolResult game_native_item_for_handle(contracts::QolItemHandle handle, void** out_item) {
    if (out_item == nullptr || handle.slot == 0 || handle.generation == 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_item = nullptr;
    const ItemHandleResolution resolved =
        item_handle_registry().resolve_item(handle, contracts::kQolItemHandleOwnerAny);
    if (resolved.error == ItemHandleError::kWrongThread) {
        return error_result(kQolErrBlocked, "wrong thread");
    }
    if (resolved.error != ItemHandleError::kNone || resolved.item == nullptr) {
        return error_result(kQolErrNotFound, "stale item handle");
    }
    *out_item = resolved.item;
    return contracts::ok();
}

QolResult serialize_item(contracts::QolItemHandle handle, uint8_t* out, uint32_t capacity,
                         uint32_t* out_size) {
    if (out_size == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_size = 0;
    if (handle.slot == 0 || handle.generation == 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    void* item = nullptr;
    QolResult result = game_native_item_for_handle(handle, &item);
    if (!contracts::succeeded(result)) return result;
    if (!game_in_world() || fn_save_save_item == nullptr) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    if (!game_memory_accessible(item, I_TYPE + sizeof(uint16_t), 'r')) {
        return error_result(kQolErrNotFound, "stale item handle");
    }

    uint8_t payload[UINT8_MAX]{};
    const int serialized_size = fn_save_save_item(payload, item);
    if (serialized_size <= 0 || serialized_size > static_cast<int>(sizeof(payload))) {
        return error_result(kQolErrInternal, "item serialization failed");
    }
    *out_size = static_cast<uint32_t>(serialized_size);
    if (out == nullptr) return contracts::ok();
    if (capacity < *out_size) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    std::memcpy(out, payload, *out_size);
    return contracts::ok();
}

QolResult inventory_item_ref_at(int32_t bag, int32_t slot,
                                contracts::QolInventoryItemRef* out) {
    if (out == nullptr || bag < 0 || slot < 0 || slot >= 16) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    InventoryItemRef source{};
    if (!::inventory_item_ref_at(bag, slot, &source)) {
        if (bag < 6 && g_inven == nullptr) {
            return error_result(kQolErrNotReady, kNotReady);
        }
        return error_result(kQolErrNotFound, kNotFound);
    }
    const ItemHandleError handle_error = copy_ref(source, out);
    if (handle_error != ItemHandleError::kNone) return item_handle_error(handle_error);
    out->space_id = bag < contracts::kQolOriginalBagCount
                        ? contracts::kQolBagSpaceOriginal
                        : contracts::kQolBagSpaceVariant;
    return contracts::ok();
}

QolResult for_each_bag_slot(contracts::QolInventoryBagSlotFn callback, void* ctx) {
    if (callback == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (g_inven == nullptr) return error_result(kQolErrNotReady, kNotReady);
    BagSlotContext state{callback, ctx};
    ::for_each_bag_slot(&bridge_bag_slot, &state);
    return contracts::ok();
}

NativeBagLayout native_bag_layout() {
    const auto now = std::chrono::steady_clock::now();
    if (!g_native_bag_layout_valid || now - g_native_bag_layout_at >= kNativeBagLayoutTtl) {
        g_native_bag_layout_cache = probe_native_bag_layout();
        g_native_bag_layout_valid = true;
        g_native_bag_layout_at = now;
    }
    return g_native_bag_layout_cache;
}

void invalidate_native_bag_layout_cache() {
    g_native_bag_layout_valid = false;
}

QolResult inventory_bag_count(int32_t space_id, int32_t* out_count) {
    if (out_count == nullptr || space_id < contracts::kQolBagSpaceOriginal) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (space_id == contracts::kQolBagSpaceOriginal) {
        *out_count = contracts::kQolOriginalBagCount;
        return contracts::ok();
    }
    if (space_id == contracts::kQolBagSpaceVariant) {
        int32_t count = 0;
        if (!probe_c18_bag_count(&count)) return error_result(kQolErrNotReady, kNotReady);
        *out_count = count - contracts::kQolOriginalBagCount;
        return contracts::ok();
    }
    return unsupported_space();
}

QolResult inventory_bag_info(const contracts::QolBagRef* ref,
                             contracts::QolBagInfo* out_info) {
    if (ref == nullptr || out_info == nullptr || ref->space_id < contracts::kQolBagSpaceOriginal ||
        ref->bag < 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_info = {};
    int32_t bag_count = 0;
    QolResult result = inventory_bag_count(ref->space_id, &bag_count);
    if (!contracts::succeeded(result)) return result;
    if (ref->bag >= bag_count) return error_result(kQolErrInvalidArgument, kInvalidArgument);

    const NativeBagLayout layout = native_bag_layout();
    if (layout.slots == nullptr) return error_result(kQolErrNotReady, kNotReady);
    int32_t capacity = 0;
    result = native_bag_capacity(ref->space_id, ref->bag, &capacity);
    if (!contracts::succeeded(result)) return result;
    int32_t occupied_slots = 0;
    const int32_t native_bag = ref->bag +
        (ref->space_id == contracts::kQolBagSpaceVariant ? kC18FirstNativeBag : 0);
    for (int32_t slot = 0; slot < 16; ++slot) {
        auto* slot_address = reinterpret_cast<void**>(layout.slots +
            static_cast<size_t>(native_bag) * 0x80 + static_cast<size_t>(slot) * sizeof(void*));
        if (!game_memory_accessible(slot_address, sizeof(*slot_address), 'r')) {
            return error_result(kQolErrNotReady, kNotReady);
        }
        if (*slot_address != nullptr) ++occupied_slots;
    }
    out_info->capacity = capacity;
    out_info->occupied_slots = occupied_slots;
    return contracts::ok();
}

QolResult inventory_space_item_at(const contracts::QolBagRef* ref, int32_t slot,
                                  contracts::QolInventoryItemRef* out) {
    if (ref == nullptr || out == nullptr || ref->space_id < contracts::kQolBagSpaceOriginal ||
        ref->bag < 0 || slot < 0 || slot >= 16) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out = {};
    int32_t bag_count = 0;
    QolResult result = inventory_bag_count(ref->space_id, &bag_count);
    if (!contracts::succeeded(result)) return result;
    if (ref->bag >= bag_count) return error_result(kQolErrInvalidArgument, kInvalidArgument);

    const NativeBagLayout layout = native_bag_layout();
    return native_item_ref_at(layout, ref->space_id, ref->bag, slot, out);
}

QolResult inventory_space_item_pointer(const contracts::QolBagRef* ref, int32_t slot,
                                       void** out) {
    if (ref == nullptr || out == nullptr || ref->space_id < contracts::kQolBagSpaceOriginal ||
        ref->bag < 0 || slot < 0 || slot >= 16) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out = nullptr;
    int32_t bag_count = 0;
    QolResult result = inventory_bag_count(ref->space_id, &bag_count);
    if (!contracts::succeeded(result)) return result;
    if (ref->bag >= bag_count) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    const NativeBagLayout layout = native_bag_layout();
    if (layout.slots == nullptr) return error_result(kQolErrNotReady, kNotReady);
    const int32_t native_bag = ref->bag +
        (ref->space_id == contracts::kQolBagSpaceVariant ? kC18FirstNativeBag : 0);
    auto* slot_address = reinterpret_cast<void**>(layout.slots +
        static_cast<size_t>(native_bag) * 0x80 + static_cast<size_t>(slot) * sizeof(void*));
    if (!game_memory_accessible(slot_address, sizeof(*slot_address), 'r')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    if (*slot_address == nullptr) return error_result(kQolErrNotFound, kNotFound);
    *out = *slot_address;
    return contracts::ok();
}

QolResult inventory_for_each_space_slot(int32_t space_id,
                                        contracts::QolInventoryBagSlotFn callback, void* ctx) {
    if (space_id < contracts::kQolBagSpaceOriginal || callback == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    int32_t bag_count = 0;
    QolResult result = inventory_bag_count(space_id, &bag_count);
    if (!contracts::succeeded(result)) return result;
    const NativeBagLayout layout = native_bag_layout();
    if (layout.slots == nullptr) return error_result(kQolErrNotReady, kNotReady);
    // 袋槽表整段只校验一次；逐槽 native_item_ref_at 会重复 game_memory_accessible，
    // autosell 每次扫描 6*16 槽时形成主要开销。物品指针仍逐项校验，保持 fail-closed。
    const int32_t native_offset =
        space_id == contracts::kQolBagSpaceVariant ? kC18FirstNativeBag : 0;
    const int32_t total_bags = bag_count + native_offset;
    if (total_bags <= 0 || total_bags > contracts::kQolMaxNativeBagCount) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    if (!game_memory_accessible(layout.slots, static_cast<size_t>(total_bags) * 0x80, 'r')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    contracts::QolInventoryItemRef item{};
    for (int32_t bag = 0; bag < bag_count; ++bag) {
        for (int32_t slot = 0; slot < 16; ++slot) {
            result = native_item_ref_at_impl(layout, space_id, bag, slot, &item, false);
            if (result.code == kQolErrNotFound) continue;
            if (!contracts::succeeded(result)) return result;
            if (callback(&item, ctx) != 0) return contracts::ok();
        }
    }
    return contracts::ok();
}

QolResult physical_item_at(int32_t bag, int32_t slot, void** out_item) {
    const NativeBagLayout layout = native_bag_layout();
    if (out_item == nullptr || bag < 0 || bag >= layout.bag_limit || slot < 0 || slot >= 16) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_item = nullptr;
    if (layout.slots == nullptr) return error_result(kQolErrNotReady, kNotReady);
    // Direct slot addressing matches set_physical_item. The previous
    // for_each_bag_slot lookup was O(6*16) per query; the projection draw gate
    // queries every visible slot and made a 96x96 scan per frame.
    auto* slot_address = reinterpret_cast<void**>(
        layout.slots + static_cast<size_t>(bag) * 0x80 + static_cast<size_t>(slot) * sizeof(void*));
    if (game_memory_accessible(slot_address, sizeof(*slot_address), 'r')) {
        *out_item = *slot_address;
        return *out_item == nullptr ? error_result(kQolErrNotFound, kNotFound) : contracts::ok();
    }
    // Host tests and early boot may provide a sentinel g_inven whose slot table
    // is not directly readable. Preserve the legacy read-only lookup as a
    // fallback instead of reporting the whole inventory unavailable.
    struct Context {
        int32_t bag;
        int32_t slot;
        void** out;
    } context{bag, slot, out_item};
    ::for_each_bag_slot([](void* item, int bag_index, int slot_index, void* raw) -> bool {
        auto* context = static_cast<Context*>(raw);
        if (context->bag == bag_index && context->slot == slot_index) {
            *context->out = item;
            return true;
        }
        return false;
    }, &context);
    return *out_item == nullptr ? error_result(kQolErrNotFound, kNotFound) : contracts::ok();
}

QolResult set_physical_item(int32_t bag, int32_t slot, void* item) {
    if (bag < 0 || bag >= 6 || slot < 0 || slot >= 16) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (g_inven == nullptr) return error_result(kQolErrNotReady, kNotReady);
    auto* slot_address = reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(g_inven) + static_cast<size_t>(bag) * 0x80 +
        static_cast<size_t>(slot) * sizeof(void*));
    if (!game_memory_accessible(slot_address, sizeof(*slot_address), 'w')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    *slot_address = item;
    return contracts::ok();
}

namespace {

QolResult native_space_slot_address(const contracts::QolBagRef* ref, int32_t slot,
                                    void*** out_address) {
    if (out_address == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (ref == nullptr || (ref->space_id != contracts::kQolBagSpaceOriginal &&
                           ref->space_id != contracts::kQolBagSpaceVariant) ||
        ref->bag < 0 || slot < 0 || slot >= 16 ||
        (ref->space_id == contracts::kQolBagSpaceOriginal &&
         ref->bag == contracts::kQolTaskBagIndex)) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    int32_t bag_count = 0;
    QolResult result = inventory_bag_count(ref->space_id, &bag_count);
    if (!contracts::succeeded(result)) return result;
    if (ref->bag >= bag_count) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    const NativeBagLayout layout = native_bag_layout();
    if (layout.slots == nullptr) return error_result(kQolErrNotReady, kNotReady);
    const int32_t native_bag = ref->bag +
        (ref->space_id == contracts::kQolBagSpaceVariant ? kC18FirstNativeBag : 0);
    if (native_bag >= layout.bag_limit) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    auto* slot_address = reinterpret_cast<void**>(layout.slots +
        static_cast<size_t>(native_bag) * 0x80 + static_cast<size_t>(slot) * sizeof(void*));
    if (!game_memory_accessible(slot_address, sizeof(*slot_address), 'w')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    *out_address = slot_address;
    return contracts::ok();
}

}  // namespace

QolResult set_native_space_item(const contracts::QolBagRef* ref, int32_t slot, void* item) {
    void** slot_address = nullptr;
    const QolResult result = native_space_slot_address(ref, slot, &slot_address);
    if (!contracts::succeeded(result)) return result;
    *slot_address = item;
    return contracts::ok();
}

QolResult swap_native_space_items(const contracts::QolBagRef* a, int32_t slot_a,
                                  const contracts::QolBagRef* b, int32_t slot_b) {
    void** address_a = nullptr;
    void** address_b = nullptr;
    QolResult result = native_space_slot_address(a, slot_a, &address_a);
    if (!contracts::succeeded(result)) return result;
    result = native_space_slot_address(b, slot_b, &address_b);
    if (!contracts::succeeded(result)) return result;
    if (*address_a == nullptr || *address_b == nullptr) {
        return error_result(kQolErrNotFound, kNotFound);
    }
    void* item_a = *address_a;
    *address_a = *address_b;
    *address_b = item_a;
    return contracts::ok();
}

QolResult detail_context(int32_t* out_desc_type, int32_t* out_bag, void** out_control) {
    if (out_desc_type == nullptr || out_bag == nullptr || out_control == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (g_base == 0) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    auto* desc_type = reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_DESC_TYPE_VMA);
    auto* current_bag = reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA);
    auto** panel_control = reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA);
    if (!game_memory_accessible(desc_type, sizeof(*desc_type), 'r') ||
        !game_memory_accessible(current_bag, sizeof(*current_bag), 'r') ||
        !game_memory_accessible(panel_control, sizeof(*panel_control), 'r')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    *out_desc_type = *desc_type;
    *out_bag = *current_bag;
    *out_control = *panel_control;
    return *out_control == nullptr ? error_result(kQolErrNotReady, kNotReady) : contracts::ok();
}

QolResult cursor_index(const void* control, int32_t* out_slot) {
    if (control == nullptr || out_slot == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_control_object_get_cursor_index == nullptr) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    *out_slot = fn_control_object_get_cursor_index(const_cast<void*>(control));
    return *out_slot < 0 ? error_result(kQolErrNotFound, kNotFound) : contracts::ok();
}

QolResult detail_item_context(contracts::QolItemDetailContext* out_context) {
    if (out_context == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out_context = {0, -1, -1, contracts::kInvalidItemHandle};
    int32_t desc_type = 0;
    int32_t bag = -1;
    void* control = nullptr;
    QolResult result = detail_context(&desc_type, &bag, &control);
    if (!contracts::succeeded(result)) return result;
    int32_t slot = -1;
    result = cursor_index(control, &slot);
    if (!contracts::succeeded(result)) return result;
    contracts::QolInventoryItemRef item_ref{};
    result = inventory_item_ref_at(bag, slot, &item_ref);
    if (!contracts::succeeded(result)) return result;
    if (item_ref.bag != bag || item_ref.slot != slot || item_ref.item.slot == 0 ||
        item_ref.item.generation == 0) {
        return error_result(kQolErrNotFound, "detail item identity unavailable");
    }
    out_context->desc_type = desc_type;
    out_context->bag = bag;
    out_context->slot = slot;
    out_context->item = item_ref.item;
    return contracts::ok();
}

bool shortcut_item_category(int32_t shortcut, int32_t* out_category) {
    if (out_category == nullptr || g_base == 0) return false;
    uint8_t** state_slot = reinterpret_cast<uint8_t**>(g_base + G_SHORTCUT_STATE_GOT_VMA);
    uint8_t** entry_slot = reinterpret_cast<uint8_t**>(g_base + G_SHORTCUT_ENTRY_GOT_VMA);
    uint8_t** page_slot = reinterpret_cast<uint8_t**>(g_base + G_SHORTCUT_PAGE_GOT_VMA);
    if (!game_memory_accessible(state_slot, sizeof(*state_slot), 'r') ||
        !game_memory_accessible(entry_slot, sizeof(*entry_slot), 'r') ||
        !game_memory_accessible(page_slot, sizeof(*page_slot), 'r')) {
        return false;
    }
    uint8_t* state = *state_slot;
    uint8_t* entries = *entry_slot;
    uint8_t* page_ptr = *page_slot;
    if (state == nullptr || entries == nullptr || page_ptr == nullptr ||
        !game_memory_accessible(state, 0x2c, 'r') ||
        !game_memory_accessible(page_ptr, 1, 'r')) {
        return false;
    }
    int index = -1;
    for (int slot = 0; slot < 6; ++slot) {
        int32_t key = 0;
        std::memcpy(&key, state + 20 + slot * 4, sizeof(key));
        if (key == shortcut) {
            index = slot;
            break;
        }
    }
    if (index < 0 && shortcut >= 0 && shortcut < 6) index = shortcut;
    if (index < 0) return false;
    const int entry_index = static_cast<int>(*page_ptr) + index;
    if (entry_index < 0) return false;
    uint8_t* entry = entries + static_cast<size_t>(entry_index) * 16;
    if (!game_memory_accessible(entry, 6, 'r') || entry[0] != 2) return false;
    const int category = static_cast<int>(entry[4]) |
                         (static_cast<int>(entry[5]) << 8);
    if (category <= 0) return false;
    *out_category = category;
    return true;
}

QolResult bag_size(int32_t bag, int32_t* out_size) {
    if (bag < 0 || bag >= 6 || out_size == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_get_bag_size == nullptr) return error_result(kQolErrNotReady, kNotReady);
    *out_size = fn_get_bag_size(bag);
    return *out_size < 0 ? error_result(kQolErrNotReady, kNotReady) : contracts::ok();
}

QolResult is_empty_bag(int32_t bag, int32_t* out_empty) {
    if (bag < 0 || bag >= 6 || out_empty == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_is_empty_bag == nullptr) return error_result(kQolErrNotReady, kNotReady);
    *out_empty = fn_is_empty_bag(bag) != 0 ? 1 : 0;
    return contracts::ok();
}

QolResult empty_bag_slot(int32_t* out_slot) {
    if (out_slot == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out_slot = -1;
    if (fn_get_empty_bag_slot == nullptr) return error_result(kQolErrNotReady, kNotReady);
    *out_slot = fn_get_empty_bag_slot();
    return contracts::ok();
}

QolResult find_save_slot(const void* item, int32_t* out_slot) {
    if (item == nullptr || out_slot == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_inven_find_save_slot == nullptr) return error_result(kQolErrNotReady, kNotReady);
    int8_t slot = -1;
    if (fn_inven_find_save_slot(const_cast<void*>(item), &slot) == 0 || slot < 0) {
        return error_result(kQolErrNotFound, kNotFound);
    }
    *out_slot = slot;
    return contracts::ok();
}

QolResult current_bag(int32_t* out_bag) {
    if (out_bag == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    int32_t desc = 0;
    void* control = nullptr;
    return detail_context(&desc, out_bag, &control);
}

QolResult set_current_bag(int32_t bag) {
    // 0..5 是原版袋，6 是扩展投影用的“无原版选中”值；C18 追加袋范围与
    // “页签总数 N”这个越界无选中值都随运行时袋布局动态放开，其他版本保持 <7。
    const int32_t upper = static_cast<int32_t>(native_bag_layout().bag_limit) + 1;
    if (bag < 0 || bag >= upper || g_base == 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    auto* direct = reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA);
    auto** got = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (!game_memory_accessible(direct, sizeof(*direct), 'w')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    *direct = static_cast<uint8_t>(bag);
    if (got != nullptr && game_memory_accessible(got, sizeof(*got), 'r') && *got != nullptr &&
        game_memory_accessible(*got, sizeof(**got), 'w')) {
        **got = static_cast<uint8_t>(bag);
    }
    return contracts::ok();
}

QolResult desc_type(int32_t* out_type) {
    if (out_type == nullptr || g_base == 0) {
        return error_result(out_type == nullptr ? kQolErrInvalidArgument : kQolErrNotReady,
                            out_type == nullptr ? kInvalidArgument : kNotReady);
    }
    auto* value = reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_DESC_TYPE_VMA);
    if (!game_memory_accessible(value, sizeof(*value), 'r')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    *out_type = *value;
    return contracts::ok();
}

QolResult set_desc_type(int32_t type) {
    if (type < 0 || type > UINT8_MAX || g_base == 0) {
        return error_result(type < 0 || type > UINT8_MAX ? kQolErrInvalidArgument : kQolErrNotReady,
                            type < 0 || type > UINT8_MAX ? kInvalidArgument : kNotReady);
    }
    auto* value = reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_DESC_TYPE_VMA);
    if (!game_memory_accessible(value, sizeof(*value), 'w')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    *value = static_cast<uint8_t>(type);
    return contracts::ok();
}

QolResult bag_capacity_word(int32_t bag, uint32_t* out_word) {
    if (bag < 0 || bag >= 6 || out_word == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (g_base == 0) return error_result(kQolErrNotReady, kNotReady);
    auto*** table_slot = reinterpret_cast<void***>(g_base + G_BAG_TABLE_VMA);
    if (!game_memory_accessible(table_slot, sizeof(*table_slot), 'r') || *table_slot == nullptr) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    void* bag_object = (*table_slot)[bag];
    if (bag_object == nullptr) return error_result(kQolErrNotFound, kNotFound);
    auto* word = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(bag_object) + BAG_OBJECT_CAPACITY);
    if (!game_memory_accessible(word, sizeof(*word), 'r')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    *out_word = *word;
    return contracts::ok();
}

QolResult set_bag_capacity_word(int32_t bag, uint32_t word) {
    if (bag < 0 || bag >= 6) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    uint32_t ignored = 0;
    const QolResult ready = bag_capacity_word(bag, &ignored);
    if (!contracts::succeeded(ready)) return ready;
    auto*** table_slot = reinterpret_cast<void***>(g_base + G_BAG_TABLE_VMA);
    void* bag_object = (*table_slot)[bag];
    auto* target = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(bag_object) + BAG_OBJECT_CAPACITY);
    if (!game_memory_accessible(target, sizeof(*target), 'w')) {
        return error_result(kQolErrNotReady, kNotReady);
    }
    *target = word;
    return contracts::ok();
}

}  // namespace qol::game

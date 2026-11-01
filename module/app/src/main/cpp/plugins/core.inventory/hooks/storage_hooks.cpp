#include "storage_hooks.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

#include "contracts/bag.h"
#include "contracts/inventory_scope.h"
#include "contracts/inventory_stage4.h"
#include "contracts/s2_writeback.h"
#include "plugins/core.inventory/api/bag_source.h"
#include "plugins/core.inventory/hooks/inventory_logging.h"
#include "plugins/core.inventory/hooks/placement.h"
#include "plugins/core.inventory/runtime/context.h"

namespace qol::features::core_inventory::hooks {
namespace {

// storage 纯函数单一来源（S2/stage4）位于 contracts/inventory_stage4.h。
namespace inventory_stage = qol::contracts::inventory_stage;

using HaveItemFn = int (*)(int32_t);
using GetItemCountFn = int (*)(int32_t);
using GetCumulateCountFn = int (*)(void*);
using ConsumeItemFn = void (*)(void*);
using RemoveItemFn = int (*)(void*);
using SaveItemFn = int (*)(void*);
using InvenMoveItemFn = int (*)(void*, int, int, int);
using IsHavingEmptySlotFn = int (*)(int32_t, int32_t);
using InvenSaveItemDirectFn = int (*)(void*, int32_t, int32_t);
using InvenFindSaveSlotFn = int (*)(void*, int8_t*);
using InvenRemoveItemDataFn = int (*)(int32_t, int32_t);

constexpr const char* kClaimOwner = "core.inventory";

const qol::contracts::QolHostSdk* g_sdk = nullptr;
const qol::contracts::QolGameHost* g_game = nullptr;
const qol::contracts::QolItemPort* g_port = nullptr;
std::atomic<bool> g_support{true};

HaveItemFn g_backup_have_item = nullptr;
GetItemCountFn g_backup_get_item_count = nullptr;
GetCumulateCountFn g_backup_get_cumulate_count = nullptr;
ConsumeItemFn g_backup_consume_item = nullptr;
RemoveItemFn g_backup_remove_item = nullptr;
SaveItemFn g_backup_save_item = nullptr;
InvenMoveItemFn g_backup_move_item = nullptr;
IsHavingEmptySlotFn g_backup_is_having_empty_slot = nullptr;
InvenSaveItemDirectFn g_backup_save_item_direct = nullptr;
InvenFindSaveSlotFn g_backup_find_save_slot = nullptr;
InvenRemoveItemDataFn g_backup_remove_item_data = nullptr;

std::atomic<uint64_t> g_consume_item_calls{0};
std::atomic<uint64_t> g_remove_item_calls{0};
std::atomic<uint64_t> g_remove_item_trace_calls{0};

thread_local bool g_in_have_item = false;
thread_local bool g_in_get_item_count = false;
thread_local bool g_in_consume_item = false;
thread_local bool g_in_remove_item = false;
struct RemoveItemCallFrame {
    void* item = nullptr;
    RemoveItemCallFrame* previous = nullptr;
};
thread_local RemoveItemCallFrame* g_remove_item_call_top = nullptr;
thread_local int g_save_item_depth = 0;
thread_local int g_save_item_direct_fail_count = 0;
thread_local std::vector<void*> g_pending_eb_items;
// 拆分地图 R-7：core.item 的 unequip wrapper 与本插件 FindSaveSlot hook 分属两个
// 插件，不能跨插件共享 TLS；unequip 作用域信号经 contracts 的 inline thread_local
// （contracts/inventory_scope.h: g_inventory_unequip_depth）下发，FindSaveSlot 在
// unequip 调用栈内据此短路。

class RemoveItemIdentityGuard {
public:
    explicit RemoveItemIdentityGuard(void* item) {
        for (RemoveItemCallFrame* frame = g_remove_item_call_top;
             frame != nullptr; frame = frame->previous) {
            if (frame->item == item) return;
        }
        frame_.item = item;
        frame_.previous = g_remove_item_call_top;
        g_remove_item_call_top = &frame_;
        entered_ = true;
    }

    ~RemoveItemIdentityGuard() {
        if (entered_) g_remove_item_call_top = frame_.previous;
    }

    RemoveItemIdentityGuard(const RemoveItemIdentityGuard&) = delete;
    RemoveItemIdentityGuard& operator=(const RemoveItemIdentityGuard&) = delete;

    bool entered() const { return entered_; }

private:
    RemoveItemCallFrame frame_{};
    bool entered_ = false;
};

void backup_missing(const char* hook_id) {
    QOL_LOG_ERROR(QolDomain::kInventory, "hook=%s backup missing",
                  hook_id == nullptr ? "unknown" : hook_id);
}

template <typename Function, typename... Args>
auto call_backup(const char* hook_id, Function backup, Args&&... args)
    -> decltype(backup(std::forward<Args>(args)...)) {
    using Return = decltype(backup(std::forward<Args>(args)...));
    if (backup == nullptr) {
        backup_missing(hook_id);
        if constexpr (std::is_void<Return>::value) {
            return;
        } else {
            return Return{};
        }
    }
    return backup(std::forward<Args>(args)...);
}

const qol::contracts::QolItemHandleApi* item_handle_api() {
    if (g_sdk == nullptr ||
        g_sdk->abi.size < offsetof(qol::contracts::QolHostSdk, item_handle) +
                              sizeof(qol::contracts::QolItemHandleApi)) return nullptr;
    return &g_sdk->item_handle;
}

void invalidate_item_handle(void* item) {
    const auto* api = item_handle_api();
    if (item == nullptr || api == nullptr || api->invalidate_item == nullptr) return;
    (void)api->invalidate_item(
        item, qol::contracts::kQolItemHandleOwnerAny);
}

bool item_handle_for_hook_item(void* item, qol::contracts::QolItemHandle* out_handle) {
    const auto* api = item_handle_api();
    if (item == nullptr || out_handle == nullptr || api == nullptr) return false;
    uint32_t owner = 0;
    if (api->lookup_item != nullptr &&
        api->lookup_item(item, out_handle, &owner) == qol::contracts::kQolItemHandleStatusOk) {
        return true;
    }
    return api->register_item != nullptr &&
           api->register_item(item, qol::contracts::kQolItemHandleOwnerGame, out_handle) ==
               qol::contracts::kQolItemHandleStatusOk;
}

const qol::contracts::QolBagSource* source_for_item_handle(
    qol::contracts::QolItemHandle handle, qol::contracts::QolInventoryItemRef* out_item) {
    if (handle.slot == 0 || handle.generation == 0 || out_item == nullptr) return nullptr;
    for (uint32_t index = 0; index < registered_bag_source_count(); ++index) {
        const auto* source = registered_bag_source_at(index);
        if (source == nullptr || source->identify == nullptr || source->item_at == nullptr) continue;
        qol::contracts::QolBagRef ref{};
        int32_t slot = -1;
        if (!qol::contracts::succeeded(source->identify(handle, &ref, &slot)) ||
            ref.space_id != source->space_id || ref.bag < 0 || slot < 0) {
            continue;
        }
        qol::contracts::QolInventoryItemRef item{};
        if (!qol::contracts::succeeded(source->item_at(ref.bag, slot, &item)) ||
            item.space_id != ref.space_id || item.bag != ref.bag || item.slot != slot ||
            item.item.slot != handle.slot || item.item.generation != handle.generation) {
            continue;
        }
        *out_item = item;
        return source;
    }
    return nullptr;
}

const qol::contracts::QolBagSource* source_for_hook_item(
    void* item, qol::contracts::QolInventoryItemRef* out_item) {
    qol::contracts::QolItemHandle handle{};
    return item_handle_for_hook_item(item, &handle)
               ? source_for_item_handle(handle, out_item)
               : nullptr;
}

bool remove_registered_source_item(void* item, int32_t count) {
    qol::contracts::QolInventoryItemRef ref{};
    const auto* source = source_for_hook_item(item, &ref);
    if (source == nullptr || source->remove == nullptr || count <= 0 || ref.count < count) {
        return false;
    }
    qol::contracts::QolBagMutationResult mutation{};
    const QolResult result = source->remove(ref.item, count, &mutation);
    if (!qol::contracts::succeeded(result)) {
        QOL_LOG_WARN(QolDomain::kInventory,
                     "registered source consume failed space=%d bag=%d slot=%d count=%d code=%d",
                     ref.space_id, ref.bag, ref.slot, count, result.code);
    }
    return qol::contracts::succeeded(result);
}

bool identify_registered_source_item(void* item, int32_t* bag, int32_t* slot) {
    qol::contracts::QolInventoryItemRef ref{};
    if (bag == nullptr || slot == nullptr || source_for_hook_item(item, &ref) == nullptr) {
        return false;
    }
    *bag = ref.bag;
    *slot = ref.slot;
    return true;
}

bool consume_registered_source_item(void* item) {
    return remove_registered_source_item(item, 1);
}

struct SourceRemovalCandidate {
    const qol::contracts::QolBagSource* source;
    qol::contracts::QolInventoryItemRef item;
};

struct SourceRemovalContext {
    int32_t category;
    SourceRemovalCandidate* candidates;
    size_t capacity;
    size_t count;
};

int32_t collect_source_removal_candidate(
    const qol::contracts::QolInventoryItemRef* item, void* raw) {
    auto* context = static_cast<SourceRemovalContext*>(raw);
    if (item != nullptr && item->category == context->category && item->count > 0 &&
        context->count < context->capacity) {
        context->candidates[context->count++] = {find_bag_source(item->space_id), *item};
    }
    return 0;
}

int remove_registered_source_category(int32_t category, int32_t count) {
    std::array<SourceRemovalCandidate, 512> candidates{};
    SourceRemovalContext context{category, candidates.data(), candidates.size(), 0};
    for (uint32_t index = 0; index < registered_bag_source_count(); ++index) {
        const auto* source = registered_bag_source_at(index);
        if (source != nullptr && source->for_each_item != nullptr) {
            (void)source->for_each_item(&collect_source_removal_candidate, &context);
        }
    }
    int32_t remaining = count;
    for (size_t index = 0; index < context.count && remaining > 0; ++index) {
        const SourceRemovalCandidate& candidate = candidates[index];
        if (candidate.source == nullptr || candidate.source->remove == nullptr) continue;
        const int32_t amount = candidate.item.count < remaining ? candidate.item.count : remaining;
        qol::contracts::QolBagMutationResult mutation{};
        if (qol::contracts::succeeded(
                candidate.source->remove(candidate.item.item, amount, &mutation))) {
            remaining -= amount;
        }
    }
    return count - remaining;
}

enum class ItemCountWriteResult : uint8_t {
    kApplied,
    kNotReady,
    kFailed,
};

const qol::contracts::QolItemCountApi* item_count_api() {
    return g_game == nullptr ? nullptr : &g_game->item_count;
}

void* physical_item_at(int bag, int slot) {
    if (g_game == nullptr || g_game->item_abi.native_item_at == nullptr) return nullptr;
    void* item = nullptr;
    const QolResult result = g_game->item_abi.native_item_at(bag, slot, &item);
    return qol::contracts::succeeded(result) ? item : nullptr;
}

bool item_count_is_encoded(void* item) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api == nullptr || api->is_encoded == nullptr || item == nullptr) return false;
    qol::contracts::QolItemCountEncoding encoding =
        qol::contracts::QolItemCountEncoding::kUnknown;
    const QolResult result = api->is_encoded(item, &encoding);
    return qol::contracts::succeeded(result) &&
           encoding == qol::contracts::QolItemCountEncoding::kEncoded;
}

bool item_count_effective(void* item, uint32_t* out_count) {
    if (item == nullptr || out_count == nullptr) return false;
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api != nullptr && api->effective != nullptr) {
        const QolResult result = api->effective(item, out_count);
        if (qol::contracts::succeeded(result)) return true;
        return false;
    }
    return false;
}

ItemCountWriteResult item_count_write_effective(void* item, uint32_t count) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api == nullptr || api->writeback_needed == nullptr ||
        api->write_effective == nullptr || item == nullptr) {
        return ItemCountWriteResult::kFailed;
    }
    int32_t needed = 0;
    const QolResult needed_result = api->writeback_needed(item, count, &needed);
    if (needed_result.code == kQolErrNotReady) return ItemCountWriteResult::kNotReady;
    if (!qol::contracts::succeeded(needed_result) || needed == 0) {
        return qol::contracts::succeeded(needed_result)
            ? ItemCountWriteResult::kApplied : ItemCountWriteResult::kFailed;
    }
    const QolResult result = api->write_effective(item, count);
    if (result.code == kQolErrNotReady) return ItemCountWriteResult::kNotReady;
    return qol::contracts::succeeded(result) ? ItemCountWriteResult::kApplied
                                             : ItemCountWriteResult::kFailed;
}

// 999 扩展模式是否已实际应用（Host-owned policy）：OK1 才视为生效；NotReady（请求已开但
// 资源组未落地）与 OK0（明确关闭）都按未启用处理，不回退、不跨 owner 读配置键。
bool item_count_extended_applied() {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api == nullptr || api->read_extended_mode == nullptr) return false;
    uint8_t extended = 0;
    return qol::contracts::succeeded(api->read_extended_mode(&extended)) && extended != 0;
}

bool item_count_feature_enabled(void* item) {
    uint32_t effective = 0;
    return item_count_extended_applied() && item_count_effective(item, &effective);
}

uint32_t read_count_field(const void* item) {
    const auto* api = item_count_api();
    uint32_t field = 0;
    if (api == nullptr || api->read_encoded_field == nullptr || item == nullptr ||
        !qol::contracts::succeeded(api->read_encoded_field(item, &field))) {
        return 0;
    }
    return field;
}

void s2_write_count_field(void* item, uint32_t field) {
    const auto* api = item_count_api();
    if (api != nullptr && api->write_encoded_field != nullptr && item != nullptr) {
        (void)api->write_encoded_field(item, field);
    }
}

// ---------------------------------------------------------------------------
// S2 写侧进位/借位纯函数已单源化至 contracts/s2_writeback.h（R-45/R-49），
// 本文件经 include 使用同一份 inline 实现。
// ---------------------------------------------------------------------------

// 物理库存包含 6 袋（含任务袋 5）；写侧读取经 Game Host 物理槽原语，避免扩展物化路径取锁。
// C18 原生袋上界由 lifecycle 按 bag_count(0)+bag_count(1) 下发。
constexpr int kInvenPhysicalBagCount = 6;
constexpr int kInvenSlotsPerBag = 16;
constexpr size_t kInvenBagStride = 0x80;

int physical_bag_limit() {
    return std::max(bags::native_bag_limit(), kInvenPhysicalBagCount);
}

// 纯内存读物理袋槽物品指针（bag 0..limit-1，含任务袋；越界/未就绪返回 nullptr）。
// 该函数只读取物理槽，不经过逻辑袋物化。
void* s2_physical_slot_item(int bag, int slot) {
    if (bag < 0 || bag >= physical_bag_limit() || slot < 0 || slot >= kInvenSlotsPerBag ||
        g_game == nullptr) {
        return nullptr;
    }
    return physical_item_at(bag, slot);
}

// 在物理袋 0..5（含任务袋）定位对象所在槽；找不到返回 false。
// 纯指针扫描，无锁、无被 Hook 函数调用；供回写前后验证对象仍在槽内，
// 防止对象被原版释放后触磁悬垂指针。
bool s2_find_physical_slot(const void* item, int* out_bag, int* out_slot) {
    if (item == nullptr) return false;
    for (int bag = 0; bag < physical_bag_limit(); ++bag) {
        for (int slot = 0; slot < kInvenSlotsPerBag; ++slot) {
            if (s2_physical_slot_item(bag, slot) == item) {
                *out_bag = bag;
                *out_slot = slot;
                return true;
            }
        }
    }
    return false;
}

// S2 写侧回写总门控（R-46 fail-closed + feature 门控对齐）：999 原子组必须已应用，
// 且对象有效并为 count-encoded。
// kUnknown 与非可堆叠（装备 marker、宝石选项
// bits18–23、袋容量 bits0–24）、关闭态与 NotReady 一律返回 false，调用方保持原版行为。
bool s2_writeback_gate(void* item) {
    return item != nullptr && qol::contracts::s2_writeback::allowed(item_count_extended_applied(),
                                                    item_count_is_encoded(item));
}

// 加/合并类取证（backup 前采集，backup 后确认回写）。
struct S2MergeCapture {
    bool armed = false;
    void* target = nullptr;
    int bag = -1;
    int slot = -1;
    uint32_t pre_field = 0;
    uint32_t pre_full = 0;
    uint32_t delta = 0;
    uint32_t new_full = 0;
};

// 采集 (bag,slot) 物理槽占用者的旧字段与预期全量。门控：双方均须 count-encoded
// 且占用者不是被移动对象本身（空槽移动/入库无合并写）。
void s2_capture_merge(void* moved_item, int bag, int slot, int32_t delta, S2MergeCapture* cap) {
    cap->armed = false;
    if (moved_item == nullptr || delta <= 0) return;
    void* occupant = s2_physical_slot_item(bag, slot);
    if (occupant == nullptr || occupant == moved_item) return;
    if (!s2_writeback_gate(moved_item) || !s2_writeback_gate(occupant)) return;
    cap->armed = true;
    cap->target = occupant;
    cap->bag = bag;
    cap->slot = slot;
    cap->pre_field = read_count_field(occupant);
    // gate 保证 999 原子组已应用，此处读取完整 128a+b。
    if (!item_count_effective(occupant, &cap->pre_full)) {
        cap->armed = false;
        return;
    }
    cap->delta = static_cast<uint32_t>(delta);
    cap->new_full = qol::contracts::s2_writeback::added_count(item_count_api(), cap->pre_full, cap->delta);
}

// MoveItem 的源槽取证：delta 是 128 的整数倍时原版只写 b 段，b 不变，
// 无法用 post!=pre 证明合并发生；改为确认源槽已不再指向被移动对象。
struct S2SourceCapture {
    bool found = false;
    void* item = nullptr;
    int bag = -1;
    int slot = -1;
};

void s2_capture_source(void* item, S2SourceCapture* out) {
    if (out == nullptr) return;
    *out = {};
    if (item == nullptr) return;
    for (int bag = 0; bag < physical_bag_limit(); ++bag) {
        for (int slot = 0; slot < kInvenSlotsPerBag; ++slot) {
            if (s2_physical_slot_item(bag, slot) == item) {
                out->found = true;
                out->item = item;
                out->bag = bag;
                out->slot = slot;
                return;
            }
        }
    }
}

// backup 后确认回写：目标槽仍指向同一对象（防替换/释放后的 UAF 读）、原版确实
// 写了 b（post != pre 且等于原版对 new_full 的预测写）才回写 a+b；其余一律
// fail-closed 跳过并记日志（同类别不同 payload 拒并等均落入此分支，不猜测）。
// delta%128==0 的合并从 b 段不可见，但源槽消失可作为“原版确实合并”的独立证据；
// 此时回写完整 128a+b，避免 a 段未进位导致源堆消失、目标数量不变。
void s2_merge_writeback(const S2MergeCapture& cap, const char* op,
                        const S2SourceCapture* source = nullptr) {
    if (!cap.armed) return;
    if (s2_physical_slot_item(cap.bag, cap.slot) != cap.target) return;
    if (!s2_writeback_gate(cap.target)) return;
    const uint32_t post_field = read_count_field(cap.target);
    const bool source_found = source != nullptr && source->found;
    const bool source_still_present =
        source_found && s2_physical_slot_item(source->bag, source->slot) == source->item;
    const bool source_removed = qol::contracts::s2_writeback::source_removed_confirms_merge(
        cap.delta, source_found, source_still_present);
    if (post_field == cap.pre_field && !source_removed) {
        if ((cap.delta % 128) == 0) {
            QOL_LOG_DEBUG(QolDomain::kInventory,
                                "S2 writeback %s degenerate skip target=%p delta=%u (delta%%128==0)",
                                op, cap.target, cap.delta);
        }
        return;
    }
    if (post_field != cap.pre_field &&
        !qol::contracts::s2_writeback::original_count_write_confirmed(item_count_api(), cap.pre_field, post_field,
                                                      cap.new_full)) {
        QOL_LOG_ERROR(QolDomain::kInventory,
                            "S2 writeback %s mismatch skip target=%p pre=0x%x post=0x%x expect_full=%u",
                            op, cap.target, cap.pre_field, post_field, cap.new_full);
        return;
    }
    // gate 未通过时不进入本写回；关闭态由原版写 b，模块不回写 a+b。
    const ItemCountWriteResult write_result =
        item_count_write_effective(cap.target, cap.new_full);
    if (write_result == ItemCountWriteResult::kNotReady) return;
    if (write_result == ItemCountWriteResult::kFailed) {
        QOL_LOG_ERROR(QolDomain::kInventory,
                      "S2 writeback %s facade write failed target=%p expect_full=%u",
                      op, cap.target, cap.new_full);
        return;
    }
    const uint32_t written = read_count_field(cap.target);
    QOL_LOG_DEBUG(QolDomain::kInventory,
                        "S2 writeback %s merge target=%p %u+%u->%u field=0x%x",
                        op, cap.target, cap.pre_full, cap.delta, cap.new_full, written);
}

// INVEN_RemoveItemData 快照最多覆盖 item port 声明的原生袋数及其槽位。
constexpr int kS2RemoveDataMaxEntries = qol::contracts::kQolMaxNativeBagCount *
                                        kInvenSlotsPerBag;

struct S2RemoveDataCapture {
    bool armed = false;
    int entry_count = 0;
    void* items[kS2RemoveDataMaxEntries] = {};
    int bags[kS2RemoveDataMaxEntries] = {};
    int slots[kS2RemoveDataMaxEntries] = {};
    uint32_t pre_raw[kS2RemoveDataMaxEntries] = {};
    uint32_t pre_full[kS2RemoveDataMaxEntries] = {};
};

// backup 前快照：启用态 + count>0 时记录全部「count-encoded 且类别匹配」的堆
// （原版写点只改这些堆；纯内存读，不取 g_virtual_bag_mtx，R-44）。类别判据与
// 原版一致：+0x08 的 bit6..15（item_count_encoding 用同一字段）。
void s2_capture_remove_data(int32_t category, int32_t count, S2RemoveDataCapture* cap) {
    cap->armed = false;
    cap->entry_count = 0;
    if (category < 0 || count <= 0 || !item_count_extended_applied()) return;
    for (int bag = 0; bag < physical_bag_limit(); ++bag) {
        for (int slot = 0; slot < kInvenSlotsPerBag; ++slot) {
            void* item = s2_physical_slot_item(bag, slot);
            if (item == nullptr) continue;
            if (!item_count_is_encoded(item)) continue;
            if (g_port == nullptr || g_port->read.category == nullptr) continue;
            qol::contracts::QolItemHandle item_handle{};
            int32_t item_category = -1;
            if (!item_handle_for_hook_item(item, &item_handle) ||
                !qol::contracts::succeeded(g_port->read.category(item_handle, &item_category)) ||
                item_category != category) {
                continue;
            }
            if (cap->entry_count >= kS2RemoveDataMaxEntries) return;  // 不可能：上限即全袋
            cap->items[cap->entry_count] = item;
            cap->bags[cap->entry_count] = bag;
            cap->slots[cap->entry_count] = slot;
            cap->pre_raw[cap->entry_count] = read_count_field(item);
            if (!item_count_effective(item, &cap->pre_full[cap->entry_count])) continue;
            ++cap->entry_count;
        }
    }
    cap->armed = cap->entry_count > 0;
}

// 999 已应用时，backup 后重扫 + 修正：消失堆（整删）只求和；唯一原版 b-only
// 写回形态的堆经 stage4_remove_item_data_plan 计算 remain 并在需要时回写 a+b。
// 关闭态由原版只写 b，模块跳过此修正；失败/矛盾一律 fail-closed。
void s2_remove_data_writeback(const S2RemoveDataCapture& cap, int32_t category,
                              int32_t count) {
    if (!cap.armed || !item_count_extended_applied()) return;
    if (count <= 0) return;
    inventory_stage::RemoveDataEntry pre[kS2RemoveDataMaxEntries] = {};
    inventory_stage::RemoveDataPost post[kS2RemoveDataMaxEntries] = {};
    for (int index = 0; index < cap.entry_count; ++index) {
        pre[index] = {cap.items[index], cap.pre_raw[index], cap.pre_full[index]};
        void* cur = s2_physical_slot_item(cap.bags[index], cap.slots[index]);
        if (cur != cap.items[index]) {
            post[index] = {cur, 0, 0};
        } else {
            const auto* api = item_count_api();
            uint32_t post_raw = 0;
            if (api == nullptr || api->read_encoded_field == nullptr ||
                !qol::contracts::succeeded(api->read_encoded_field(cur, &post_raw))) return;
            uint32_t effective = 0;
            if (!item_count_effective(cur, &effective)) return;
            post[index] = {cur, post_raw, effective};
        }
    }
    const inventory_stage::RemoveDataPlan plan =
        inventory_stage::remove_item_data_plan(item_count_api(), pre, post, cap.entry_count, count);
    if (!plan.correct) return;
    const int index = plan.entry_index;
    void* item = s2_physical_slot_item(cap.bags[index], cap.slots[index]);
    if (item != cap.items[index]) return;  // 二次确认：对象已被释放/移动则不触磁
    const ItemCountWriteResult write_result =
        item_count_write_effective(item, plan.remain);
    if (write_result != ItemCountWriteResult::kApplied) return;
    const uint32_t written = read_count_field(item);
    QOL_LOG_INFO(QolDomain::kInventory,
                        "S2 writeback RemoveItemData category=%d count=%d item=%p "
                        "%u->%u field=0x%x",
                        category, count, item, cap.pre_full[index], plan.remain, written);
}

// FindSaveSlot 与紧随其后的 SaveItem/SaveItemDirect 之间没有参数通道：原生
// SaveItem 只把槽号传给 SaveItemDirect，袋号来自变体私有 scratch 单元
//（G_INVEN_SAVED_BAG_VMA，仅 C18 改版存在，且模块按 32 位写会在只写字节的
// 路径上留下垃圾高位）。注册源收编的目标因此改由模块内 thread-local pending
// 传递，不依赖变体私有单元。
struct PendingSaveDestination {
    void* item = nullptr;
    int32_t space_id = -1;  // kQolBagSpaceOriginal(0)/kQolBagSpaceVariant(1) 或注册源 space(>=2)
    int32_t bag = -1;
    int32_t slot = -1;
};
thread_local PendingSaveDestination g_pending_save_destination;

void remember_pending_save(void* item, int32_t space_id, int32_t bag, int32_t slot) {
    if (item == nullptr || space_id < 0 || bag < 0 || slot < 0) return;
    // 只有 SaveItem 调用栈内的 FindSaveSlot 才需要把目标袋带进 SaveItemDirect；
    // 商店门禁等“先查后买”的调用会在 SaveItem 内部再查一次，提前记录反而会
    // 让未实际写入的 pending 残留到后续写入。
    if (g_save_item_depth <= 0) return;
    g_pending_save_destination = {item, space_id, bag, slot};
}

bool take_pending_save(void* item, int32_t slot, PendingSaveDestination* out) {
    if (out == nullptr) return false;
    const PendingSaveDestination& pending = g_pending_save_destination;
    if (pending.item != item || pending.slot != slot || pending.space_id < 0) return false;
    *out = pending;
    g_pending_save_destination = PendingSaveDestination{};
    return true;
}

bool hook_item_category(void* item, int32_t* out_category) {
    if (item == nullptr || out_category == nullptr) return false;
    const auto* port = g_port;
    if (port == nullptr || port->read.category == nullptr) return false;
    qol::contracts::QolItemHandle handle{};
    if (!item_handle_for_hook_item(item, &handle)) return false;
    int32_t category = -1;
    if (!qol::contracts::succeeded(port->read.category(handle, &category)) || category < 0) {
        return false;
    }
    *out_category = category;
    return true;
}

bool read_native_save_domain(int32_t category, int32_t* out_domain) {
    if (out_domain == nullptr || g_game == nullptr ||
        g_game->abi.minor < qol::contracts::kGameHostSavedBagMinor ||
        g_game->read_only.item.native_save_domain == nullptr) {
        return false;
    }
    int32_t domain = -1;
    if (!qol::contracts::succeeded(
            g_game->read_only.item.native_save_domain(category, &domain)) ||
        domain < 0) {
        return false;
    }
    *out_domain = domain;
    return true;
}

bool build_transfer_for_hook_item(void* item, qol::contracts::QolBagTransfer* out_transfer,
                                  int32_t* out_category, uint32_t* out_payload_size) {
    if (item == nullptr || out_transfer == nullptr) return false;
    const auto* port = g_port;
    if (port == nullptr || !qol::contracts::qol_item_port_bag_space_size_valid(port) ||
        port->read.serialize_item == nullptr || port->read.category == nullptr) {
        return false;
    }
    qol::contracts::QolItemHandle handle{};
    if (!item_handle_for_hook_item(item, &handle)) return false;
    // 类别单一来源是 item port（core.inventory 不依赖 item.facts）。
    int32_t category = -1;
    uint32_t required = 0;
    QolResult result = port->read.serialize_item(handle, nullptr, 0, &required);
    if (!qol::contracts::succeeded(result) || required == 0 ||
        required > qol::contracts::kQolBagTransferPayloadMaxSize) {
        return false;
    }
    int32_t port_category = -1;
    if (!qol::contracts::succeeded(port->read.category(handle, &port_category)) ||
        port_category <= 0) {
        return false;
    }
    category = port_category;
    qol::contracts::QolBagTransfer transfer{};
    uint32_t serialized = 0;
    result = port->read.serialize_item(
        handle, transfer.payload, qol::contracts::kQolBagTransferPayloadCapacity, &serialized);
    if (!qol::contracts::succeeded(result) || serialized != required) return false;
    transfer.category = category;
    int32_t count = 1;
    if (port->read.cumulate_count != nullptr) {
        int32_t observed = 0;
        if (qol::contracts::succeeded(port->read.cumulate_count(handle, &observed)) &&
            observed > 0) {
            count = observed;
        }
    }
    transfer.count = count;
    transfer.payload_size = serialized;
    *out_transfer = transfer;
    if (out_category != nullptr) *out_category = category;
    if (out_payload_size != nullptr) *out_payload_size = serialized;
    return true;
}

bool hook_item_stackable(void* item) {
    if (item == nullptr || g_port == nullptr || g_port->read.count_encoding == nullptr) {
        return false;
    }
    qol::contracts::QolItemHandle handle{};
    if (!item_handle_for_hook_item(item, &handle)) return false;
    qol::contracts::QolItemCountEncoding encoding =
        qol::contracts::QolItemCountEncoding::kUnknown;
    if (!qol::contracts::succeeded(g_port->read.count_encoding(handle, &encoding))) {
        return false;
    }
    return encoding == qol::contracts::QolItemCountEncoding::kEncoded;
}

const qol::contracts::QolBagSource* registered_source_for_space(int32_t space_id) {
    for (uint32_t index = 0; index < registered_bag_source_count(); ++index) {
        const auto* source = registered_bag_source_at(index);
        if (source != nullptr && source->space_id == space_id) return source;
    }
    return nullptr;
}

bool decode_flat_bag(int32_t flat_bag, int32_t* out_space_id, int32_t* out_bag) {
    return qol::contracts::qol_bag_flat_decode(flat_bag, out_space_id, out_bag);
}

struct ExtensionCountContext {
    int32_t category;
    int64_t count;
};

int32_t count_source_item(const qol::contracts::QolInventoryItemRef* item, void* raw) {
    auto* context = static_cast<ExtensionCountContext*>(raw);
    if (item != nullptr && item->category == context->category && item->count > 0) {
        context->count += item->count;
    }
    return 0;
}

int registered_source_item_count(int32_t category) {
    ExtensionCountContext context{category, 0};
    for (uint32_t index = 0; index < registered_bag_source_count(); ++index) {
        const auto* source = registered_bag_source_at(index);
        if (source != nullptr && source->for_each_item != nullptr) {
            (void)source->for_each_item(&count_source_item, &context);
        }
    }
    return context.count > INT32_MAX ? INT32_MAX : static_cast<int>(context.count);
}

// storage 查询/消耗/删除分流统一走 contracts::inventory_stage 纯函数；
// registered_source_item_count 与 inventory_stage::CountExtension 签名一致，
// 直接作为 extension_count 回调传入，递归护栏沿用本插件 TLS 标志。

int64_t count_empty_slots_all(int32_t needed) {
    int64_t empty = 0;
    if (g_port != nullptr && qol::contracts::qol_item_port_bag_space_size_valid(g_port) &&
        g_port->read.bag_count != nullptr && g_port->read.bag_info != nullptr &&
        g_port->read.item_at != nullptr) {
        for (int32_t space = qol::contracts::kQolBagSpaceOriginal;
             space <= qol::contracts::kQolBagSpaceVariant; ++space) {
            int32_t bag_count = 0;
            if (!qol::contracts::succeeded(g_port->read.bag_count(space, &bag_count)) ||
                bag_count < 0) {
                continue;
            }
            for (int32_t bag = 0; bag < bag_count; ++bag) {
                if (space == qol::contracts::kQolBagSpaceOriginal &&
                    bag == qol::contracts::kQolTaskBagIndex) {
                    continue;
                }
                qol::contracts::QolBagRef ref{space, bag};
                qol::contracts::QolBagInfo info{};
                if (!qol::contracts::succeeded(g_port->read.bag_info(&ref, &info)) ||
                    info.capacity < 0) {
                    continue;
                }
                for (int32_t slot = 0; slot < info.capacity; ++slot) {
                    qol::contracts::QolInventoryItemRef item{};
                    const QolResult result = g_port->read.item_at(&ref, slot, &item);
                    if (result.code == kQolErrNotFound) ++empty;
                    if (empty >= needed) return empty;
                }
            }
        }
    }
    for (uint32_t index = 0; index < registered_bag_source_count(); ++index) {
        const auto* source = registered_bag_source_at(index);
        if (source == nullptr || source->count == nullptr || source->info == nullptr ||
            source->item_at == nullptr) {
            continue;
        }
        const int32_t count = source->count();
        if (count < 0) continue;
        for (int32_t bag = 0; bag < count; ++bag) {
            qol::contracts::QolBagInfo info{};
            if (!qol::contracts::succeeded(source->info(bag, &info)) || info.capacity < 0) {
                continue;
            }
            for (int32_t slot = 0; slot < info.capacity; ++slot) {
                qol::contracts::QolInventoryItemRef item{};
                const QolResult result = source->item_at(bag, slot, &item);
                if (result.code == kQolErrNotFound) ++empty;
                if (empty >= needed) return empty;
            }
        }
    }
    return empty;
}

// 注册源收编的落袋动作：flat 袋号路径与模块 pending 路径共用，保证校验、转移
// 构造与失败计数语义一致。
bool import_registered_item_direct(void* item, int32_t space_id, int32_t bag, int32_t slot,
                                   const char* origin) {
    const auto* source = registered_source_for_space(space_id);
    if (source == nullptr || source->info == nullptr || source->import_item == nullptr) {
        return false;
    }
    qol::contracts::QolBagInfo info{};
    if (!qol::contracts::succeeded(source->info(bag, &info)) || slot < 0 ||
        slot >= info.capacity) {
        return false;
    }
    qol::contracts::QolBagTransfer transfer{};
    if (!build_transfer_for_hook_item(item, &transfer, nullptr, nullptr)) return false;
    qol::contracts::QolBagMutationResult imported{};
    const QolResult result = source->import_item(&transfer, bag, slot, &imported);
    if (!qol::contracts::succeeded(result)) {
        ++g_save_item_direct_fail_count;
        QOL_LOG_ERROR(QolDomain::kInventory,
                      "SaveItemDirect eb failed origin=%s item=%p space=%d bag=%d slot=%d code=%d",
                      origin, item, space_id, bag, slot, static_cast<int32_t>(result.code));
        return false;
    }
    g_pending_eb_items.push_back(item);
    core_inventory_log_write(0, __FILE__, __LINE__,
                        "SaveItemDirect eb imported origin=%s item=%p space=%d bag=%d slot=%d",
                        origin, item, space_id, bag, slot);
    return true;
}

void* find_item_wrapper(int32_t category) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight, {"hook:INVEN_FindItem"});
    if (g_game == nullptr || g_game->item_abi.find_item_wrapper == nullptr) {
        QOL_LOG_WARN(QolDomain::kInventory,
                     "INVEN_FindItem Game Host dispatch unavailable category=%d", category);
        return nullptr;
    }
    return g_game->item_abi.find_item_wrapper(category);
}

int have_item_wrapper(int32_t category) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight, {"hook:INVEN_HaveItem"});
    if (!support_enabled()) {
        return call_backup("hook:INVEN_HaveItem", g_backup_have_item, category);
    }
    if (g_backup_have_item == nullptr) {
        backup_missing("hook:INVEN_HaveItem");
        return 0;
    }
    return inventory_stage::have_item(category, g_backup_have_item,
                                      registered_source_item_count, g_in_have_item);
}

int get_item_count_wrapper(int32_t category) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight, {"hook:INVEN_GetItemCount"});
    if (!support_enabled()) {
        return call_backup("hook:INVEN_GetItemCount", g_backup_get_item_count, category);
    }
    if (g_backup_get_item_count == nullptr) {
        backup_missing("hook:INVEN_GetItemCount");
        return 0;
    }
    return inventory_stage::get_item_count(category, g_backup_get_item_count,
                                           registered_source_item_count,
                                           g_in_get_item_count);
}

int get_cumulate_count_wrapper(void* item) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight, {"hook:ITEM_GetCumulateCount"});
    if (!support_enabled()) {
        return call_backup("hook:ITEM_GetCumulateCount", g_backup_get_cumulate_count, item);
    }
    if (g_backup_get_cumulate_count == nullptr) {
        backup_missing("hook:ITEM_GetCumulateCount");
        return 0;
    }
    const auto* item_count = g_game == nullptr ? nullptr : &g_game->item_count;
    return inventory_stage::get_cumulate_count(item, item_count,
                                               g_backup_get_cumulate_count);
}

int is_having_empty_slot_wrapper(int32_t needed, int32_t include_task_bag) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight, {"hook:INVEN_IsHavingEmptySlot"});
    if (!support_enabled()) {
        return call_backup("hook:INVEN_IsHavingEmptySlot", g_backup_is_having_empty_slot,
                           needed, include_task_bag);
    }
    if (g_backup_is_having_empty_slot == nullptr) {
        backup_missing("hook:INVEN_IsHavingEmptySlot");
        return 0;
    }
    if (needed <= 0) return 1;
    if (include_task_bag != 0) {
        return call_backup("hook:INVEN_IsHavingEmptySlot", g_backup_is_having_empty_slot,
                           needed, include_task_bag);
    }
    if (g_port == nullptr || !qol::contracts::qol_item_port_bag_space_size_valid(g_port) ||
        g_port->read.bag_count == nullptr || g_port->read.bag_info == nullptr ||
        g_port->read.item_at == nullptr) {
        return call_backup("hook:INVEN_IsHavingEmptySlot", g_backup_is_having_empty_slot,
                           needed, include_task_bag);
    }
    return count_empty_slots_all(needed) >= needed ? 1 : 0;
}

void consume_item_wrapper(void* item) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:INVEN_ConsumeItem"});
    if (!support_enabled()) {
        call_backup("hook:INVEN_ConsumeItem", g_backup_consume_item, item);
        return;
    }
    const uint64_t call = g_consume_item_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    qol::contracts::QolInventoryItemRef source_ref{};
    const bool extension_item = source_for_hook_item(item, &source_ref) != nullptr;
    const int extension_bag = extension_item ? source_ref.bag : -1;
    const int extension_slot = extension_item ? source_ref.slot : -1;
    if (core_inventory_log_debug_enabled()) {
        core_inventory_log_write(0, __FILE__, __LINE__,
                            "INVEN_ConsumeItem call=%llu item=%p extension=%d bag=%d slot=%d "
                            "recursive=%d backup=%p",
                            static_cast<unsigned long long>(call), item, extension_item ? 1 : 0,
                            extension_bag, extension_slot, g_in_consume_item ? 1 : 0,
                            reinterpret_cast<void*>(g_backup_consume_item));
    }
    if (!extension_item && g_backup_consume_item == nullptr) {
        backup_missing("hook:INVEN_ConsumeItem");
        return;
    }
    // S2 写侧借位（减语义；R-45/R-46/R-49，仅启用态）：原版读 b 判 count>1——
    // b≤1 且全量≥2（a≥1）时原版落入整堆删除分支，a 段随对象一起丢失。处置：
    // backup 前把 b 预置为 2（a 与低位原样），原版按 b=2>1 走 b-1 递减写 b=1；
    // 返回后确认预置生效再回写 a+b=full-1，失败则还原预置字段。b≥2 时原版递减
    // 天然正确不写；全量≤1 时删除即原版语义，不拦。
    // 关闭态与 NotReady 不进入 S2 写侧；原版按 b 执行消耗/删除，模块不预置或回写 a+b。
    bool s2_borrow_armed = false;
    int borrow_bag = -1;
    int borrow_slot = -1;
    uint32_t borrow_pre_field = 0;
    uint32_t borrow_pre_full = 0;
    if (!extension_item && g_backup_consume_item != nullptr &&
        item_count_feature_enabled(item) &&
        s2_writeback_gate(item) &&
        s2_find_physical_slot(item, &borrow_bag, &borrow_slot)) {
        borrow_pre_field = read_count_field(item);
        uint32_t borrow_view = 0;
        const qol::contracts::QolItemCountApi* api = item_count_api();
        if (item_count_effective(item, &borrow_pre_full) &&
            api != nullptr && api->view_from_canonical != nullptr &&
            qol::contracts::succeeded(api->view_from_canonical(borrow_pre_full, 0,
                                                                &borrow_view)) &&
            borrow_pre_full >= 2 && borrow_view <= 1) {
            uint32_t preset_field = 0;
            if (api->field_from_canonical != nullptr &&
                qol::contracts::succeeded(api->field_from_canonical(borrow_pre_field, 2, 0,
                                                                      &preset_field))) {
                s2_write_count_field(item, preset_field);
                s2_borrow_armed = true;
            }
        }
    }
    const bool dispatched = inventory_stage::consume_item(
        item, identify_registered_source_item, consume_registered_source_item,
        g_backup_consume_item, g_in_consume_item);
    if (s2_borrow_armed && !extension_item) {
        if (s2_physical_slot_item(borrow_bag, borrow_slot) != item) {
            invalidate_item_handle(item);
            // 原版仍删除了整堆（与 0x104818 的 b>1 递减分支反汇编结论矛盾）：
            // 对象已释放，禁止触磁字段，只记 ERROR 供取证。
            QOL_LOG_ERROR(QolDomain::kInventory,
                                "S2 writeback ConsumeItem borrow item removed unexpectedly "
                                "item=%p bag=%d slot=%d pre_full=%u",
                                item, borrow_bag, borrow_slot, borrow_pre_full);
        } else if (s2_writeback_gate(item)) {
            const uint32_t post_field = read_count_field(item);
            if (qol::contracts::s2_writeback::original_count_write_confirmed(item_count_api(), borrow_pre_field,
                                                             post_field, 1)) {
                const uint32_t new_full = qol::contracts::s2_writeback::decremented_count(borrow_pre_full);
                const ItemCountWriteResult write_result =
                    item_count_write_effective(item, new_full);
                if (write_result == ItemCountWriteResult::kNotReady) {
                    s2_write_count_field(item, borrow_pre_field);
                } else if (write_result == ItemCountWriteResult::kApplied) {
                    QOL_LOG_INFO(QolDomain::kInventory,
                                 "S2 writeback ConsumeItem borrow item=%p %u->%u",
                                 item, borrow_pre_full, new_full);
                } else {
                    s2_write_count_field(item, borrow_pre_field);
                }
            } else {
                // 原版未按预期递减：还原预置字段，保持进 hook 前原样。
                s2_write_count_field(item, borrow_pre_field);
                QOL_LOG_ERROR(QolDomain::kInventory,
                                    "S2 writeback ConsumeItem borrow mismatch item=%p "
                                    "pre=0x%x post=0x%x restored",
                                    item, borrow_pre_field, post_field);
            }
        }
    }
    if (!dispatched && extension_item) {
        QOL_LOG_ERROR(QolDomain::kInventory,
                            "ConsumeItem extension consume failed bag=%d slot=%d item=%p",
                            extension_bag, extension_slot, item);
    }
    if (core_inventory_log_debug_enabled()) {
        core_inventory_log_write(0, __FILE__, __LINE__,
                            "INVEN_ConsumeItem call=%llu dispatched=%d extension=%d bag=%d "
                            "slot=%d s2_borrow=%d",
                            static_cast<unsigned long long>(call), dispatched ? 1 : 0,
                            extension_item ? 1 : 0, extension_bag, extension_slot,
                            s2_borrow_armed ? 1 : 0);
    }
    return;
}

int remove_item_wrapper(void* item) {
    const RemoveItemIdentityGuard identity_guard(item);
    if (!identity_guard.entered()) return 0;
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:INVEN_RemoveItem"});
    const bool trace_enabled = core_inventory_log_debug_enabled();
    const uint64_t trace_call = trace_enabled
                                    ? g_remove_item_trace_calls.fetch_add(
                                          1, std::memory_order_relaxed) + 1
                                    : 0;
    const bool recursive = g_in_remove_item;
    const auto log_remove_call = [&](const char* dispatch, const char* branch, int result) {
        if (!trace_enabled) return;
        core_inventory_log_write(0, __FILE__, __LINE__,
                            "INVEN_RemoveItem call=%llu item=%p recursive=%d backup=%p "
                            "dispatch=%s branch=%s result=%d",
                            static_cast<unsigned long long>(trace_call),
                            item, recursive ? 1 : 0,
                            reinterpret_cast<void*>(g_backup_remove_item), dispatch, branch,
                            result);
    };
    if (!support_enabled()) {
        const int result = call_backup("hook:INVEN_RemoveItem", g_backup_remove_item, item);
        log_remove_call("native", "support_disabled", result);
        return result;
    }
    const uint64_t call = g_remove_item_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    QOL_LOG_DEBUG_EVERY(QolDomain::kInventory, 60,
                            "RemoveItem every_calls=60 call=%llu item=%p recursive=%d backup=%p",
                        static_cast<unsigned long long>(call), item,
                        g_in_remove_item ? 1 : 0,
                        reinterpret_cast<void*>(g_backup_remove_item));
    if (g_backup_remove_item == nullptr) {
        backup_missing("hook:INVEN_RemoveItem");
        log_remove_call("native", "backup_missing", 0);
        return 0;
    }
    qol::contracts::QolInventoryItemRef source_ref{};
    const auto* source = source_for_hook_item(item, &source_ref);
    if (source != nullptr) {
        if (source->remove == nullptr || source_ref.count <= 0) {
            log_remove_call("source", "invalid_source_item", 0);
            return 0;
        }
        qol::contracts::QolBagMutationResult mutation{};
        const int result = qol::contracts::succeeded(
                               source->remove(source_ref.item, source_ref.count, &mutation))
                               ? 1
                               : 0;
        log_remove_call("source", "remove", result);
        return result;
    }
    const int result =
        inventory_stage::remove_item(item, nullptr, nullptr, g_backup_remove_item, g_in_remove_item);
    log_remove_call("native", "stage4", result);
    return result;
}

int find_save_slot_wrapper(void* item, int8_t* out_slot) {
    g_pending_save_destination = PendingSaveDestination{};
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:INVEN_FindSaveSlot"});
    if (!support_enabled())
        return call_backup("hook:INVEN_FindSaveSlot", g_backup_find_save_slot, item, out_slot);
    if (g_backup_find_save_slot == nullptr) {
        backup_missing("hook:INVEN_FindSaveSlot");
        return 0;
    }
    // 门禁：unequip 栈内与 domain 不可知/非托管域保持原生选择路径；模块只托管
    // domain 0（原版袋 0..4 + C18 追加袋）的入库。
    if (qol::contracts::g_inventory_unequip_depth > 0)
        return call_backup("hook:INVEN_FindSaveSlot", g_backup_find_save_slot, item, out_slot);
    int32_t category = -1;
    int32_t domain = -1;
    if (!hook_item_category(item, &category) || !read_native_save_domain(category, &domain) ||
        domain != 0) {
        return call_backup("hook:INVEN_FindSaveSlot", g_backup_find_save_slot, item, out_slot);
    }
    // 托管域不再调原生 FindSaveSlot：原生判据只有 category 相同，会把 payload
    // 身份不同（词条/强化不同）的堆错误合并；统一由 placement resolver 校验
    // 身份后给出目标，不回退原生（避免 category-only 错误合并）。
    qol::contracts::QolBagTransfer transfer{};
    if (!build_transfer_for_hook_item(item, &transfer, &category, nullptr)) return 0;
    // count_encoding 读面不可用时，count>1 已足以证明这是堆叠物；不能因此跳过
    // 已有堆，否则会退回“只要有空槽就新建堆”的旧行为。
    const bool stackable = hook_item_stackable(item) || transfer.count > 1;
    placement::PlacementTarget target{};
    if (!placement::resolve_placement(transfer, stackable, -1, -1, &target)) return 0;
    // C18 追加袋号（原生扁平 6..）写不进 SaveItem 的 5 位槽字节，且各变体的
    // SaveItem 是否读 scratch 单元不一致；space1/space>=2 统一由模块 pending
    // 带到 SaveItemDirect，space0 直接记录原生袋/槽。
    if (target.space_id == qol::contracts::kQolBagSpaceVariant) {
        remember_pending_save(item, qol::contracts::kQolBagSpaceVariant,
                              qol::contracts::kQolOriginalBagCount + target.bag, target.slot);
    } else {
        remember_pending_save(item, target.space_id, target.bag, target.slot);
    }
    if (out_slot != nullptr) *out_slot = static_cast<int8_t>(target.slot);
    core_inventory_log_write(0, __FILE__, __LINE__,
                        "FindSaveSlot resolved category=%d space=%d bag=%d slot=%d merge=%d",
                        category, target.space_id, target.bag, target.slot,
                        target.merge ? 1 : 0);
    return 1;
}

int save_item_wrapper(void* item) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:INVEN_SaveItem"});
    if (!support_enabled())
        return call_backup("hook:INVEN_SaveItem", g_backup_save_item, item);
    if (g_backup_save_item == nullptr) {
        backup_missing("hook:INVEN_SaveItem");
        return 0;
    }
    const size_t pending_base = g_pending_eb_items.size();
    const int fail_base = g_save_item_direct_fail_count;
    ++g_save_item_depth;
    const int result = g_backup_save_item(item);
    --g_save_item_depth;
    bool direct_imported = false;
    while (g_pending_eb_items.size() > pending_base) {
        void* imported_item = g_pending_eb_items.back();
        g_pending_eb_items.pop_back();
        const auto* port = g_port;
        if (port != nullptr && port->read.free_item != nullptr) {
            (void)port->read.free_item(imported_item);
        }
        direct_imported = true;
    }
    // 原生 SaveItem 已返回，pending 只服务这一次调用；残留会污染后续 SaveItemDirect。
    g_pending_save_destination = PendingSaveDestination{};
    if (direct_imported) {
        core_inventory_log_write(0, __FILE__, __LINE__,
                            "SaveItem direct_eb item=%p backup=%p", item,
                            reinterpret_cast<void*>(g_backup_save_item));
        return 1;
    }
    const bool direct_failed = g_save_item_direct_fail_count > fail_base;
    if (direct_failed) {
        core_inventory_log_write(0, __FILE__, __LINE__,
                            "SaveItem direct_eb_failed item=%p", item);
    }
    if (result != 0 && !direct_failed) return result;
    int32_t category = -1;
    const bool have_category = hook_item_category(item, &category);
    QOL_LOG_ERROR(QolDomain::kInventory,
                  "INVEN_SaveItem failed category=%d direct_failed=%d backup=%p",
                  have_category ? category : -1, direct_failed ? 1 : 0,
                  reinterpret_cast<void*>(g_backup_save_item));
    return 0;
}

int save_item_direct_wrapper(void* item, int32_t bag, int32_t slot) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:INVEN_SaveItemDirect"});
    if (!support_enabled())
        return call_backup("hook:INVEN_SaveItemDirect", g_backup_save_item_direct,
                           item, bag, slot);
    // pending 目标必须早于 flat 分支与原版 backup 消费：C18 改版的 SaveItem 会用
    // 变体私有 scratch 里的袋号调用本函数，该值可能来自更早一次写操作，直接放行会
    // 把物品写进错误槽位。
    if (g_save_item_depth > 0) {
        PendingSaveDestination pending{};
        if (take_pending_save(item, slot, &pending)) {
            // space0：resolver 选中的原生袋/槽直写（含原生同身份合并语义）。
            // space1：pending.bag 已是原生扁平袋号（kQolOriginalBagCount + bag），
            // 与既有 variant pending 兼容，同样走原生 backup 直写。
            if (pending.space_id == qol::contracts::kQolBagSpaceOriginal ||
                pending.space_id == qol::contracts::kQolBagSpaceVariant) {
                return call_backup("hook:INVEN_SaveItemDirect", g_backup_save_item_direct, item,
                                   pending.bag, pending.slot);
            }
            if (pending.space_id >= qol::contracts::kQolBagSpaceFirstRegistered) {
                return import_registered_item_direct(item, pending.space_id, pending.bag,
                                                     pending.slot, "pending")
                           ? 1
                           : 0;
            }
        }
    }
    if (bag >= qol::contracts::kQolBagFlatRegisteredBase) {
        if (g_save_item_depth <= 0) {
            core_inventory_log_write(0, __FILE__, __LINE__,
                                "SaveItemDirect eb rejected item=%p bag=%d slot=%d",
                                item, bag, slot);
            return 0;
        }
        int32_t space_id = -1;
        int32_t local_bag = -1;
        if (!decode_flat_bag(bag, &space_id, &local_bag)) return 0;
        return import_registered_item_direct(item, space_id, local_bag, slot, "flat") ? 1 : 0;
    }
    // INVEN_SaveItemDirect（加语义）：空槽写入不需要回写——对象创建侧数量由
    // MakeItem wrapper / 模块 s2 写点保证 a+b 完整；同身份堆叠合并时原版只写 b
    //（+0xe8/+0x110/+0x168 三处 SetBitValue 写点），合并可能 ITEMPOOL_Free(item)，
    // 因此入参对象全量必须在 backup 前读取。原版返回值不可信（决策册 §2.2），
    // 成败判定改用「目标槽指针未替换 + 字段等于原版预测 b 写」。
    uint32_t source_count = 0;
    if (item != nullptr) item_count_effective(item, &source_count);
    S2MergeCapture merge{};
    s2_capture_merge(item, bag, slot, static_cast<int32_t>(source_count), &merge);
    const int result = call_backup("hook:INVEN_SaveItemDirect", g_backup_save_item_direct,
                                   item, bag, slot);
    s2_merge_writeback(merge, "SaveItemDirect");
    return result;
}

int remove_item_data_wrapper(int32_t category, int32_t count) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:INVEN_RemoveItemData"});
    if (!support_enabled())
        return call_backup("hook:INVEN_RemoveItemData", g_backup_remove_item_data,
                           category, count);
    // INVEN_RemoveItemData（减语义，批量按类别删除；反汇编 0x1040a8）：顺序遍历
    // 6 袋，匹配堆整删（INVEN_RemoveItemDirect）累计 w22，最后一堆部分删除时
    // `SetBitValue(+0x10, 31, 25, cum + w22 - count)`。cum 经 H-17 getter（启用态
    // = S2 全量），算术正确但 b 写对 remain>127 或旧 a 残留丢进位 → original-first
    // 快照/重扫修正。调用方：MIXSYSTEM_UseStuff（合成扣料）、QUESTSYSTEM
    // （任务物品回收）、UIMix/ProcessUnpack/SaveItemData 回滚、NetworkStore。
    // count<=0 语义未冻结（-1 分支存在），不快照不修正；扩展袋对象不参与物理袋
    // 快照，原版只删原版堆。
    //
    const bool extension_fallback = count > 0 && category > 0 &&
                                    registered_bag_source_count() > 0;
    const int total_before =
        extension_fallback ? get_item_count_wrapper(category) : 0;
    S2RemoveDataCapture capture{};
    if (g_backup_remove_item_data != nullptr) {
        s2_capture_remove_data(category, count, &capture);
    }
    const int result = call_backup("hook:INVEN_RemoveItemData", g_backup_remove_item_data,
                                   category, count);
    s2_remove_data_writeback(capture, category, count);
    if (extension_fallback) {
        const int total_after = get_item_count_wrapper(category);
        const int shortfall =
            inventory_stage::remove_data_extension_shortfall(total_before, total_after, count);
        if (shortfall > 0) {
            const int consumed = remove_registered_source_category(category, shortfall);
            if (consumed < shortfall) {
                QOL_LOG_INFO(QolDomain::kInventory,
                                    "RemoveItemData registered-source fallback category=%d "
                                    "need=%d consumed=%d",
                                    category, shortfall, consumed);
            }
        }
    }
    return result;
}

int move_item_wrapper(void* item, int count, int target_bag, int target_slot) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:INVEN_MoveItem"});
    if (!support_enabled())
        return call_backup("hook:INVEN_MoveItem", g_backup_move_item,
                           item, count, target_bag, target_slot);
    qol::contracts::QolInventoryItemRef source_item{};
    if (source_for_hook_item(item, &source_item) != nullptr) {
        QOL_LOG_WARN(QolDomain::kInventory,
                     "MoveItem rejected registered-source item bag=%d slot=%d",
                     source_item.bag, source_item.slot);
        return 0;
    }

    // S2 写侧合并进位（加语义；R-45/R-46/R-49）：backup 前采集目标槽旧全量，
    // backup 后确认原版确实写了 b（post != pre 且等于原版预测写）再回写 a+b。
    S2MergeCapture move_merge{};
    S2SourceCapture move_source{};
    if (g_backup_move_item != nullptr) {
        s2_capture_merge(item, target_bag, target_slot, count, &move_merge);
        if (move_merge.armed && (move_merge.delta % 128) == 0) {
            s2_capture_source(item, &move_source);
        }
    }
    const int result = call_backup("hook:INVEN_MoveItem", g_backup_move_item,
                                   item, count, target_bag, target_slot);
    s2_merge_writeback(move_merge, "MoveItem", &move_source);
    QOL_LOG_DEBUG_EVERY(QolDomain::kInventory, 60,
                        "MoveItem every_calls=60 passthrough item=%p count=%d target=%d/%d "
                        "result=%d",
                        item, count, target_bag, target_slot, result);
    return result;
}

#define STORAGE_HOOK_CLAIM(id, symbol, handler)                                           \
    {id, symbol, qol::contracts::ResourceMechanism::kEntryHook,                           \
     qol::contracts::ResourcePolicy::kPrivate,                                            \
     qol::contracts::ResourceRequirement::kRequired,                                      \
     qol::contracts::ResourceFailure::kFailClosed, true, kClaimOwner, "storage hook",      \
     nullptr, 120, reinterpret_cast<void*>(&handler)}

const qol::contracts::ResourceClaim kClaims[] = {
    STORAGE_HOOK_CLAIM("hook:INVEN_FindItem", "F_FIND_ITEM_VMA", find_item_wrapper),
    STORAGE_HOOK_CLAIM("hook:INVEN_HaveItem", "F_INVEN_HAVE_ITEM_VMA", have_item_wrapper),
    STORAGE_HOOK_CLAIM("hook:INVEN_GetItemCount", "F_INVEN_GET_ITEM_COUNT_VMA",
                       get_item_count_wrapper),
    STORAGE_HOOK_CLAIM("hook:ITEM_GetCumulateCount", "F_GET_CUMULATE_COUNT_VMA",
                       get_cumulate_count_wrapper),
    STORAGE_HOOK_CLAIM("hook:INVEN_IsHavingEmptySlot", "F_INVEN_IS_HAVING_EMPTY_SLOT_VMA",
                       is_having_empty_slot_wrapper),
    STORAGE_HOOK_CLAIM("hook:INVEN_ConsumeItem", "F_CONSUME_ITEM_VMA", consume_item_wrapper),
    STORAGE_HOOK_CLAIM("hook:INVEN_RemoveItem", "F_REMOVE_ITEM_VMA", remove_item_wrapper),
    STORAGE_HOOK_CLAIM("hook:INVEN_SaveItem", "F_INVEN_SAVE_ITEM_VMA", save_item_wrapper),
    STORAGE_HOOK_CLAIM("hook:INVEN_FindSaveSlot", "F_INVEN_FIND_SAVE_SLOT_VMA",
                       find_save_slot_wrapper),
    STORAGE_HOOK_CLAIM("hook:INVEN_SaveItemDirect", "F_INVEN_SAVE_ITEM_DIRECT_VMA",
                       save_item_direct_wrapper),
    STORAGE_HOOK_CLAIM("hook:INVEN_RemoveItemData", "F_INVEN_REMOVE_ITEM_DATA_VMA",
                       remove_item_data_wrapper),
    STORAGE_HOOK_CLAIM("hook:INVEN_MoveItem", "F_INVEN_MOVE_ITEM_VMA", move_item_wrapper),
};

#undef STORAGE_HOOK_CLAIM

void** backup_slot_for_handler(void* handler) {
    if (handler == reinterpret_cast<void*>(&have_item_wrapper)) {
        return reinterpret_cast<void**>(&g_backup_have_item);
    }
    if (handler == reinterpret_cast<void*>(&get_item_count_wrapper)) {
        return reinterpret_cast<void**>(&g_backup_get_item_count);
    }
    if (handler == reinterpret_cast<void*>(&get_cumulate_count_wrapper)) {
        return reinterpret_cast<void**>(&g_backup_get_cumulate_count);
    }
    if (handler == reinterpret_cast<void*>(&is_having_empty_slot_wrapper)) {
        return reinterpret_cast<void**>(&g_backup_is_having_empty_slot);
    }
    if (handler == reinterpret_cast<void*>(&consume_item_wrapper)) {
        return reinterpret_cast<void**>(&g_backup_consume_item);
    }
    if (handler == reinterpret_cast<void*>(&remove_item_wrapper)) {
        return reinterpret_cast<void**>(&g_backup_remove_item);
    }
    if (handler == reinterpret_cast<void*>(&save_item_wrapper)) {
        return reinterpret_cast<void**>(&g_backup_save_item);
    }
    if (handler == reinterpret_cast<void*>(&find_save_slot_wrapper)) {
        return reinterpret_cast<void**>(&g_backup_find_save_slot);
    }
    if (handler == reinterpret_cast<void*>(&save_item_direct_wrapper)) {
        return reinterpret_cast<void**>(&g_backup_save_item_direct);
    }
    if (handler == reinterpret_cast<void*>(&remove_item_data_wrapper)) {
        return reinterpret_cast<void**>(&g_backup_remove_item_data);
    }
    if (handler == reinterpret_cast<void*>(&move_item_wrapper)) {
        return reinterpret_cast<void**>(&g_backup_move_item);
    }
    return nullptr;
}

}  // namespace

bool attach(const qol::contracts::QolHostSdk* sdk, const qol::contracts::QolGameHost* game,
            const qol::contracts::QolItemPort* port) {
    if (sdk == nullptr || game == nullptr || port == nullptr) return false;
    g_sdk = sdk;
    g_game = game;
    g_port = port;
    core_inventory_log_bind_sdk(sdk);
    if (g_game->item_abi.bind_find_item_backup == nullptr) return false;
    QOL_LOG_INFO(QolDomain::kInventory, "storage hooks attached hooks=12");
    return true;
}

void detach() {
    g_sdk = nullptr;
    g_game = nullptr;
    g_port = nullptr;
    g_backup_have_item = nullptr;
    g_backup_get_item_count = nullptr;
    g_backup_get_cumulate_count = nullptr;
    g_backup_consume_item = nullptr;
    g_backup_remove_item = nullptr;
    g_backup_save_item = nullptr;
    g_backup_move_item = nullptr;
    g_backup_is_having_empty_slot = nullptr;
    g_backup_save_item_direct = nullptr;
    g_backup_find_save_slot = nullptr;
    g_backup_remove_item_data = nullptr;
    core_inventory_log_bind_sdk(nullptr);
}

const qol::contracts::ResourceClaim* claims(uint32_t* out_count) {
    if (out_count != nullptr) *out_count = sizeof(kClaims) / sizeof(kClaims[0]);
    return kClaims;
}

int32_t bind_backup(const char* resource_id, void* backup) {
    if (resource_id == nullptr || backup == nullptr) return 1;
    for (const auto& claim : kClaims) {
        if (std::strcmp(claim.id, resource_id) != 0) continue;
        if (std::strcmp(claim.id, "hook:INVEN_FindItem") == 0) {
            if (g_game == nullptr || g_game->item_abi.bind_find_item_backup == nullptr) return 1;
            const auto result = g_game->item_abi.bind_find_item_backup(
                reinterpret_cast<qol::contracts::QolFindItemAbiFn>(backup));
            return qol::contracts::succeeded(result) ? 0 : 1;
        }
        void** slot = backup_slot_for_handler(claim.handler);
        if (slot == nullptr) return 1;
        *slot = backup;
        return 0;
    }
    QOL_LOG_ERROR(QolDomain::kInventory, "unknown storage resource id=%s", resource_id);
    return 1;
}

void set_support(bool enabled) { g_support.store(enabled, std::memory_order_release); }

bool support_enabled() { return g_support.load(std::memory_order_acquire); }

}  // namespace qol::features::core_inventory::hooks

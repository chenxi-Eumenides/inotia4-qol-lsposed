#include "inventory_hooks.h"
#include "../api/projection.h"
#include "../usecases/consume_item.h"
#include "../usecases/item_action.h"
#include "../bags/native_bag_limit.h"
#include "../runtime/context.h"
#include "claims.h"
#include "inventory_logging.h"

#include "contracts/plugin.h"
#include "contracts/bag.h"
#include "contracts/host_sdk.h"
#include "contracts/inventory_refresh.h"
#include "contracts/inventory_scope.h"
#include "contracts/ui.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

namespace qol::features::core_item {
const contracts::QolBagSource* find_bag_source(int32_t space_id);
uint32_t registered_bag_source_count();
const contracts::QolBagSource* registered_bag_source_at(uint32_t index);
extern const contracts::QolItemPort* g_port;
}

namespace {

using EquipItemFromInvenToSlotFn = int (*)(void*, int32_t, int32_t, int32_t);
using PutJewelFn = int (*)(void*, void*);
using UnequipFn = int (*)(void*, int32_t);
using ButtonDestroyExeFn = void (*)(void*);
using UiEquipButtonUseMercenarySealExeFn = void (*)(void*);
using UiEquipOkDestroyItemFn = uint64_t (*)();
using CharUseItemExFn = int (*)(void*, void*, int);
using CharProcessShortcutFn = int (*)(void*, int);
using ShortcutGetStateFn = int (*)(void*, void*);
using ItemSystemDivideFn = void* (*)(void*, int32_t);
using MakeItemFn = void* (*)(int32_t, int32_t, int32_t);

const qol::contracts::QolHostSdk* g_sdk = nullptr;
const qol::contracts::QolGameHost* g_game = nullptr;
const qol::contracts::ItemFactsApi* g_item_facts = nullptr;
std::atomic<bool> g_inventory_support{true};

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
    for (uint32_t index = 0;
         index < qol::features::core_item::registered_bag_source_count(); ++index) {
        const auto* source = qol::features::core_item::registered_bag_source_at(index);
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

bool selected_registered_source_item(qol::contracts::QolInventoryItemRef* out_item) {
    if (out_item == nullptr || !qol::contracts::item_facts_api_size_valid(g_item_facts) ||
        g_item_facts->detail_item_context == nullptr) return false;
    qol::contracts::QolItemDetailContext detail{};
    return qol::contracts::succeeded(g_item_facts->detail_item_context(&detail)) &&
           source_for_item_handle(detail.item, out_item) != nullptr;
}

bool use_registered_source_category(int32_t category) {
    for (uint32_t index = 0;
         index < qol::features::core_item::registered_bag_source_count(); ++index) {
        const auto* source = qol::features::core_item::registered_bag_source_at(index);
        if (source == nullptr || source->count == nullptr || source->info == nullptr ||
            source->item_at == nullptr || source->use == nullptr) {
            continue;
        }
        const int32_t bag_count = source->count();
        if (bag_count < 0) continue;
        for (int32_t bag = 0; bag < bag_count; ++bag) {
            qol::contracts::QolBagInfo info{};
            if (!qol::contracts::succeeded(source->info(bag, &info)) || info.capacity < 0) {
                continue;
            }
            for (int32_t slot = 0; slot < info.capacity; ++slot) {
                qol::contracts::QolInventoryItemRef item{};
                const QolResult result = source->item_at(bag, slot, &item);
                if (result.code == kQolErrNotFound) continue;
                if (!qol::contracts::succeeded(result) || item.category != category ||
                    item.count <= 0) {
                    continue;
                }
                if (qol::contracts::succeeded(
                        source->use({source->space_id, bag}, slot))) {
                    return true;
                }
            }
        }
    }
    return false;
}

EquipItemFromInvenToSlotFn g_backup_equip_item_from_inven_to_slot = nullptr;
PutJewelFn g_backup_put_jewel = nullptr;
UnequipFn g_backup_unequip_item_to_inven = nullptr;
ButtonDestroyExeFn g_backup_button_destroy_exe = nullptr;  // R-55 预演标记源
// R-63 佣兵徽章专用使用按钮函数级接管：UIEquip_ButtonUseMercenarySealExe(0xb8144)。
UiEquipButtonUseMercenarySealExeFn g_backup_button_use_mercenary_seal_exe = nullptr;
// R-55 原版背包详情出售接管：UIEquip_OKDestroyItem(0xb83d0)。返回值仅按钮预演读取
// （弹窗展示金额），用 UiEquipOkDestroyItemFn（uint64_t()）捕获 x0。
UiEquipOkDestroyItemFn g_backup_ok_destroy_item = nullptr;
CharUseItemExFn g_backup_char_use_item_ex = nullptr;
CharProcessShortcutFn g_backup_char_process_shortcut = nullptr;
ShortcutGetStateFn g_backup_shortcut_get_state = nullptr;
// S2 写侧进位框架 H-18/H-19（ITEMSYSTEM_Divide/MakeItem）的 backup。
ItemSystemDivideFn g_backup_item_system_divide = nullptr;
MakeItemFn g_backup_make_item = nullptr;

thread_local bool g_in_unequip_item_to_inven = false;
thread_local uint64_t g_active_shortcut_trace = 0;
// 单条 CHAR_ProcessShortcut 汇总行所需的中间观测：GetState 在 begin/end 之间被原版
// 调用多次，只记录次数与最后一次结果，避免每次回调各占一行。
thread_local uint32_t g_active_shortcut_state_calls = 0;
thread_local int32_t g_active_shortcut_last_state = -1;
// R-55：UIEquip_ButtonDestroyExe 动态范围内（含其同步调用的 OKDestroyItem 预演）。
thread_local bool g_in_equip_destroy_button = false;
thread_local bool g_has_vanilla_sell_preview_context = false;
thread_local qol::contracts::QolItemDetailContext g_vanilla_sell_preview_context{};
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

uint32_t read_count_field(const void* item);

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

void* inventory_item_at(int bag, int slot) {
    if (g_game == nullptr || g_game->item_abi.item_identity_at == nullptr ||
        g_game->item_abi.native_item_for_handle == nullptr) return nullptr;
    qol::contracts::QolItemHandle handle{};
    if (!qol::contracts::succeeded(
            g_game->item_abi.item_identity_at(bag, slot, &handle))) return nullptr;
    void* item = nullptr;
    const QolResult result = g_game->item_abi.native_item_for_handle(handle, &item);
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

// ---------------------------------------------------------------------------
// S2 写侧进位框架（R-45 拆段回写、R-46 类别门控 fail-closed、R-49 写侧调用方级
// 进位/借位）。原版数量写入全部经 UTIL_SetBitValue(旧字段值, 25, 31, 新b) 只写
// b 段（bits25–31）；S2 下 a 段（bits22–24）由本框架在函数级 hook 内按每函数
// 语义回写 a+b。禁止 hook UTIL_SetBitValue（无物品指针，无法判类别，R-46/R-49）。
//
// 锁纪律（R-44）：本节 wrapper 一律不取 g_virtual_bag_mtx，不在持锁下调原版；
// 目标槽读取用 s2_physical_slot_item 纯内存读，不取扩展端口锁。
//
// 本 TU 仍实现 ITEMSYSTEM_Divide（H-18/H-19）；INVEN_MoveItem、INVEN_SaveItemDirect、
// INVEN_ConsumeItem、INVEN_RemoveItemData（H-21）的 S2 写侧修正已随 storage wrapper 迁入
// core.inventory/hooks/storage_hooks.cpp。
//
// 逐函数语义表（16 函数 / 24 写点；语义=设/加/减，操作数=hook 内可得的数量来源）：
// 勘察基线：.tmp/dualmode-full.asm（llvm-objdump -d libgame.so，冻结基线）。
// | 函数                          | 写点 | 语义           | 操作数来源                     | 目标物品来源              | 状态 |
// | INVEN_MoveItem                | 4    | 加（合并）     | count 入参 + 目标槽旧全量      | target(bag,slot) 槽内对象 | 已实现（core.inventory storage_hooks；原版 b 写确认后回写） |
// | INVEN_SaveItemDirect          | 3    | 加（入库合并） | 入参对象全量 + 槽内旧全量      | (bag,slot) 槽内对象       | 已实现（core.inventory storage_hooks）；delta≡0 mod 128 退化 fail-closed 跳过 |
// | UIStore_BuyItem               | 2    | 设（购买量）   | UIInputItemCount 全局数量      | CopyAsNewUID 产物 x21     | 无需接管：0xd2588 写点 b=100（ITEM_IsRealBroken 装备 marker，非数量）；0xd25cc 写点 b=购买量，购买输入框 max=99（UIStore_ButtonBuyExe 0xd17cc `mov x3, #0x63`，UIInputItemCount_ButtonLeft/RightExe 在 [1,max] 滚动），≤127 b 写即全量；静态货架分支直接 SaveItem（数量由 MakeSale/MakeItem 生成，产物恒 b=1 完整） |
// | ITEMSYSTEM_Divide             | 2    | 设+减（拆堆）  | count 入参 + 源堆旧全量        | 返回新对象 + item 入参    | 已实现 item_system_divide_wrapper |
// | ITEMSYSTEM_CreateItem         | 2    | 设（初始化）   | 函数内常量                     | 新建对象                  | 无需接管：数量位写点仅 `mov w3,#0x1; SetBitValue(31,25,1)`（默认创建 1 个，0x10be9c+0x1a4 区）；另一 31/25 写点 `w3=0x64` 是装备 marker（非数量）；ABI 实为 `void* (int32_t category)` 单参（GAME_StartNewGame 0x1001fc w0=3/4/5 实证）；其余 SetBitValue 写类别 bit6..15（+0x08）、价值 bit0..24、佣兵随机 bit0..7，均不属数量语义 |
// | UIMix_StartMix                | 1    | 设（产物量）   | 配方区 +0x8（静态表数据）      | MIXSYSTEM_MakeItem out 对象 | 无需接管：产物量 = 配方表只读静态数据（模块无任何写点），原版数量域 ≤99 ≤127，b 写即全量；`cmp #0x1; b.le`（0xc0964 区）数量 ≤1 时连写点都不进 |
// | DEALSYSTEM_MakeSale           | 1    | 设（货架生成） | ITEMSYSTEM_MakeItem ×5         | 货架对象                  | 无需接管：唯一 31/25 写点 `w3=0x7e`（0xf6754 区）是上架装备 marker=126，非数量；数量产生全部经 ITEMSYSTEM_MakeItem（产物恒 b=1，无需回写）；语义修正：本函数是商店特卖货架生成，不是售出 |
// | GAME_StartNewGame             | 1    | 设（初始量）   | 函数内常量 5                   | 新建对象                  | 无需接管：唯一数量写点 `mov w3,#0x5; SetBitValue(31,25,5)`（0x100200 区），初始量 5 ≤127 b 写即全量；前两个 CreateItem（类别 3/4）无数量写 |
// | INVEN_RemoveItemData          | 1    | 减（批量删除） | category/count 入参            | 多堆遍历（bag0..5 顺序）  | 已实现（core.inventory storage_hooks）：快照/重扫 + stage4_remove_item_data_plan 修正部分删堆（整删消失堆只求和）；遍历顺序已冻结（0x1040a8：外层 bag 0..5 × 内层槽，Getter 读 cum、`b.lt` 判部分删）；R-56 扩展桥接：物理实扣不足按 category 从扩展袋补扣 |
// | INVEN_SaveItemData            | 1    | 加（批量创建） | category/count 入参            | 多堆/空槽                 | 无需接管：可堆叠分支 `cmp w20,#0x62; b.gt`（0x104694 区）>98 每堆写 99、≤98 写 count，每笔 ≤99 ≤127 b 写即全量；不可堆叠分支每件数量 1；category=0 分支是加钱（INVEN_AddMoney），无数量写 |
// | INVEN_ConsumeItem             | 1    | 减（1）        | 恒 1 + 对象旧全量              | item 入参                 | 已实现（core.inventory storage_hooks；b≤1 借位预置 + 回写） |
// | ITEMSYSTEM_MakeItem           | 1    | 设（装备强化 marker，非数量） | CAL_Calculate 结果 | CreatePerfectItem 返回装备 | 勘误（反汇编 0x10c6c8）：原登记「设（创建量）count 入参 arg2」错误——真实 ABI 是 `void* (category, lookup_key, luck)`（0x10c6d8 `mov w23,w1` 用于静态表 uint16 匹配、0x10c6e4 存 w2 作品质门槛比较，均非数量）；唯一 31/25 写点 0x10ca3c 仅对可强化装备写强化 marker（可堆叠类别 bit0=0 不进该分支，产物数量恒由 CreateItem 写 b=1，S2 完整）。旧 make_item_wrapper 把 x1（lookup_key）当 count 回写，导致拾取 1 个得 lookup_key 个（药水 2/卷轴 3/材料 4，真机实证）；H-20 已整体移除，原 hook 名保留编号不复用 |
// | ITEMSYSTEM_ProcessUnpack      | 1    | 设（产物量）   | 拆包表 u8 字段（表项 +0x4）    | CreatePerfectItem 产物    | 无需接管：产物量 = 拆包表只读静态数据（模块无任何写点），原版数量域 ≤99 ≤127，b 写即全量；开放边界：静态表若含 >99 值需数据审计复核（VM 取证抽查） |
// | MAPITEMSYSTEM_CreateItem      | 1    | 设（生成量）   | count 入参（w1）               | CreateItem 产物（尾调 Add，返回值非对象） | 无需接管：4 个调用点 count 域全部为原版静态数据——UINpc_ExeNpcTask 0xc2f60 `mov w1,#1`、EVTSYSTEM_Process 0xfc710 事件表字段、QUESTSYSTEM_ApplyReward 0x1238e4 任务表 u16；掉落/任务数量设计域 ≤99 ≤127；开放边界：若未来发现丢弃/移仓路径把 S2 全量（>127）作 count 传入本函数则需复核（当前全量反汇编未见该路径） |
// | NetworkStore_InitializeMenuData | 1  | 设（货架量）   | 网络商品数据 +0x18 字段        | CreateItem 产物           | 待勘察：数量来自网络数据（0x15b7b0+0x25xx 写点 `[x25+0x18]` 解引用），离线不可达无法冻结值域；勘察方法：联网抓包/内存 dump 商品表 +0x18 域，或真机挂回调记录 |
// | NetworkStore_AddItem          | 1    | 设（接收量）   | 网络收货数据 +0x18 字段        | CreateItem 产物（返回值非对象） | 待勘察：写点 0x15d7cc 区 `w3=[收货数据+0x18]`，数量域同上不可达；勘察方法同上 |
// ---------------------------------------------------------------------------

// 物理库存包含 6 袋（含任务袋 5）；写侧读取经 Game Host 物理槽原语，避免扩展物化路径取锁。
// C18 原生袋上界由 lifecycle 按 bag_count(0)+bag_count(1) 下发。
constexpr int kInvenPhysicalBagCount = 6;
constexpr int kInvenSlotsPerBag = 16;

int physical_bag_limit() {
    return std::max(qol::features::core_item::bags::native_bag_limit(),
                    kInvenPhysicalBagCount);
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
bool extension_unequip_to_registered_source(void* character, int32_t equip_slot);

int unequip_item_to_inven_wrapper(void* character, int32_t equip_slot) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:CHAR_UnequipItemToInven"});
    if (!inventory_native_hook_support_enabled())
        return call_backup("hook:CHAR_UnequipItemToInven", g_backup_unequip_item_to_inven,
                           character, equip_slot);
    if (g_backup_unequip_item_to_inven == nullptr) {
        backup_missing("hook:CHAR_UnequipItemToInven");
        return 0;
    }
    const qol::contracts::InventoryUnequipScope unequip_scope;
    const int result = stage4_unequip_item_to_inven(character, equip_slot,
                                                    g_backup_unequip_item_to_inven,
                                                    extension_unequip_to_registered_source,
                                                    g_in_unequip_item_to_inven);
    (void)unequip_scope;
    QOL_LOG_DEBUG_EVERY(QolDomain::kInventory, 60,
                        "UnequipItemToInven every_calls=60 equip_slot=%d result=%d recursive=%d",
                        equip_slot, result, g_in_unequip_item_to_inven ? 1 : 0);
    if (result == 0) {
        QOL_LOG_ERROR(QolDomain::kInventory,
                      "UnequipItemToInven failed equip_slot=%d", equip_slot);
    }
    return result;
}

constexpr int32_t kNativeStackLimit = 126;

struct RegisteredDestination {
    const qol::contracts::QolBagSource* source = nullptr;
    int32_t bag = -1;
    int32_t slot = -1;
};
bool hook_item_category(void* item, int32_t* out_category) {
    if (item == nullptr || out_category == nullptr) return false;
    const auto* port = qol::features::core_item::g_port;
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
    const auto* port = qol::features::core_item::g_port;
    if (port == nullptr || !qol::contracts::qol_item_port_bag_space_size_valid(port) ||
        port->read.serialize_item == nullptr || port->read.category == nullptr) {
        return false;
    }
    qol::contracts::QolItemHandle handle{};
    if (!item_handle_for_hook_item(item, &handle)) return false;
    uint32_t required = 0;
    QolResult result = port->read.serialize_item(handle, nullptr, 0, &required);
    if (!qol::contracts::succeeded(result) || required == 0 ||
        required > qol::contracts::kQolBagTransferPayloadMaxSize) {
        return false;
    }
    // 类别以 port 读取为准；g_item_facts->category 预读结果会被覆盖，不做预读。
    int32_t category = -1;
    if (!qol::contracts::succeeded(port->read.category(handle, &category)) ||
        category <= 0) {
        return false;
    }
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
    if (item == nullptr || g_item_facts == nullptr || g_item_facts->count_encoding == nullptr) {
        return false;
    }
    qol::contracts::QolItemHandle handle{};
    if (!item_handle_for_hook_item(item, &handle)) return false;
    qol::contracts::QolItemCountEncoding encoding =
        qol::contracts::QolItemCountEncoding::kUnknown;
    if (!qol::contracts::succeeded(g_item_facts->count_encoding(handle, &encoding))) {
        return false;
    }
    return encoding == qol::contracts::QolItemCountEncoding::kEncoded;
}

bool find_registered_destination(const qol::contracts::QolBagTransfer& transfer,
                                 bool stackable,
                                 RegisteredDestination* out_destination) {
    if (out_destination == nullptr) return false;
    RegisteredDestination empty_destination{};
    bool has_empty = false;
    for (uint32_t source_index = 0;
         source_index < qol::features::core_item::registered_bag_source_count(); ++source_index) {
        const auto* source = qol::features::core_item::registered_bag_source_at(source_index);
        if (source == nullptr || source->count == nullptr || source->info == nullptr ||
            source->item_at == nullptr || source->import_item == nullptr) {
            continue;
        }
        const int32_t bag_count = source->count();
        if (bag_count < 0) continue;
        for (int32_t bag = 0; bag < bag_count; ++bag) {
            qol::contracts::QolBagInfo info{};
            if (!qol::contracts::succeeded(source->info(bag, &info)) || info.capacity < 0) {
                continue;
            }
            for (int32_t slot = 0; slot < info.capacity; ++slot) {
                qol::contracts::QolInventoryItemRef existing{};
                const QolResult probe = source->item_at(bag, slot, &existing);
                if (probe.code == kQolErrNotFound) {
                    if (!has_empty) {
                        empty_destination = {source, bag, slot};
                        has_empty = true;
                    }
                    continue;
                }
                if (!qol::contracts::succeeded(probe)) continue;
                if (stackable && existing.category == transfer.category && existing.count > 0 &&
                    existing.count + transfer.count <= kNativeStackLimit) {
                    *out_destination = {source, bag, slot};
                    return true;
                }
            }
        }
    }
    if (has_empty) {
        *out_destination = empty_destination;
        return true;
    }
    return false;
}
const qol::contracts::QolBagSource* registered_source_for_space(int32_t space_id) {
    for (uint32_t index = 0;
         index < qol::features::core_item::registered_bag_source_count(); ++index) {
        const auto* source = qol::features::core_item::registered_bag_source_at(index);
        if (source != nullptr && source->space_id == space_id) return source;
    }
    return nullptr;
}

bool decode_flat_bag(int32_t flat_bag, int32_t* out_space_id, int32_t* out_bag) {
    return qol::contracts::qol_bag_flat_decode(flat_bag, out_space_id, out_bag);
}

bool extension_unequip_to_registered_source(void* character, int32_t equip_slot) {
    const auto* extras = qol::features::core_item::g_character_extras_port;
    if (character == nullptr || equip_slot < 0 || extras == nullptr ||
        extras->read.equipped_item == nullptr) {
        return false;
    }
    void* equipped = nullptr;
    if (!qol::contracts::succeeded(extras->read.equipped_item(character, equip_slot, &equipped)) ||
        equipped == nullptr) {
        return false;
    }
    int32_t category = -1;
    int32_t domain = -1;
    if (!hook_item_category(equipped, &category) ||
        !read_native_save_domain(category, &domain) || domain != 0) {
        return false;
    }
    qol::contracts::QolBagTransfer transfer{};
    if (!build_transfer_for_hook_item(equipped, &transfer, nullptr, nullptr)) return false;
    RegisteredDestination destination{};
    if (!find_registered_destination(transfer, hook_item_stackable(equipped), &destination)) {
        return false;
    }
    qol::contracts::QolBagMutationResult imported{};
    if (!qol::contracts::succeeded(
            destination.source->import_item(&transfer, destination.bag, destination.slot,
                                            &imported))) {
        return false;
    }
    if (g_game == nullptr || g_game->abi.minor < qol::contracts::kGameHostSavedBagMinor ||
        g_game->character_write.reset_equipped_item == nullptr ||
        !qol::contracts::succeeded(
            g_game->character_write.reset_equipped_item(character, equip_slot))) {
        qol::contracts::QolBagMutationResult removed{};
        (void)destination.source->remove(imported.item, transfer.count, &removed);
        return false;
    }
    const auto* port = qol::features::core_item::g_port;
    if (port != nullptr && port->read.free_item != nullptr) {
        (void)port->read.free_item(equipped);
    }
    inventory_log_write(0, __FILE__, __LINE__,
                        "Unequip adopt category=%d space=%d bag=%d slot=%d",
                        category, destination.source->space_id, destination.bag,
                        destination.slot);
    return true;
}
void* item_system_divide_wrapper(void* item, int32_t count) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:ITEMSYSTEM_Divide"});
    if (!inventory_native_hook_support_enabled())
        return call_backup("hook:ITEMSYSTEM_Divide", g_backup_item_system_divide, item, count);
    // ITEMSYSTEM_Divide（设 + 减）：拆出 count 入栈新对象（设值），源堆扣减
    //（借位）。原版对源堆与新对象都只写 b；新对象为新建对象（a 应为 0），
    // 直接按入参 clamp 后覆盖写 a+b；源堆仅在校验「原版确实写了 b」后回写
    // a+b=pre_full-count。源堆旧字段必须在 backup 前读取——整堆拆分时原版
    // 可能释放源对象；剩余 >0 时源对象保留，调后读取才安全。拆分失败
    //（new_item=null）或非 count-encoded 时全部跳过，保持原版结果。
    bool gate = item != nullptr && count > 0 && s2_writeback_gate(item);
    uint32_t pre_field = 0;
    uint32_t pre_full = 0;
    if (gate) {
        pre_field = read_count_field(item);
        // gate 保证 999 原子组已应用，此处读取完整 128a+b。
        if (!item_count_effective(item, &pre_full)) gate = false;
    }
    void* new_item = call_backup("hook:ITEMSYSTEM_Divide", g_backup_item_system_divide,
                                 item, count);
    if (!gate || new_item == nullptr) return new_item;
    if (pre_full > static_cast<uint32_t>(count) && s2_writeback_gate(item)) {
        // 源堆保留剩余（不释放），此读安全；整堆拆分（pre_full==count）时
        // 原版可能已释放源对象，不做任何源堆触磁。
        const uint32_t post_field = read_count_field(item);
        const uint32_t remain = qol::contracts::s2_writeback::subtracted_count(
            pre_full, static_cast<uint32_t>(count));
        if (qol::contracts::s2_writeback::original_count_write_confirmed(item_count_api(), pre_field, post_field,
                                                         remain)) {
            const ItemCountWriteResult write_result =
                item_count_write_effective(item, remain);
            if (write_result == ItemCountWriteResult::kFailed) return new_item;
            QOL_LOG_INFO(QolDomain::kInventory,
                                "S2 writeback Divide source item=%p %u-%u->%u",
                                item, pre_full, count, remain);
        } else if (post_field != pre_field) {
            QOL_LOG_ERROR(QolDomain::kInventory,
                                "S2 writeback Divide source mismatch skip item=%p "
                                "pre=0x%x post=0x%x expect_full=%u",
                                item, pre_field, post_field, remain);
        }
    }
    if (new_item != item && s2_writeback_gate(new_item)) {
        const uint32_t new_full = static_cast<uint32_t>(count);
        const ItemCountWriteResult write_result =
            item_count_write_effective(new_item, new_full);
        if (write_result == ItemCountWriteResult::kFailed) return new_item;
        QOL_LOG_INFO(QolDomain::kInventory,
                            "S2 writeback Divide new item=%p count=%u", new_item, new_full);
    }
    return new_item;
}

void* make_item_wrapper(int32_t category, int32_t arg2, int32_t flag) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:ITEMSYSTEM_MakeItem"});
    if (!inventory_native_hook_support_enabled())
        return call_backup("hook:ITEMSYSTEM_MakeItem", g_backup_make_item,
                           category, arg2, flag);
    // ITEMSYSTEM_MakeItem（0x10c6c8）：反汇编证明 arg2（w1）是与静态表 +0x2 域
    // 匹配的查找/品质参数——CHARSYSTEM_DropItem 传 2..5（掉落品质分支）、
    // DEALSYSTEM_MakeSale 传 5——不是数量；产物数量 = CAL_Calculate 掉落公式
    //（原版写点 0x10ca3c `SetBitValue(31,25,公式值)`，公式域 ≤99，b 写即全量，
    // 无需接管，R-49）。曾把 arg2 当 count 回写（`count>1` 恒真）导致掉落物一
    // 落地数量即被污染成 2/3/4/5（真机实证：药水 2/卷轴 3/材料 4，VM-38）。
    // 现在数量回写计划经纯函数 stage4_make_item_writeback_count（恒 0 = 不写），
    // wrapper 纯透传，仅保留观察日志。
    void* item = call_backup("hook:ITEMSYSTEM_MakeItem", g_backup_make_item,
                             category, arg2, flag);
    static_cast<void>(stage4_make_item_writeback_count(category, arg2, flag));
    return item;
}
int provider_equip_item(void* character, int32_t flat_bag, int32_t slot) {
    const auto* state_write = qol::features::core_item::g_item_state_write;
    const auto* extras = qol::features::core_item::g_character_extras_port;
    int32_t space_id = -1;
    int32_t local_bag = -1;
    if (!decode_flat_bag(flat_bag, &space_id, &local_bag)) return 0;
    const auto* source = registered_source_for_space(space_id);
    if (source == nullptr || source->item_at == nullptr || source->export_item == nullptr ||
        source->remove == nullptr || source->import_item == nullptr) {
        return 0;
    }
    qol::contracts::QolInventoryItemRef source_item{};
    if (!qol::contracts::succeeded(source->item_at(local_bag, slot, &source_item))) return 0;
    qol::contracts::QolBagTransfer new_transfer{};
    if (!qol::contracts::succeeded(source->export_item(source_item.item, 1, &new_transfer))) {
        return 0;
    }
    if (state_write == nullptr || state_write->load_item == nullptr ||
        state_write->equip_handle == nullptr || g_game == nullptr ||
        g_game->item_abi.native_item_for_handle == nullptr ||
        extras == nullptr ||
        extras->read.can_equip == nullptr ||
        extras->read.find_equip_slot == nullptr ||
        extras->read.equipped_item == nullptr ||
        extras->read.party_member == nullptr ||
        g_game->abi.minor < qol::contracts::kGameHostSavedBagMinor ||
        g_game->character_write.reset_equipped_item == nullptr) {
        return 0;
    }
    qol::contracts::QolItemHandle new_handle{};
    if (!qol::contracts::succeeded(state_write->load_item(
            new_transfer.payload, new_transfer.payload_size, &new_handle)) ||
        new_handle.slot == 0 || new_handle.generation == 0) {
        return 0;
    }
    void* new_item = nullptr;
    if (!qol::contracts::succeeded(
            g_game->item_abi.native_item_for_handle(new_handle, &new_item)) ||
        new_item == nullptr) {
        return 0;
    }
    const auto* port = qol::features::core_item::g_port;
    const auto free_native = [port](void* item) {
        if (item != nullptr && port != nullptr && port->read.free_item != nullptr) {
            (void)port->read.free_item(item);
        }
    };
    const auto restore_new = [&]() {
        qol::contracts::QolBagMutationResult restored{};
        (void)source->import_item(&new_transfer, local_bag, slot, &restored);
    };
    int32_t allowed = 0;
    if (!qol::contracts::succeeded(
            extras->read.can_equip(
                character, new_item, &allowed)) ||
        allowed <= 0) {
        free_native(new_item);
        return 0;
    }
    int32_t target = -1;
    if (!qol::contracts::succeeded(
            extras->read.find_equip_slot(character, new_item, &target)) ||
        target < 0 || target >= 10) {
        free_native(new_item);
        return 0;
    }
    int32_t two_handed = 0;
    if (g_game->abi.minor >= qol::contracts::kGameHostTwoHandedMinor &&
        g_game->read_only.item.is_two_handed != nullptr) {
        (void)g_game->read_only.item.is_two_handed(new_transfer.category, &two_handed);
    }
    int32_t old_slots[2] = {target, -1};
    int old_count = 1;
    if (two_handed == 1) {
        const int32_t paired = target == 5 ? 6 : (target == 6 ? 5 : -1);
        if (paired >= 0) old_slots[old_count++] = paired;
    }
    void* old_items[2] = {nullptr, nullptr};
    qol::contracts::QolBagTransfer old_transfers[2]{};
    for (int index = 0; index < old_count; ++index) {
        if (!qol::contracts::succeeded(extras->read.equipped_item(
                character, old_slots[index], &old_items[index]))) {
            free_native(new_item);
            return 0;
        }
        if (old_items[index] != nullptr &&
            !build_transfer_for_hook_item(old_items[index], &old_transfers[index], nullptr,
                                          nullptr)) {
            free_native(new_item);
            return 0;
        }
    }
    RegisteredDestination second_destination{};
    if (old_items[1] != nullptr &&
        !find_registered_destination(old_transfers[1], hook_item_stackable(old_items[1]),
                                     &second_destination)) {
        free_native(new_item);
        return 0;
    }
    int32_t role = -1;
    for (int32_t candidate = 0; candidate < 3; ++candidate) {
        void* member = nullptr;
        if (qol::contracts::succeeded(
                extras->read.party_member(candidate, &member)) &&
            member == character) {
            role = candidate;
            break;
        }
    }
    if (role < 0) {
        free_native(new_item);
        return 0;
    }
    qol::contracts::QolBagMutationResult removed_new{};
    if (!qol::contracts::succeeded(source->remove(source_item.item, 1, &removed_new))) {
        free_native(new_item);
        return 0;
    }
    qol::contracts::QolBagMutationResult imported_old[2]{};
    int imported_count = 0;
    const auto rollback_imports = [&]() {
        for (int index = 0; index < imported_count; ++index) {
            qol::contracts::QolBagMutationResult removed{};
            (void)source->remove(imported_old[index].item, old_transfers[index].count, &removed);
        }
        imported_count = 0;
    };
    if (old_items[0] != nullptr) {
        if (!qol::contracts::succeeded(
                source->import_item(&old_transfers[0], local_bag, slot, &imported_old[0]))) {
            restore_new();
            free_native(new_item);
            return 0;
        }
        imported_count = 1;
    }
    if (old_items[1] != nullptr) {
        if (!qol::contracts::succeeded(source->import_item(
                &old_transfers[1], second_destination.bag, second_destination.slot,
                &imported_old[1]))) {
            rollback_imports();
            restore_new();
            free_native(new_item);
            return 0;
        }
        imported_count = 2;
    }
    int cleared = 0;
    for (int index = 0; index < old_count; ++index) {
        if (old_items[index] == nullptr) continue;
        if (!qol::contracts::succeeded(
                g_game->character_write.reset_equipped_item(character, old_slots[index]))) {
            for (int restore_index = 0; restore_index < cleared; ++restore_index) {
                if (old_items[restore_index] != nullptr) {
                    (void)g_game->character_write.set_equipped_item(
                        character, old_slots[restore_index], old_items[restore_index]);
                }
            }
            rollback_imports();
            restore_new();
            free_native(new_item);
            return 0;
        }
        ++cleared;
    }
    qol::contracts::QolItemInventoryStateWriteResult write_result{};
    if (!qol::contracts::succeeded(
            state_write->equip_handle(role, new_handle, target, &write_result))) {
        for (int index = 0; index < old_count; ++index) {
            if (old_items[index] != nullptr) {
                (void)g_game->character_write.set_equipped_item(
                    character, old_slots[index], old_items[index]);
            }
        }
        rollback_imports();
        restore_new();
        free_native(new_item);
        return 0;
    }
    for (int index = 0; index < old_count; ++index) free_native(old_items[index]);
    inventory_log_write(0, __FILE__, __LINE__,
                        "provider_equip category=%d target=%d old_count=%d flat=%d slot=%d",
                        new_transfer.category, target, old_count, flat_bag, slot);
    return 1;
}

int equip_item_from_inven_to_slot_wrapper(void* character, int32_t bag, int32_t slot,
                                          int32_t equip_slot) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:CHAR_EquipItemFromInvenToSlot"});
    if (!inventory_native_hook_support_enabled()) {
        if (bag >= qol::contracts::kQolBagFlatRegisteredBase) return 0;
        return call_backup("hook:CHAR_EquipItemFromInvenToSlot",
                           g_backup_equip_item_from_inven_to_slot,
                           character, bag, slot, equip_slot);
    }
    if (bag >= qol::contracts::kQolBagFlatRegisteredBase) {
        const int provider_result = provider_equip_item(character, bag, slot);
        if (provider_result == 0) {
            QOL_LOG_ERROR(QolDomain::kInventory,
                          "provider equip failed flat_bag=%d slot=%d equip_slot=%d",
                          bag, slot, equip_slot);
        }
        return provider_result;
    }
    if (g_backup_equip_item_from_inven_to_slot == nullptr) {
        backup_missing("hook:CHAR_EquipItemFromInvenToSlot");
        return 0;
    }
    void* item = inventory_item_at(bag, slot);
    qol::contracts::QolInventoryItemRef source_ref{};
    if (item != nullptr && source_for_hook_item(item, &source_ref) != nullptr) {
        QOL_LOG_WARN(QolDomain::kInventory,
                     "equip rejected registered-source item bag=%d slot=%d",
                     source_ref.bag, source_ref.slot);
        return 0;
    }
    const int result = stage4_equip_item(
        character, bag, slot, equip_slot, inventory_item_at, nullptr, nullptr,
        g_backup_equip_item_from_inven_to_slot,
        nullptr);
    QOL_LOG_INFO(QolDomain::kInventory,
                        "EquipItemFromInvenToSlot bag=%d slot=%d equip_slot=%d result=%d",
                        bag, slot, equip_slot, result);
    return result;
}

int put_jewel_wrapper(void* equip_item, void* jewel_item) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:ITEMSYSTEM_PutJewel"});
    if (!inventory_native_hook_support_enabled())
        return call_backup("hook:ITEMSYSTEM_PutJewel", g_backup_put_jewel,
                           equip_item, jewel_item);
    if (g_backup_put_jewel == nullptr) {
        backup_missing("hook:ITEMSYSTEM_PutJewel");
        return 3;
    }
    // Registered-source items are valid here: the provider owns their data, and
    // material consumption is routed back through QolBagSource::remove by the
    // INVEN_ConsumeItem hook. The caller that initiated the native apply is
    // responsible for persisting any provider-owned target payload.
    return stage4_put_jewel(equip_item, jewel_item, nullptr, g_backup_put_jewel, nullptr);
}

// R-55：详情出售按钮函数级标记。原按钮在同一调用内先调 UIEquip_OKDestroyItem
// 预演（x23=0x666）取确认框展示金额、再创建 YesNo 弹窗；预演期间置 thread_local
// 标记，供 ok_destroy_item_wrapper 区分「预演」与「弹窗 OK 真实结算」。
// 出售门禁（D2c-2b-1）：ButtonDestroyExe 已被本域 hook，同址不可二次 NativeHook，
// 功能域（special_equip）经 hooks/dispatch 布尔门登记谓词，wrapper 入口统一查询；
// resource_id 与本 claim 一致。未登记即放行（与旧 g_sell_gate==nullptr 同义）。
void button_destroy_exe_wrapper(void* button) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:UIEquip_ButtonDestroyExe"});
    if (!inventory_native_hook_support_enabled()) {
        call_backup("hook:UIEquip_ButtonDestroyExe", g_backup_button_destroy_exe, button);
        return;
    }
    if (g_sdk != nullptr && g_sdk->hooks.intercept_blocks != nullptr &&
        g_sdk->hooks.intercept_blocks("hook:UIEquip_ButtonDestroyExe", nullptr) > 0) {
        QOL_LOG_INFO(QolDomain::kInventory, "sell button blocked by gate button=%p", button);
        return;
    }
    // The desc observation hook can be stale after UIEquip recreates the button.
    // This function-level entry is always live, so it is the final fallback that
    // keeps an extension item from falling through to the original sell popup.
    const auto* projection = qol::features::core_item::projection_callbacks();
    if (projection != nullptr && projection->handle_extension_sell != nullptr &&
        projection->handle_extension_sell()) {
        return;
    }
    const bool previous = g_in_equip_destroy_button;
    g_in_equip_destroy_button = true;
    call_backup("hook:UIEquip_ButtonDestroyExe", g_backup_button_destroy_exe, button);
    g_in_equip_destroy_button = previous;
}

// 仅为已注册袋源提供显式 use 回调；没有回调时不把投影对象交给原版物品池。
void button_use_mercenary_seal_exe_wrapper(void* button) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:UIEquip_ButtonUseMercenarySealExe"});
    if (!inventory_native_hook_support_enabled()) {
        call_backup("hook:UIEquip_ButtonUseMercenarySealExe",
                    g_backup_button_use_mercenary_seal_exe, button);
        return;
    }
    const auto* projection = qol::features::core_item::projection_callbacks();
    if (projection != nullptr && projection->handle_use_mercenary_seal != nullptr &&
        projection->handle_use_mercenary_seal()) {
        return;
    }
    qol::contracts::QolInventoryItemRef source_ref{};
    if (selected_registered_source_item(&source_ref)) {
        const auto* source = qol::features::core_item::find_bag_source(source_ref.space_id);
        if (source != nullptr && source->use != nullptr) {
            (void)source->use({source_ref.space_id, source_ref.bag}, source_ref.slot);
        } else {
            QOL_LOG_WARN(QolDomain::kInventory,
                         "mercenary seal use unsupported by registered bag source bag=%d slot=%d",
                         source_ref.bag, source_ref.slot);
        }
        return;
    }
    if (g_backup_button_use_mercenary_seal_exe == nullptr) {
        backup_missing("hook:UIEquip_ButtonUseMercenarySealExe");
        return;
    }
    call_backup("hook:UIEquip_ButtonUseMercenarySealExe",
                g_backup_button_use_mercenary_seal_exe, button);
}

// R-55 原版背包详情出售接管（VM-41）。UIEquip_OKDestroyItem(0xb83d0) 无参：背包
// 结算分支从面板上下文读 desc_type(0x304a43)/bag(0x304a41)/ctrl(0x3049e8)，再经
// ControlObject_GetCursorIndex 取槽，随后调 0x1261c4 只按 b 段结算。两态由
// button_destroy_exe_wrapper 设置的 thread_local 标记区分：按钮预演只回填展示金额、
// 弹窗 OK 才真实结算（返回地址经 Dobby 桥后不可信，不作判定依据）。
// 关闭态/非背包详情/校验失败一律 backup（fail-safe，不半执行）。
uint64_t ok_destroy_item_wrapper() {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:UIEquip_OKDestroyItem"});
    if (!inventory_native_hook_support_enabled()) {
        g_has_vanilla_sell_preview_context = false;
        return call_backup("hook:UIEquip_OKDestroyItem", g_backup_ok_destroy_item);
    }
    const auto run_backup = []() -> uint64_t {
        return call_backup("hook:UIEquip_OKDestroyItem", g_backup_ok_destroy_item);
    };
    // The detail slot, item identity and display mode are sampled together; the UI control stays
    // private to Game Adapter.
    if (!qol::contracts::item_facts_api_size_valid(g_item_facts) ||
        g_item_facts->detail_item_context == nullptr) {
        g_has_vanilla_sell_preview_context = false;
        return run_backup();
    }
    qol::contracts::QolItemDetailContext detail{};
    if (!qol::contracts::succeeded(g_item_facts->detail_item_context(&detail)) ||
        detail.desc_type != kVanillaBagDetailType) {
        g_has_vanilla_sell_preview_context = false;
        return run_backup();
    }
    const int32_t bag = detail.bag;
    const int32_t slot = detail.slot;
    if (bag == 5) {
        g_has_vanilla_sell_preview_context = false;
        return run_backup();
    }
    if (bag < 0 || bag >= physical_bag_limit() || slot < 0 ||
        slot >= kInvenSlotsPerBag || detail.item.slot == 0 || detail.item.generation == 0) {
        g_has_vanilla_sell_preview_context = false;
        return run_backup();
    }

    // Registered-source items are not owned by the physical inventory pool.
    qol::contracts::QolInventoryItemRef source_ref{};
    if (source_for_item_handle(detail.item, &source_ref) != nullptr) {
        QOL_LOG_WARN(QolDomain::kInventory,
                     "vanilla destroy blocked registered-source item bag=%d slot=%d",
                     source_ref.bag, source_ref.slot);
        g_has_vanilla_sell_preview_context = false;
        return 0;
    }
    void* item = s2_physical_slot_item(bag, slot);
    if (item == nullptr) {
        g_has_vanilla_sell_preview_context = false;
        return run_backup();
    }
    qol::contracts::QolItemHandle observed_handle{};
    if (!item_handle_for_hook_item(item, &observed_handle) ||
        observed_handle.slot != detail.item.slot ||
        observed_handle.generation != detail.item.generation) {
        g_has_vanilla_sell_preview_context = false;
        return run_backup();
    }
    const qol::contracts::QolItemCountApi* count_api = item_count_api();
    uint32_t canonical = 0;
    if (!item_count_is_encoded(item) || count_api == nullptr || count_api->canonical == nullptr ||
        !qol::contracts::succeeded(count_api->canonical(item, &canonical))) {
        g_has_vanilla_sell_preview_context = false;
        return run_backup();
    }
    const VanillaSellRoute route =
        vanilla_sell_route(bag, true, canonical, g_in_equip_destroy_button);
    if (route == VanillaSellRoute::kBackup) {
        g_has_vanilla_sell_preview_context = false;
        return run_backup();
    }
    if (route == VanillaSellRoute::kTakeover &&
        (!g_has_vanilla_sell_preview_context ||
         !vanilla_sell_detail_context_matches(g_vanilla_sell_preview_context, detail))) {
        g_has_vanilla_sell_preview_context = false;
        return run_backup();
    }
    if (route == VanillaSellRoute::kTakeover) g_has_vanilla_sell_preview_context = false;
    if (g_item_facts->category == nullptr || g_item_facts->is_no_sell == nullptr) {
        g_has_vanilla_sell_preview_context = false;
        return run_backup();
    }
    int32_t category = 0;
    int32_t no_sell = 0;
    const qol::contracts::QolItemHandle item_handle = detail.item;
    if (!qol::contracts::succeeded(g_item_facts->category(item_handle, &category)) ||
        !qol::contracts::succeeded(g_item_facts->is_no_sell(category, &no_sell)) ||
        category <= 0 || no_sell != 0) {
        QOL_LOG_INFO(QolDomain::kInventory,
                            "vanilla sell backup reason=no_sell bag=%d slot=%d category=%d", bag,
                            slot, category);
        g_has_vanilla_sell_preview_context = false;
        return run_backup();
    }
    if (!qol::contracts::item_facts_api_quote_size_valid(g_item_facts) ||
        g_item_facts->quote == nullptr) {
        g_has_vanilla_sell_preview_context = false;
        return run_backup();
    }
    qol::contracts::QolSaleQuote quote{};
    quote.struct_size = sizeof(quote);
    if (!qol::contracts::succeeded(
            g_item_facts->quote(item_handle, qol::contracts::kDetailSell, &quote)) ||
        quote.struct_size < sizeof(quote) || quote.count <= 0 || quote.price < 0 ||
        quote.multiplier_den == 0 || quote.multiplier_num > quote.multiplier_den) {
        QOL_LOG_INFO(QolDomain::kInventory,
            "vanilla sell backup reason=quote_unavailable bag=%d slot=%d category=%d canonical=%u",
            bag, slot, category, canonical);
        g_has_vanilla_sell_preview_context = false;
        return run_backup();
    }
    const int64_t price = quote.price;

    if (route == VanillaSellRoute::kPreview) {
        // 按钮预演：只回填弹窗展示金额，不结算。
        g_vanilla_sell_preview_context = detail;
        g_has_vanilla_sell_preview_context = true;
        QOL_LOG_INFO(QolDomain::kInventory,
            "vanilla sell preview bag=%d slot=%d category=%d canonical=%u count=%d unit=%lld price=%lld multiplier=%u/%u",
            bag, slot, category, canonical, quote.count, static_cast<long long>(quote.unit_price),
            quote.multiplier_num, quote.multiplier_den,
            static_cast<long long>(price));
        return static_cast<uint64_t>(price);
    }

    // kTakeover：加钱 → 删整堆 → 刷新。加钱/删堆失败不回 backup（避免按原版错误
    // 金额二次结算），失败即退款中止。
    if (g_game == nullptr || g_game->world_action.add_money == nullptr ||
        g_game->world_action.minus_money == nullptr ||
        g_game->inventory_write.remove_item_direct == nullptr) {
        return run_backup();
    }
    if (!qol::contracts::succeeded(g_game->world_action.add_money(price))) {
        QOL_LOG_ERROR(QolDomain::kInventory,
                            "vanilla sell credit failed bag=%d slot=%d price=%lld", bag, slot,
                            static_cast<long long>(price));
        return 0;
    }
    // 提交前复核槽内对象未变（防并发替换/释放）。
    if (s2_physical_slot_item(bag, slot) != item) {
        const QolResult refund = g_game->world_action.minus_money(price);
        if (qol::contracts::succeeded(refund)) {
            QOL_LOG_ERROR(QolDomain::kInventory,
                          "vanilla sell stale before remove bag=%d slot=%d refunded=1", bag, slot);
        } else {
            QOL_LOG_ERROR(QolDomain::kInventory,
                          "vanilla sell stale before remove bag=%d slot=%d refund_failed=1 code=%d",
                          bag, slot, refund.code);
        }
        return 0;
    }
    (void)g_game->inventory_write.remove_item_direct(bag, slot);
    if (s2_physical_slot_item(bag, slot) == item) {
        const QolResult refund = g_game->world_action.minus_money(price);
        if (qol::contracts::succeeded(refund)) {
            QOL_LOG_ERROR(QolDomain::kInventory,
                          "vanilla sell remove failed bag=%d slot=%d refunded=1", bag, slot);
        } else {
            QOL_LOG_ERROR(QolDomain::kInventory,
                          "vanilla sell remove failed bag=%d slot=%d refund_failed=1 code=%d",
                          bag, slot, refund.code);
        }
        return 0;
    }
    (void)inventory_native_hook_call_refresh_item_area_original();
    QOL_LOG_INFO(QolDomain::kInventory,
        "vanilla sell committed bag=%d slot=%d category=%d canonical=%u count=%d unit=%lld price=%lld multiplier=%u/%u",
        bag, slot, category, canonical, quote.count, static_cast<long long>(quote.unit_price),
        quote.multiplier_num, quote.multiplier_den,
        static_cast<long long>(price));
    return 0;
}

struct ShortcutTraceScope {
    uint64_t previous;
    uint32_t previous_state_calls;
    int32_t previous_last_state;

    explicit ShortcutTraceScope(uint64_t trace_id)
        : previous(g_active_shortcut_trace),
          previous_state_calls(g_active_shortcut_state_calls),
          previous_last_state(g_active_shortcut_last_state) {
        g_active_shortcut_trace = trace_id;
        g_active_shortcut_state_calls = 0;
        g_active_shortcut_last_state = -1;
    }

    ~ShortcutTraceScope() {
        g_active_shortcut_trace = previous;
        g_active_shortcut_state_calls = previous_state_calls;
        g_active_shortcut_last_state = previous_last_state;
    }
};

int native_category_count(int32_t category) {
    const auto* port = qol::features::core_item::g_port;
    if (category <= 0 || port == nullptr ||
        !qol::contracts::qol_item_port_bag_space_size_valid(port) ||
        port->read.bag_count == nullptr || port->read.bag_info == nullptr ||
        port->read.item_at == nullptr) {
        return 0;
    }
    int64_t total = 0;
    for (int32_t space = qol::contracts::kQolBagSpaceOriginal;
         space <= qol::contracts::kQolBagSpaceVariant; ++space) {
        int32_t bag_count = 0;
        if (!qol::contracts::succeeded(port->read.bag_count(space, &bag_count)) ||
            bag_count < 0) {
            continue;
        }
        for (int32_t bag = 0; bag < bag_count; ++bag) {
            const qol::contracts::QolBagRef ref{space, bag};
            qol::contracts::QolBagInfo info{};
            if (!qol::contracts::succeeded(port->read.bag_info(&ref, &info)) ||
                info.capacity < 0) {
                continue;
            }
            for (int32_t slot = 0; slot < info.capacity; ++slot) {
                qol::contracts::QolInventoryItemRef item{};
                const QolResult result = port->read.item_at(&ref, slot, &item);
                if (result.code == kQolErrNotFound) continue;
                if (!qol::contracts::succeeded(result) || item.category != category) continue;
                total += item.count;
                if (total > INT32_MAX) return INT32_MAX;
            }
        }
    }
    return static_cast<int>(total);
}

int char_process_shortcut_wrapper(void* character, int shortcut) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:CHAR_ProcessShortcut"});
    if (g_backup_char_process_shortcut == nullptr) {
        backup_missing("hook:CHAR_ProcessShortcut");
        return 0;
    }
    // 一次快捷键只留一行：begin/end 与 GetState 原本各自一条，同一次调用内的
    // 三条日志必须靠 trace id 人工关联，是日志量的主要来源之一。
    const bool trace_enabled = shortcut >= 0 && inventory_log_debug_enabled();
    const ShortcutTraceScope trace_scope(trace_enabled ? 1 : 0);
    if (g_game != nullptr && g_game->item_abi.shortcut_item_category != nullptr) {
        int32_t category = 0;
        if (qol::contracts::succeeded(
                g_game->item_abi.shortcut_item_category(shortcut, &category)) &&
            category > 0) {
            const int native_count = native_category_count(category);
            if (native_count <= 0 && use_registered_source_category(category)) {
                inventory_log_write(0, __FILE__, __LINE__,
                                    "CHAR_ProcessShortcut registered-source category=%d shortcut=%d",
                                    category, shortcut);
                return 1;
            }
        }
    }
    const int result = g_backup_char_process_shortcut(character, shortcut);
    if (trace_enabled) {
        inventory_log_write(0, __FILE__, __LINE__,
                            "CHAR_ProcessShortcut character=%p shortcut=%d result=%d state=%d state_calls=%u",
                            character, shortcut, result,
                            g_active_shortcut_last_state, g_active_shortcut_state_calls);
    }
    return result;
}

int shortcut_get_state_wrapper(void* character, void* shortcut_state) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:SHORTCUT_GetState"});
    if (g_backup_shortcut_get_state == nullptr) {
        backup_missing("hook:SHORTCUT_GetState");
        return 0;
    }
    const int state = g_backup_shortcut_get_state(character, shortcut_state);
    if (g_active_shortcut_trace != 0 && inventory_log_debug_enabled()) {
        ++g_active_shortcut_state_calls;
        g_active_shortcut_last_state = state;
    }
    return state;
}

int char_use_item_ex_wrapper(void* character, void* item, int flag) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:CHAR_UseItemEx"});
    if (g_backup_char_use_item_ex == nullptr) {
        backup_missing("hook:CHAR_UseItemEx");
        return 0;
    }
    const int result = g_backup_char_use_item_ex(character, item, flag);
    const uint64_t trace_id = g_active_shortcut_trace;
    if (trace_id != 0 && inventory_log_debug_enabled()) {
        inventory_log_write(0, __FILE__, __LINE__,
                            "CHAR_UseItemEx trace=%llu character=%p item=%p flag=%d result=%d",
                            static_cast<unsigned long long>(trace_id), character, item, flag,
                            result);
    }
    return result;
}

// ---------------------------------------------------------------------------
// 声明式资源声明（B2a-1）：入口 hook 的 claims 表。wrapper 业务体在本 TU，
// handler 取址只能在此处；同插件的 plugin.cpp 经本目录 claims.h 直接引用
// inventory_hooks_claims / inventory_hooks_bind_backup 取得本表并交还 backup。
// 安装顺序、失败回滚与物理还原归宿主声明式引擎（hooks/install + hooks/transaction +
// host/plugin_registry），本 TU 不再自带安装引擎。
// ---------------------------------------------------------------------------
constexpr const char* kClaimOwner = "core.item";
constexpr const char* kClaimReason =
    "库存行为 wrapper 的函数级入口：扩展袋读改写、S2 写侧进位/借位、出售门禁与 "
    "R-55/R-56 回调层接管均需在函数入口分流；业务体归属 plugins/core.item/inventory";

#define INVENTORY_HOOK_CLAIM(CLAIM_ID, CLAIM_SYMBOL, HANDLER)                          \
    {CLAIM_ID, CLAIM_SYMBOL, qol::contracts::ResourceMechanism::kEntryHook,           \
     qol::contracts::ResourcePolicy::kPrivate, qol::contracts::ResourceRequirement::kRequired, \
     qol::contracts::ResourceFailure::kFailClosed, true, kClaimOwner, kClaimReason,    \
     nullptr, 120, reinterpret_cast<void*>(&HANDLER)}

const qol::contracts::ResourceClaim kInventoryHookClaims[] = {
    {"hook:CHAR_ProcessShortcut", "F_CHAR_PROCESS_SHORTCUT_VMA",
     qol::contracts::ResourceMechanism::kEntryHook,
     qol::contracts::ResourcePolicy::kPrivate,
     qol::contracts::ResourceRequirement::kOptional,
     qol::contracts::ResourceFailure::kFailSoft, true, kClaimOwner,
     "shortcut-use diagnosis: log arguments/result and transparently call the original",
     nullptr, 120, reinterpret_cast<void*>(&char_process_shortcut_wrapper)},
    {"hook:SHORTCUT_GetState", "F_SHORTCUT_GET_STATE_VMA",
     qol::contracts::ResourceMechanism::kEntryHook,
     qol::contracts::ResourcePolicy::kPrivate,
     qol::contracts::ResourceRequirement::kOptional,
     qol::contracts::ResourceFailure::kFailSoft, true, kClaimOwner,
     "shortcut-use diagnosis: log state only during CHAR_ProcessShortcut",
     nullptr, 120, reinterpret_cast<void*>(&shortcut_get_state_wrapper)},
    {"hook:CHAR_UseItemEx", "F_CHAR_USE_ITEM_EX_VMA",
     qol::contracts::ResourceMechanism::kEntryHook,
     qol::contracts::ResourcePolicy::kPrivate,
     qol::contracts::ResourceRequirement::kOptional,
     qol::contracts::ResourceFailure::kFailSoft, true, kClaimOwner,
     "shortcut-use diagnosis: log dispatch only during CHAR_ProcessShortcut",
     nullptr, 120, reinterpret_cast<void*>(&char_use_item_ex_wrapper)},
    INVENTORY_HOOK_CLAIM("hook:CHAR_EquipItemFromInvenToSlot",
                         "F_EQUIP_ITEM_FROM_INVEN_TO_SLOT_VMA",
                         equip_item_from_inven_to_slot_wrapper),
    INVENTORY_HOOK_CLAIM("hook:ITEMSYSTEM_PutJewel", "F_PUT_JEWEL_VMA", put_jewel_wrapper),
    INVENTORY_HOOK_CLAIM("hook:CHAR_UnequipItemToInven", "F_UNEQUIP_VMA",
                         unequip_item_to_inven_wrapper),
    INVENTORY_HOOK_CLAIM("hook:UIEquip_ButtonDestroyExe", "F_UIEQUIP_BUTTON_DESTROY_EXE_VMA",
                         button_destroy_exe_wrapper),
    INVENTORY_HOOK_CLAIM("hook:UIEquip_OKDestroyItem", "F_UIEQUIP_OK_DESTROY_ITEM_VMA",
                         ok_destroy_item_wrapper),
    INVENTORY_HOOK_CLAIM("hook:UIEquip_ButtonUseMercenarySealExe",
                         "F_UIEQUIP_BUTTON_USE_MERCENARY_SEAL_EXE_VMA",
                         button_use_mercenary_seal_exe_wrapper),
    INVENTORY_HOOK_CLAIM("hook:ITEMSYSTEM_Divide", "F_ITEMSYSTEM_DIVIDE_VMA",
                         item_system_divide_wrapper),
    INVENTORY_HOOK_CLAIM("hook:ITEMSYSTEM_MakeItem", "F_MAKE_ITEM_VMA", make_item_wrapper),
};
#undef INVENTORY_HOOK_CLAIM

constexpr uint32_t kInventoryHookClaimCount =
    sizeof(kInventoryHookClaims) / sizeof(kInventoryHookClaims[0]);

// claim.handler → 该 wrapper 的 backup 槽（原有 g_backup_* 全局）。以 handler 做唯一映射，
// 避免 id 表与 backup 表两处重复维护；未登记 handler 返回 nullptr，桥按 fail-closed 处理。
// g_backup_* 是有类型函数指针，槽地址统一 reinterpret_cast<void**> 交给机制层。
void** backup_slot_for_handler(void* handler) {
#define BACKUP_SLOT(HANDLER, SLOT) \
    if (handler == reinterpret_cast<void*>(&HANDLER)) return reinterpret_cast<void**>(&SLOT)
    BACKUP_SLOT(equip_item_from_inven_to_slot_wrapper, g_backup_equip_item_from_inven_to_slot);
    BACKUP_SLOT(put_jewel_wrapper, g_backup_put_jewel);
    BACKUP_SLOT(unequip_item_to_inven_wrapper, g_backup_unequip_item_to_inven);
    BACKUP_SLOT(button_destroy_exe_wrapper, g_backup_button_destroy_exe);
    BACKUP_SLOT(ok_destroy_item_wrapper, g_backup_ok_destroy_item);
    BACKUP_SLOT(button_use_mercenary_seal_exe_wrapper, g_backup_button_use_mercenary_seal_exe);
    BACKUP_SLOT(char_use_item_ex_wrapper, g_backup_char_use_item_ex);
    BACKUP_SLOT(char_process_shortcut_wrapper, g_backup_char_process_shortcut);
    BACKUP_SLOT(shortcut_get_state_wrapper, g_backup_shortcut_get_state);
    BACKUP_SLOT(item_system_divide_wrapper, g_backup_item_system_divide);
    BACKUP_SLOT(make_item_wrapper, g_backup_make_item);
#undef BACKUP_SLOT
    return nullptr;
}

}  // namespace

void inventory_native_hook_set_support(bool enabled) {
    g_inventory_support.store(enabled, std::memory_order_release);
}

bool inventory_native_hook_support_enabled() {
    return g_inventory_support.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Claims 与 backup 交还的模块入口；宿主资源回调经 core.item descriptor 调用。
// ---------------------------------------------------------------------------

const qol::contracts::ResourceClaim* inventory_hooks_claims(uint32_t* out_count) {
    if (out_count != nullptr) *out_count = kInventoryHookClaimCount;
    return kInventoryHookClaims;
}

int32_t inventory_hooks_bind_backup(const char* resource_id, void* backup) {
    // 空 backup 或未知 id 一律非 0 → 宿主逆序回滚并置 failed（fail-closed）。
    if (resource_id == nullptr || backup == nullptr) return 1;
    for (const qol::contracts::ResourceClaim& claim : kInventoryHookClaims) {
        if (std::strcmp(claim.id, resource_id) != 0) continue;
        void** slot = backup_slot_for_handler(claim.handler);
        if (slot == nullptr) return 1;
        *slot = backup;
        if (std::strcmp(claim.id, "hook:CHAR_ProcessShortcut") == 0 ||
            std::strcmp(claim.id, "hook:SHORTCUT_GetState") == 0 ||
            std::strcmp(claim.id, "hook:CHAR_UseItemEx") == 0) {
            QOL_LOG_INFO(QolDomain::kInventory,
                         "diagnostic hook bound resource=%s backup=%p", claim.id, backup);
        }
        return 0;
    }
    QOL_LOG_ERROR(QolDomain::kInventory,
                  "inventory_hooks bind backup: unknown resource id %s", resource_id);
    return 1;
}

// 模块主动刷新走 provider 的 trampoline：inventory_refresh 的 owner 是 extension_bag，
// core.item 只经 capability 请求原函数，不保留原生符号（provider 缺失时返回 false，
// 调用方按 no-op 降级）。
bool inventory_native_hook_call_refresh_item_area_original() {
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) {
        backup_missing("hook:UIEquip_RefreshItemArea");
        return false;
    }
    const void* iface = nullptr;
    const QolResult result = g_sdk->capabilities.require(
        qol::contracts::kInventoryRefreshCapabilityId,
        {static_cast<uint16_t>(qol::contracts::kInventoryRefreshVersionMajor),
         static_cast<uint16_t>(qol::contracts::kInventoryRefreshVersionMinor)},
        &iface);
    const auto* refresh = qol::contracts::succeeded(result) && iface != nullptr
                              ? static_cast<const qol::contracts::InventoryRefreshApi*>(iface)
                              : nullptr;
    if (refresh == nullptr || refresh->call_original == nullptr) {
        backup_missing("hook:UIEquip_RefreshItemArea");
        return false;
    }
    return refresh->call_original();
}

bool inventory_native_hook_bind_sdk(const qol::contracts::QolHostSdk* sdk) {
    if (sdk == nullptr || sdk->hooks.intercept_blocks == nullptr) {
        g_sdk = nullptr;
        inventory_log_bind_sdk(nullptr);
        return false;
    }
    g_sdk = sdk;
    inventory_log_bind_sdk(sdk);
    return true;
}

bool inventory_native_hook_bind_game(const qol::contracts::QolGameHost* game) {
    constexpr size_t kItemAbiEnd = offsetof(qol::contracts::QolGameHost, item_abi) +
                                   sizeof(qol::contracts::QolItemAbiDispatchApi);
    if (game == nullptr || game->abi.size < kItemAbiEnd ||
        !qol::contracts::satisfies({game->abi.major, game->abi.minor},
                                   {qol::contracts::kGameHostMajor,
                                    qol::contracts::kGameHostMinor}) ||
        game->item_count.effective == nullptr || game->item_abi.find_item_wrapper == nullptr ||
        game->item_abi.bind_find_item_backup == nullptr ||
        game->item_abi.original_find_item_slot == nullptr ||
        game->item_abi.item_identity_at == nullptr ||
        game->item_abi.begin_original_only == nullptr ||
        game->item_abi.end_original_only == nullptr ||
        game->item_abi.native_item_at == nullptr ||
        game->item_abi.native_item_for_handle == nullptr) {
        g_game = nullptr;
        return false;
    }
    g_game = game;
    return true;
}

bool inventory_native_hook_bind_item_facts(const qol::contracts::ItemFactsApi* facts) {
    g_item_facts = facts;
    return facts != nullptr;
}

namespace qol::features::inventory_hooks {
namespace {

QolResult count_write_error(int32_t code, const char* message) {
    return qol::contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

}  // namespace

QolResult write_detached_item_count(const qol::contracts::QolItemHandle& item, uint32_t count) {
    if (item.slot == 0 || item.generation == 0 || count == 0) {
        return count_write_error(kQolErrInvalidArgument, "invalid item count request");
    }
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api == nullptr || api->is_encoded == nullptr || g_game == nullptr ||
        g_game->item_abi.native_item_for_handle == nullptr) {
        return count_write_error(kQolErrNotReady, "item count unavailable");
    }
    void* native_item = nullptr;
    QolResult result = g_game->item_abi.native_item_for_handle(item, &native_item);
    if (!qol::contracts::succeeded(result) || native_item == nullptr) {
        return count_write_error(kQolErrNotFound, "item handle not found");
    }
    qol::contracts::QolItemCountEncoding encoding =
        qol::contracts::QolItemCountEncoding::kUnknown;
    result = api->is_encoded(native_item, &encoding);
    if (!qol::contracts::succeeded(result) ||
        encoding == qol::contracts::QolItemCountEncoding::kUnknown) {
        return count_write_error(kQolErrNotReady, "item count encoding unavailable");
    }
    if (encoding != qol::contracts::QolItemCountEncoding::kEncoded) {
        return count_write_error(kQolErrUnsupported, "item not stackable");
    }
    switch (item_count_write_effective(native_item, count)) {
        case ItemCountWriteResult::kApplied:
            return qol::contracts::ok();
        case ItemCountWriteResult::kFailed:
            return count_write_error(kQolErrInternal, "item count write failed");
        case ItemCountWriteResult::kNotReady:
            break;
    }
    // 999 扩展模式未落地时回退到 provider 的非扩展编码；编码布局只在 provider 内解释。
    if (api->read_encoded_field == nullptr || api->field_from_canonical == nullptr ||
        api->write_encoded_field == nullptr) {
        return count_write_error(kQolErrNotReady, "item count writer unavailable");
    }
    uint32_t field = 0;
    result = api->read_encoded_field(native_item, &field);
    if (!qol::contracts::succeeded(result)) return result;
    uint32_t updated = 0;
    result = api->field_from_canonical(field, count, 0, &updated);
    if (!qol::contracts::succeeded(result)) return result;
    return api->write_encoded_field(native_item, updated);
}

}  // namespace qol::features::inventory_hooks

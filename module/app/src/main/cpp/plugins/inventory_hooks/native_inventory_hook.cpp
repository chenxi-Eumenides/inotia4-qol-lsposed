#include "native_inventory_hook.h"
#include "inventory_hook_stage4.h"
#include "claims.h"
#include "inventory_logging.h"

#include "../../contracts/plugin.h"
#include "../../contracts/extension_bag.h"
#include "../../contracts/host_sdk.h"
#include "../../contracts/ui.h"

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <utility>

namespace {

using HaveItemFn = int (*)(int32_t);
using GetItemCountFn = int (*)(int32_t);
using GetCumulateCountFn = int (*)(void*);
using ConsumeItemFn = void (*)(void*);
using RemoveItemFn = int (*)(void*);
using SaveItemFn = int (*)(void*);
using InvenMoveItemFn = int (*)(void*, int, int, int);
using UiEquipRefreshItemAreaFn = void (*)();
using EquipItemFromInvenToSlotFn = int (*)(void*, int32_t, int32_t, int32_t);
using PutJewelFn = int (*)(void*, void*);
using IsHavingEmptySlotFn = int (*)(int32_t, int32_t);
using UnequipFn = int (*)(void*, int32_t);
using ButtonEquipExeFn = void (*)(void*);
using ButtonDestroyExeFn = void (*)(void*);
using ButtonUnequipExeFn = void (*)(void*);
using UiEquipOkConfirmUseItemFn = void (*)(void*);
using UiEquipButtonUseMercenarySealExeFn = void (*)(void*);
using UiEquipOkDestroyItemFn = uint64_t (*)();
using UiEquipEquipControlEventProcFn = uint64_t (*)(void*, uint64_t, void*, void*);
using InvenSaveItemDirectFn = int (*)(void*, int32_t, int32_t);
using ItemSystemDivideFn = void* (*)(void*, int32_t);
using MakeItemFn = void* (*)(int32_t, int32_t, int32_t);
using InvenRemoveItemDataFn = int (*)(int32_t, int32_t);

constexpr uint32_t kNativeApiVersion = 2;

const qol::contracts::QolHostSdk* g_sdk = nullptr;
const qol::contracts::QolGameHost* g_game = nullptr;
const qol::contracts::ItemFactsApi* g_item_facts = nullptr;

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

void transfer_item_handle_to_extension_bag(void* item) {
    const auto* api = item_handle_api();
    if (item == nullptr || api == nullptr) return;
    const uint32_t provider = qol::contracts::kQolItemHandleOwnerProvider;
    if (api->rebind_item != nullptr &&
        api->rebind_item(item, provider) ==
            qol::contracts::kQolItemHandleStatusOk) return;
    if (api->register_item != nullptr) {
        qol::contracts::QolItemHandle handle{};
        (void)api->register_item(item, provider, &handle);
    }
}

bool begin_item_handle_invalidation(void* item, qol::contracts::QolItemHandle* checkpoint) {
    const auto* api = item_handle_api();
    return item != nullptr && checkpoint != nullptr && api != nullptr &&
           api->begin_invalidation != nullptr &&
           api->begin_invalidation(
               item, qol::contracts::kQolItemHandleOwnerAny, checkpoint) ==
               qol::contracts::kQolItemHandleStatusOk;
}

void finish_item_handle_invalidation(qol::contracts::QolItemHandle checkpoint,
                                     bool committed) {
    const auto* api = item_handle_api();
    if (api != nullptr && api->finish_invalidation != nullptr) {
        (void)api->finish_invalidation(checkpoint, committed ? 1 : 0);
    }
}

// D2c-2a：扩展背包经 capability 现取（不缓存跨帧指针）。能力缺失或 enabled()==false
// 与原 extension_bag_enabled()==false 等价（EB disabled）；调用方按 disabled 降级
// （与 D2b-1/D2b-2 口径一致）。本 TU 不再直接依赖 EB 头与 EB 符号（弹窗 UI 见
// 下方消费者侧内联）。
const qol::contracts::ExtensionBagApi* eb_active() {
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const QolResult result = g_sdk->capabilities.require(
        qol::contracts::kExtensionBagCapabilityId,
        QolVersionRange{static_cast<uint16_t>(qol::contracts::kExtensionBagVersionMajor),
                        static_cast<uint16_t>(qol::contracts::kExtensionBagVersionMinor)},
        &iface);
    if (!qol::contracts::succeeded(result)) return nullptr;
    const qol::contracts::ExtensionBagApi* eb =
        static_cast<const qol::contracts::ExtensionBagApi*>(iface);
    if (eb == nullptr || eb->enabled == nullptr || !eb->enabled()) return nullptr;
    return eb;
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

bool identify_extension_item(void* item, int32_t* out_bag, int32_t* out_slot) {
    const auto* eb = eb_active();
    qol::contracts::QolItemHandle handle{};
    return eb != nullptr && eb->identify_native_item != nullptr &&
           item_handle_for_hook_item(item, &handle) &&
           eb->identify_native_item(handle, out_bag, out_slot);
}

bool consume_extension_item(void* item) {
    const auto* eb = eb_active();
    qol::contracts::QolItemHandle handle{};
    return eb != nullptr && eb->consume_native_item != nullptr &&
           item_handle_for_hook_item(item, &handle) && eb->consume_native_item(handle);
}

bool remove_extension_item(void* item) {
    const auto* eb = eb_active();
    qol::contracts::QolItemHandle handle{};
    return eb != nullptr && eb->remove_native_item != nullptr &&
           item_handle_for_hook_item(item, &handle) && eb->remove_native_item(handle);
}

bool equip_extension_item(void* character, void* item, int32_t bag, int32_t slot,
                          int32_t equip_slot) {
    const auto* eb = eb_active();
    qol::contracts::QolItemHandle handle{};
    return eb != nullptr && eb->equip_projected_item != nullptr &&
           item_handle_for_hook_item(item, &handle) &&
           eb->equip_projected_item(character, handle, bag, slot, equip_slot);
}

int put_jewel_extension_items(void* equip_item, void* jewel_item,
                              Stage4JewelBackup backup) {
    const auto* eb = eb_active();
    qol::contracts::QolItemHandle equip_handle{};
    qol::contracts::QolItemHandle jewel_handle{};
    if (eb == nullptr || eb->put_jewel_native == nullptr ||
        !item_handle_for_hook_item(equip_item, &equip_handle) ||
        !item_handle_for_hook_item(jewel_item, &jewel_handle)) return 3;
    struct BackupContext {
        Stage4JewelBackup backup;
        void* equip_item;
        void* jewel_item;
    };
    static thread_local BackupContext* active_backup = nullptr;
    BackupContext context{backup, equip_item, jewel_item};
    const auto bridge = [](qol::contracts::QolItemHandle, qol::contracts::QolItemHandle) -> int32_t {
        return active_backup == nullptr || active_backup->backup == nullptr
                   ? 3
                   : active_backup->backup(active_backup->equip_item, active_backup->jewel_item);
    };
    BackupContext* previous = active_backup;
    active_backup = &context;
    const int result = eb->put_jewel_native(equip_handle, jewel_handle, bridge);
    active_backup = previous;
    return result;
}

// D2c-2a：原 EB port extension_bag_show_*_popup（TextData 6=背包已满 / 7=袋非空）在
// 消费者侧内联。契约 ExtensionBagApi 未导出弹窗入口（handle_original_bag_unequip 只
// 回传标志），弹窗为纯游戏 UI 调用，由本消费者自持，避免直接依赖 EB 符号。
void show_inventory_popup(int text_id) {
    if (g_sdk == nullptr || g_sdk->ui == nullptr || g_sdk->ui->show_text_popup == nullptr) return;
    (void)g_sdk->ui->show_text_popup(text_id);
}

std::mutex g_hook_mutex;
NativeHookFunType g_hook_func = nullptr;
NativeUnhookFunType g_unhook_func = nullptr;

HaveItemFn g_backup_have_item = nullptr;
GetItemCountFn g_backup_get_item_count = nullptr;
GetCumulateCountFn g_backup_get_cumulate_count = nullptr;
ConsumeItemFn g_backup_consume_item = nullptr;
RemoveItemFn g_backup_remove_item = nullptr;
SaveItemFn g_backup_save_item = nullptr;
InvenMoveItemFn g_backup_move_item = nullptr;
UiEquipRefreshItemAreaFn g_backup_refresh_item_area = nullptr;
EquipItemFromInvenToSlotFn g_backup_equip_item_from_inven_to_slot = nullptr;
PutJewelFn g_backup_put_jewel = nullptr;
IsHavingEmptySlotFn g_backup_is_having_empty_slot = nullptr;
UnequipFn g_backup_unequip_item_to_inven = nullptr;
ButtonEquipExeFn g_backup_button_equip_exe = nullptr;
ButtonDestroyExeFn g_backup_button_destroy_exe = nullptr;  // R-55 预演标记源
ButtonUnequipExeFn g_backup_button_unequip_exe = nullptr;
UiEquipOkConfirmUseItemFn g_backup_ok_confirm_use_item = nullptr;
// R-63 佣兵徽章专用使用按钮函数级接管：UIEquip_ButtonUseMercenarySealExe(0xb8144)。
UiEquipButtonUseMercenarySealExeFn g_backup_button_use_mercenary_seal_exe = nullptr;
// R-55 原版背包详情出售接管：UIEquip_OKDestroyItem(0xb83d0)。返回值仅按钮预演读取
// （弹窗展示金额），用 UiEquipOkDestroyItemFn（uint64_t()）捕获 x0。
UiEquipOkDestroyItemFn g_backup_ok_destroy_item = nullptr;
UiEquipEquipControlEventProcFn g_backup_equip_control_event_proc = nullptr;
// S2 写侧进位框架新增 hook（H-18/H-19/H-20/H-21）的 backup。
InvenSaveItemDirectFn g_backup_save_item_direct = nullptr;
ItemSystemDivideFn g_backup_item_system_divide = nullptr;
MakeItemFn g_backup_make_item = nullptr;
InvenRemoveItemDataFn g_backup_remove_item_data = nullptr;

std::atomic<uint64_t> g_consume_item_calls{0};
std::atomic<uint64_t> g_remove_item_calls{0};

thread_local bool g_in_have_item = false;
thread_local bool g_in_get_item_count = false;
thread_local bool g_in_is_having_empty_slot = false;
thread_local bool g_in_consume_item = false;
thread_local bool g_in_unequip_item_to_inven = false;
thread_local bool g_in_remove_item = false;
// R-55：UIEquip_ButtonDestroyExe 动态范围内（含其同步调用的 OKDestroyItem 预演）。
thread_local bool g_in_equip_destroy_button = false;
thread_local bool g_has_vanilla_sell_preview_context = false;
thread_local qol::contracts::QolItemDetailContext g_vanilla_sell_preview_context{};
thread_local int g_refresh_depth = 0;

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
// 已实现 5 个：INVEN_MoveItem、INVEN_SaveItemDirect、ITEMSYSTEM_Divide、
// INVEN_ConsumeItem、INVEN_RemoveItemData（H-21）。
//
// 逐函数语义表（16 函数 / 24 写点；语义=设/加/减，操作数=hook 内可得的数量来源）：
// 勘察基线：.tmp/dualmode-full.asm（llvm-objdump -d libgame.so，冻结基线）。
// | 函数                          | 写点 | 语义           | 操作数来源                     | 目标物品来源              | 状态 |
// | INVEN_MoveItem                | 4    | 加（合并）     | count 入参 + 目标槽旧全量      | target(bag,slot) 槽内对象 | 已实现 move_item_wrapper（原版 b 写确认后回写） |
// | INVEN_SaveItemDirect          | 3    | 加（入库合并） | 入参对象全量 + 槽内旧全量      | (bag,slot) 槽内对象       | 已实现 save_item_direct_wrapper；delta≡0 mod 128 退化 fail-closed 跳过 |
// | UIStore_BuyItem               | 2    | 设（购买量）   | UIInputItemCount 全局数量      | CopyAsNewUID 产物 x21     | 无需接管：0xd2588 写点 b=100（ITEM_IsRealBroken 装备 marker，非数量）；0xd25cc 写点 b=购买量，购买输入框 max=99（UIStore_ButtonBuyExe 0xd17cc `mov x3, #0x63`，UIInputItemCount_ButtonLeft/RightExe 在 [1,max] 滚动），≤127 b 写即全量；静态货架分支直接 SaveItem（数量由 MakeSale/MakeItem 生成，产物恒 b=1 完整） |
// | ITEMSYSTEM_Divide             | 2    | 设+减（拆堆）  | count 入参 + 源堆旧全量        | 返回新对象 + item 入参    | 已实现 item_system_divide_wrapper |
// | ITEMSYSTEM_CreateItem         | 2    | 设（初始化）   | 函数内常量                     | 新建对象                  | 无需接管：数量位写点仅 `mov w3,#0x1; SetBitValue(31,25,1)`（默认创建 1 个，0x10be9c+0x1a4 区）；另一 31/25 写点 `w3=0x64` 是装备 marker（非数量）；ABI 实为 `void* (int32_t category)` 单参（GAME_StartNewGame 0x1001fc w0=3/4/5 实证）；其余 SetBitValue 写类别 bit6..15（+0x08）、价值 bit0..24、佣兵随机 bit0..7，均不属数量语义 |
// | UIMix_StartMix                | 1    | 设（产物量）   | 配方区 +0x8（静态表数据）      | MIXSYSTEM_MakeItem out 对象 | 无需接管：产物量 = 配方表只读静态数据（模块无任何写点），原版数量域 ≤99 ≤127，b 写即全量；`cmp #0x1; b.le`（0xc0964 区）数量 ≤1 时连写点都不进 |
// | DEALSYSTEM_MakeSale           | 1    | 设（货架生成） | ITEMSYSTEM_MakeItem ×5         | 货架对象                  | 无需接管：唯一 31/25 写点 `w3=0x7e`（0xf6754 区）是上架装备 marker=126，非数量；数量产生全部经 ITEMSYSTEM_MakeItem（产物恒 b=1，无需回写）；语义修正：本函数是商店特卖货架生成，不是售出 |
// | GAME_StartNewGame             | 1    | 设（初始量）   | 函数内常量 5                   | 新建对象                  | 无需接管：唯一数量写点 `mov w3,#0x5; SetBitValue(31,25,5)`（0x100200 区），初始量 5 ≤127 b 写即全量；前两个 CreateItem（类别 3/4）无数量写 |
// | INVEN_RemoveItemData          | 1    | 减（批量删除） | category/count 入参            | 多堆遍历（bag0..5 顺序）  | 已实现 remove_item_data_wrapper：快照/重扫 + stage4_remove_item_data_plan 修正部分删堆（整删消失堆只求和）；遍历顺序已冻结（0x1040a8：外层 bag 0..5 × 内层槽，Getter 读 cum、`b.lt` 判部分删）；R-56 扩展桥接：物理实扣不足按 category 从扩展袋补扣 |
// | INVEN_SaveItemData            | 1    | 加（批量创建） | category/count 入参            | 多堆/空槽                 | 无需接管：可堆叠分支 `cmp w20,#0x62; b.gt`（0x104694 区）>98 每堆写 99、≤98 写 count，每笔 ≤99 ≤127 b 写即全量；不可堆叠分支每件数量 1；category=0 分支是加钱（INVEN_AddMoney），无数量写 |
// | INVEN_ConsumeItem             | 1    | 减（1）        | 恒 1 + 对象旧全量              | item 入参                 | 已实现 consume_item_wrapper（b≤1 借位预置 + 回写） |
// | ITEMSYSTEM_MakeItem           | 1    | 设（装备强化 marker，非数量） | CAL_Calculate 结果 | CreatePerfectItem 返回装备 | 勘误（反汇编 0x10c6c8）：原登记「设（创建量）count 入参 arg2」错误——真实 ABI 是 `void* (category, lookup_key, luck)`（0x10c6d8 `mov w23,w1` 用于静态表 uint16 匹配、0x10c6e4 存 w2 作品质门槛比较，均非数量）；唯一 31/25 写点 0x10ca3c 仅对可强化装备写强化 marker（可堆叠类别 bit0=0 不进该分支，产物数量恒由 CreateItem 写 b=1，S2 完整）。旧 make_item_wrapper 把 x1（lookup_key）当 count 回写，导致拾取 1 个得 lookup_key 个（药水 2/卷轴 3/材料 4，真机实证）；H-20 已整体移除，原 hook 名保留编号不复用 |
// | ITEMSYSTEM_ProcessUnpack      | 1    | 设（产物量）   | 拆包表 u8 字段（表项 +0x4）    | CreatePerfectItem 产物    | 无需接管：产物量 = 拆包表只读静态数据（模块无任何写点），原版数量域 ≤99 ≤127，b 写即全量；开放边界：静态表若含 >99 值需数据审计复核（VM 取证抽查） |
// | MAPITEMSYSTEM_CreateItem      | 1    | 设（生成量）   | count 入参（w1）               | CreateItem 产物（尾调 Add，返回值非对象） | 无需接管：4 个调用点 count 域全部为原版静态数据——UINpc_ExeNpcTask 0xc2f60 `mov w1,#1`、EVTSYSTEM_Process 0xfc710 事件表字段、QUESTSYSTEM_ApplyReward 0x1238e4 任务表 u16；掉落/任务数量设计域 ≤99 ≤127；开放边界：若未来发现丢弃/移仓路径把 S2 全量（>127）作 count 传入本函数则需复核（当前全量反汇编未见该路径） |
// | NetworkStore_InitializeMenuData | 1  | 设（货架量）   | 网络商品数据 +0x18 字段        | CreateItem 产物           | 待勘察：数量来自网络数据（0x15b7b0+0x25xx 写点 `[x25+0x18]` 解引用），离线不可达无法冻结值域；勘察方法：联网抓包/内存 dump 商品表 +0x18 域，或真机挂回调记录 |
// | NetworkStore_AddItem          | 1    | 设（接收量）   | 网络收货数据 +0x18 字段        | CreateItem 产物（返回值非对象） | 待勘察：写点 0x15d7cc 区 `w3=[收货数据+0x18]`，数量域同上不可达；勘察方法同上 |
// ---------------------------------------------------------------------------

// 物理库存包含 6 袋（含任务袋 5）；写侧读取经 Game Host 物理槽原语，避免扩展物化路径取锁。
constexpr int kInvenPhysicalBagCount = 6;
constexpr int kInvenSlotsPerBag = 16;
constexpr size_t kInvenBagStride = 0x80;

// 纯内存读物理袋槽物品指针（bag 0..5，含任务袋；越界/未就绪返回 nullptr）。
// 该函数只读取物理槽，不经过逻辑袋物化。
void* s2_physical_slot_item(int bag, int slot) {
    if (bag < 0 || bag >= kInvenPhysicalBagCount || slot < 0 || slot >= kInvenSlotsPerBag ||
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
    for (int bag = 0; bag < kInvenPhysicalBagCount; ++bag) {
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
    return item != nullptr && s2_writeback::allowed(item_count_extended_applied(),
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
    cap->new_full = s2_writeback::added_count(item_count_api(), cap->pre_full, cap->delta);
}

// backup 后确认回写：目标槽仍指向同一对象（防替换/释放后的 UAF 读）、原版确实
// 写了 b（post != pre 且等于原版对 new_full 的预测写）才回写 a+b；其余一律
// fail-closed 跳过并记日志（退化 delta≡0 mod 128、同类别不同 payload 拒并等均
// 落入此分支，不猜测）。
void s2_merge_writeback(const S2MergeCapture& cap, const char* op) {
    if (!cap.armed) return;
    if (s2_physical_slot_item(cap.bag, cap.slot) != cap.target) return;
    if (!s2_writeback_gate(cap.target)) return;
    const uint32_t post_field = read_count_field(cap.target);
    if (post_field == cap.pre_field) {
        if ((cap.delta % 128) == 0) {
            QOL_LOG_INFO(QolDomain::kInventory,
                                "S2 writeback %s degenerate skip target=%p delta=%u (delta%%128==0)",
                                op, cap.target, cap.delta);
        }
        return;
    }
    if (!s2_writeback::original_count_write_confirmed(item_count_api(), cap.pre_field, post_field,
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
    QOL_LOG_INFO(QolDomain::kInventory,
                        "S2 writeback %s merge target=%p %u+%u->%u field=0x%x",
                        op, cap.target, cap.pre_full, cap.delta, cap.new_full, written);
}

// The resource table uses this only as a non-null placeholder. The runtime claim is replaced by
// the Game Adapter-owned ABI wrapper before the hook transaction installs it.
void* find_item_wrapper(int32_t) { return nullptr; }

int extension_item_count(int32_t category) {
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    if (eb == nullptr) return 0;
    if (eb->native_call_active != nullptr && eb->native_call_active()) return 0;
    if (eb->for_each_logical_item == nullptr) return 0;
    struct Context { int32_t category; int count; } context{category, 0};
    eb->for_each_logical_item([](const qol::contracts::QolInventoryExtensionItemRef* ref,
                                 void* raw) -> int32_t {
        Context* context = static_cast<Context*>(raw);
        if (ref != nullptr && ref->category == context->category && ref->count > 0) {
            context->count += ref->count;
        }
        return 0;
    }, &context);
    return context.count;
}

int have_item_wrapper(int32_t category) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:INVEN_HaveItem"});
    if (g_backup_have_item == nullptr) {
        backup_missing("hook:INVEN_HaveItem");
        return 0;
    }
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    if (eb != nullptr && eb->native_call_active != nullptr && eb->native_call_active()) {
        return stage4_have_item_original_only(category, g_backup_have_item, g_in_have_item);
    }
    return stage4_have_item(category, g_backup_have_item,
                            eb != nullptr ? extension_item_count : nullptr,
                            g_in_have_item);
}

int get_item_count_wrapper(int32_t category) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:INVEN_GetItemCount"});
    if (g_backup_get_item_count == nullptr) {
        backup_missing("hook:INVEN_GetItemCount");
        return 0;
    }
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    if (eb != nullptr && eb->native_call_active != nullptr && eb->native_call_active()) {
        return stage4_get_item_count_original_only(category, g_backup_get_item_count,
                                                   g_in_get_item_count);
    }
    return stage4_get_item_count(category, g_backup_get_item_count,
                                 eb != nullptr ? extension_item_count : nullptr,
                                 g_in_get_item_count);
}

// S2 读侧统一入口（R-49）：原版 getter 只见 b 段（bits25–31），本 wrapper 对
// count-encoded 类别按模式视图解码（启用态 `count=128a+b`，关闭态只读 b 且不夹 99）。
// 门控判据沿用 R-40/R-41 的单源谓词 item_count_encoding，
// 判定不可用（kUnknown）时 fail-closed 直通 backup（R-46）；非可堆叠（装备
// marker/宝石选项/袋容量）不解释 bits22–24，一律 original-first。纪律：backup
// 是纯读原版函数（UTIL_GetBitValue/MEM_ReadUint8，反汇编 0x106094–0x106124
// 证实），不回调任何被 Hook 函数；wrapper 不取 g_virtual_bag_mtx、不读模块
// 状态，因此无需 TLS 直通守卫；数量字段经 item_count ABI 访问。分流逻辑在
// stage4_get_cumulate_count（纯函数，供 Host 断言）。
int get_cumulate_count_wrapper(void* item) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:ITEM_GetCumulateCount"});
    if (g_backup_get_cumulate_count == nullptr) {
        backup_missing("hook:ITEM_GetCumulateCount");
        return 0;
    }
    const qol::contracts::QolItemCountApi* item_count =
        g_game == nullptr ? nullptr : &g_game->item_count;
    return stage4_get_cumulate_count(item, item_count, g_backup_get_cumulate_count);
}

int is_having_empty_slot_wrapper(int32_t needed, int32_t include_task_bag) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:INVEN_IsHavingEmptySlot"});
    if (g_backup_is_having_empty_slot == nullptr) {
        backup_missing("hook:INVEN_IsHavingEmptySlot");
        return 0;
    }
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    if (eb != nullptr && eb->native_call_active != nullptr && eb->native_call_active()) {
        return stage4_is_having_empty_slot_original_only(
            needed, include_task_bag, g_backup_is_having_empty_slot,
            g_in_is_having_empty_slot);
    }
    return stage4_is_having_empty_slot(needed, include_task_bag,
                                        g_backup_is_having_empty_slot,
                                        (eb != nullptr && eb->has_empty_slots != nullptr)
                                            ? eb->has_empty_slots
                                            : nullptr,
                                        g_in_is_having_empty_slot);
}

int unequip_item_to_inven_wrapper(void* character, int32_t equip_slot) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:CHAR_UnequipItemToInven"});
    if (g_backup_unequip_item_to_inven == nullptr) {
        backup_missing("hook:CHAR_UnequipItemToInven");
        return 0;
    }
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    const int result = stage4_unequip_item_to_inven(character, equip_slot,
                                                    g_backup_unequip_item_to_inven,
                                                    (eb != nullptr &&
                                                     eb->adopt_unequipped_item != nullptr)
                                                        ? eb->adopt_unequipped_item
                                                        : nullptr,
                                                    g_in_unequip_item_to_inven);
    QOL_LOG_DEBUG_EVERY(QolDomain::kInventory, 60,
                        "UnequipItemToInven every_calls=60 equip_slot=%d result=%d recursive=%d",
                        equip_slot, result, g_in_unequip_item_to_inven ? 1 : 0);
    return result;
}

void consume_item_wrapper(void* item) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:INVEN_ConsumeItem"});
    const uint64_t call = g_consume_item_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    QOL_LOG_DEBUG_EVERY(QolDomain::kInventory, 60,
                            "ConsumeItem every_calls=60 call=%llu item=%p recursive=%d backup=%p",
                        static_cast<unsigned long long>(call), item,
                        g_in_consume_item ? 1 : 0,
                        reinterpret_cast<void*>(g_backup_consume_item));
    int extension_bag = -1;
    int extension_slot = -1;
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    const bool extension_item = eb != nullptr &&
                                identify_extension_item(item, &extension_bag, &extension_slot);
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
    const bool dispatched = stage4_consume_item(
        item,
        (eb != nullptr && eb->identify_native_item != nullptr) ? identify_extension_item : nullptr,
        (eb != nullptr && eb->consume_native_item != nullptr) ? consume_extension_item : nullptr,
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
            if (s2_writeback::original_count_write_confirmed(item_count_api(), borrow_pre_field,
                                                             post_field, 1)) {
                const uint32_t new_full = s2_writeback::decremented_count(borrow_pre_full);
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
    return;
}

int remove_item_wrapper(void* item) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:INVEN_RemoveItem"});
    const uint64_t call = g_remove_item_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    QOL_LOG_DEBUG_EVERY(QolDomain::kInventory, 60,
                            "RemoveItem every_calls=60 call=%llu item=%p recursive=%d backup=%p",
                        static_cast<unsigned long long>(call), item,
                        g_in_remove_item ? 1 : 0,
                        reinterpret_cast<void*>(g_backup_remove_item));
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    if (g_backup_remove_item == nullptr) {
        backup_missing("hook:INVEN_RemoveItem");
        return 0;
    }
    qol::contracts::QolItemHandle handle_checkpoint{};
    const bool handle_suspended = begin_item_handle_invalidation(item, &handle_checkpoint);
    const int result = stage4_remove_item(item,
                                          (eb != nullptr && eb->identify_native_item != nullptr)
                                              ? identify_extension_item
                                              : nullptr,
                                          (eb != nullptr && eb->remove_native_item != nullptr)
                                              ? remove_extension_item
                                              : nullptr,
                                          g_backup_remove_item,
                                          g_in_remove_item);
    if (handle_suspended) finish_item_handle_invalidation(handle_checkpoint, result != 0);
    else if (result != 0) invalidate_item_handle(item);
    return result;
}

int save_item_wrapper(void* item) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:INVEN_SaveItem"});
    // INVEN_SaveItem 是所有"已创建物品放入背包"的唯一漏斗（任务奖励/事件
    // 发奖/开箱/拾取/商店/合成等 18 条调用点）。原版全袋无空位时
    // FindSaveSlot 失败返回 0 → 上层会掉地/静默消失。此处 backup 返回 0 时
    // 转扩展袋空位接管（adopt 进扩展槽），返回 1 让上层按成功处理，
    // 发奖代码零改动即支持扩展背包。
    // 商店扩展视图会把窗口原版袋容量字临时放大；买入等原版写入若以放大
    // 容量扫空槽会把物品写进超过真实容量的槽位。backup 前先恢复商店投影。
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    if (eb != nullptr) {
        if (eb->store_restore_for_original_write != nullptr) {
            eb->store_restore_for_original_write();
        }
        // S2 拾取并入扩展同类堆（R-52）：物品可堆叠且扩展袋存在同 category、
        // 归一化 payload 相同的堆，且模式视图相加不超上限时直接并入扩展堆并
        // 上报成功；否则保持原版入库（含原版同类堆合并）。修复拾取可堆叠
        // 物品在原版背包新开格、不并入扩展同类堆的缺陷。
        qol::contracts::QolItemHandle handle{};
        if (eb->merge_native_item != nullptr && item_handle_for_hook_item(item, &handle) &&
            eb->merge_native_item(handle)) {
            invalidate_item_handle(item);
            return 1;
        }
    }
    if (g_backup_save_item == nullptr) {
        backup_missing("hook:INVEN_SaveItem");
        return 0;
    }
    const int result = g_backup_save_item(item);
    if (result != 0) return result;
    qol::contracts::QolItemHandle adopt_handle{};
    if (eb != nullptr && eb->adopt_native_item != nullptr &&
        item_handle_for_hook_item(item, &adopt_handle) && eb->adopt_native_item(adopt_handle)) {
        transfer_item_handle_to_extension_bag(item);
        return 1;
    }
    return 0;
}

// ---- S2 写侧进位框架新增 hook（H-18..H-20）----
// 共同纪律：original-first、失败/不确定回退原版结果；wrapper 不取
// g_virtual_bag_mtx、不调用任何被 Hook 函数（R-44）；类别与 999 应用态门控 fail-closed。
// 仅 999 已应用时模块回写 a+b；关闭态交还原版只写 b。

int save_item_direct_wrapper(void* item, int32_t bag, int32_t slot) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:INVEN_SaveItemDirect"});
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

void* item_system_divide_wrapper(void* item, int32_t count) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:ITEMSYSTEM_Divide"});
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
        const uint32_t remain = s2_writeback::subtracted_count(
            pre_full, static_cast<uint32_t>(count));
        if (s2_writeback::original_count_write_confirmed(item_count_api(), pre_field, post_field,
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

// INVEN_RemoveItemData 快照条目上限：6 袋 × 16 槽。
constexpr int kS2RemoveDataMaxEntries = kInvenPhysicalBagCount * kInvenSlotsPerBag;

struct S2RemoveDataCapture {
    bool armed = false;
    int entry_count = 0;
    void* items[kS2RemoveDataMaxEntries] = {};
    int bags[kS2RemoveDataMaxEntries] = {};
    int slots[kS2RemoveDataMaxEntries] = {};
    uint32_t pre_full[kS2RemoveDataMaxEntries] = {};
};

// backup 前快照：启用态 + count>0 时记录全部「count-encoded 且类别匹配」的堆
// （原版写点只改这些堆；纯内存读，不取 g_virtual_bag_mtx，R-44）。类别判据与
// 原版一致：+0x08 的 bit6..15（item_count_encoding 用同一字段）。
void s2_capture_remove_data(int32_t category, int32_t count, S2RemoveDataCapture* cap) {
    cap->armed = false;
    cap->entry_count = 0;
    if (category < 0 || count <= 0 || !item_count_extended_applied()) return;
    for (int bag = 0; bag < kInvenPhysicalBagCount; ++bag) {
        for (int slot = 0; slot < kInvenSlotsPerBag; ++slot) {
            void* item = s2_physical_slot_item(bag, slot);
            if (item == nullptr) continue;
            if (!item_count_is_encoded(item)) continue;
            if (g_item_facts == nullptr || g_item_facts->category == nullptr) continue;
            qol::contracts::QolItemHandle item_handle{};
            int32_t item_category = -1;
            if (!item_handle_for_hook_item(item, &item_handle) ||
                !qol::contracts::succeeded(g_item_facts->category(item_handle, &item_category)) ||
                item_category != category) {
                continue;
            }
            if (cap->entry_count >= kS2RemoveDataMaxEntries) return;  // 不可能：上限即全袋
            cap->items[cap->entry_count] = item;
            cap->bags[cap->entry_count] = bag;
            cap->slots[cap->entry_count] = slot;
            if (!item_count_effective(item, &cap->pre_full[cap->entry_count])) continue;
            ++cap->entry_count;
        }
    }
    cap->armed = cap->entry_count > 0;
}

// 999 已应用时，backup 后重扫 + 修正：消失堆（整删）只求和；唯一缩减堆经
// stage4_remove_item_data_plan 计算 remain，post 与快照不一致（b 截断或旧 a
// 残留）时回写 a+b。关闭态由原版只写 b，模块跳过此修正；失败/矛盾一律 fail-closed。
void s2_remove_data_writeback(const S2RemoveDataCapture& cap, int32_t category,
                              int32_t count) {
    if (!cap.armed || !item_count_extended_applied()) return;
    if (count <= 0) return;
    Stage4RemoveDataEntry pre[kS2RemoveDataMaxEntries] = {};
    Stage4RemoveDataPost post[kS2RemoveDataMaxEntries] = {};
    for (int index = 0; index < cap.entry_count; ++index) {
        pre[index] = {cap.items[index], cap.pre_full[index]};
        void* cur = s2_physical_slot_item(cap.bags[index], cap.slots[index]);
        if (cur != cap.items[index]) {
            post[index] = {nullptr, 0};
        } else {
            uint32_t effective = 0;
            if (!item_count_effective(cur, &effective)) return;
            post[index] = {cur, effective};
        }
    }
    const Stage4RemoveDataPlan plan =
        stage4_remove_item_data_plan(item_count_api(), pre, post, cap.entry_count, count);
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

int remove_item_data_wrapper(int32_t category, int32_t count) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:INVEN_RemoveItemData"});
    // INVEN_RemoveItemData（减语义，批量按类别删除；反汇编 0x1040a8）：顺序遍历
    // 6 袋，匹配堆整删（INVEN_RemoveItemDirect）累计 w22，最后一堆部分删除时
    // `SetBitValue(+0x10, 31, 25, cum + w22 - count)`。cum 经 H-17 getter（启用态
    // = S2 全量），算术正确但 b 写对 remain>127 或旧 a 残留丢进位 → original-first
    // 快照/重扫修正。调用方：MIXSYSTEM_UseStuff（合成扣料）、QUESTSYSTEM
    // （任务物品回收）、UIMix/ProcessUnpack/SaveItemData 回滚、NetworkStore。
    // count<=0 语义未冻结（-1 分支存在），不快照不修正；扩展袋对象不参与物理袋
    // 快照，原版只删原版堆。
    //
    // R-56 扩展桥接：原版 INVEN_RemoveItemData 只遍历物理袋 0..5，扩展袋对象不
    // 参与；与 H-01..H-05 的 original-first + 扩展兜底语义对齐，物理实扣不足的
    // 部分按 category 从扩展袋补扣（合成药水/宝石孔/混沌/传说等按类别批量扣料即
    // 自动生效）。物理实扣 = 调用前后 H-03 总数差（H-03 = 物理 + 扩展，原版不动
    // 扩展，故差值即物理实扣）。
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    const bool extension_fallback = count > 0 && category > 0 && eb != nullptr;
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
            stage4_remove_data_extension_shortfall(total_before, total_after, count);
        if (shortfall > 0 && eb->consume_category != nullptr) {
            const int consumed = eb->consume_category(category, shortfall);
            if (consumed < shortfall) {
                QOL_LOG_INFO(QolDomain::kInventory,
                                    "RemoveItemData extension fallback category=%d "
                                    "need=%d consumed=%d",
                                    category, shortfall, consumed);
            }
        }
    }
    return result;
}

void refresh_item_area_wrapper() {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:UIEquip_RefreshItemArea"});
    if (g_backup_refresh_item_area == nullptr) {
        backup_missing("hook:UIEquip_RefreshItemArea");
        return;
    }
    if (g_refresh_depth > 0) {
        inventory_native_hook_call_refresh_item_area_original();
        return;
    }
    ++g_refresh_depth;
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    if (eb != nullptr && eb->refresh_item_area_with_gate != nullptr) {
        eb->refresh_item_area_with_gate();
    } else {
        inventory_native_hook_call_refresh_item_area_original();
    }
    --g_refresh_depth;
}

void log_move_item_observation(const char* phase, void* item, int count, int target_bag,
                               int target_slot,
                               const qol::contracts::ExtensionBagMoveObservation& observation) {
    QOL_LOG_INFO(QolDomain::kInventory,
        "MoveItem %s item=%p count=%d target=%d/%d caller=unknown "
        "extension=%d/%d/%d handle=%u owner_state=%d view=%d session=%d source=%d/%d source_handle=%u/%u target_handle=%u/%u "
        "digest=0x%llx nonnull=%d physical_valid=%d",
        phase, item, count, target_bag, target_slot,
        observation.extension_item ? 1 : 0,
        observation.extension_bag, observation.extension_slot, observation.extension_handle,
        observation.ownership_state, observation.module_view_index,
        observation.drag_session_active ? 1 : 0, observation.source_bag, observation.source_slot,
        observation.source_item.slot, observation.source_item.generation,
        observation.target_item.slot, observation.target_item.generation,
        static_cast<unsigned long long>(observation.physical_digest), observation.physical_nonnull,
        observation.physical_valid ? 1 : 0);
}

int move_item_wrapper(void* item, int count, int target_bag, int target_slot) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:INVEN_MoveItem"});
    // 身份与取证数据在 g_virtual_bag_mtx 短临界区内采集，返回后立即放锁；禁止持锁
    // 调 backup，因为原版函数可能进入 RemoveItem/SaveItem/ConsumeItem hooks 再 try_lock。
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    qol::contracts::ExtensionBagMoveObservation before{};
    qol::contracts::QolItemHandle item_handle{};
    const bool captured = eb != nullptr && eb->capture_move_item_observation != nullptr &&
                          item_handle_for_hook_item(item, &item_handle) &&
                          eb->capture_move_item_observation(item_handle, target_bag, target_slot,
                                                            &before);
    // 仅允许装备交换 wrapper 主动借入物理槽时通过；普通投影/拖动仍必须
    // 拒绝 backup，避免扩展对象进入原版移动链。该例外不放宽其它 caller。
    const bool internal_equip_active =
        eb != nullptr && eb->internal_equip_active != nullptr && eb->internal_equip_active();
    if (captured && before.extension_item && !internal_equip_active) {
        QOL_LOG_ERROR(QolDomain::kInventory,
            "MoveItem GUARD reject item=%p count=%d target=%d/%d caller=unknown "
            "extension=%d/%d handle=%u owner_state=%d view=%d session=%d source=%d/%d source_handle=%u/%u target_handle=%u/%u "
            "digest=0x%llx nonnull=%d physical_valid=%d backup=skipped",
            item, count, target_bag, target_slot,
            before.extension_bag,
            before.extension_slot, before.extension_handle, before.ownership_state,
            before.module_view_index, before.drag_session_active ? 1 : 0,
            before.source_bag, before.source_slot, before.source_item.slot,
            before.source_item.generation, before.target_item.slot,
            before.target_item.generation,
            static_cast<unsigned long long>(before.physical_digest), before.physical_nonnull,
            before.physical_valid ? 1 : 0);
        return 0;
    }

    const bool observe = captured &&
                         (before.drag_session_active || before.module_view_index >= 0);
    if (observe) {
        log_move_item_observation("pre", item, count, target_bag, target_slot, before);
    }
    // S2 写侧合并进位（加语义；R-45/R-46/R-49）：backup 前采集目标槽旧全量，
    // backup 后确认原版确实写了 b（post != pre 且等于原版预测写）再回写 a+b。
    // 空槽移动无合并写；扩展对象在上方 GUARD 已拒绝 backup，不进入本路径。
    S2MergeCapture move_merge{};
    if (g_backup_move_item != nullptr) {
        s2_capture_merge(item, target_bag, target_slot, count, &move_merge);
    }
    const int result = call_backup("hook:INVEN_MoveItem", g_backup_move_item,
                                   item, count, target_bag, target_slot);
    s2_merge_writeback(move_merge, "MoveItem");
    if (observe) {
        qol::contracts::ExtensionBagMoveObservation after{};
        if (eb != nullptr && eb->capture_move_item_observation != nullptr &&
            item_handle_for_hook_item(item, &item_handle) &&
            eb->capture_move_item_observation(item_handle, target_bag, target_slot, &after)) {
            log_move_item_observation("post", item, count, target_bag, target_slot, after);
        }
    } else {
        QOL_LOG_DEBUG_EVERY(QolDomain::kInventory, 60,
                            "MoveItem every_calls=60 passthrough item=%p count=%d target=%d/%d "
                            "caller=unknown result=%d",
                            item, count, target_bag, target_slot,
                            result);
    }
    return result;
}

int equip_item_from_inven_to_slot_wrapper(void* character, int32_t bag, int32_t slot,
                                          int32_t equip_slot) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:CHAR_EquipItemFromInvenToSlot"});
    if (g_backup_equip_item_from_inven_to_slot == nullptr) {
        backup_missing("hook:CHAR_EquipItemFromInvenToSlot");
        return 0;
    }
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    if (eb != nullptr && eb->internal_equip_active != nullptr && eb->internal_equip_active()) {
        return call_backup("hook:CHAR_EquipItemFromInvenToSlot",
                           g_backup_equip_item_from_inven_to_slot,
                           character, bag, slot, equip_slot);
    }
    const int result = stage4_equip_item(
        character, bag, slot, equip_slot, inventory_item_at,
        (eb != nullptr && eb->identify_native_item != nullptr) ? identify_extension_item : nullptr,
        (eb != nullptr && eb->equip_projected_item != nullptr) ? equip_extension_item : nullptr,
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
    if (g_backup_put_jewel == nullptr) {
        backup_missing("hook:ITEMSYSTEM_PutJewel");
        return 3;
    }
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    return stage4_put_jewel(
        equip_item, jewel_item,
        (eb != nullptr && eb->identify_native_item != nullptr) ? identify_extension_item : nullptr,
        g_backup_put_jewel,
        (eb != nullptr && eb->put_jewel_native != nullptr) ? put_jewel_extension_items : nullptr);
}

void button_equip_exe_wrapper(void* button) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:UIEquip_ButtonEquipExe"});
    // 装备按钮函数级接管：详情物品为扩展槽背包物品时在原函数内联删源
    // （b7e04）之前分流，源物品销毁与袋表移交由扩展事务完成；其余一律
    // 走原函数，保持完全原版流程。
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    const qol::contracts::ExtensionBagEquipButtonResult result =
        (eb != nullptr && eb->handle_backpack_button_equip_result != nullptr)
            ? eb->handle_backpack_button_equip_result()
            : qol::contracts::ExtensionBagEquipButtonResult::kNotExtension;
    if (result == qol::contracts::ExtensionBagEquipButtonResult::kHandled) return;
    if (result == qol::contracts::ExtensionBagEquipButtonResult::kBlocked) {
        QOL_LOG_WARN(QolDomain::kInventory,
                            "ButtonEquipExe extension item blocked; original backup skipped");
        return;
    }
    call_backup("hook:UIEquip_ButtonEquipExe", g_backup_button_equip_exe, button);
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
    if (g_sdk != nullptr && g_sdk->hooks.intercept_blocks != nullptr &&
        g_sdk->hooks.intercept_blocks("hook:UIEquip_ButtonDestroyExe", nullptr) > 0) {
        QOL_LOG_INFO(QolDomain::kInventory, "sell button blocked by gate button=%p", button);
        return;
    }
    const bool previous = g_in_equip_destroy_button;
    g_in_equip_destroy_button = true;
    call_backup("hook:UIEquip_ButtonDestroyExe", g_backup_button_destroy_exe, button);
    g_in_equip_destroy_button = previous;
}

void button_unequip_exe_wrapper(void* button) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:UIEquip_ButtonUnequipExe"});
    // 卸下按钮函数级接管：desc_type=1 卸袋（原版袋/扩展袋）时模块接管，
    // 原版背包满时把袋对象放回其他袋行或收编扩展空位；物品绝不消失。
    // 卸装备与其他场景一律走原函数。
    bool no_space = false;
    bool not_empty = false;
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    if (eb != nullptr && eb->handle_original_bag_unequip != nullptr &&
        eb->handle_original_bag_unequip(&no_space, &not_empty)) {
        if (not_empty) {
            show_inventory_popup(7);  // TextData 7 = "袋非空"
        } else if (no_space) {
            show_inventory_popup(6);  // TextData 6 = "背包已满"
        }
        return;
    }
    call_backup("hook:UIEquip_ButtonUnequipExe", g_backup_button_unequip_exe, button);
}

void ok_confirm_use_item_wrapper(void* item) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:UIEquip_OKConfirmUseItem"});
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    qol::contracts::QolItemHandle use_handle{};
    if (eb != nullptr && eb->handle_confirm_use_item != nullptr &&
        item_handle_for_hook_item(item, &use_handle) &&
        eb->handle_confirm_use_item(use_handle)) {
        return;
    }
    if (g_backup_ok_confirm_use_item == nullptr) {
        backup_missing("hook:UIEquip_OKConfirmUseItem");
        return;
    }
    call_backup("hook:UIEquip_OKConfirmUseItem", g_backup_ok_confirm_use_item, item);
}

// R-63 佣兵徽章使用按钮函数级接管：详情物品为扩展袋佣兵徽章时由扩展侧接管
// （用模块详情身份调原版 MakeMercenary，消耗交给已 Hook 的 INVEN_RemoveItem）；
// 其余（原版袋物品/非佣兵徽章）一律透传原函数，保持完全原版流程。
void button_use_mercenary_seal_exe_wrapper(void* button) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:UIEquip_ButtonUseMercenarySealExe"});
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    if (eb != nullptr && eb->handle_use_mercenary_seal != nullptr &&
        eb->handle_use_mercenary_seal()) {
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
    if (bag < 0 || bag >= kInvenPhysicalBagCount || slot < 0 ||
        slot >= kInvenSlotsPerBag || detail.item.slot == 0 || detail.item.generation == 0) {
        g_has_vanilla_sell_preview_context = false;
        return run_backup();
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

uint64_t equip_control_event_proc_wrapper(void* control, uint64_t event, void* x2, void* param) {
    const qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:UIEquip_EquipControlEventProc"});
    if (g_backup_equip_control_event_proc == nullptr) {
        backup_missing("hook:UIEquip_EquipControlEventProc");
        return 0;
    }
    uint64_t original_result = 0;
    const qol::contracts::ExtensionBagApi* eb = eb_active();
    const qol::contracts::ExtensionBagEquipEventResult result =
        (eb != nullptr && eb->handle_equip_control_event != nullptr)
            ? eb->handle_equip_control_event(control, event, x2, param,
                                             g_backup_equip_control_event_proc, &original_result)
            : qol::contracts::ExtensionBagEquipEventResult::kNotExtension;
    if (result == qol::contracts::ExtensionBagEquipEventResult::kHandled) return original_result;
    if (result == qol::contracts::ExtensionBagEquipEventResult::kBlocked) return 0;
    return call_backup("hook:UIEquip_EquipControlEventProc",
                       g_backup_equip_control_event_proc, control, event, x2, param);
}

// ---------------------------------------------------------------------------
// 声明式资源声明（B2a-1）：24 条入口 hook 的 claims 表。wrapper 业务体在本 TU，
// handler 取址只能在此处；同插件的 plugin.cpp 经本目录 claims.h 直接引用
// inventory_hooks_claims / inventory_hooks_bind_backup 取得本表并交还 backup。
// 安装顺序、失败回滚与物理还原归宿主声明式引擎（hooks/install + hooks/transaction +
// host/plugin_registry），本 TU 不再自带安装引擎。
// ---------------------------------------------------------------------------
constexpr const char* kClaimOwner = "inventory_hooks";
constexpr const char* kClaimReason =
    "库存行为 wrapper 的函数级入口：扩展袋读改写、S2 写侧进位/借位、出售门禁与 "
    "R-55/R-56 回调层接管均需在函数入口分流；业务体随 D2c-2a 迁入本插件（plugins/inventory_hooks）";

#define INVENTORY_HOOK_CLAIM(CLAIM_ID, CLAIM_SYMBOL, HANDLER)                          \
    {CLAIM_ID, CLAIM_SYMBOL, qol::contracts::ResourceMechanism::kEntryHook,           \
     qol::contracts::ResourcePolicy::kPrivate, qol::contracts::ResourceRequirement::kRequired, \
     qol::contracts::ResourceFailure::kFailClosed, true, kClaimOwner, kClaimReason,    \
     nullptr, 120, reinterpret_cast<void*>(&HANDLER)}

const qol::contracts::ResourceClaim kInventoryHookClaims[] = {
    INVENTORY_HOOK_CLAIM("hook:INVEN_FindItem", "F_FIND_ITEM_VMA", find_item_wrapper),
    INVENTORY_HOOK_CLAIM("hook:INVEN_HaveItem", "F_INVEN_HAVE_ITEM_VMA", have_item_wrapper),
    INVENTORY_HOOK_CLAIM("hook:INVEN_GetItemCount", "F_INVEN_GET_ITEM_COUNT_VMA",
                         get_item_count_wrapper),
    INVENTORY_HOOK_CLAIM("hook:ITEM_GetCumulateCount", "F_GET_CUMULATE_COUNT_VMA",
                         get_cumulate_count_wrapper),
    INVENTORY_HOOK_CLAIM("hook:INVEN_ConsumeItem", "F_CONSUME_ITEM_VMA", consume_item_wrapper),
    INVENTORY_HOOK_CLAIM("hook:INVEN_RemoveItem", "F_REMOVE_ITEM_VMA", remove_item_wrapper),
    INVENTORY_HOOK_CLAIM("hook:CHAR_EquipItemFromInvenToSlot",
                         "F_EQUIP_ITEM_FROM_INVEN_TO_SLOT_VMA",
                         equip_item_from_inven_to_slot_wrapper),
    INVENTORY_HOOK_CLAIM("hook:ITEMSYSTEM_PutJewel", "F_PUT_JEWEL_VMA", put_jewel_wrapper),
    INVENTORY_HOOK_CLAIM("hook:INVEN_IsHavingEmptySlot", "F_INVEN_IS_HAVING_EMPTY_SLOT_VMA",
                         is_having_empty_slot_wrapper),
    INVENTORY_HOOK_CLAIM("hook:CHAR_UnequipItemToInven", "F_UNEQUIP_VMA",
                         unequip_item_to_inven_wrapper),
    INVENTORY_HOOK_CLAIM("hook:UIEquip_ButtonEquipExe", "F_UIEQUIP_BUTTON_EQUIP_EXE_VMA",
                         button_equip_exe_wrapper),
    INVENTORY_HOOK_CLAIM("hook:UIEquip_ButtonUnequipExe", "F_UIEQUIP_BUTTON_UNEQUIP_EXE_VMA",
                         button_unequip_exe_wrapper),
    INVENTORY_HOOK_CLAIM("hook:UIEquip_ButtonDestroyExe", "F_UIEQUIP_BUTTON_DESTROY_EXE_VMA",
                         button_destroy_exe_wrapper),
    INVENTORY_HOOK_CLAIM("hook:UIEquip_OKConfirmUseItem", "F_UIEQUIP_OK_CONFIRM_USE_ITEM_VMA",
                         ok_confirm_use_item_wrapper),
    INVENTORY_HOOK_CLAIM("hook:UIEquip_OKDestroyItem", "F_UIEQUIP_OK_DESTROY_ITEM_VMA",
                         ok_destroy_item_wrapper),
    INVENTORY_HOOK_CLAIM("hook:UIEquip_ButtonUseMercenarySealExe",
                         "F_UIEQUIP_BUTTON_USE_MERCENARY_SEAL_EXE_VMA",
                         button_use_mercenary_seal_exe_wrapper),
    INVENTORY_HOOK_CLAIM("hook:INVEN_SaveItem", "F_INVEN_SAVE_ITEM_VMA", save_item_wrapper),
    INVENTORY_HOOK_CLAIM("hook:INVEN_MoveItem", "F_INVEN_MOVE_ITEM_VMA", move_item_wrapper),
    INVENTORY_HOOK_CLAIM("hook:UIEquip_EquipControlEventProc",
                         "F_UIEQUIP_EQUIP_CONTROL_EVENT_PROC_VMA",
                         equip_control_event_proc_wrapper),
    INVENTORY_HOOK_CLAIM("hook:UIEquip_RefreshItemArea", "F_UIEQUIP_REFRESH_ITEM_AREA_VMA",
                         refresh_item_area_wrapper),
    INVENTORY_HOOK_CLAIM("hook:INVEN_SaveItemDirect", "F_INVEN_SAVE_ITEM_DIRECT_VMA",
                         save_item_direct_wrapper),
    INVENTORY_HOOK_CLAIM("hook:ITEMSYSTEM_Divide", "F_ITEMSYSTEM_DIVIDE_VMA",
                         item_system_divide_wrapper),
    INVENTORY_HOOK_CLAIM("hook:ITEMSYSTEM_MakeItem", "F_MAKE_ITEM_VMA", make_item_wrapper),
    INVENTORY_HOOK_CLAIM("hook:INVEN_RemoveItemData", "F_INVEN_REMOVE_ITEM_DATA_VMA",
                         remove_item_data_wrapper),
};
#undef INVENTORY_HOOK_CLAIM

constexpr uint32_t kInventoryHookClaimCount =
    sizeof(kInventoryHookClaims) / sizeof(kInventoryHookClaims[0]);
std::array<qol::contracts::ResourceClaim, kInventoryHookClaimCount> g_runtime_hook_claims{};

// claim.handler → 该 wrapper 的 backup 槽（原有 g_backup_* 全局）。以 handler 做唯一映射，
// 避免 id 表与 backup 表两处重复维护；未登记 handler 返回 nullptr，桥按 fail-closed 处理。
// g_backup_* 是有类型函数指针，槽地址统一 reinterpret_cast<void**> 交给机制层。
void** backup_slot_for_handler(void* handler) {
#define BACKUP_SLOT(HANDLER, SLOT) \
    if (handler == reinterpret_cast<void*>(&HANDLER)) return reinterpret_cast<void**>(&SLOT)
    BACKUP_SLOT(have_item_wrapper, g_backup_have_item);
    BACKUP_SLOT(get_item_count_wrapper, g_backup_get_item_count);
    BACKUP_SLOT(get_cumulate_count_wrapper, g_backup_get_cumulate_count);
    BACKUP_SLOT(consume_item_wrapper, g_backup_consume_item);
    BACKUP_SLOT(remove_item_wrapper, g_backup_remove_item);
    BACKUP_SLOT(equip_item_from_inven_to_slot_wrapper, g_backup_equip_item_from_inven_to_slot);
    BACKUP_SLOT(put_jewel_wrapper, g_backup_put_jewel);
    BACKUP_SLOT(is_having_empty_slot_wrapper, g_backup_is_having_empty_slot);
    BACKUP_SLOT(unequip_item_to_inven_wrapper, g_backup_unequip_item_to_inven);
    BACKUP_SLOT(button_equip_exe_wrapper, g_backup_button_equip_exe);
    BACKUP_SLOT(button_unequip_exe_wrapper, g_backup_button_unequip_exe);
    BACKUP_SLOT(button_destroy_exe_wrapper, g_backup_button_destroy_exe);
    BACKUP_SLOT(ok_confirm_use_item_wrapper, g_backup_ok_confirm_use_item);
    BACKUP_SLOT(ok_destroy_item_wrapper, g_backup_ok_destroy_item);
    BACKUP_SLOT(button_use_mercenary_seal_exe_wrapper, g_backup_button_use_mercenary_seal_exe);
    BACKUP_SLOT(save_item_wrapper, g_backup_save_item);
    BACKUP_SLOT(move_item_wrapper, g_backup_move_item);
    BACKUP_SLOT(equip_control_event_proc_wrapper, g_backup_equip_control_event_proc);
    BACKUP_SLOT(refresh_item_area_wrapper, g_backup_refresh_item_area);
    BACKUP_SLOT(save_item_direct_wrapper, g_backup_save_item_direct);
    BACKUP_SLOT(item_system_divide_wrapper, g_backup_item_system_divide);
    BACKUP_SLOT(make_item_wrapper, g_backup_make_item);
    BACKUP_SLOT(remove_item_data_wrapper, g_backup_remove_item_data);
#undef BACKUP_SLOT
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// 全局桥：插件 plugins/inventory_hooks 的访问入口（双向不 include，见 plugin.cpp 头注释）。
// 清除阶段：B2c 业务迁入 plugins/inventory_hooks 后两函数随业务迁入并删除。
// ---------------------------------------------------------------------------

const qol::contracts::ResourceClaim* inventory_hooks_claims(uint32_t* out_count) {
    if (out_count != nullptr) *out_count = kInventoryHookClaimCount;
    for (uint32_t index = 0; index < kInventoryHookClaimCount; ++index) {
        g_runtime_hook_claims[index] = kInventoryHookClaims[index];
    }
    if (g_game != nullptr && g_game->item_abi.find_item_wrapper != nullptr) {
        for (auto& claim : g_runtime_hook_claims) {
            if (std::strcmp(claim.id, "hook:INVEN_FindItem") == 0) {
                claim.handler = reinterpret_cast<void*>(g_game->item_abi.find_item_wrapper);
                break;
            }
        }
    }
    return g_runtime_hook_claims.data();
}

int32_t inventory_hooks_bind_backup(const char* resource_id, void* backup) {
    // 空 backup 或未知 id 一律非 0 → 宿主逆序回滚并置 failed（fail-closed）。
    if (resource_id == nullptr || backup == nullptr) return 1;
    for (const qol::contracts::ResourceClaim& claim : g_runtime_hook_claims) {
        if (std::strcmp(claim.id, resource_id) != 0) continue;
        if (std::strcmp(claim.id, "hook:INVEN_FindItem") == 0 && g_game != nullptr &&
            g_game->item_abi.bind_find_item_backup != nullptr &&
            claim.handler == reinterpret_cast<void*>(g_game->item_abi.find_item_wrapper)) {
            return g_game->item_abi.bind_find_item_backup(
                       reinterpret_cast<qol::contracts::QolFindItemAbiFn>(backup)).ok == kQolOk
                       ? 0 : 1;
        }
        void** slot = backup_slot_for_handler(claim.handler);
        if (slot == nullptr) return 1;
        *slot = backup;
        return 0;
    }
    QOL_LOG_ERROR(QolDomain::kInventory,
                  "inventory_hooks bind backup: unknown resource id %s", resource_id);
    return 1;
}

bool inventory_native_hook_call_refresh_item_area_original() {
    const UiEquipRefreshItemAreaFn original = g_backup_refresh_item_area;
    if (original == nullptr) {
        backup_missing("hook:UIEquip_RefreshItemArea");
        return false;
    }
    original();
    return true;
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
    for (uint32_t index = 0; index < kInventoryHookClaimCount; ++index) {
        g_runtime_hook_claims[index] = kInventoryHookClaims[index];
        if (std::strcmp(g_runtime_hook_claims[index].id, "hook:INVEN_FindItem") == 0 &&
            game->item_abi.find_item_wrapper != nullptr) {
            g_runtime_hook_claims[index].handler =
                reinterpret_cast<void*>(game->item_abi.find_item_wrapper);
        }
    }
    return true;
}

bool inventory_native_hook_bind_item_facts(const qol::contracts::ItemFactsApi* facts) {
    g_item_facts = facts;
    return facts != nullptr;
}

void inventory_native_hook_on_api(const NativeAPIEntries* entries) {
    if (entries == nullptr || entries->hook_func == nullptr || entries->unhook_func == nullptr) {
        QOL_LOG_ERROR(QolDomain::kInventory, "native_init received invalid API entries");
        return;
    }
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (entries->version < kNativeApiVersion) {
        QOL_LOG_ERROR(QolDomain::kInventory,
                            "native API version unsupported: %u", entries->version);
        return;
    }
    g_hook_func = entries->hook_func;
    g_unhook_func = entries->unhook_func;
    QOL_LOG_INFO(QolDomain::kInventory,
                        "native_init called api_version=%u hook_func=%p",
                        entries->version, reinterpret_cast<void*>(g_hook_func));
    // 资源安装由宿主声明式引擎在 assembly 装配期执行（plugins/inventory_hooks 插件）。
}

void inventory_native_hook_on_module_loaded(const char* name, void*) {
    if (name == nullptr || std::strstr(name, "libgame.so") == nullptr) return;
    QOL_LOG_INFO(QolDomain::kInventory, "module loaded: %s", name);
    // 纯日志：安装时机改由 assembly 声明式装配决定，模块加载不再自行触发安装。
}

NativeHookFunType native_hook_func() { return g_hook_func; }
NativeUnhookFunType native_unhook_func() { return g_unhook_func; }

extern "C" [[gnu::visibility("default")]] [[gnu::used]]
NativeOnModuleLoaded native_init(const NativeAPIEntries* entries) {
    inventory_native_hook_on_api(entries);
    return inventory_native_hook_on_module_loaded;
}

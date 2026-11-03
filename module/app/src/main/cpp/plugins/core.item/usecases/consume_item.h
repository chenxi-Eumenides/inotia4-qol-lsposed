#pragma once

// item 侧 stage4 动作专用纯函数（equip/put_jewel/unequip/install_transaction/
// make_item_writeback_count 等）。storage 纯函数（have/get/get_cumulate/consume/
// remove/remove_data_plan/shortfall）已单源化至 contracts/inventory_stage4.h，
// 由 core.item 与 core.inventory 共同 include。

#include <cstddef>
#include <cstdint>

#include "../../../contracts/game_host.h"
#include "../../../contracts/game_ports/item_port.h"

// 原版装备页出售/销毁结算数量语义（纯函数，供 Host 断言）。
// 反汇编（基线 .tmp/dualmode-full.asm，本次 .tmp/sell-vanilla/1261c4.asm 复核）：
// UIEquip_ButtonDestroyExe(0xb6240) → UIEquip_OKDestroyItem(0xb83d0) → 0xb8468
// bl 0x1261c4；0x1261c4 在 0x1261fc `ldr w0,[x21,#0x10]` + 0x126208
// `bl UTIL_GetBitValue(_,31,25)` 直接读 +0x10 的 b 段（bits25–31），随后 clamp：
// b∈[1,99] 取 b，否则取 1，再 `unit*b*7/10`。该读点曾计划以 R-51 重定向改指
// ITEM_GetCumulateCount；该方案已证伪并回退，重定向表
//（g_stack_getter_redirect_patches）不含此点。现由 R-55 UIEquip_OKDestroyItem
// 回调层在启用态接管结算（取 hooked getter 的 canonical），关闭态与未知调用者仍走
// 原版直读。本函数保留为原版回退语义参照，并作为 VM-37 缺陷基线。
//   低位视图 b∈[1,99] 时取 b，否则取 1。
uint32_t native_equip_sell_count(uint32_t field,
                                 const qol::contracts::QolItemCountApi* item_count);

// ---------------------------------------------------------------------------
// 原版背包详情出售接管（R-55，VM-41）。UIEquip_OKDestroyItem(0xb83d0) 无参，背包
// 结算分支从面板上下文读袋/槽后调 0x1261c4 只按 b 段结算（>99 的 canonical 被当 1）。
// 模块在启用态直接接管该回调：取 hooked getter 的 canonical、按 0.7 加钱、删整堆、
// 刷新；关闭态与未知调用者一律 backup（原版逐指令不变）。
//
// 两态判定：UIEquip_OKDestroyItem 仅两处调用者（全量反汇编核实）——按钮预演
// （UIEquip_ButtonDestroyExe 0x126288 `bl 0xb83d0`，x23=0x666，只算展示金额）与
// 弹窗 OK（UIPopupMsg_ButtonOKExe 0xcaa14 `blr x1`，真实结算）。hook 在
// button_destroy_exe_wrapper 内以 thread_local 标记「正在按钮内」：预演只回填展示
// 金额（kPreview），弹窗 OK 才真实接管（kTakeover）。不用返回地址判定——经 Dobby
// 桥后 LR 不可信（既有 MoveItem caller 日志即为桥地址）。
enum class VanillaSellRoute : uint8_t { kBackup, kPreview, kTakeover };
constexpr int32_t kVanillaBagDetailType = 2;
// UIEquip_ButtonDestroyExe 的预演协议把 x23 置为该值后调用 UIEquip_OKDestroyItem；
// monster 结算块 0x126298 据此只计算展示金额、不写钱不删物品（c18 0x1260c4 无此分支）。
constexpr uint64_t kVanillaSellPreviewFlag = 0x666;
// c18 背包详情结算块 0x1260c4：((unit*65)/100)*count（0x126108 `mov w1,#0x41` /
// 0x126110 `mov w1,#0x64`）。用于确认框展示金额，必须与原版实际结算同序同舍入。
int64_t c18_detail_sell_price(int64_t unit_price, int32_t count);

VanillaSellRoute vanilla_sell_route(int32_t bag, bool count_encoded,
                                    uint32_t canonical_count, bool button_dry_run);
bool vanilla_sell_detail_context_matches(
    const qol::contracts::QolItemDetailContext& preview,
    const qol::contracts::QolItemDetailContext& confirmation);
// ITEMSYSTEM_MakeItem 数量回写计划（VM-38 缺陷修复，纯函数供 Host 断言）。
// 反汇编证明（0x10c6c8，基线 .tmp/dualmode-full.asm）：arg2（w1）是与静态表
// +0x2 域匹配的查找/品质参数（CHARSYSTEM_DropItem 传 2..5、DEALSYSTEM_MakeSale
// 传 5），不是数量；产物数量 = CAL_Calculate 掉落公式（原版写点 0x10ca3c
// `SetBitValue(31,25,公式值)`，公式域 ≤99，b 写即全量）。因此 MakeItem 恒
// 返回 0 = 不回写数量；任何把 arg2 写入数量位的路径都会把掉落物数量污染成
// 2/3/4/5（真机实证：药水 2/卷轴 3/材料 4）。
uint32_t stage4_make_item_writeback_count(int32_t category, int32_t arg2, int32_t flag);

using Stage4IdentifyItem = bool (*)(void* item, int32_t* bag, int32_t* slot);
using Stage4ItemAt = void* (*)(int32_t bag, int32_t slot);
using Stage4EquipExtension = bool (*)(void* character, void* item, int32_t bag, int32_t slot,
                                      int32_t equip_slot);
using Stage4EquipBackup = int (*)(void* character, int32_t bag, int32_t slot,
                                  int32_t equip_slot);
int stage4_equip_item(void* character, int32_t bag, int32_t slot, int32_t equip_slot,
                      Stage4ItemAt item_at, Stage4IdentifyItem identify,
                      Stage4EquipExtension extension_equip,
                      Stage4EquipBackup backup,
                      Stage4ItemAt extension_item_at = nullptr);

using Stage4JewelBackup = int (*)(void* equip_item, void* jewel_item);
using Stage4JewelExtension = int (*)(void* equip_item, void* jewel_item,
                                     Stage4JewelBackup backup);
int stage4_put_jewel(void* equip_item, void* jewel_item, Stage4IdentifyItem identify,
                     Stage4JewelBackup backup, Stage4JewelExtension extension_put);

using Stage4UnequipBackup = int (*)(void* character, int32_t equip_slot);
using Stage4UnequipExtension = bool (*)(void* character, int32_t equip_slot);
int stage4_unequip_item_to_inven(void* character, int32_t equip_slot,
                                 Stage4UnequipBackup backup,
                                 Stage4UnequipExtension extension_adopt,
                                 bool& recursive_guard);

using Stage4HookInstall = int (*)(void* target, void* replacement, void** backup);
using Stage4HookUninstall = int (*)(void* target);
struct Stage4HookSpec {
    void* target;
    void* replacement;
    void** backup;
};
bool stage4_install_transaction(const Stage4HookSpec* hooks, std::size_t count,
                                Stage4HookInstall install,
                                Stage4HookUninstall uninstall);

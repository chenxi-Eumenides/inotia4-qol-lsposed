#pragma once

#include <cstddef>
#include <cstdint>

#include "../../contracts/game_host.h"
#include "../../contracts/game_ports/item_port.h"

using Stage4HaveBackup = int (*)(int32_t);
using Stage4CountBackup = int (*)(int32_t);
using Stage4EmptyBackup = int (*)(int32_t, int32_t);
using Stage4CountExtension = int (*)(int32_t);
using Stage4EmptyExtension = bool (*)(int32_t, int32_t);

int stage4_have_item(int32_t category, Stage4HaveBackup backup,
                     Stage4CountExtension extension_count, bool& recursive_guard);
int stage4_get_item_count(int32_t category, Stage4CountBackup backup,
                          Stage4CountExtension extension_count, bool& recursive_guard);
int stage4_have_item_original_only(int32_t category, Stage4HaveBackup backup,
                                   bool& recursive_guard);
int stage4_get_item_count_original_only(int32_t category, Stage4CountBackup backup,
                                        bool& recursive_guard);

using Stage4CumulateBackup = int (*)(void*);
// S2 读侧统一入口（R-49）的纯逻辑分流：通过插件上下文注入的 item_count facade 读取
// effective；facade 未驻留/未就绪或返回 unsupported 时 original-first 走 backup，
// backup 缺失按 0。插件侧不经 contracts/item_count.h 的宿主访问器。
int stage4_get_cumulate_count(void* item, const qol::contracts::QolItemCountApi* item_count,
                              Stage4CumulateBackup backup);

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
int stage4_is_having_empty_slot(int32_t needed, int32_t include_task_bag,
                                Stage4EmptyBackup backup,
                                Stage4EmptyExtension extension_has_empty,
                                bool& recursive_guard);
int stage4_is_having_empty_slot_original_only(int32_t needed, int32_t include_task_bag,
                                              Stage4EmptyBackup backup,
                                              bool& recursive_guard);

using Stage4IdentifyItem = bool (*)(void* item, int32_t* bag, int32_t* slot);
using Stage4ConsumeExtension = bool (*)(void* item);
using Stage4ConsumeBackup = void (*)(void* item);
using Stage4RemoveExtension = bool (*)(void* item);
using Stage4RemoveBackup = int (*)(void* item);

bool stage4_consume_item(void* item, Stage4IdentifyItem identify,
                         Stage4ConsumeExtension extension_consume,
                         Stage4ConsumeBackup backup, bool& recursive_guard);
int stage4_remove_item(void* item, Stage4IdentifyItem identify,
                       Stage4RemoveExtension extension_remove,
                       Stage4RemoveBackup backup, bool& recursive_guard);

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

// ---------------------------------------------------------------------------
// S2 写侧 INVEN_RemoveItemData 修正计划（R-45/R-49，纯函数供 Host 断言）。
// 原版（0x1040a8）顺序遍历 6 袋 × 槽，对匹配类别的可堆叠堆：整堆删除累计
// w22，最后一堆部分删除时写 `b = cum(Getter 全量) + w22 - count`。启用态下
// cum 已由 H-17 getter 解码为 S2 全量（算术正确），但 b 写对 remain > 127
// 或旧 a 残留的情形丢失进位 → 由调用方快照 + 重扫后按本函数计算修正。
// ---------------------------------------------------------------------------
struct Stage4RemoveDataEntry {
    void* item;        // 快照槽内对象指针
    uint32_t pre_full; // 快照时启用态 S2 全量
};

struct Stage4RemoveDataPost {
    void* item;        // 重扫槽内对象指针（nullptr = 槽已清/被替换）
    uint32_t post_view; // 重扫时启用态 S2 解码值
};

struct Stage4RemoveDataPlan {
    bool correct = false;  // true = 必须对 entry_index 槽回写 remain（a+b）
    int entry_index = -1;  // 需修正的快照条目下标
    uint32_t remain = 0;   // 正确剩余全量（0..999）
};

// 输入：pre（快照数组）、post（与 pre 等长、按同一槽序重扫）、count（原版入参）。
// 判定：count<=0 或无缩减堆 → 不修正；缩减堆不唯一或数据矛盾（sum_vanished >
// count、remain 越域、守恒不成立）→ fail-closed 不修正；缩减唯一且 remain !=
// post_view → 修正。关闭态（R-47）原版 b 写天然正确，调用方不应以启用态数据
// 调用本函数。
Stage4RemoveDataPlan stage4_remove_item_data_plan(
    const qol::contracts::QolItemCountApi* item_count, const Stage4RemoveDataEntry* pre,
    const Stage4RemoveDataPost* post, int entry_count, int32_t count);

// H-21 扩展桥接（R-56）物理实扣量 → 扩展补扣量（纯函数供 Host 断言）。
// 原版 INVEN_RemoveItemData 只遍历物理袋 0..5；物理实扣 = 调用前后 H-03 总数差
// （H-03 = 物理 + 扩展，原版不动扩展，故差值即物理实扣）。返回需从扩展袋补扣的
// 数量 = max(0, requested - physical_removed)，requested<=0 或数据反向（after>before）
// 时按 0 处理，避免负补扣。
int stage4_remove_data_extension_shortfall(int total_before, int total_after,
                                           int requested);

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

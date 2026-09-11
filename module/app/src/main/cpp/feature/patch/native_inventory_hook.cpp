#include "native_inventory_hook.h"
#include "inventory_find_item_poc.h"
#include "inventory_hook_stage4.h"

#include "core/native/extension_bag_port.h"
#include "core/native/stack_codec.h"
#include "feature/extension_bag/game_ui_virtbag.h"
#include "game_access.h"
#include "game_inventory.h"
#include "game_patch.h"
#include "game_state.h"

#include <android/log.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace {

constexpr char kTag[] = "Inotia4NativeHook";
constexpr uint32_t kNativeApiVersion = 2;

std::mutex g_hook_mutex;
NativeHookFunType g_hook_func = nullptr;
NativeUnhookFunType g_unhook_func = nullptr;
std::atomic<bool> g_installing{false};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_install_blocked{false};

FindItemFn g_backup_find_item = nullptr;
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
// R-55 原版背包详情出售接管：UIEquip_OKDestroyItem(0xb83d0)。返回值仅按钮预演读取
// （弹窗展示金额），用 UiEquipOkDestroyItemFn（uint64_t()）捕获 x0。
UiEquipOkDestroyItemFn g_backup_ok_destroy_item = nullptr;
UiEquipEquipControlEventProcFn g_backup_equip_control_event_proc = nullptr;
// S2 写侧进位框架新增 hook（H-18/H-19/H-20/H-21）的 backup。
InvenSaveItemDirectFn g_backup_save_item_direct = nullptr;
ItemSystemDivideFn g_backup_item_system_divide = nullptr;
MakeItemFn g_backup_make_item = nullptr;
InvenRemoveItemDataFn g_backup_remove_item_data = nullptr;

std::atomic<uint64_t> g_find_item_calls{0};
std::atomic<uint64_t> g_consume_item_calls{0};
std::atomic<uint64_t> g_remove_item_calls{0};

struct InstalledHook {
    void* target;
    void** backup;
    const char* name;
};

thread_local bool g_in_find_item = false;
thread_local bool g_in_have_item = false;
thread_local bool g_in_get_item_count = false;
thread_local bool g_in_is_having_empty_slot = false;
thread_local bool g_in_consume_item = false;
thread_local bool g_in_unequip_item_to_inven = false;
thread_local bool g_in_remove_item = false;
// R-55：UIEquip_ButtonDestroyExe 动态范围内（含其同步调用的 OKDestroyItem 预演）。
thread_local bool g_in_equip_destroy_button = false;
thread_local int g_refresh_depth = 0;

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
// 勘察基线：.tmp/dualmode-full.asm（llvm-objdump -d libgame.so，2026-09-11 冻结）。
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
// | ITEMSYSTEM_MakeItem           | 1    | 设（装备强化 marker，非数量） | CAL_Calculate 结果 | CreatePerfectItem 返回装备 | 勘误（2026-09-11 反汇编 0x10c6c8）：原登记「设（创建量）count 入参 arg2」错误——真实 ABI 是 `void* (category, lookup_key, luck)`（0x10c6d8 `mov w23,w1` 用于静态表 uint16 匹配、0x10c6e4 存 w2 作品质门槛比较，均非数量）；唯一 31/25 写点 0x10ca3c 仅对可强化装备写强化 marker（可堆叠类别 bit0=0 不进该分支，产物数量恒由 CreateItem 写 b=1，S2 完整）。旧 make_item_wrapper 把 x1（lookup_key）当 count 回写，导致拾取 1 个得 lookup_key 个（药水 2/卷轴 3/材料 4，真机实证）；H-20 已整体移除，原 hook 名保留编号不复用 |
// | ITEMSYSTEM_ProcessUnpack      | 1    | 设（产物量）   | 拆包表 u8 字段（表项 +0x4）    | CreatePerfectItem 产物    | 无需接管：产物量 = 拆包表只读静态数据（模块无任何写点），原版数量域 ≤99 ≤127，b 写即全量；开放边界：静态表若含 >99 值需数据审计复核（VM 取证抽查） |
// | MAPITEMSYSTEM_CreateItem      | 1    | 设（生成量）   | count 入参（w1）               | CreateItem 产物（尾调 Add，返回值非对象） | 无需接管：4 个调用点 count 域全部为原版静态数据——UINpc_ExeNpcTask 0xc2f60 `mov w1,#1`、EVTSYSTEM_Process 0xfc710 事件表字段、QUESTSYSTEM_ApplyReward 0x1238e4 任务表 u16；掉落/任务数量设计域 ≤99 ≤127；开放边界：若未来发现丢弃/移仓路径把 S2 全量（>127）作 count 传入本函数则需复核（当前全量反汇编未见该路径） |
// | NetworkStore_InitializeMenuData | 1  | 设（货架量）   | 网络商品数据 +0x18 字段        | CreateItem 产物           | 待勘察：数量来自网络数据（0x15b7b0+0x25xx 写点 `[x25+0x18]` 解引用），离线不可达无法冻结值域；勘察方法：联网抓包/内存 dump 商品表 +0x18 域，或真机挂回调记录 |
// | NetworkStore_AddItem          | 1    | 设（接收量）   | 网络收货数据 +0x18 字段        | CreateItem 产物（返回值非对象） | 待勘察：写点 0x15d7cc 区 `w3=[收货数据+0x18]`，数量域同上不可达；勘察方法同上 |
// ---------------------------------------------------------------------------

// G_INVEN_VMA 槽位布局（game_symbols.h：6 袋 × 0x80 步长，每袋 16 槽、每槽 8B 指针，
// 含任务袋 5）。写侧回写需要绕过扩展端口直接读物理槽，避免扩展物化路径取锁。
constexpr int kInvenPhysicalBagCount = 6;
constexpr int kInvenSlotsPerBag = 16;
constexpr size_t kInvenBagStride = 0x80;

// 纯内存读物理袋槽物品指针（bag 0..5，含任务袋；越界/未就绪返回 nullptr）。
// 刻意不经 inventory_item_at：该函数对逻辑袋走扩展物化、对物理袋调 H-17 getter，
// 写侧取证只需原始指针，不需要两者。
void* s2_physical_slot_item(int bag, int slot) {
    if (bag < 0 || bag >= kInvenPhysicalBagCount || slot < 0 || slot >= kInvenSlotsPerBag ||
        g_inven == nullptr) {
        return nullptr;
    }
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g_inven) +
                                     static_cast<size_t>(bag) * kInvenBagStride +
                                     static_cast<size_t>(slot) * sizeof(void*));
}

uint32_t s2_read_count_field(const void* item) {
    return *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(item) + I_COUNT);
}

void s2_write_count_field(void* item, uint32_t field) {
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT) = field;
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

// S2 写侧回写总门控（R-46 fail-closed）：模块启用 + 对象有效 + count-encoded。
// kUnknown 与非可堆叠（装备 marker bits25–31、宝石选项 bits18–23、袋容量
// bits0–24）一律返回 false，调用方保持原版行为；堆叠上限关闭态按 R-47 决策 b
// 以模式视图（b 域）参与取证与回写，effective_write_count 只写 b、保留 a。
bool s2_writeback_gate(void* item) {
    return item != nullptr && extension_bag_enabled() &&
           item_count_encoding(item) == stack_codec::CountEncoding::kEncoded;
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
    cap->pre_field = s2_read_count_field(occupant);
    // 模式视图旧全量（R-47 决策 b）：启用态 128a+b，关闭态只读 b。
    cap->pre_full =
        stack_codec::effective_read_count(cap->pre_field, stack_limit_enabled());
    cap->delta = static_cast<uint32_t>(delta);
    cap->new_full =
        s2_writeback::added_count(cap->pre_full, cap->delta, stack_limit_enabled());
}

// backup 后确认回写：目标槽仍指向同一对象（防替换/释放后的 UAF 读）、原版确实
// 写了 b（post != pre 且等于原版对 new_full 的预测写）才回写 a+b；其余一律
// fail-closed 跳过并记日志（退化 delta≡0 mod 128、同类别不同 payload 拒并等均
// 落入此分支，不猜测）。
void s2_merge_writeback(const S2MergeCapture& cap, const char* op) {
    if (!cap.armed) return;
    if (s2_physical_slot_item(cap.bag, cap.slot) != cap.target) return;
    const uint32_t post_field = s2_read_count_field(cap.target);
    if (post_field == cap.pre_field) {
        if ((cap.delta % 128) == 0) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "S2 writeback %s degenerate skip target=%p delta=%u (delta%%128==0)",
                                op, cap.target, cap.delta);
        }
        return;
    }
    if (!s2_writeback::original_count_write_confirmed(cap.pre_field, post_field, cap.new_full)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "S2 writeback %s mismatch skip target=%p pre=0x%x post=0x%x expect_full=%u",
                            op, cap.target, cap.pre_field, post_field, cap.new_full);
        return;
    }
    // 模式感知回写（R-47 决策 b）：启用态全量拆段（进位内建）；关闭态只写 b、
    // 保留 a（new_full 已是 b 域值，此写与原版 b 写逐位一致，幂等）。
    const uint32_t written = stack_codec::effective_write_count(
        post_field, cap.new_full, stack_limit_enabled());
    s2_write_count_field(cap.target, written);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "S2 writeback %s merge target=%p %u+%u->%u field=0x%x",
                        op, cap.target, cap.pre_full, cap.delta, cap.new_full, written);
}

void log_extension_item_observation(const char* operation, void* item, bool recursive,
                                    int result_known, int result) {
    if (virtual_bag_native_call_active()) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "%s original-only native_call=1 recursive=%d result_known=%d result=%d",
                            operation, recursive ? 1 : 0, result_known, result);
        return;
    }
    if (!extension_bag_enabled()) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "%s extension disabled recursive=%d result_known=%d result=%d",
                            operation, recursive ? 1 : 0, result_known, result);
        return;
    }
    int bag = -1;
    int slot = -1;
    const bool extension_item = extension_bag_identify_native_item(item, &bag, &slot);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "%s extension_item=%d bag=%d slot=%d recursive=%d result_known=%d result=%d",
                        operation, extension_item ? 1 : 0, bag, slot, recursive ? 1 : 0,
                        result_known, result);
}

void* find_item_wrapper(int32_t category) {
    const uint64_t call = g_find_item_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (virtual_bag_native_call_active() || g_in_find_item || g_backup_find_item == nullptr) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "FindItem call=%llu category=%d recursive=%d backup=%p",
                            static_cast<unsigned long long>(call), category,
                            g_in_find_item ? 1 : 0,
                            reinterpret_cast<void*>(g_backup_find_item));
        void* result = inventory_find_item_original_first(
            category, g_backup_find_item, nullptr, g_in_find_item);
        log_extension_item_observation("FindItem", result, g_in_find_item, 1, result != nullptr ? 1 : 0);
        return result;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "FindItem call=%llu category=%d backup=%p",
                        static_cast<unsigned long long>(call), category,
                        reinterpret_cast<void*>(g_backup_find_item));
    void* result = inventory_find_item_original_first(
        category, g_backup_find_item,
        extension_bag_enabled() ? extension_bag_find_native_item : nullptr,
        g_in_find_item);
    log_extension_item_observation("FindItem", result, false, 1, result != nullptr ? 1 : 0);
    return result;
}

int extension_item_count(int32_t category) {
    if (virtual_bag_native_call_active()) return 0;
    struct Context { int32_t category; int count; } context{category, 0};
    extension_bag_for_each_logical_item([](int, int, int item_category, int item_count, void* raw) -> bool {
        Context* context = static_cast<Context*>(raw);
        if (item_category == context->category && item_count > 0) context->count += item_count;
        return false;
    }, &context);
    return context.count;
}

int have_item_wrapper(int32_t category) {
    if (virtual_bag_native_call_active()) {
        return stage4_have_item_original_only(category, g_backup_have_item, g_in_have_item);
    }
    return stage4_have_item(category, g_backup_have_item,
                            extension_bag_enabled() ? extension_item_count : nullptr,
                            g_in_have_item);
}

int get_item_count_wrapper(int32_t category) {
    if (virtual_bag_native_call_active()) {
        return stage4_get_item_count_original_only(category, g_backup_get_item_count,
                                                   g_in_get_item_count);
    }
    return stage4_get_item_count(category, g_backup_get_item_count,
                                 extension_bag_enabled() ? extension_item_count : nullptr,
                                 g_in_get_item_count);
}

// S2 读侧统一入口（R-49）：原版 getter 只见 b 段（bits25–31），本 wrapper 对
// count-encoded 类别按模式视图解码（R-45/R-47 决策 b：启用态 `count=128a+b`、
// 关闭态只读 b）。门控判据沿用 R-40/R-41 的单源谓词 item_count_encoding，
// 判定不可用（kUnknown）时 fail-closed 直通 backup（R-46）；非可堆叠（装备
// marker/宝石选项/袋容量）不解释 bits22–24，一律 original-first。纪律：backup
// 是纯读原版函数（UTIL_GetBitValue/MEM_ReadUint8，反汇编 0x106094–0x106124
// 证实），不回调任何被 Hook 函数；wrapper 不取 g_virtual_bag_mtx、不读模块
// 状态，因此无需 TLS 直通守卫；数量位偏移使用 game_symbols.h 的 I_COUNT，不写
// 裸偏移。分流逻辑在 stage4_get_cumulate_count（纯函数，供后续 Host 断言）。
int get_cumulate_count_wrapper(void* item) {
    const stack_codec::CountEncoding encoding =
        item == nullptr ? stack_codec::CountEncoding::kUnknown
                        : item_count_encoding(item);
    const uint32_t raw_count_field =
        item == nullptr
            ? 0u
            : *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(item) +
                                                 I_COUNT);
    return stage4_get_cumulate_count(item, raw_count_field, encoding,
                                     g_backup_get_cumulate_count,
                                     stack_limit_enabled());
}

int is_having_empty_slot_wrapper(int32_t needed, int32_t include_task_bag) {
    if (virtual_bag_native_call_active()) {
        return stage4_is_having_empty_slot_original_only(
            needed, include_task_bag, g_backup_is_having_empty_slot,
            g_in_is_having_empty_slot);
    }
    return stage4_is_having_empty_slot(needed, include_task_bag,
                                        g_backup_is_having_empty_slot,
                                        extension_bag_enabled() ? extension_bag_has_empty_slots : nullptr,
                                        g_in_is_having_empty_slot);
}

int unequip_item_to_inven_wrapper(void* character, int32_t equip_slot) {
    const int result = stage4_unequip_item_to_inven(character, equip_slot,
                                                    g_backup_unequip_item_to_inven,
                                                    extension_bag_enabled()
                                                        ? extension_bag_adopt_unequipped_item
                                                        : nullptr,
                                                    g_in_unequip_item_to_inven);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "UnequipItemToInven equip_slot=%d result=%d recursive=%d",
                        equip_slot, result, g_in_unequip_item_to_inven ? 1 : 0);
    return result;
}

void consume_item_wrapper(void* item) {
    const uint64_t call = g_consume_item_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "ConsumeItem call=%llu item=%p recursive=%d backup=%p",
                        static_cast<unsigned long long>(call), item,
                        g_in_consume_item ? 1 : 0,
                        reinterpret_cast<void*>(g_backup_consume_item));
    int extension_bag = -1;
    int extension_slot = -1;
    const bool extension_item = extension_bag_enabled() &&
                                extension_bag_identify_native_item(item, &extension_bag,
                                                                   &extension_slot);
    // S2 写侧借位（减语义；R-45/R-46/R-49，仅启用态）：原版读 b 判 count>1——
    // b≤1 且全量≥2（a≥1）时原版落入整堆删除分支，a 段随对象一起丢失。处置：
    // backup 前把 b 预置为 2（a 与低位原样），原版按 b=2>1 走 b-1 递减写 b=1；
    // 返回后确认预置生效再回写 a+b=full-1，失败则还原预置字段。b≥2 时原版递减
    // 天然正确不写；全量≤1 时删除即原版语义，不拦。
    // 堆叠上限关闭态（R-47 决策 b）不预置借位：有效数量就是 b，b≤1 时消耗到
    // 空按原版删除即关闭态语义（不为了保留 a 而让有效数量 0 的堆无法消耗）；
    // b≥2 的原版递减只写 b，a 保留不变。
    bool s2_borrow_armed = false;
    int borrow_bag = -1;
    int borrow_slot = -1;
    uint32_t borrow_pre_field = 0;
    uint32_t borrow_pre_full = 0;
    if (!extension_item && g_backup_consume_item != nullptr && stack_limit_enabled() &&
        s2_writeback_gate(item) &&
        s2_find_physical_slot(item, &borrow_bag, &borrow_slot)) {
        borrow_pre_field = s2_read_count_field(item);
        borrow_pre_full = stack_codec::s2_read_count(borrow_pre_field);
        if (borrow_pre_full >= 2 && stack_codec::s2_split_b(borrow_pre_full) <= 1) {
            s2_write_count_field(item,
                                 (borrow_pre_field & ~stack_codec::kS2MaskB) |
                                     (stack_codec::s2_split_b(2) << stack_codec::kS2ShiftB));
            s2_borrow_armed = true;
        }
    }
    const bool dispatched = stage4_consume_item(
        item, extension_bag_enabled() ? extension_bag_identify_native_item : nullptr,
        extension_bag_enabled() ? extension_bag_consume_native_item : nullptr,
        g_backup_consume_item, g_in_consume_item);
    if (s2_borrow_armed && !extension_item) {
        if (s2_physical_slot_item(borrow_bag, borrow_slot) != item) {
            // 原版仍删除了整堆（与 0x104818 的 b>1 递减分支反汇编结论矛盾）：
            // 对象已释放，禁止触磁字段，只记 ERROR 供取证。
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "S2 writeback ConsumeItem borrow item removed unexpectedly "
                                "item=%p bag=%d slot=%d pre_full=%u",
                                item, borrow_bag, borrow_slot, borrow_pre_full);
        } else {
            const uint32_t post_field = s2_read_count_field(item);
            const uint32_t expected =
                (borrow_pre_field & ~stack_codec::kS2MaskB) |
                (stack_codec::s2_split_b(1) << stack_codec::kS2ShiftB);
            if (post_field == expected) {
                const uint32_t new_full = s2_writeback::decremented_count(borrow_pre_full);
                s2_write_count_field(item, stack_codec::s2_write_count(post_field, new_full));
                __android_log_print(ANDROID_LOG_INFO, kTag,
                                    "S2 writeback ConsumeItem borrow item=%p %u->%u",
                                    item, borrow_pre_full, new_full);
            } else {
                // 原版未按预期递减：还原预置字段，保持进 hook 前原样。
                s2_write_count_field(item, borrow_pre_field);
                __android_log_print(ANDROID_LOG_ERROR, kTag,
                                    "S2 writeback ConsumeItem borrow mismatch item=%p "
                                    "pre=0x%x post=0x%x restored",
                                    item, borrow_pre_field, post_field);
            }
        }
    }
    if (!dispatched && extension_item) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "ConsumeItem extension consume failed bag=%d slot=%d item=%p",
                            extension_bag, extension_slot, item);
    }
    return;
}

int remove_item_wrapper(void* item) {
    const uint64_t call = g_remove_item_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "RemoveItem call=%llu item=%p recursive=%d backup=%p",
                        static_cast<unsigned long long>(call), item,
                        g_in_remove_item ? 1 : 0,
                        reinterpret_cast<void*>(g_backup_remove_item));
    return stage4_remove_item(item,
                              extension_bag_enabled() ? extension_bag_identify_native_item : nullptr,
                              extension_bag_enabled() ? extension_bag_remove_native_item : nullptr,
                              g_backup_remove_item,
                              g_in_remove_item);
}

int save_item_wrapper(void* item) {
    // INVEN_SaveItem 是所有"已创建物品放入背包"的唯一漏斗（任务奖励/事件
    // 发奖/开箱/拾取/商店/合成等 18 条调用点）。原版全袋无空位时
    // FindSaveSlot 失败返回 0 → 上层会掉地/静默消失。此处 backup 返回 0 时
    // 转扩展袋空位接管（adopt 进扩展槽），返回 1 让上层按成功处理，
    // 发奖代码零改动即支持扩展背包。
    // 商店扩展视图会把窗口原版袋容量字临时放大；买入等原版写入若以放大
    // 容量扫空槽会把物品写进超过真实容量的槽位。backup 前先恢复商店投影。
    if (extension_bag_enabled()) {
        extension_bag_store_restore_for_original_write();
        // S2 拾取并入扩展同类堆（R-52）：物品可堆叠且扩展袋存在同 category、
        // 归一化 payload 相同的堆，且模式视图相加不超上限时直接并入扩展堆并
        // 上报成功；否则保持原版入库（含原版同类堆合并）。修复拾取可堆叠
        // 物品在原版背包新开格、不并入扩展同类堆的缺陷。
        if (extension_bag_merge_native_item(item)) return 1;
    }
    const int result = g_backup_save_item(item);
    if (result != 0) return result;
    if (extension_bag_adopt_native_item(item)) return 1;
    return 0;
}

// ---- S2 写侧进位框架新增 hook（H-18..H-20）----
// 共同纪律：original-first、失败/不确定回退原版结果；wrapper 不取
// g_virtual_bag_mtx、不调用任何被 Hook 函数（R-44）；类别门控 fail-closed
//（R-46），回写按模式视图（R-47 决策 b：启用态全量 a+b、关闭态只写 b 保留 a）。

int save_item_direct_wrapper(void* item, int32_t bag, int32_t slot) {
    // INVEN_SaveItemDirect（加语义）：空槽写入不需要回写——对象创建侧数量由
    // MakeItem wrapper / 模块 s2 写点保证 a+b 完整；同身份堆叠合并时原版只写 b
    //（+0xe8/+0x110/+0x168 三处 SetBitValue 写点），合并可能 ITEMPOOL_Free(item)，
    // 因此入参对象全量必须在 backup 前读取。原版返回值不可信（决策册 §2.2），
    // 成败判定改用「目标槽指针未替换 + 字段等于原版预测 b 写」。
    S2MergeCapture merge{};
    s2_capture_merge(item, bag, slot,
                     item == nullptr
                         ? 0
                         : static_cast<int32_t>(stack_codec::effective_read_count(
                               s2_read_count_field(item), stack_limit_enabled())),
                     &merge);
    const int result = g_backup_save_item_direct == nullptr
        ? 0 : g_backup_save_item_direct(item, bag, slot);
    s2_merge_writeback(merge, "SaveItemDirect");
    return result;
}

void* item_system_divide_wrapper(void* item, int32_t count) {
    // ITEMSYSTEM_Divide（设 + 减）：拆出 count 入栈新对象（设值），源堆扣减
    //（借位）。原版对源堆与新对象都只写 b；新对象为新建对象（a 应为 0），
    // 直接按入参 clamp 后覆盖写 a+b；源堆仅在校验「原版确实写了 b」后回写
    // a+b=pre_full-count。源堆旧字段必须在 backup 前读取——整堆拆分时原版
    // 可能释放源对象；剩余 >0 时源对象保留，调后读取才安全。拆分失败
    //（new_item=null）或非 count-encoded 时全部跳过，保持原版结果。
    const bool gate = item != nullptr && count > 0 && s2_writeback_gate(item);
    uint32_t pre_field = 0;
    uint32_t pre_full = 0;
    if (gate) {
        pre_field = s2_read_count_field(item);
        // 模式视图旧全量（R-47 决策 b）：启用态 128a+b，关闭态只读 b。
        pre_full =
            stack_codec::effective_read_count(pre_field, stack_limit_enabled());
    }
    void* new_item = g_backup_item_system_divide == nullptr
        ? nullptr : g_backup_item_system_divide(item, count);
    if (!gate || new_item == nullptr) return new_item;
    if (pre_full > static_cast<uint32_t>(count)) {
        // 源堆保留剩余（不释放），此读安全；整堆拆分（pre_full==count）时
        // 原版可能已释放源对象，不做任何源堆触磁。
        const uint32_t post_field = s2_read_count_field(item);
        const uint32_t remain = s2_writeback::subtracted_count(
            pre_full, static_cast<uint32_t>(count), stack_limit_enabled());
        if (s2_writeback::original_count_write_confirmed(pre_field, post_field, remain)) {
            // 模式感知回写（R-47 决策 b）：启用态全量拆段；关闭态只写 b、保留 a。
            s2_write_count_field(item,
                                 stack_codec::effective_write_count(
                                     post_field, remain, stack_limit_enabled()));
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "S2 writeback Divide source item=%p %u-%u->%u",
                                item, pre_full, count, remain);
        } else if (post_field != pre_field) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "S2 writeback Divide source mismatch skip item=%p "
                                "pre=0x%x post=0x%x expect_full=%u",
                                item, pre_field, post_field, remain);
        }
    }
    if (new_item != item && s2_writeback_gate(new_item)) {
        const uint32_t new_full =
            stack_codec::effective_clamp(static_cast<uint32_t>(count),
                                         stack_limit_enabled());
        // 模式感知回写（R-47 决策 b）：启用态全量拆段；关闭态只写 b、保留 a。
        s2_write_count_field(new_item,
                             stack_codec::effective_write_count(
                                 s2_read_count_field(new_item), new_full,
                                 stack_limit_enabled()));
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "S2 writeback Divide new item=%p count=%u", new_item, new_full);
    }
    return new_item;
}

void* make_item_wrapper(int32_t category, int32_t arg2, int32_t flag) {
    // ITEMSYSTEM_MakeItem（0x10c6c8）：反汇编证明 arg2（w1）是与静态表 +0x2 域
    // 匹配的查找/品质参数——CHARSYSTEM_DropItem 传 2..5（掉落品质分支）、
    // DEALSYSTEM_MakeSale 传 5——不是数量；产物数量 = CAL_Calculate 掉落公式
    //（原版写点 0x10ca3c `SetBitValue(31,25,公式值)`，公式域 ≤99，b 写即全量，
    // 无需接管，R-49）。曾把 arg2 当 count 回写（`count>1` 恒真）导致掉落物一
    // 落地数量即被污染成 2/3/4/5（真机实证：药水 2/卷轴 3/材料 4，VM-38）。
    // 现在数量回写计划经纯函数 stage4_make_item_writeback_count（恒 0 = 不写），
    // wrapper 纯透传，仅保留观察日志。
    void* item = g_backup_make_item == nullptr
        ? nullptr : g_backup_make_item(category, arg2, flag);
    const uint32_t writeback =
        stage4_make_item_writeback_count(category, arg2, flag);
    if (item != nullptr && writeback > 0 && s2_writeback_gate(item)) {
        // 不可达（当前恒 0）；保留结构使未来若证实存在可信数量源时只改纯函数。
        s2_write_count_field(item,
                             stack_codec::effective_write_count(
                                 s2_read_count_field(item), writeback,
                                 stack_limit_enabled()));
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "S2 writeback MakeItem set item=%p count=%u", item, writeback);
    }
    return item;
}

// INVEN_RemoveItemData 快照条目上限：6 袋 × 16 槽（game_symbols.h G_INVEN_VMA 布局）。
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
    if (category < 0 || count <= 0 || !stack_limit_enabled()) return;
    for (int bag = 0; bag < kInvenPhysicalBagCount; ++bag) {
        for (int slot = 0; slot < kInvenSlotsPerBag; ++slot) {
            void* item = s2_physical_slot_item(bag, slot);
            if (item == nullptr) continue;
            if (item_count_encoding(item) != stack_codec::CountEncoding::kEncoded) continue;
            const uint16_t flags =
                *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(item) +
                                                   I_TYPE);
            if (((flags >> 6) & 0x3FF) != category) continue;
            if (cap->entry_count >= kS2RemoveDataMaxEntries) return;  // 不可能：上限即全袋
            cap->items[cap->entry_count] = item;
            cap->bags[cap->entry_count] = bag;
            cap->slots[cap->entry_count] = slot;
            cap->pre_full[cap->entry_count] =
                stack_codec::effective_read_count(s2_read_count_field(item), true);
            ++cap->entry_count;
        }
    }
    cap->armed = cap->entry_count > 0;
}

// backup 后重扫 + 修正：消失堆（整删）只求和；唯一缩减堆经
// stage4_remove_item_data_plan 计算 remain，post 与快照不一致（b 截断或旧 a
// 残留）时按模式视图回写 a+b。失败/矛盾一律 fail-closed 保持原版结果并记日志。
void s2_remove_data_writeback(const S2RemoveDataCapture& cap, int32_t category,
                              int32_t count) {
    if (!cap.armed) return;
    if (!stack_limit_enabled() || count <= 0) return;  // 关闭态原版 b 写天然正确
    Stage4RemoveDataEntry pre[kS2RemoveDataMaxEntries] = {};
    Stage4RemoveDataPost post[kS2RemoveDataMaxEntries] = {};
    for (int index = 0; index < cap.entry_count; ++index) {
        pre[index] = {cap.items[index], cap.pre_full[index]};
        void* cur = s2_physical_slot_item(cap.bags[index], cap.slots[index]);
        if (cur != cap.items[index]) {
            post[index] = {nullptr, 0};
        } else {
            post[index] = {cur,
                           stack_codec::effective_read_count(s2_read_count_field(cur), true)};
        }
    }
    const Stage4RemoveDataPlan plan =
        stage4_remove_item_data_plan(pre, post, cap.entry_count, count);
    if (!plan.correct) return;
    const int index = plan.entry_index;
    void* item = s2_physical_slot_item(cap.bags[index], cap.slots[index]);
    if (item != cap.items[index]) return;  // 二次确认：对象已被释放/移动则不触磁
    const uint32_t written = stack_codec::effective_write_count(
        s2_read_count_field(item), plan.remain, true);
    s2_write_count_field(item, written);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "S2 writeback RemoveItemData category=%d count=%d item=%p "
                        "%u->%u field=0x%x",
                        category, count, item, cap.pre_full[index], plan.remain, written);
}

int remove_item_data_wrapper(int32_t category, int32_t count) {
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
    const bool extension_fallback =
        count > 0 && category > 0 && extension_bag_enabled();
    const int total_before =
        extension_fallback ? get_item_count_wrapper(category) : 0;
    S2RemoveDataCapture capture{};
    if (g_backup_remove_item_data != nullptr) {
        s2_capture_remove_data(category, count, &capture);
    }
    const int result = g_backup_remove_item_data == nullptr
        ? 0 : g_backup_remove_item_data(category, count);
    s2_remove_data_writeback(capture, category, count);
    if (extension_fallback) {
        const int total_after = get_item_count_wrapper(category);
        const int shortfall =
            stage4_remove_data_extension_shortfall(total_before, total_after, count);
        if (shortfall > 0) {
            const int consumed = extension_bag_consume_category(category, shortfall);
            if (consumed < shortfall) {
                __android_log_print(ANDROID_LOG_INFO, kTag,
                                    "RemoveItemData extension fallback category=%d "
                                    "need=%d consumed=%d",
                                    category, shortfall, consumed);
            }
        }
    }
    return result;
}

void refresh_item_area_wrapper() {
    if (g_refresh_depth > 0) {
        inventory_native_hook_call_refresh_item_area_original();
        return;
    }
    ++g_refresh_depth;
    virtual_bag_refresh_item_area_with_gate();
    --g_refresh_depth;
}

uint64_t move_item_caller_offset() {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    return g_base != 0 && caller >= g_base ? static_cast<uint64_t>(caller - g_base) : 0;
}

void log_move_item_observation(const char* phase, void* item, int count, int target_bag,
                               int target_slot, uint64_t caller_offset,
                               const VirtualBagMoveItemObservation& observation) {
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "MoveItem %s item=%p count=%d target=%d/%d caller=libgame+0x%llx "
        "extension=%d/%d/%d handle=%u owner_state=%d view=%d session=%d source=%d/%d source_ptr=%p target_ptr=%p "
        "digest=0x%llx nonnull=%d physical_valid=%d",
        phase, item, count, target_bag, target_slot,
        static_cast<unsigned long long>(caller_offset), observation.extension_item ? 1 : 0,
        observation.extension_bag, observation.extension_slot, observation.extension_handle,
        observation.ownership_state, observation.module_view_index,
        observation.drag_session_active ? 1 : 0, observation.source_bag, observation.source_slot,
        observation.source_ptr, observation.target_ptr,
        static_cast<unsigned long long>(observation.physical_digest), observation.physical_nonnull,
        observation.physical_valid ? 1 : 0);
}

int move_item_wrapper(void* item, int count, int target_bag, int target_slot) {
    // 身份与取证数据在 g_virtual_bag_mtx 短临界区内采集，返回后立即放锁；禁止持锁
    // 调 backup，因为原版函数可能进入 RemoveItem/SaveItem/ConsumeItem hooks 再 try_lock。
    VirtualBagMoveItemObservation before{};
    const bool captured = virtual_bag_capture_move_item_observation(
        item, target_bag, target_slot, &before);
    const uint64_t caller_offset = move_item_caller_offset();
    // 仅允许装备交换 wrapper 主动借入物理槽时通过；普通投影/拖动仍必须
    // 拒绝 backup，避免扩展对象进入原版移动链。该例外不放宽其它 caller。
    if (captured && before.extension_item && !extension_bag_internal_equip_active()) {
        __android_log_print(
            ANDROID_LOG_ERROR, kTag,
            "MoveItem GUARD reject item=%p count=%d target=%d/%d caller=libgame+0x%llx "
            "extension=%d/%d handle=%u owner_state=%d view=%d session=%d source=%d/%d source_ptr=%p target_ptr=%p "
            "digest=0x%llx nonnull=%d physical_valid=%d backup=skipped",
            item, count, target_bag, target_slot,
            static_cast<unsigned long long>(caller_offset), before.extension_bag,
            before.extension_slot, before.extension_handle, before.ownership_state,
            before.module_view_index, before.drag_session_active ? 1 : 0,
            before.source_bag, before.source_slot, before.source_ptr, before.target_ptr,
            static_cast<unsigned long long>(before.physical_digest), before.physical_nonnull,
            before.physical_valid ? 1 : 0);
        return 0;
    }

    const bool observe = captured &&
                         (before.drag_session_active || before.module_view_index >= 0);
    if (observe) {
        log_move_item_observation("pre", item, count, target_bag, target_slot,
                                  caller_offset, before);
    }
    // S2 写侧合并进位（加语义；R-45/R-46/R-49）：backup 前采集目标槽旧全量，
    // backup 后确认原版确实写了 b（post != pre 且等于原版预测写）再回写 a+b。
    // 空槽移动无合并写；扩展对象在上方 GUARD 已拒绝 backup，不进入本路径。
    S2MergeCapture move_merge{};
    if (g_backup_move_item != nullptr) {
        s2_capture_merge(item, target_bag, target_slot, count, &move_merge);
    }
    const int result = g_backup_move_item == nullptr
        ? 0 : g_backup_move_item(item, count, target_bag, target_slot);
    s2_merge_writeback(move_merge, "MoveItem");
    if (observe) {
        VirtualBagMoveItemObservation after{};
        if (virtual_bag_capture_move_item_observation(item, target_bag, target_slot, &after)) {
            log_move_item_observation("post", item, count, target_bag, target_slot,
                                      caller_offset, after);
        }
    } else {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "MoveItem passthrough item=%p count=%d target=%d/%d "
                            "caller=libgame+0x%llx result=%d",
                            item, count, target_bag, target_slot,
                            static_cast<unsigned long long>(caller_offset), result);
    }
    return result;
}

int equip_item_from_inven_to_slot_wrapper(void* character, int32_t bag, int32_t slot,
                                          int32_t equip_slot) {
    if (extension_bag_internal_equip_active()) {
        if (g_backup_equip_item_from_inven_to_slot == nullptr) return 0;
        return g_backup_equip_item_from_inven_to_slot(character, bag, slot, equip_slot);
    }
    const int result = stage4_equip_item(character, bag, slot, equip_slot, inventory_item_at,
                                         extension_bag_enabled()
                                             ? extension_bag_identify_native_item
                                             : nullptr,
                                         extension_bag_enabled()
                                             ? extension_bag_equip_projected_item
                                             : nullptr,
                                         g_backup_equip_item_from_inven_to_slot,
                                         extension_bag_enabled()
                                             ? extension_bag_view_item_at
                                             : nullptr);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "EquipItemFromInvenToSlot bag=%d slot=%d equip_slot=%d result=%d",
                        bag, slot, equip_slot, result);
    return result;
}

int put_jewel_wrapper(void* equip_item, void* jewel_item) {
    return stage4_put_jewel(equip_item, jewel_item,
                            extension_bag_enabled() ? extension_bag_identify_native_item : nullptr,
                            g_backup_put_jewel,
                            extension_bag_enabled() ? virtual_bag_put_jewel_native : nullptr);
}

void button_equip_exe_wrapper(void* button) {
    // 装备按钮函数级接管：详情物品为扩展槽背包物品时在原函数内联删源
    // （b7e04）之前分流，源物品销毁与袋表移交由扩展事务完成；其余一律
    // 走原函数，保持完全原版流程。
    const VirtualBagEquipButtonResult result =
        virtual_bag_handle_backpack_button_equip_result();
    if (result == VirtualBagEquipButtonResult::kHandled) return;
    if (result == VirtualBagEquipButtonResult::kBlocked) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "ButtonEquipExe extension item blocked; original backup skipped");
        return;
    }
    g_backup_button_equip_exe(button);
}

// R-55：详情出售按钮函数级标记。原按钮在同一调用内先调 UIEquip_OKDestroyItem
// 预演（x23=0x666）取确认框展示金额、再创建 YesNo 弹窗；预演期间置 thread_local
// 标记，供 ok_destroy_item_wrapper 区分「预演」与「弹窗 OK 真实结算」。
void button_destroy_exe_wrapper(void* button) {
    const bool previous = g_in_equip_destroy_button;
    g_in_equip_destroy_button = true;
    if (g_backup_button_destroy_exe != nullptr) g_backup_button_destroy_exe(button);
    g_in_equip_destroy_button = previous;
}

void button_unequip_exe_wrapper(void* button) {
    // 卸下按钮函数级接管：desc_type=1 卸袋（原版袋/扩展袋）时模块接管，
    // 原版背包满时把袋对象放回其他袋行或收编扩展空位；物品绝不消失。
    // 卸装备与其他场景一律走原函数。
    bool no_space = false;
    bool not_empty = false;
    if (extension_bag_handle_original_bag_unequip(&no_space, &not_empty)) {
        if (not_empty) {
            extension_bag_show_not_empty_popup();
        } else if (no_space) {
            extension_bag_show_no_space_popup();
        }
        return;
    }
    g_backup_button_unequip_exe(button);
}

void ok_confirm_use_item_wrapper(void* item) {
    if (virtual_bag_handle_confirm_use_item(item)) return;
    if (g_backup_ok_confirm_use_item == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "UIEquip_OKConfrimUseItem backup unavailable item=%p", item);
        return;
    }
    g_backup_ok_confirm_use_item(item);
}

// R-55 原版背包详情出售接管（VM-41）。UIEquip_OKDestroyItem(0xb83d0) 无参：背包
// 结算分支从面板上下文读 desc_type(0x304a43)/bag(0x304a41)/ctrl(0x3049e8)，再经
// ControlObject_GetCursorIndex 取槽，随后调 0x1261c4 只按 b 段结算。两态由
// button_destroy_exe_wrapper 设置的 thread_local 标记区分：按钮预演只回填展示金额、
// 弹窗 OK 才真实结算（返回地址经 Dobby 桥后不可信，不作判定依据）。
// 关闭态/非背包详情/校验失败一律 backup（fail-safe，不半执行）。
uint64_t ok_destroy_item_wrapper() {
    const auto call_backup = []() -> uint64_t {
        return g_backup_ok_destroy_item == nullptr ? 0 : g_backup_ok_destroy_item();
    };
    const VanillaSellRoute route =
        vanilla_sell_route(stack_limit_enabled(), g_in_equip_destroy_button);
    if (route == VanillaSellRoute::kBackup) return call_backup();

    // 仅背包物品详情（desc_type==2）走结算；装备详情(0)/扩展袋(1) 回原版。
    if (g_base == 0 || fn_control_object_get_cursor_index == nullptr) return call_backup();
    const uint8_t desc_type = *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_DESC_TYPE_VMA);
    if (desc_type != kUIEquipBagDescType) return call_backup();

    const uint8_t bag = *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA);
    void* ctrl = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA);
    if (ctrl == nullptr) return call_backup();
    const int slot = fn_control_object_get_cursor_index(ctrl);
    if (slot < 0 || slot >= kInvenSlotsPerBag) return call_backup();

    void* item = s2_physical_slot_item(bag, slot);
    if (item == nullptr) return call_backup();
    // 类别门控（R-46 fail-closed）：非 count-encoded（装备/宝石/袋对象）不接管。
    if (item_count_encoding(item) != stack_codec::CountEncoding::kEncoded) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "vanilla sell backup reason=not_count_encoded bag=%d slot=%d item=%p",
                            bag, slot, item);
        return call_backup();
    }
    if (fn_get_bit == nullptr || fn_item_is_no_sell == nullptr ||
        fn_item_get_sell_price == nullptr || fn_get_cumulate_count == nullptr) {
        return call_backup();
    }
    const uint16_t flags =
        *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
    const int category = fn_get_bit(flags, 15, 6);
    if (category <= 0 || fn_item_is_no_sell(category) != 0) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "vanilla sell backup reason=no_sell bag=%d slot=%d category=%d", bag,
                            slot, category);
        return call_backup();
    }
    // canonical 经已 hook 的 getter：启用态返回 128a+b 全量（R-49）。
    const uint32_t canonical = static_cast<uint32_t>(fn_get_cumulate_count(item));
    const int64_t unit_price = static_cast<int64_t>(fn_item_get_sell_price(item));
    int64_t price = 0;
    if (!vanilla_sell_money(unit_price, canonical, &price)) {
        __android_log_print(
            ANDROID_LOG_INFO, kTag,
            "vanilla sell backup reason=price_bounds bag=%d slot=%d category=%d canonical=%u unit=%lld",
            bag, slot, category, canonical, static_cast<long long>(unit_price));
        return call_backup();
    }

    if (route == VanillaSellRoute::kPreview) {
        // 按钮预演：只回填弹窗展示金额，不结算。
        __android_log_print(
            ANDROID_LOG_INFO, kTag,
            "vanilla sell preview bag=%d slot=%d category=%d canonical=%u unit=%lld price=%lld",
            bag, slot, category, canonical, static_cast<long long>(unit_price),
            static_cast<long long>(price));
        return static_cast<uint64_t>(price);
    }

    // kTakeover：加钱 → 删整堆 → 刷新。加钱/删堆失败不回 backup（避免按原版错误
    // 金额二次结算），失败即退款中止。
    if (fn_add_money == nullptr || fn_minus_money == nullptr ||
        fn_remove_item_direct == nullptr) {
        return call_backup();
    }
    if (!fn_add_money(price)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "vanilla sell credit failed bag=%d slot=%d price=%lld", bag, slot,
                            static_cast<long long>(price));
        return 0;
    }
    // 提交前复核槽内对象未变（防并发替换/释放）。
    if (s2_physical_slot_item(bag, slot) != item) {
        fn_minus_money(price);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "vanilla sell stale before remove bag=%d slot=%d refunded=1", bag,
                            slot);
        return 0;
    }
    fn_remove_item_direct(bag, slot);
    if (s2_physical_slot_item(bag, slot) == item) {
        fn_minus_money(price);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "vanilla sell remove failed bag=%d slot=%d refunded=1", bag, slot);
        return 0;
    }
    if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "vanilla sell committed bag=%d slot=%d category=%d canonical=%u unit=%lld price=%lld",
        bag, slot, category, canonical, static_cast<long long>(unit_price),
        static_cast<long long>(price));
    return 0;
}

uint64_t equip_control_event_proc_wrapper(void* control, uint64_t event, void* x2, void* param) {
    uint64_t original_result = 0;
    const VirtualBagEquipControlEventResult result =
        virtual_bag_handle_equip_control_event(
            control, event, x2, param,
            g_backup_equip_control_event_proc, &original_result);
    if (result == VirtualBagEquipControlEventResult::kHandled) return original_result;
    if (result == VirtualBagEquipControlEventResult::kBlocked) return 0;
    return g_backup_equip_control_event_proc == nullptr
        ? 0 : g_backup_equip_control_event_proc(control, event, x2, param);
}

bool target_is_executable(uintptr_t target, const char* name) {
    if (target == 0 || !game_memory_accessible(reinterpret_cast<void*>(target), 4, 'x')) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "%s target invalid: %p", name,
                            reinterpret_cast<void*>(target));
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "%s target validated: %p", name,
                        reinterpret_cast<void*>(target));
    return true;
}

bool rollback_installed_hooks(const InstalledHook* hooks, std::size_t count) {
    bool rolled_back = true;
    for (std::size_t index = count; index > 0; --index) {
        const InstalledHook& hook = hooks[index - 1];
        const int result = g_unhook_func == nullptr || hook.target == nullptr
            ? -1 : g_unhook_func(hook.target);
        if (result == 0) {
            if (hook.backup != nullptr) *hook.backup = nullptr;
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "hook rollback OK name=%s", hook.name);
        } else {
            rolled_back = false;
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "hook rollback failed name=%s result=%d", hook.name, result);
        }
    }
    if (!rolled_back) {
        g_install_blocked.store(true, std::memory_order_release);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "hook rollback incomplete; retry blocked");
    }
    return rolled_back;
}

bool install_locked() {
    if (g_installed.load(std::memory_order_acquire)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "hook install skipped: already installed");
        return true;
    }
    if (g_install_blocked.load(std::memory_order_acquire)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "hook install blocked after rollback failure");
        return false;
    }
    if (g_hook_func == nullptr || !bridge_ready()) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "hook install deferred: hook_func=%p base=%p",
                            reinterpret_cast<void*>(g_hook_func),
                            reinterpret_cast<void*>(g_base));
        return false;
    }

    const uintptr_t find_item = g_base + fn_resolve("F_FIND_ITEM_VMA", F_FIND_ITEM_VMA);
    const uintptr_t have_item = g_base + fn_resolve("F_INVEN_HAVE_ITEM_VMA", F_INVEN_HAVE_ITEM_VMA);
    const uintptr_t get_item_count = g_base + fn_resolve("F_INVEN_GET_ITEM_COUNT_VMA", F_INVEN_GET_ITEM_COUNT_VMA);
    const uintptr_t get_cumulate_count = g_base + fn_resolve(
        "F_GET_CUMULATE_COUNT_VMA", F_GET_CUMULATE_COUNT_VMA);
    const uintptr_t consume_item = g_base + fn_resolve("F_CONSUME_ITEM_VMA", F_CONSUME_ITEM_VMA);
    const uintptr_t remove_item = g_base + fn_resolve("F_REMOVE_ITEM_VMA", F_REMOVE_ITEM_VMA);
    const uintptr_t equip_item_from_inven_to_slot =
        g_base + fn_resolve("F_EQUIP_ITEM_FROM_INVEN_TO_SLOT_VMA", F_EQUIP_ITEM_FROM_INVEN_TO_SLOT_VMA);
    const uintptr_t put_jewel = g_base + fn_resolve("F_PUT_JEWEL_VMA", F_PUT_JEWEL_VMA);
    const uintptr_t is_having_empty_slot = g_base + fn_resolve(
        "F_INVEN_IS_HAVING_EMPTY_SLOT_VMA", F_INVEN_IS_HAVING_EMPTY_SLOT_VMA);
    const uintptr_t unequip_item_to_inven = g_base + fn_resolve(
        "F_UNEQUIP_VMA", F_UNEQUIP_VMA);
    const uintptr_t button_equip_exe = g_base + fn_resolve(
        "F_UIEQUIP_BUTTON_EQUIP_EXE_VMA", F_UIEQUIP_BUTTON_EQUIP_EXE_VMA);
    const uintptr_t button_unequip_exe = g_base + fn_resolve(
        "F_UIEQUIP_BUTTON_UNEQUIP_EXE_VMA", F_UIEQUIP_BUTTON_UNEQUIP_EXE_VMA);
    // R-55 详情出售按钮：预演标记源（与 ok_destroy_item 同事务安装）。
    const uintptr_t button_destroy_exe = g_base + fn_resolve(
        "F_UIEQUIP_BUTTON_DESTROY_EXE_VMA", F_UIEQUIP_BUTTON_DESTROY_EXE_VMA);
    const uintptr_t ok_confirm_use_item = g_base + fn_resolve(
        "F_UIEQUIP_OK_CONFIRM_USE_ITEM_VMA", F_UIEQUIP_OK_CONFIRM_USE_ITEM_VMA);
    // R-55 原版背包详情出售接管（追加链尾）。
    const uintptr_t ok_destroy_item = g_base + fn_resolve(
        "F_UIEQUIP_OK_DESTROY_ITEM_VMA", F_UIEQUIP_OK_DESTROY_ITEM_VMA);
    const uintptr_t save_item = g_base + fn_resolve(
        "F_INVEN_SAVE_ITEM_VMA", F_INVEN_SAVE_ITEM_VMA);
    const uintptr_t move_item = g_base + fn_resolve(
        "F_INVEN_MOVE_ITEM_VMA", F_INVEN_MOVE_ITEM_VMA);
    const uintptr_t equip_control_event_proc = g_base + fn_resolve(
        "F_UIEQUIP_EQUIP_CONTROL_EVENT_PROC_VMA", F_UIEQUIP_EQUIP_CONTROL_EVENT_PROC_VMA);
    const uintptr_t refresh_item_area = g_base + fn_resolve(
        "F_UIEQUIP_REFRESH_ITEM_AREA_VMA", F_UIEQUIP_REFRESH_ITEM_AREA_VMA);
    // S2 写侧进位框架新增（H-18..H-21，追加链尾；符号/typedef 沿用既有登记，
    // 无新增 VMA 常量）。
    const uintptr_t save_item_direct = g_base + fn_resolve(
        "F_INVEN_SAVE_ITEM_DIRECT_VMA", F_INVEN_SAVE_ITEM_DIRECT_VMA);
    const uintptr_t item_system_divide = g_base + fn_resolve(
        "F_ITEMSYSTEM_DIVIDE_VMA", F_ITEMSYSTEM_DIVIDE_VMA);
    const uintptr_t make_item = g_base + fn_resolve(
        "F_MAKE_ITEM_VMA", F_MAKE_ITEM_VMA);
    const uintptr_t remove_item_data = g_base + fn_resolve(
        "F_INVEN_REMOVE_ITEM_DATA_VMA", F_INVEN_REMOVE_ITEM_DATA_VMA);
    if (!target_is_executable(find_item, "INVEN_FindItem") ||
        !target_is_executable(have_item, "INVEN_HaveItem") ||
        !target_is_executable(get_item_count, "INVEN_GetItemCount") ||
        !target_is_executable(get_cumulate_count, "ITEM_GetCumulateCount") ||
        !target_is_executable(consume_item, "INVEN_ConsumeItem") ||
        !target_is_executable(remove_item, "INVEN_RemoveItem") ||
        !target_is_executable(equip_item_from_inven_to_slot, "CHAR_EquipItemFromInvenToSlot") ||
        !target_is_executable(put_jewel, "ITEMSYSTEM_PutJewel") ||
        !target_is_executable(is_having_empty_slot, "INVEN_IsHavingEmptySlot") ||
        !target_is_executable(unequip_item_to_inven, "CHAR_UnequipItemToInven") ||
        !target_is_executable(button_equip_exe, "UIEquip_ButtonEquipExe") ||
        !target_is_executable(button_unequip_exe, "UIEquip_ButtonUnequipExe") ||
        !target_is_executable(button_destroy_exe, "UIEquip_ButtonDestroyExe") ||
        !target_is_executable(ok_confirm_use_item, "UIEquip_OKConfrimUseItem") ||
        !target_is_executable(ok_destroy_item, "UIEquip_OKDestroyItem") ||
        !target_is_executable(save_item, "INVEN_SaveItem") ||
        !target_is_executable(move_item, "INVEN_MoveItem") ||
        !target_is_executable(equip_control_event_proc, "UIEquip_EquipControlEventProc") ||
        !target_is_executable(refresh_item_area, "UIEquip_RefreshItemArea") ||
        !target_is_executable(save_item_direct, "INVEN_SaveItemDirect") ||
        !target_is_executable(item_system_divide, "ITEMSYSTEM_Divide") ||
        !target_is_executable(make_item, "ITEMSYSTEM_MakeItem") ||
        !target_is_executable(remove_item_data, "INVEN_RemoveItemData")) {
        return false;
    }

    g_backup_find_item = nullptr;
    g_backup_have_item = nullptr;
    g_backup_get_item_count = nullptr;
    g_backup_get_cumulate_count = nullptr;
    g_backup_consume_item = nullptr;
    g_backup_remove_item = nullptr;
    g_backup_equip_item_from_inven_to_slot = nullptr;
    g_backup_put_jewel = nullptr;
    g_backup_is_having_empty_slot = nullptr;
    g_backup_unequip_item_to_inven = nullptr;
    g_backup_button_equip_exe = nullptr;
    g_backup_button_unequip_exe = nullptr;
    g_backup_button_destroy_exe = nullptr;
    g_backup_ok_confirm_use_item = nullptr;
    g_backup_ok_destroy_item = nullptr;
    g_backup_save_item = nullptr;
    g_backup_move_item = nullptr;
    g_backup_refresh_item_area = nullptr;
    g_backup_equip_control_event_proc = nullptr;
    g_backup_save_item_direct = nullptr;
    g_backup_item_system_divide = nullptr;
    g_backup_make_item = nullptr;
    g_backup_remove_item_data = nullptr;

    InstalledHook installed[23]{};
    std::size_t installed_count = 0;
    const auto install_hook = [&](void* target, void* replacement, void** backup,
                                  const char* name) -> bool {
        const int result = g_hook_func(target, replacement, backup);
        if (result == 0) {
            installed[installed_count++] = {target, backup, name};
        }
        if (result != 0 || backup == nullptr || *backup == nullptr) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "%s hook failed result=%d backup=%p; rolling back %zu hooks",
                                name, result, backup == nullptr ? nullptr : *backup,
                                installed_count);
            rollback_installed_hooks(installed, installed_count);
            return false;
        }
        return true;
    };

    if (!install_hook(reinterpret_cast<void*>(find_item),
                      reinterpret_cast<void*>(find_item_wrapper),
                      reinterpret_cast<void**>(&g_backup_find_item), "FindItem") ||
        !install_hook(reinterpret_cast<void*>(have_item),
                      reinterpret_cast<void*>(have_item_wrapper),
                      reinterpret_cast<void**>(&g_backup_have_item), "HaveItem") ||
        !install_hook(reinterpret_cast<void*>(get_item_count),
                      reinterpret_cast<void*>(get_item_count_wrapper),
                      reinterpret_cast<void**>(&g_backup_get_item_count), "GetItemCount") ||
        !install_hook(reinterpret_cast<void*>(get_cumulate_count),
                      reinterpret_cast<void*>(get_cumulate_count_wrapper),
                      reinterpret_cast<void**>(&g_backup_get_cumulate_count),
                      "GetCumulateCount") ||
        !install_hook(reinterpret_cast<void*>(consume_item),
                      reinterpret_cast<void*>(consume_item_wrapper),
                      reinterpret_cast<void**>(&g_backup_consume_item), "ConsumeItem") ||
        !install_hook(reinterpret_cast<void*>(remove_item),
                      reinterpret_cast<void*>(remove_item_wrapper),
                      reinterpret_cast<void**>(&g_backup_remove_item), "RemoveItem") ||
        !install_hook(reinterpret_cast<void*>(equip_item_from_inven_to_slot),
                      reinterpret_cast<void*>(equip_item_from_inven_to_slot_wrapper),
                      reinterpret_cast<void**>(&g_backup_equip_item_from_inven_to_slot),
                      "EquipItemFromInvenToSlot") ||
        !install_hook(reinterpret_cast<void*>(put_jewel),
                      reinterpret_cast<void*>(put_jewel_wrapper),
                      reinterpret_cast<void**>(&g_backup_put_jewel), "PutJewel") ||
        !install_hook(reinterpret_cast<void*>(is_having_empty_slot),
                      reinterpret_cast<void*>(is_having_empty_slot_wrapper),
                      reinterpret_cast<void**>(&g_backup_is_having_empty_slot),
                      "IsHavingEmptySlot") ||
        !install_hook(reinterpret_cast<void*>(unequip_item_to_inven),
                      reinterpret_cast<void*>(unequip_item_to_inven_wrapper),
                      reinterpret_cast<void**>(&g_backup_unequip_item_to_inven),
                      "UnequipItemToInven") ||
        !install_hook(reinterpret_cast<void*>(button_equip_exe),
                      reinterpret_cast<void*>(button_equip_exe_wrapper),
                      reinterpret_cast<void**>(&g_backup_button_equip_exe),
                      "ButtonEquipExe") ||
        !install_hook(reinterpret_cast<void*>(button_unequip_exe),
                      reinterpret_cast<void*>(button_unequip_exe_wrapper),
                      reinterpret_cast<void**>(&g_backup_button_unequip_exe),
                      "ButtonUnequipExe") ||
        !install_hook(reinterpret_cast<void*>(button_destroy_exe),
                      reinterpret_cast<void*>(button_destroy_exe_wrapper),
                      reinterpret_cast<void**>(&g_backup_button_destroy_exe),
                      "ButtonDestroyExe") ||
        !install_hook(reinterpret_cast<void*>(ok_confirm_use_item),
                      reinterpret_cast<void*>(ok_confirm_use_item_wrapper),
                      reinterpret_cast<void**>(&g_backup_ok_confirm_use_item),
                      "UIEquip_OKConfrimUseItem") ||
        !install_hook(reinterpret_cast<void*>(ok_destroy_item),
                      reinterpret_cast<void*>(ok_destroy_item_wrapper),
                      reinterpret_cast<void**>(&g_backup_ok_destroy_item),
                      "UIEquip_OKDestroyItem") ||
        !install_hook(reinterpret_cast<void*>(save_item),
                      reinterpret_cast<void*>(save_item_wrapper),
                      reinterpret_cast<void**>(&g_backup_save_item),
                      "SaveItem") ||
        !install_hook(reinterpret_cast<void*>(move_item),
                      reinterpret_cast<void*>(move_item_wrapper),
                      reinterpret_cast<void**>(&g_backup_move_item),
                      "MoveItem") ||
        !install_hook(reinterpret_cast<void*>(equip_control_event_proc),
                      reinterpret_cast<void*>(equip_control_event_proc_wrapper),
                      reinterpret_cast<void**>(&g_backup_equip_control_event_proc),
                      "UIEquip_EquipControlEventProc") ||
        !install_hook(reinterpret_cast<void*>(refresh_item_area),
                      reinterpret_cast<void*>(refresh_item_area_wrapper),
                      reinterpret_cast<void**>(&g_backup_refresh_item_area),
                      "UIEquip_RefreshItemArea") ||
        !install_hook(reinterpret_cast<void*>(save_item_direct),
                      reinterpret_cast<void*>(save_item_direct_wrapper),
                      reinterpret_cast<void**>(&g_backup_save_item_direct),
                      "SaveItemDirect") ||
        !install_hook(reinterpret_cast<void*>(item_system_divide),
                      reinterpret_cast<void*>(item_system_divide_wrapper),
                      reinterpret_cast<void**>(&g_backup_item_system_divide),
                      "ItemSystemDivide") ||
        !install_hook(reinterpret_cast<void*>(make_item),
                      reinterpret_cast<void*>(make_item_wrapper),
                      reinterpret_cast<void**>(&g_backup_make_item),
                      "ItemSystemMakeItem") ||
        !install_hook(reinterpret_cast<void*>(remove_item_data),
                      reinterpret_cast<void*>(remove_item_data_wrapper),
                      reinterpret_cast<void**>(&g_backup_remove_item_data),
                      "RemoveItemData")) {
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "hook install OK api=%u count=23 FindItem=%p/%p GetItemCount=%p/%p GetCumulateCount=%p/%p ConsumeItem=%p/%p RemoveItem=%p/%p OKConfirmUseItem=%p/%p OKDestroyItem=%p/%p EquipItemFromInvenToSlot=%p/%p MoveItem=%p/%p EquipControlEventProc=%p/%p RefreshItemArea=%p/%p SaveItemDirect=%p/%p ItemSystemDivide=%p/%p ItemSystemMakeItem=%p/%p RemoveItemData=%p/%p ButtonDestroyExe=%p/%p",
                        kNativeApiVersion,
                        reinterpret_cast<void*>(find_item), reinterpret_cast<void*>(g_backup_find_item),
                        reinterpret_cast<void*>(get_item_count), reinterpret_cast<void*>(g_backup_get_item_count),
                        reinterpret_cast<void*>(get_cumulate_count), reinterpret_cast<void*>(g_backup_get_cumulate_count),
                        reinterpret_cast<void*>(consume_item), reinterpret_cast<void*>(g_backup_consume_item),
                        reinterpret_cast<void*>(remove_item), reinterpret_cast<void*>(g_backup_remove_item),
                        reinterpret_cast<void*>(ok_confirm_use_item), reinterpret_cast<void*>(g_backup_ok_confirm_use_item),
                        reinterpret_cast<void*>(ok_destroy_item), reinterpret_cast<void*>(g_backup_ok_destroy_item),
                        reinterpret_cast<void*>(equip_item_from_inven_to_slot),
                        reinterpret_cast<void*>(g_backup_equip_item_from_inven_to_slot),
                        reinterpret_cast<void*>(move_item),
                        reinterpret_cast<void*>(g_backup_move_item),
                        reinterpret_cast<void*>(equip_control_event_proc),
                        reinterpret_cast<void*>(g_backup_equip_control_event_proc),
                        reinterpret_cast<void*>(refresh_item_area),
                        reinterpret_cast<void*>(g_backup_refresh_item_area),
                        reinterpret_cast<void*>(save_item_direct),
                        reinterpret_cast<void*>(g_backup_save_item_direct),
                        reinterpret_cast<void*>(item_system_divide),
                        reinterpret_cast<void*>(g_backup_item_system_divide),
                        reinterpret_cast<void*>(make_item),
                        reinterpret_cast<void*>(g_backup_make_item),
                        reinterpret_cast<void*>(remove_item_data),
                        reinterpret_cast<void*>(g_backup_remove_item_data),
                        reinterpret_cast<void*>(button_destroy_exe),
                        reinterpret_cast<void*>(g_backup_button_destroy_exe));
    return true;
}

}  // namespace

void inventory_native_hook_call_refresh_item_area_original() {
    const UiEquipRefreshItemAreaFn original = g_backup_refresh_item_area != nullptr
        ? g_backup_refresh_item_area : fn_ui_equip_refresh_item_area;
    if (original != nullptr) original();
}

void inventory_native_hook_on_api(const NativeAPIEntries* entries) {
    if (entries == nullptr || entries->hook_func == nullptr || entries->unhook_func == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "native_init received invalid API entries");
        return;
    }
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (entries->version < kNativeApiVersion) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "native API version unsupported: %u", entries->version);
        return;
    }
    g_hook_func = entries->hook_func;
    g_unhook_func = entries->unhook_func;
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "native_init called api_version=%u hook_func=%p",
                        entries->version, reinterpret_cast<void*>(g_hook_func));
    install_locked();
}

void inventory_native_hook_on_module_loaded(const char* name, void*) {
    if (name == nullptr || std::strstr(name, "libgame.so") == nullptr) return;
    __android_log_print(ANDROID_LOG_INFO, kTag, "module loaded: %s", name);
    inventory_native_hook_install_if_ready();
}

void inventory_native_hook_install_if_ready() {
    if (g_installing.exchange(true, std::memory_order_acq_rel)) return;
    {
        std::lock_guard<std::mutex> lock(g_hook_mutex);
        install_locked();
    }
    g_installing.store(false, std::memory_order_release);
}

extern "C" [[gnu::visibility("default")]] [[gnu::used]]
NativeOnModuleLoaded native_init(const NativeAPIEntries* entries) {
    inventory_native_hook_on_api(entries);
    return inventory_native_hook_on_module_loaded;
}

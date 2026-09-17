// special_equip.cpp —— 特殊装备解锁：从 ITEMDATABASE +7 bit4 的 NPC 保护中放行非特殊 NPC。
//
// 设计口径、取舍理由与角色闸门归属见 feature.h 头部注释。

#include "features/special_equip/feature.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "core/native/qol_log.h"
#include "features/special_equip/rules.h"
#include "game/access/char.h"
#include "game/access/item_class.h"

namespace qol::features::special_equip {
namespace {

// LSPosed NativeHook 写入的原始函数指针（宿主经 on_resource_ready 交还）：wrapper 必须经
// backup 转发，否则原版判定丢失。ABI 与游戏函数一致，此处定义不引入符号头。
using CanEquipFn = int (*)(void*, void*);
using CanUnequipFn = int (*)(void*, void*);

std::atomic<bool> g_gate{false};

std::atomic<void*> g_backup_can_equip{nullptr};
std::atomic<void*> g_backup_can_unequip{nullptr};

// 真机验收用的「翻转计数」：只有「原版拒绝 → 本域放行」才 +1，便于确认生效范围。
std::atomic<uint64_t> g_override_count{0};

// 真机排障用的「未放行原因」预算：UI 每次构建详情菜单都会调，不设上限会刷爆日志。
std::atomic<int> g_reject_log_budget{120};

// 原版判定链重放依赖（不含「菜单角色」——那只在出售门禁与安装门控需要）。
bool replay_deps_ready() {
    return game::item_equip_primitives_ready() && game::char_can_change_equip_resolved();
}

// 仅 debug 日志开启时记录，避免热路径噪声（穿装/卸下都是用户操作级频率，无需预算门控）。
void log_override(const char* what, void* ch, void* item) {
    if (!qol_log_debug_enabled()) return;
    QOL_LOG_DEBUG(QolDomain::kSpecialEquip, "%s overridden ch=%p item=%p count=%llu", what, ch, item,
                  static_cast<unsigned long long>(g_override_count.load(std::memory_order_relaxed)));
}

// 「原版拒绝且本域也未放行」的原因快照。UI 的装备按钮灰化正是走这条路径，
// 打出来即可判定是 ①真装备 / ③角色权限 / ④等级 中的哪一道挡住的。
void log_reject(int category, void* ch, void* item) {
    if (!qol_log_debug_enabled()) return;
    if (g_reject_log_budget.fetch_sub(1, std::memory_order_relaxed) <= 0) return;
    const uint8_t* rec = category >= 0
        ? static_cast<const uint8_t*>(game::item_class_record(static_cast<uint16_t>(category)))
        : nullptr;
    QOL_LOG_DEBUG(QolDomain::kSpecialEquip,
                  "not overridden ch=%p item=%p cat=%d bit4=%d ctype=%d mercSlot=%d "
                  "realEquip=%d canChange=%d chLv=%d itemLv=%d",
                  ch, item, category,
                  rec != nullptr ? (game::item_class_no_equip_bit(rec) ? 1 : 0) : -1,
                  game::char_type(ch), game::char_merc_slot(ch),
                  game::item_equip_primitives_ready()
                      ? (game::item_is_real_equip(item) ? 1 : 0)
                      : -1,
                  game::char_can_change_equip_resolved()
                      ? (game::char_can_change_equip(ch) ? 1 : 0)
                      : -1,
                  game::char_level(ch),
                  game::item_equip_primitives_ready() ? game::item_equip_level(item) : -1);
}

}  // namespace

// 依赖是否齐备。缺任一即 fail-closed（与 attribute_range / simple_mode 的 once 语义一致）：
// 两个入口 hook 的符号解析由宿主负责（解析失败会使插件 failed），此处只校验原版判定链
// 重放所需的游戏原语：ITEM_IsRealEquip / ITEM_GetEquipLevel / CHAR_CanChangeEquip /
// PARTY_GetMenuCharacter。
bool deps_ready() {
    return replay_deps_ready() && game::char_menu_character_resolved();
}

void log_deps_missing() {
    QOL_LOG_ERROR(QolDomain::kSpecialEquip,
                  "symbols unresolved (is_real_equip=%d can_change_equip=%d equip_level=%d "
                  "menu_char=%d); not installed",
                  game::item_is_real_equip_resolved(), game::char_can_change_equip_resolved(),
                  game::item_equip_level_resolved(), game::char_menu_character_resolved());
}

void set_gate(bool enabled) {
    g_gate.store(enabled, std::memory_order_release);
    QOL_LOG_INFO(QolDomain::kSpecialEquip, "enabled=%d", enabled);
}

bool gate_enabled() {
    return g_gate.load(std::memory_order_acquire);
}

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    (void)ctx;
    if (resource_id == nullptr) return 0;
    if (std::strcmp(resource_id, kResourceIdCanEquipItem) == 0) {
        g_backup_can_equip.store(backup, std::memory_order_release);
        return 0;
    }
    if (std::strcmp(resource_id, kResourceIdCanUnequipItem) == 0) {
        g_backup_can_unequip.store(backup, std::memory_order_release);
        return 0;
    }
    QOL_LOG_WARN(QolDomain::kSpecialEquip, "unknown resource id: %s", resource_id);
    return 0;
}

int can_equip_wrapper(void* ch, void* item) {
    const CanEquipFn backup =
        reinterpret_cast<CanEquipFn>(g_backup_can_equip.load(std::memory_order_acquire));
    if (backup == nullptr) return 0;                  // 未安装完成：不下放行（不会发生）
    if (backup(ch, item) != 0) return 1;              // 原版放行 → 快速路径，不介入
    if (ch == nullptr || item == nullptr) return 0;
    if (!replay_deps_ready()) {
        log_reject(game::item_class_category(item), ch, item);
        return 0;                                     // 依赖缺失：fail-closed，保持原版拒绝
    }
    if (!equip_replay_allows(game::item_is_real_equip(item), game::char_can_change_equip(ch),
                             game::char_level(ch), game::item_equip_level(item))) {
        log_reject(game::item_class_category(item), ch, item);  // UI 灰化走这条路径
        return 0;
    }
    g_override_count.fetch_add(1, std::memory_order_relaxed);
    log_override("can_equip", ch, item);
    return 1;
}

int can_unequip_wrapper(void* ch, void* item) {
    const CanUnequipFn backup =
        reinterpret_cast<CanUnequipFn>(g_backup_can_unequip.load(std::memory_order_acquire));
    if (backup == nullptr) return 0;
    if (backup(ch, item) != 0) return 1;
    if (ch == nullptr || item == nullptr) return 0;
    if (!game::char_can_change_equip_resolved()) {
        log_reject(game::item_class_category(item), ch, item);
        return 0;
    }
    if (!unequip_replay_allows(game::char_can_change_equip(ch))) {
        log_reject(game::item_class_category(item), ch, item);
        return 0;
    }
    g_override_count.fetch_add(1, std::memory_order_relaxed);
    log_override("can_unequip", ch, item);
    return 1;
}

bool sell_gate_should_block() {
    if (!gate_enabled()) return false;  // 插件未启用：谓词常驻但空转放行
    const int desc_type = game::equip_desc_role_type();
    void* ch = game::char_menu_character();
    const bool resolved = game::char_can_change_equip_resolved();
    const bool can_change = resolved && ch != nullptr && game::char_can_change_equip(ch);
    if (!sell_gate_decides_block(desc_type, ch != nullptr, resolved, can_change)) {
        return false;
    }
    QOL_LOG_INFO(QolDomain::kSpecialEquip,
                 "sell rejected: menu char cannot change equip ch=%p", ch);
    return true;
}

}  // namespace qol::features::special_equip

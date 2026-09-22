// special_equip.cpp —— 特殊装备解锁：从 ITEMDATABASE +7 bit4 的 NPC 保护中放行非特殊 NPC。
//
// 设计口径、取舍理由与角色闸门归属见 feature.h 头部注释。

#include "feature.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdio>

#include "rules.h"

namespace qol::features::special_equip {
namespace {

// LSPosed NativeHook 写入的原始函数指针（宿主经 on_resource_ready 交还）：wrapper 必须经
// backup 转发，否则原版判定丢失。ABI 与游戏函数一致，此处定义不引入符号头。
using CanEquipFn = int (*)(void*, void*);
using CanUnequipFn = int (*)(void*, void*);

std::atomic<bool> g_gate{false};
const contracts::ItemFactsApi* g_item_facts = nullptr;
const contracts::CharacterFactsApi* g_character_facts = nullptr;

std::atomic<void*> g_backup_can_equip{nullptr};
std::atomic<void*> g_backup_can_unequip{nullptr};

// 真机验收用的「翻转计数」：只有「原版拒绝 → 本域放行」才 +1，便于确认生效范围。
std::atomic<uint64_t> g_override_count{0};

// 真机排障用的「未放行原因」预算：UI 每次构建详情菜单都会调，不设上限会刷爆日志。
std::atomic<int> g_reject_log_budget{120};

void log_write(int32_t level, const char* format, va_list args) {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->log.write == nullptr || format == nullptr) return;
    char message[contracts::kMessageCap + 1]{};
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    if (length < 0) return;
    const uint32_t message_length = static_cast<uint32_t>(
        length < static_cast<int>(contracts::kMessageCap) ? length : contracts::kMessageCap);
    (void)sdk->log.write(level, 17, "plugins/special_equip/feature.cpp", message, message_length);
}

void log_debug_impl(const char* format, ...) {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->log.debug_enabled == nullptr || sdk->log.debug_enabled() == 0) {
        return;
    }
    va_list args;
    va_start(args, format);
    log_write(0, format, args);
    va_end(args);
}

// 原版判定链重放依赖（不含「菜单角色」——那只在出售门禁与安装门控需要）。
bool replay_deps_ready() {
    const auto* game = game_host();
    const auto* extras = character_extras();
    return game != nullptr && g_item_facts != nullptr && g_item_facts->is_real_equip != nullptr &&
           game->read_only.item.equip_level != nullptr &&
           extras != nullptr && extras->read.can_change_equip != nullptr;
}

// 仅 debug 日志开启时记录，避免热路径噪声（穿装/卸下都是用户操作级频率，无需预算门控）。
void log_override(const char* what, void* ch, void* item) {
    log_debug_impl("%s overridden ch=%p item=%p count=%llu", what, ch, item,
              static_cast<unsigned long long>(g_override_count.load(std::memory_order_relaxed)));
}

// 「原版拒绝且本域也未放行」的原因快照。UI 的装备按钮灰化正是走这条路径，
// 打出来即可判定是 ①真装备 / ③角色权限 / ④等级 中的哪一道挡住的。
void log_reject(int category, void* ch, void* item) {
    if (g_reject_log_budget.fetch_sub(1, std::memory_order_relaxed) <= 0) return;
    const auto* game = game_host();
    int32_t type = -1;
    int32_t merc_slot = -1;
    int32_t level = -1;
    int32_t can_change = 0;
    int32_t real_equip = 0;
    int32_t item_level = -1;
    if (g_character_facts != nullptr && g_character_facts->type != nullptr) {
        (void)g_character_facts->type(ch, &type);
    }
    if (character_extras() != nullptr && character_extras()->read.merc_slot != nullptr) {
        (void)character_extras()->read.merc_slot(ch, &merc_slot);
    }
    if (character_extras() != nullptr && character_extras()->read.level != nullptr) {
        (void)character_extras()->read.level(ch, &level);
    }
    if (character_extras() != nullptr && character_extras()->read.can_change_equip != nullptr) {
        (void)character_extras()->read.can_change_equip(ch, &can_change);
    }
    if (g_item_facts != nullptr && g_item_facts->is_real_equip != nullptr) {
        (void)g_item_facts->is_real_equip(item, &real_equip);
    }
    if (game != nullptr && game->read_only.item.equip_level != nullptr) {
        (void)game->read_only.item.equip_level(item, &item_level);
    }
    int32_t item_category = category;
    int32_t no_equip_bit = -1;
    if (g_item_facts != nullptr && g_item_facts->category != nullptr) {
        (void)g_item_facts->category(item, &item_category);
    }
    if (game != nullptr && game->read_only.item.class_no_equip_bit != nullptr && item_category >= 0) {
        (void)game->read_only.item.class_no_equip_bit(item_category, &no_equip_bit);
    }
    log_debug_impl("not overridden ch=%p item=%p cat=%d bit4=%d ctype=%d mercSlot=%d "
              "realEquip=%d canChange=%d chLv=%d itemLv=%d",
              ch, item, item_category, no_equip_bit, type, merc_slot, real_equip, can_change, level,
              item_level);
}

}  // namespace

void log_debug(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log_write(0, format, args);
    va_end(args);
}

void log_info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log_write(1, format, args);
    va_end(args);
}

void log_warn(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log_write(2, format, args);
    va_end(args);
}

void log_error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log_write(3, format, args);
    va_end(args);
}

// 依赖是否齐备。缺任一即 fail-closed（与 attribute_range / simple_mode 的 once 语义一致）：
// 两个入口 hook 的符号解析由宿主负责（解析失败会使插件 failed），此处只校验原版判定链
// 重放所需的游戏原语：ITEM_IsRealEquip / ITEM_GetEquipLevel / CHAR_CanChangeEquip /
// PARTY_GetMenuCharacter。
bool deps_ready() {
    const auto* game = game_host();
    return replay_deps_ready() && game != nullptr && character_extras() != nullptr &&
           character_extras()->read.menu_character != nullptr;
}

void log_deps_missing() {
    const auto* game = game_host();
    log_error("symbols unresolved (is_real_equip=%d can_change_equip=%d equip_level=%d "
              "menu_char=%d); not installed",
              g_item_facts != nullptr && g_item_facts->is_real_equip != nullptr,
              game != nullptr && character_extras()->read.can_change_equip != nullptr,
              game != nullptr && game->read_only.item.equip_level != nullptr,
              game != nullptr && character_extras()->read.menu_character != nullptr);
}

void set_gate(bool enabled) {
    g_gate.store(enabled, std::memory_order_release);
    log_info("enabled=%d", enabled);
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
    log_warn("unknown resource id: %s", resource_id);
    return 0;
}

void set_item_facts(const contracts::ItemFactsApi* facts) { g_item_facts = facts; }

void set_character_facts(const contracts::CharacterFactsApi* facts) { g_character_facts = facts; }

int can_equip_wrapper(void* ch, void* item) {
    const CanEquipFn backup =
        reinterpret_cast<CanEquipFn>(g_backup_can_equip.load(std::memory_order_acquire));
    if (backup == nullptr) return 0;                  // 未安装完成：不下放行（不会发生）
    if (backup(ch, item) != 0) return 1;              // 原版放行 → 快速路径，不介入
    if (ch == nullptr || item == nullptr) return 0;
    if (!replay_deps_ready()) {
        log_reject(-1, ch, item);
        return 0;                                     // 依赖缺失：fail-closed，保持原版拒绝
    }
    const auto* game = game_host();
    int32_t is_real_equip = 0;
    int32_t can_change_equip = 0;
    int32_t char_level = -1;
    int32_t item_level = -1;
    if (game == nullptr || g_item_facts == nullptr ||
        g_item_facts->is_real_equip(item, &is_real_equip).ok != kQolOk ||
        character_extras()->read.can_change_equip(ch, &can_change_equip).ok != kQolOk ||
        character_extras()->read.level(ch, &char_level).ok != kQolOk ||
        game->read_only.item.equip_level(item, &item_level).ok != kQolOk) {
        log_reject(-1, ch, item);
        return 0;
    }
    if (!equip_replay_allows(is_real_equip != 0, can_change_equip != 0, char_level, item_level)) {
        log_reject(-1, ch, item);  // UI 灰化走这条路径
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
    const auto* game = game_host();
    if (game == nullptr || character_extras()->read.can_change_equip == nullptr) {
        log_reject(-1, ch, item);
        return 0;
    }
    int32_t can_change_equip = 0;
    if (character_extras()->read.can_change_equip(ch, &can_change_equip).ok != kQolOk ||
        !unequip_replay_allows(can_change_equip != 0)) {
        log_reject(-1, ch, item);
        return 0;
    }
    g_override_count.fetch_add(1, std::memory_order_relaxed);
    log_override("can_unequip", ch, item);
    return 1;
}

int32_t sell_gate_should_block(void* ctx) {
    (void)ctx;
    if (!gate_enabled()) return 0;  // 插件未启用：谓词常驻但空转放行
    const auto* game = game_host();
    if (game == nullptr || character_extras()->read.menu_character == nullptr ||
        character_extras()->read.can_change_equip == nullptr ||
        game->read_only.item.equip_desc_role_type == nullptr) {
        return 1;
    }
    void* ch = nullptr;
    int32_t can_change = 0;
    if (character_extras()->read.menu_character(&ch).ok != kQolOk || ch == nullptr ||
        character_extras()->read.can_change_equip(ch, &can_change).ok != kQolOk) {
        return 1;
    }
    int32_t desc_type = -1;
    if (game->read_only.item.equip_desc_role_type(&desc_type).ok != kQolOk) return 1;
    if (!sell_gate_decides_block(desc_type, true, true, can_change != 0)) return 0;
    log_info("sell rejected: menu char cannot change equip ch=%p", ch);
    return 1;
}

}  // namespace qol::features::special_equip

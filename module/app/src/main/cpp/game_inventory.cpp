// game_inventory.cpp —— 背包域：背包/装备/金钱操作（parse 域）
// 由 game_ops_action.cpp / game_ops_value.cpp 拆分生成（纯搬代码，零逻辑变更）。

#include "game_inventory.h"

#include "game_access.h"
#include "game_state.h"
#include "game_ops_common.h"
#include "game_patch.h"
#include "game_ui_virtbag.h"
#include "stack_codec.h"

#include <android/log.h>
#include <cstdint>
#include <string>

// ---- 辅助定义（背包共用）----
std::string inventory_gained_json(void* const* before) {
    if (fn_get_bit == nullptr || fn_get_cumulate_count == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, "Inotia4Export", "inventory_gained_json: fn_get_bit/fn_get_cumulate_count not resolved");
        return std::string();
    }
    std::string s;
    int n = 0;
    struct Ctx { void* const* before; std::string* s; int* n; } ctx{before, &s, &n};
    for_each_bag_slot([](void* item, int b, int j, void* c) -> bool {
        Ctx* p = static_cast<Ctx*>(c);
        // ITEM_GetCumulateCount 自动适配 patch：可堆叠返回数量、装备/不可堆叠返回 1（已归一化）
        int count = fn_get_cumulate_count(item);
        void* old = p->before[b * 16 + j];
        if (old == item) {
            count -= fn_get_cumulate_count(old);
            if (count <= 0) return false;
        } else if (old != nullptr) {
            return false;  // 同槽不同指针：旧物品被消耗/替换，非新增
        }
        if (*p->n > 0) *p->s += ",";
        uint16_t flags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
        *p->s += "{\"bag\":" + std::to_string(b);
        *p->s += ",\"slot\":" + std::to_string(j);
        *p->s += ",\"category\":" + std::to_string(fn_get_bit(flags, 15, 6));
        *p->s += ",\"count\":" + std::to_string(count) + "}";
        ++*p->n;
        return false;
    }, &ctx);
    return s;
}

std::string data_op_set_money(int64_t money) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_set_money == nullptr) return op_err("symbol not resolved");
    fn_set_money(money);
    return op_ok();
}

std::string data_op_add_money(int64_t delta) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_add_money == nullptr) return op_err("symbol not resolved");
    int r = fn_add_money(delta);
    return r ? op_ok() : op_err("add money failed");
}

std::string data_op_add_item(int32_t category, int32_t count) {
    if (!game_in_world()) return op_err("not in game");
    if (count <= 0) return op_err("bad count");
    if (fn_create_item == nullptr || fn_inven_save_item == nullptr || fn_inven_find_save_slot == nullptr)
        return op_err("symbol not resolved");
    // ITEMSYSTEM_CreateItem 创建物品对象（MakeItem 带 search_tbl 校验会失败，CreateItem 无此限制）
    void* item = fn_create_item(category, 0, 0, 0);
    if (item == nullptr) return op_err("create item failed");
    // 可堆叠判定：ITEMCLASSBASE 记录 +6 bit0（item_is_equip 同源，与 ITEM_GetCumulateCount 一致，
    // 与 patch 无关）。不可用 fn_get_cumulate_count 返回值区分——装备与数量 1 的可堆叠均返回 1。
    if (count > 1 && item_is_equip(item))
        return op_err("item not stackable");
    if (count > 1) {
        count = static_cast<int>(stack_codec::clamp_count(static_cast<uint32_t>(count), stack_limit_enabled()));
        uint32_t cf = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT);
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT) =
            stack_codec::write_count(cf, static_cast<uint32_t>(count));
    }
    int save_slot = fn_inven_find_save_slot(item, 0);
    if (save_slot <= 0) return op_err("inventory full");
    if (!fn_inven_save_item(item, nullptr)) return op_err("save item failed");
    return op_ok();
}

std::string data_op_minus_money(int64_t delta) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_minus_money == nullptr) return op_err("symbol not resolved");
    int r = fn_minus_money(delta);
    return r ? op_ok() : op_err("insufficient money");
}

std::string data_op_remove_item(int32_t category) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_remove_item == nullptr || fn_get_bit == nullptr) return op_err("symbol not resolved");
    // 按类别删第一个匹配物品（INVEN_RemoveItem 按 item 指针删，需先按类别定位）
    struct Ctx { int32_t category; int r; int bag; int slot; bool found; } ctx{category, 0, -1, -1, false};
    for_each_bag_slot([](void* item, int bag, int slot, void* c) -> bool {
        Ctx* p = static_cast<Ctx*>(c);
        uint16_t flags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
        if (fn_get_bit(flags, 15, 6) == p->category) {
            p->r = fn_remove_item(item);
            p->bag = bag;
            p->slot = slot;
            p->found = true;
            return true;
        }
        return false;
    }, &ctx);
    if (!ctx.found) return op_err("item not found");
    if (ctx.r) virtual_bag_sync_projected_slot(ctx.bag, ctx.slot);
    return ctx.r ? op_ok() : op_err("item not found");
}

std::string data_op_jewel(int role, int bag, int slot, int equip_slot) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_put_jewel == nullptr || fn_is_jewel == nullptr || fn_remove_item_direct == nullptr)
        return op_err("symbol not resolved");
    if (bag < 0 || bag >= 6 || slot < 0 || slot >= 16) return op_err("bad slot");
    if (equip_slot < 0 || equip_slot >= C_EQUIP_SLOTS) return op_err("bad equip slot");
    void* jewel = inventory_item_at(bag, slot);
    if (jewel == nullptr) return op_err("jewel not found");
    void* equip = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ch) + C_EQUIP + equip_slot * 8);
    if (equip == nullptr) return op_err("equip slot empty");
    int r = fn_put_jewel(equip, jewel);
    if (r != 0) return r == 2 ? op_err("no socket") : op_err("not jewel");
    // PutJewel 不消耗宝石物品本身，镶嵌成功后手动删除背包中的宝石（防刷宝石）
    fn_remove_item_direct(bag, slot);
    virtual_bag_sync_projected_slot(bag, slot);
    return op_ok();
}
std::string data_op_enchant(int role, int bag, int slot, int equip_slot) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_enchant_item == nullptr || fn_is_enchant_scroll == nullptr || fn_consume_item == nullptr || fn_get_bit == nullptr)
        return op_err("symbol not resolved");
    if (bag < 0 || bag >= 6 || slot < 0 || slot >= 16) return op_err("bad slot");
    if (equip_slot < 0 || equip_slot >= C_EQUIP_SLOTS) return op_err("bad equip slot");
    void* equip = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ch) + C_EQUIP + equip_slot * 8);
    if (equip == nullptr) return op_err("equip slot empty");
    void* scroll = inventory_item_at(bag, slot);
    if (scroll == nullptr) return op_err("scroll not found");
    uint16_t sflags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(scroll) + I_TYPE);
    int scroll_cat = fn_get_bit(sflags, 15, 6);
    if (!fn_is_enchant_scroll(scroll_cat)) return op_err("not enchant scroll");
    // ITEMSYSTEM_EnchantItem 成功(0)后写回 +0x1A 新附魔等级；卷轴消耗由调用方负责（UIEquip_ApplyStuff 同款）
    int r = fn_enchant_item(equip, scroll_cat);
    if (r != 0) return r == 7 ? op_err("not enchantable") : op_err("enchant failed");
    fn_consume_item(scroll);
    virtual_bag_sync_projected_slot(bag, slot);
    return op_ok();
}
std::string data_op_equip(int role, int bag, int slot) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (g_inven == nullptr || fn_equip_item == nullptr || fn_can_equip == nullptr)
        return op_err("symbol not resolved");
    if (bag < 0 || bag >= 6 || slot < 0 || slot >= 16) return op_err("bad slot");
    uint8_t* bag_slots = reinterpret_cast<uint8_t*>(g_inven) + bag * 0x80;
    void* item = *reinterpret_cast<void**>(bag_slots + slot * 8);
    if (item == nullptr) return op_err("slot empty");
    if (!fn_can_equip(ch, item)) return op_err("cannot equip");
    // CHAR_EquipItem 在目标槽已被占用时返回 0；先查槽位，占用则自动脱下旧装备再穿。
    if (fn_find_equip_slot != nullptr && fn_get_equip_item != nullptr && fn_unequip != nullptr) {
        int target = fn_find_equip_slot(ch, item);
        if (target >= 0 && target < C_EQUIP_SLOTS) {
            void* occupied = fn_get_equip_item(ch, target);
            if (occupied != nullptr) {
                fn_unequip(ch, target);
            }
        }
    }
    int r = fn_equip_item(ch, item);
    if (r) virtual_bag_sync_projected_bag();
    return r ? op_ok() : op_err("equip failed");
}
std::string data_op_unequip(int role, int32_t equip_slot) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_unequip == nullptr) return op_err("symbol not resolved");
    if (equip_slot < 0 || equip_slot >= C_EQUIP_SLOTS) return op_err("bad slot");
    int r = fn_unequip(ch, equip_slot);
    if (r) virtual_bag_sync_projected_bag();
    return r ? op_ok() : op_err("unequip failed");
}
std::string data_op_use_item(int bag, int slot) {
    if (!game_in_world()) return op_err("not in game");
    if (const char* ui = ui_blocked()) {
        std::string err = "ui occupied: ";
        err += ui;
        return op_err(err.c_str());
    }
    if (fn_get_bit == nullptr) return op_err("symbol not resolved");
    void* item = inventory_item_at(bag, slot);
    if (item == nullptr) return op_err("slot empty");
    uint16_t flags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
    int category = fn_get_bit(flags, 15, 6);
    // 解封/开箱/骰子类物品 fn_is_use 返回 0（IsUseAfterConfirm 判定集不含这些类别），
    // 但它们有自己的独立使用路径，不受 fn_is_use 限制
    bool sealed_or_box = (fn_is_sealed != nullptr && fn_is_sealed(category)) ||
                         (fn_is_item_box != nullptr && fn_is_item_box(category)) ||
                         (fn_is_dice != nullptr && fn_is_dice(category));
    if (!sealed_or_box && fn_is_use != nullptr && !fn_is_use(category))
        return op_err("item not usable");

    void* leader = member_or_null(0);

    // 按 UIEquip_SetDescMenu 按钮判定链分派（权威：反汇编 UI 按钮显隐逻辑）
    // 骰子（0x34-0x38）— 两段式：掷骰只生成 pending 返回变化量（不应用），接受/拒绝由 dice-accept/dice-reject 端点处理
    if (fn_is_dice != nullptr && fn_is_dice(category)) {
        if (fn_status_dice_roll == nullptr || fn_get_stat_base == nullptr)
            return op_err("symbol not resolved");
        if (leader == nullptr) return op_err("no leader");
        // 有未确认结果时拒绝再掷（flag bit0，ButtonRollExe 置位/Create+Apply 复位）
        uint8_t* flag = *reinterpret_cast<uint8_t**>(g_base + G_STATUSDICE_FLAG_GOT_VMA);
        if (flag != nullptr && (*flag & 1u)) return op_err("dice result pending, accept or reject first");
        int8_t char_idx = *reinterpret_cast<int8_t*>(reinterpret_cast<uint8_t*>(leader) + 0xd);
        int type = category - 0x34;
        if (char_idx < 0 || char_idx > 5) return op_err("bad char");
        if (type < 0 || type > 4) return op_err("bad dice type");
        int base[5];
        for (int i = 0; i < 5; ++i) base[i] = fn_get_stat_base(leader, i);
        if (!fn_status_dice_roll(char_idx, type)) return op_err("dice roll failed");
        int8_t* pending = *reinterpret_cast<int8_t**>(g_base + G_STATUSDICE_PENDING_GOT_VMA);
        if (pending == nullptr) return op_err("dice result missing");
        // 无 pending 时掷骰即消耗（原版 ButtonRollExe 语义），置 flag 待确认
        if (fn_consume_item != nullptr) fn_consume_item(item);
        if (flag != nullptr) *flag |= 1u;
        virtual_bag_sync_projected_slot(bag, slot);
        std::string s = "{\"ok\":true,\"base\":[";
        for (int i = 0; i < 5; ++i) {
            if (i > 0) s += ",";
            s += std::to_string(base[i]);
        }
        s += "],\"pending\":[";
        for (int i = 0; i < 5; ++i) {
            if (i > 0) s += ",";
            s += std::to_string(pending[i]);
        }
        s += "],\"delta\":[";
        for (int i = 0; i < 5; ++i) {
            if (i > 0) s += ",";
            s += std::to_string(pending[i] - base[i]);
        }
        s += "]}";
        return s;
    }
    // 解封（0x3a6-0x3ab）— ITEMSYSTEM_ReleaseSealed 独立路径，成功后手动消耗
    if (fn_is_sealed != nullptr && fn_is_sealed(category) && fn_release_sealed != nullptr) {
        void* before[96] = {nullptr};
        for_each_bag_slot([](void* item, int b, int j, void* c) -> bool {
            static_cast<void**>(c)[b * 16 + j] = item;
            return false;
        }, before);
        int ok = fn_release_sealed(category);
        if (ok) {
            if (fn_consume_item != nullptr) fn_consume_item(item);
            virtual_bag_sync_projected_slot(bag, slot);
            return "{\"ok\":true,\"gained\":[" + inventory_gained_json(before) + "]}";
        }
        return op_err("release sealed failed");
    }
    // 开箱（0x3ef-0x3f1）— ITEMSYSTEM_OpenItemBox 独立路径，成功后手动消耗
    if (fn_is_item_box != nullptr && fn_is_item_box(category) && fn_open_item_box != nullptr) {
        void* before[96] = {nullptr};
        for_each_bag_slot([](void* item, int b, int j, void* c) -> bool {
            static_cast<void**>(c)[b * 16 + j] = item;
            return false;
        }, before);
        int ok = fn_open_item_box(category);
        if (ok) {
            if (fn_consume_item != nullptr) fn_consume_item(item);
            virtual_bag_sync_projected_slot(bag, slot);
            return "{\"ok\":true,\"gained\":[" + inventory_gained_json(before) + "]}";
        }
        return op_err("open box failed");
    }

    // 其余类型：CHAR_UseItemEx — 药水/卷轴/技能书/配方书/佣兵卡/增益/超药水/打包物
    //   内部成功时已调 INVEN_ConsumeItem；失败（CD/状态不符）不消耗
    if (leader == nullptr) return op_err("no leader");
    if (fn_char_use_item_ex == nullptr) return op_err("symbol not resolved");
    int ok = fn_char_use_item_ex(leader, item, 0);
    // 用药成功且药水教学激活（obj170==6）→ 复现官方 0xec340 教学完成链（CHAR_ProcessShortcut 用药后检查）
    if (ok && tutorial_state() == 6) tutorial_cancel();
    if (ok) virtual_bag_sync_projected_slot(bag, slot);
    return ok ? op_ok() : op_err("on cooldown");
}
std::string data_op_dice_accept() {
    if (!game_in_world()) return op_err("not in game");
    if (fn_set_stat_base == nullptr || fn_get_stat_base == nullptr)
        return op_err("symbol not resolved");
    uint8_t* flag = *reinterpret_cast<uint8_t**>(g_base + G_STATUSDICE_FLAG_GOT_VMA);
    if (flag == nullptr || !(*flag & 1u)) return op_err("no dice result pending");
    void* leader = member_or_null(0);
    if (leader == nullptr) return op_err("no leader");
    int8_t* pending = *reinterpret_cast<int8_t**>(g_base + G_STATUSDICE_PENDING_GOT_VMA);
    if (pending == nullptr) return op_err("dice result missing");
    // 骰子已在掷骰时消耗，accept 只应用结果不重复消耗
    int base[5];
    for (int i = 0; i < 5; ++i) base[i] = fn_get_stat_base(leader, i);
    for (int i = 0; i < 5; ++i) fn_set_stat_base(leader, i, pending[i]);
    if (flag != nullptr) *flag &= ~1u;
    std::string s = "{\"ok\":true,\"base\":[";
    for (int i = 0; i < 5; ++i) {
        if (i > 0) s += ",";
        s += std::to_string(base[i]);
    }
    s += "],\"applied\":[";
    for (int i = 0; i < 5; ++i) {
        if (i > 0) s += ",";
        s += std::to_string(pending[i]);
    }
    s += "],\"delta\":[";
    for (int i = 0; i < 5; ++i) {
        if (i > 0) s += ",";
        s += std::to_string(pending[i] - base[i]);
    }
    s += "]}";
    return s;
}
std::string data_op_dice_reject() {
    if (!game_in_world()) return op_err("not in game");
    uint8_t* flag = *reinterpret_cast<uint8_t**>(g_base + G_STATUSDICE_FLAG_GOT_VMA);
    if (flag == nullptr || !(*flag & 1u)) return op_err("no dice result pending");
    *flag &= ~1u;
    return op_ok();
}
std::string data_op_discard_item(int bag, int slot) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_remove_item_direct == nullptr) return op_err("symbol not resolved");
    if (inventory_item_at(bag, slot) == nullptr) return op_err("slot empty");
    fn_remove_item_direct(bag, slot);
    if (inventory_item_at(bag, slot) != nullptr) return op_err("discard failed");
    virtual_bag_sync_projected_slot(bag, slot);
    return op_ok();
}
std::string data_op_sell_item(int bag, int slot) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_remove_item_direct == nullptr || fn_add_money == nullptr || fn_item_get_price == nullptr)
        return op_err("symbol not resolved");
    void* item = inventory_item_at(bag, slot);
    if (item == nullptr) return op_err("slot empty");
    // 合法出售（v0.4.20）：ITEM_GetPrice 返回原始价格，出售价 = 原价 / 5
    // （改版币制：5 铜 = 1 银，出售价 = 真实价格 ÷ 5）
    int64_t base_price = fn_item_get_price(item);
    int64_t price = base_price / 5;
    fn_remove_item_direct(bag, slot);
    if (inventory_item_at(bag, slot) != nullptr) return op_err("sell failed");
    virtual_bag_sync_projected_slot(bag, slot);
    fn_add_money(price);
    return "{\"ok\":true,\"price\":" + std::to_string(price) + "}";
}
std::string data_op_move_item(int bag, int slot, int count, int to_bag, int to_slot) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_inven_move_item == nullptr) return op_err("symbol not resolved");
    void* item = inventory_item_at(bag, slot);
    if (item == nullptr) return op_err("slot empty");
    if (count <= 0) return op_err("bad count");
    if (to_bag < 0 || to_bag >= 6 || to_slot < 0 || to_slot >= 16) return op_err("bad target");
    if (bag == to_bag && slot == to_slot) return op_err("same slot");
    int r = fn_inven_move_item(item, count, to_bag, to_slot);
    // 返回 1=成功（mov w1,#0x1），0/失败返回空——按目标槽是否有物品判定
    if (r) {
        virtual_bag_sync_projected_slot(bag, slot);
        virtual_bag_sync_projected_slot(to_bag, to_slot);
    }
    return r ? op_ok() : op_err("move failed");
}

void append_item_attrs(std::string& s, void* item) {
    uint8_t* it = reinterpret_cast<uint8_t*>(item);
    if (fn_get_damage != nullptr) {
        s += ",\"damage\":" + std::to_string(fn_get_damage(item));
    }
    if (fn_get_defense != nullptr) {
        s += ",\"defense\":" + std::to_string(fn_get_defense(item));
    }
    s += ",\"magic_rate\":" + std::to_string(it[I_MAGIC_RATE]);
    if (fn_item_get_ability_level != nullptr) {
        s += ",\"need_level\":" + std::to_string(fn_item_get_ability_level(item));
    }
    // v0.4.64 位域拆解（docs/systems/inventory.md §2.4 反汇编确认）
    uint8_t socket = it[I_SOCKET];
    s += ",\"socket\":" + std::to_string(socket);
    s += ",\"socket_filled\":" + std::to_string((socket >> 0) & 0x0F);
    s += ",\"socket_total\":" + std::to_string((socket >> 4) & 0x0F);
    uint16_t enchant = *reinterpret_cast<uint16_t*>(it + I_ENCHANT);
    s += ",\"enchant\":" + std::to_string(enchant);
    s += ",\"chaos\":" + std::string((enchant & 1) ? "true" : "false");
    s += ",\"enchant_id\":" + std::to_string((enchant >> 11) & 0x1F);
    s += ",\"enchant_level\":" + std::to_string((enchant >> 6) & 0x1F);
    uint32_t cnt = *reinterpret_cast<uint32_t*>(it + I_COUNT);
    s += ",\"chaos_level\":" + std::to_string((cnt >> 0) & 0xFF);
    s += ",\"chaos_rate\":" + std::to_string((cnt >> 8) & 0xFF);
    // options = 词缀值数组（兼容旧字段）；optionIds = 词缀索引数组（节点 +0x00 低 7 位，与 options 对齐）
    uint8_t* opt = *reinterpret_cast<uint8_t**>(it + I_OPTION_LIST);
    bool ofirst = true;
    int ocount = 0;
    s += ",\"options\":[";
    while (opt != nullptr && ocount < 32) {
        if (!ofirst) s += ",";
        s += std::to_string(*reinterpret_cast<int16_t*>(opt + O_VALUE));
        ofirst = false;
        opt = *reinterpret_cast<uint8_t**>(opt + O_NEXT);
        ++ocount;
    }
    s += "]";
    uint8_t* opt2 = *reinterpret_cast<uint8_t**>(it + I_OPTION_LIST);
    ofirst = true;
    ocount = 0;
    s += ",\"option_ids\":[";
    while (opt2 != nullptr && ocount < 32) {
        if (!ofirst) s += ",";
        s += std::to_string(*reinterpret_cast<uint16_t*>(opt2 + O_INDEX) & 0x7F);
        ofirst = false;
        opt2 = *reinterpret_cast<uint8_t**>(opt2 + O_NEXT);
        ++ocount;
    }
    s += "]";
    // option_types：词缀节点 type（O_INDEX bit13-15：0=词缀 1=宝石），与 option_ids 对齐，Kotlin 据此拆分 bonus/gem
    uint8_t* opt3 = *reinterpret_cast<uint8_t**>(it + I_OPTION_LIST);
    ofirst = true;
    ocount = 0;
    s += ",\"option_types\":[";
    while (opt3 != nullptr && ocount < 32) {
        if (!ofirst) s += ",";
        s += std::to_string((*reinterpret_cast<uint16_t*>(opt3 + O_INDEX) >> 13) & 0x7);
        ofirst = false;
        opt3 = *reinterpret_cast<uint8_t**>(opt3 + O_NEXT);
        ++ocount;
    }
    s += "]";
}

// v0.5.12 ⑤ 装备判定：ITEMCLASSBASE 记录 +6 bit0=1 可堆叠 / 0 不可堆叠（装备）。与 ITEM_GetCumulateCount 同源。
bool item_is_equip(void* item) {
    if (g_base == 0 || item == nullptr) return false;
    uint8_t* it = reinterpret_cast<uint8_t*>(item);
    uint16_t flags = *reinterpret_cast<uint16_t*>(it + I_TYPE);
    int category = (flags >> 6) & 0x3FF;
    uint8_t* class_data = *reinterpret_cast<uint8_t**>(*reinterpret_cast<void**>(g_base + G_ITEMCLASS_DATA_GOT_VMA));
    uint8_t* size_ptr = *reinterpret_cast<uint8_t**>(g_base + G_ITEMCLASS_SIZE_GOT_VMA);
    if (class_data == nullptr || size_ptr == nullptr) return false;
    uint8_t stride = *size_ptr;
    if (stride == 0) return false;
    return (class_data[category * stride + 6] & 1) == 0;
}

std::string build_inventory_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    // INVEN_pItem(G_INVEN_VMA)：背包槽数组，6 袋 × 0x80 步长，每槽 8B 物品指针。
    // 每袋 16 槽（6×16=96，与真机实测 slotCount 总和一致）。
    // 空槽=0 指针；物品 +0x08 类型位域(u16)、+0x10 数量位域(u32)。
    constexpr int BAG_COUNT = 6;
    constexpr size_t BAG_STRIDE = 0x80;
    constexpr int SLOTS_PER_BAG = 16;
    std::string s = "{\"bags\":[";
    for (int b = 0; b < BAG_COUNT; ++b) {
        if (b > 0) s += ",";
        s += "{\"bag\":" + std::to_string(b) + ",\"items\":[";
        int filled = 0;
        if (g_inven != nullptr) {
            uint8_t* bag_slots = reinterpret_cast<uint8_t*>(g_inven) + b * BAG_STRIDE;
            for (int j = 0; j < SLOTS_PER_BAG; ++j) {
                void* item = *reinterpret_cast<void**>(bag_slots + j * 8);
                if (item == nullptr) continue;
                uint16_t flags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
                if (filled > 0) s += ",";
                s += "{\"slot\":" + std::to_string(j);
                s += ",\"type_flags\":" + std::to_string(flags);
                s += ",\"raw_rarity\":" + std::to_string((flags >> 2) & 0x0F);
                if (fn_get_bit != nullptr) {
                    s += ",\"category\":" + std::to_string(fn_get_bit(flags, 15, 6));
                }
                if (fn_get_cumulate_count != nullptr) {
                    // count 语义：ITEM_GetCumulateCount 按类型区分——可堆叠类返回 bit22-31，
                    // 不可堆叠类（装备）返回 1（旧实现裸读位域导致装备 count=100）
                    s += ",\"count\":" + std::to_string(fn_get_cumulate_count(item));
                }
                s += std::string(",\"equip\":") + (item_is_equip(item) ? "true" : "false");
                if (fn_get_rarity != nullptr) {
                    s += ",\"rarity\":" + std::to_string(fn_get_rarity(item));
                }
                append_item_attrs(s, item);
                s += "}";
                ++filled;
            }
        }
        s += "],\"capacity\":16,\"slot_count\":" + std::to_string(filled) + "}";
    }
    // 扩展逻辑袋 6..10（extensionBagEnabled 且 world 时并入；schema 与原版袋一致）
    s += virtual_bag_inventory_bags_json();
    s += "]}";
    return s;
}

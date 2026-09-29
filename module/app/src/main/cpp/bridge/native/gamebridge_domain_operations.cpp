#include "gamebridge_internal.h"

#include "bridge/native/character_write_result.h"
#include "bridge/native/item_write_result.h"
#include "contracts/commands.h"
#include "core/native/inventory_trade.h"
#include "game/adapter_ports/inventory_extension_facade.h"
#include "game/adapter_ports/item_count_facade.h"
#include "game/access/world.h"
#include "game/ops/character_write.h"
#include "game/ops/item_dice.h"
#include "game/ops/item_inventory_state_write.h"
#include "infra/commands.h"
#include "infra/config.h"

namespace {

// ITM-13：bridge 写入口向 game::ops 传中立 provider；不可用时传 nullptr（game::ops 保留原版路径）。
const qol::contracts::QolInventoryExtensionProvider* neutral_extension_provider() {
    return qol::game::inventory_extension_available()
               ? qol::game::inventory_extension_provider_if_resident()
               : nullptr;
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpJewel(JNIEnv* env, jclass, jint role, jint bag, jint slot, jint equipSlot) {
#if QOL_KERNEL_ONLY
    (void)role;
    (void)bag;
    (void)slot;
    (void)equipSlot;
    return kernel_only_result(env);
#else
    qol::contracts::QolItemInventoryStateWriteResult op{};
    const QolResult result =
        qol::game::ops::put_jewel(static_cast<int>(role), static_cast<int>(bag),
                                  static_cast<int>(slot), static_cast<int>(equipSlot),
                                  neutral_extension_provider(), &op);
    return op_result(env, "op_jewel", ("role=" + str_of(role) + " " + "bag=" + str_of(bag) + " " + "slot=" + str_of(slot) + " " + "equipSlot=" + str_of(equipSlot)),
                     qol::bridge::item_write_result_json(result, "item write failed"));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpEnchant(JNIEnv* env, jclass, jint role, jint bag, jint slot, jint equipSlot) {
#if QOL_KERNEL_ONLY
    (void)role;
    (void)bag;
    (void)slot;
    (void)equipSlot;
    return kernel_only_result(env);
#else
    qol::contracts::QolItemInventoryStateWriteResult op{};
    const QolResult result =
        qol::game::ops::enchant(static_cast<int>(role), static_cast<int>(bag),
                                static_cast<int>(slot), static_cast<int>(equipSlot),
                                neutral_extension_provider(), &op);
    return op_result(env, "op_enchant", ("role=" + str_of(role) + " " + "bag=" + str_of(bag) + " " + "slot=" + str_of(slot) + " " + "equipSlot=" + str_of(equipSlot)),
                     qol::bridge::item_write_result_json(result, "item write failed"));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetAutoAttack(JNIEnv* env, jclass, jint role, jint onoff) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result =
        qol::game::ops::set_auto_attack(static_cast<int>(role), static_cast<int32_t>(onoff), &op);
    return op_result(env, "op_set_auto_attack", ("role=" + str_of(role) + " " + "onoff=" + str_of(onoff)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetSkillUsage(JNIEnv* env, jclass, jint role, jint onoff) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::set_skill_usage(static_cast<int>(role),
                                                             static_cast<int32_t>(onoff), &op);
    return op_result(env, "op_set_skill_usage", ("role=" + str_of(role) + " " + "onoff=" + str_of(onoff)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpEquip(JNIEnv* env, jclass, jint role, jint bag, jint slot) {
#if QOL_KERNEL_ONLY
    (void)role;
    (void)bag;
    (void)slot;
    return kernel_only_result(env);
#else
    qol::contracts::QolItemInventoryStateWriteResult op{};
    const QolResult result = qol::game::ops::equip_item(
        static_cast<int>(role), static_cast<int>(bag), static_cast<int>(slot),
        neutral_extension_provider(), &op);
    return op_result(env, "op_equip", ("role=" + str_of(role) + " " + "bag=" + str_of(bag) + " " + "slot=" + str_of(slot)),
                     qol::bridge::item_write_result_json(result, "item write failed"));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpUnequip(JNIEnv* env, jclass, jint role, jint slot) {
#if QOL_KERNEL_ONLY
    (void)role;
    (void)slot;
    return kernel_only_result(env);
#else
    qol::contracts::QolItemInventoryStateWriteResult op{};
    const QolResult result = qol::game::ops::unequip_item(
        static_cast<int>(role), static_cast<int32_t>(slot), neutral_extension_provider(), &op);
    return op_result(env, "op_unequip", ("role=" + str_of(role) + " " + "slot=" + str_of(slot)),
                     qol::bridge::item_write_result_json(result, "item write failed"));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSwitchPlayer(JNIEnv* env, jclass, jint slot) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::switch_player(static_cast<int32_t>(slot), &op);
    return op_result(env, "op_switch_player", ("slot=" + str_of(slot)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpPartySwap(JNIEnv* env, jclass, jint a, jint b) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::party_swap(static_cast<int32_t>(a),
                                                        static_cast<int32_t>(b), &op);
    return op_result(env, "op_party_swap", ("a=" + str_of(a) + " " + "b=" + str_of(b)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpTeleport(JNIEnv* env, jclass, jint mapId, jint x, jint y) {
    // 旧三参导出保留（JNI 面冻结），固定 force=false；新调用方走 nativeOpTeleportEx
    return op_result(env, "op_teleport", ("mapId=" + str_of(mapId) + " " + "x=" + str_of(x) + " " + "y=" + str_of(y)), qol::game::world_teleport(static_cast<int32_t>(mapId), static_cast<int32_t>(x), static_cast<int32_t>(y), false).str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpTeleportEx(JNIEnv* env, jclass, jint mapId, jint x, jint y, jboolean force) {
    return op_result(env, "op_teleport_ex", ("mapId=" + str_of(mapId) + " " + "x=" + str_of(x) + " " + "y=" + str_of(y) + " force=" + str_of(static_cast<int>(force))), qol::game::world_teleport(static_cast<int32_t>(mapId), static_cast<int32_t>(x), static_cast<int32_t>(y), force == JNI_TRUE).str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpRemoveItem(JNIEnv* env, jclass, jint category) {
#if QOL_KERNEL_ONLY
    (void)category;
    return kernel_only_result(env);
#else
    qol::contracts::QolItemInventoryStateWriteResult out{};
    const QolResult result =
        qol::game::ops::remove_item(static_cast<int32_t>(category), &out);
    return op_result(env, "op_remove_item", ("category=" + str_of(category)),
                     qol::bridge::item_write_result_json(result, "item write failed"));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpLearnAction(JNIEnv* env, jclass, jint role, jint actionId, jint level) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::learn_action(static_cast<int>(role),
                                                          static_cast<int32_t>(actionId),
                                                          static_cast<int32_t>(level), &op);
    return op_result(env, "op_learn_action", ("role=" + str_of(role) + " " + "actionId=" + str_of(actionId) + " " + "level=" + str_of(level)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetEventsJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_events_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpMove(JNIEnv* env, jclass, jint x, jint y) {
    return op_result(env, "op_move", ("x=" + str_of(x) + " " + "y=" + str_of(y)), qol::game::world_move(static_cast<int32_t>(x), static_cast<int32_t>(y)).str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpWalk(JNIEnv* env, jclass, jint direction) {
    return op_result(env, "op_walk", ("direction=" + str_of(direction)), qol::game::world_walk(static_cast<int32_t>(direction)).str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpWalkStop(JNIEnv* env, jclass) {
    return op_result(env, "op_walk_stop", (std::string("")), qol::game::world_walk_stop().str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpInteract(JNIEnv* env, jclass) {
    return op_result(env, "op_interact", (std::string("")), qol::game::world_interact().str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpAttack(JNIEnv* env, jclass, jint role, jint target_slot) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::attack_target(static_cast<int32_t>(role),
                                                           static_cast<int32_t>(target_slot), &op);
    return op_result(env, "op_attack", ("role=" + str_of(role) + " " + "target_slot=" + str_of(target_slot)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpStopCombat(JNIEnv* env, jclass, jint role) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::stop_combat(static_cast<int32_t>(role), &op);
    return op_result(env, "op_stop_combat", ("role=" + str_of(role)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpUseItem(JNIEnv* env, jclass, jint bag, jint slot) {
#if QOL_KERNEL_ONLY
    (void)bag;
    (void)slot;
    return kernel_only_result(env);
#else
    const std::string args = "bag=" + str_of(bag) + " " + "slot=" + str_of(slot);
    if (!game_in_world()) {
        return op_result(env, "op_use_item", args, qol::bridge::item_error_json("not in game"));
    }
    if (bag == 5) {
        return op_result(env, "op_use_item", args, qol::bridge::item_error_json("task bag excluded"));
    }
    InventoryItemRef item_ref;
    if (!inventory_item_ref_at(bag, slot, &item_ref)) {
        return op_result(env, "op_use_item", args, qol::bridge::item_error_json("slot empty"));
    }
    // 扩展逻辑袋：经中立 provider 透传插件字符串（不套信封）。
    if (item_ref.kind == InventoryItemKind::kProvider) {
        const qol::contracts::QolInventoryExtensionProvider* provider =
            neutral_extension_provider();
        char buffer[4096]{};
        uint32_t length = 0;
        if (provider == nullptr || provider->use_item == nullptr ||
            provider->use_item(bag, slot, buffer, sizeof(buffer), &length) == 0 ||
            length > sizeof(buffer)) {
            return op_result(env, "op_use_item", args,
                             qol::bridge::item_error_json("extension item use failed"));
        }
        return op_result(env, "op_use_item", args, std::string(buffer, length));
    }
    qol::contracts::QolItemInventoryStateWriteResult out{};
    const QolResult result = qol::game::ops::use_item(bag, slot, &out);
    return op_result(env, "op_use_item", args, qol::bridge::item_use_result_json(result, out));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpDiceAccept(JNIEnv* env, jclass) {
#if QOL_KERNEL_ONLY
    return kernel_only_result(env);
#else
    qol::contracts::QolItemDiceAcceptResult out{};
    const QolResult result = qol::game::ops::accept_dice(&out);
    return op_result(env, "op_dice_accept", (std::string("")),
                     qol::bridge::item_dice_accept_result_json(result, out));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpDiceReject(JNIEnv* env, jclass) {
#if QOL_KERNEL_ONLY
    return kernel_only_result(env);
#else
    const QolResult result = qol::game::ops::reject_dice();
    return op_result(env, "op_dice_reject", (std::string("")),
                     qol::bridge::item_write_result_json(result, "item write failed"));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpDiscardItem(JNIEnv* env, jclass, jint bag, jint slot) {
#if QOL_KERNEL_ONLY
    (void)bag;
    (void)slot;
    return kernel_only_result(env);
#else
    const std::string args = "bag=" + str_of(bag) + " " + "slot=" + str_of(slot);
    InventoryItemRef expected_ref;
    const bool found = inventory_item_ref_at(bag, slot, &expected_ref);
    const inventory_trade::Status status =
        inventory_trade::discard(bag, slot, found ? expected_ref.native_item : nullptr);
    // 现状 api/native 成功走 op_ok() → 缓存刷新恰一次（sell 不刷新，保持差异）。
    if (status == inventory_trade::Status::kOk) frame_cache_force_refresh();
    return op_result(env, "op_discard_item", args, qol::bridge::item_discard_result_json(status));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSellItem(JNIEnv* env, jclass, jint bag, jint slot) {
#if QOL_KERNEL_ONLY
    (void)bag;
    (void)slot;
    return kernel_only_result(env);
#else
    const std::string args = "bag=" + str_of(bag) + " " + "slot=" + str_of(slot);
    InventoryItemRef expected_ref;
    const bool found = inventory_item_ref_at(bag, slot, &expected_ref);
    const inventory_trade::Result result =
        inventory_trade::sell(bag, slot, found ? expected_ref.native_item : nullptr, "api");
    return op_result(env, "op_sell_item", args, qol::bridge::item_sell_result_json(result));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpMoveItem(JNIEnv* env, jclass, jint bag, jint slot, jint count, jint toBag, jint toSlot) {
#if QOL_KERNEL_ONLY
    (void)bag;
    (void)slot;
    (void)count;
    (void)toBag;
    (void)toSlot;
    return kernel_only_result(env);
#else
    const std::string args = "bag=" + str_of(bag) + " " + "slot=" + str_of(slot) + " " + "count=" + str_of(count) + " " + "toBag=" + str_of(toBag) + " " + "toSlot=" + str_of(toSlot);
    if (!game_in_world()) {
        return op_result(env, "op_move_item", args, qol::bridge::item_error_json("not in game"));
    }
    if (bag == 5 || toBag == 5) {
        return op_result(env, "op_move_item", args,
                         qol::bridge::item_error_json("task bag excluded"));
    }
    const qol::contracts::QolInventoryExtensionProvider* provider =
        neutral_extension_provider();
    if (provider != nullptr && provider->owns_bag != nullptr &&
        (provider->owns_bag(bag) != 0 || provider->owns_bag(toBag) != 0)) {
        if (count <= 0) {
            return op_result(env, "op_move_item", args, qol::bridge::item_error_json("bad count"));
        }
        if (toSlot < 0 || toSlot >= 16) {
            return op_result(env, "op_move_item", args, qol::bridge::item_error_json("bad target"));
        }
        // 扩展逻辑袋：走中立 provider `move_item`（唯一实现点，字节同源透传，不刷）；
        // 缓冲不足视为不可达边界，fallback 与 owner 兜底同款（见 T2j 方案 §3）。
        char move_buf[4096]{};
        uint32_t move_len = 0;
        if (provider->move_item != nullptr &&
            provider->move_item(bag, slot, toBag, toSlot, move_buf, sizeof(move_buf),
                                &move_len) != 0 &&
            move_len <= sizeof(move_buf)) {
            return op_result(env, "op_move_item", args, std::string(move_buf, move_len));
        }
        return op_result(env, "op_move_item", args,
                         qol::bridge::item_error_json("extension bag move failed"));
    }
    if (provider != nullptr && provider->module_view_installed != nullptr &&
        provider->module_view_installed() != 0) {
        return op_result(env, "op_move_item", args,
                         qol::bridge::item_error_json("extension view open; movement disabled (P2)"));
    }
    // 原版袋：与 owner 回落同一 typed 实现；成功补刷恰一次复刻旧 op_ok()（EB 分支不刷）。
    qol::contracts::QolItemInventoryStateWriteResult out{};
    const QolResult result = qol::game::ops::move_item(
        static_cast<int32_t>(bag), static_cast<int32_t>(slot), static_cast<int32_t>(count),
        static_cast<int32_t>(toBag), static_cast<int32_t>(toSlot), &out);
    if (qol::contracts::succeeded(result)) frame_cache_force_refresh();
    return op_result(env, "op_move_item", args,
                     qol::bridge::item_write_result_json(result, "item write failed"));
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpIncludeParty(JNIEnv* env, jclass, jint mercSlot) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::party_include(static_cast<int>(mercSlot), &op);
    return op_result(env, "op_include_party", ("mercSlot=" + str_of(mercSlot)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpExcludeParty(JNIEnv* env, jclass, jint mercSlot) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::party_exclude(static_cast<int>(mercSlot), &op);
    return op_result(env, "op_exclude_party", ("mercSlot=" + str_of(mercSlot)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpDischarge(JNIEnv* env, jclass, jint mercSlot) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::party_discharge(static_cast<int>(mercSlot), &op);
    return op_result(env, "op_discharge", ("mercSlot=" + str_of(mercSlot)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpWithdraw(JNIEnv* env, jclass, jint mercSlot, jint equipSlot) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::party_withdraw(
        static_cast<int>(mercSlot), static_cast<int32_t>(equipSlot), &op);
    return op_result(env, "op_withdraw", ("mercSlot=" + str_of(mercSlot) + " " + "equipSlot=" + str_of(equipSlot)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetHp(JNIEnv* env, jclass, jint role, jint hp) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result =
        qol::game::ops::set_hp(static_cast<int>(role), static_cast<int32_t>(hp), &op);
    return op_result(env, "op_set_hp", ("role=" + str_of(role) + " " + "hp=" + str_of(hp)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetMp(JNIEnv* env, jclass, jint role, jint mp) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result =
        qol::game::ops::set_mp(static_cast<int>(role), static_cast<int32_t>(mp), &op);
    return op_result(env, "op_set_mp", ("role=" + str_of(role) + " " + "mp=" + str_of(mp)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetAttr(JNIEnv* env, jclass, jint role, jint attrIndex, jint value) {
    qol::contracts::QolCharacterWriteOpResult op{};
    const QolResult result = qol::game::ops::set_attr(static_cast<int>(role),
                                                      static_cast<int32_t>(attrIndex),
                                                      static_cast<int32_t>(value), &op);
    return op_result(env, "op_set_attr", ("role=" + str_of(role) + " " + "attrIndex=" + str_of(attrIndex) + " " + "value=" + str_of(value)),
                     qol::bridge::character_write_result_json(result, op));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpAddItem(JNIEnv* env, jclass, jint category, jint count, jint socket,
                                                   jint enhance_remaining, jint rarity) {
#if QOL_KERNEL_ONLY
    (void)category;
    (void)count;
    (void)socket;
    (void)enhance_remaining;
    (void)rarity;
    return kernel_only_result(env);
#else
    qol::contracts::QolItemInventoryStateWriteResult out{};
    const QolResult result = qol::game::ops::add_item(
        static_cast<int32_t>(category), static_cast<int32_t>(count), static_cast<int32_t>(socket),
        static_cast<int32_t>(enhance_remaining), static_cast<int32_t>(rarity),
        qol::game::item_count_api_if_resident(), &out);
    return op_result(env, "op_add_item", ("category=" + str_of(category) + " " + "count=" + str_of(count) + " " + "socket=" + str_of(socket) + " " + "enhanceRemaining=" + str_of(enhance_remaining) + " " + "rarity=" + str_of(rarity)),
                     qol::bridge::item_write_result_json(result, "item write failed"));
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetStackLimitEnabled(JNIEnv*, jclass, jboolean enabled) {
    // 只做参数转换：开关下发为一条命令（kCommandSetConfigBool），由主线程在下一逻辑帧
    // 写配置，stack_limit 插件的门禁随 sync_gates 安装/还原 clamp 并更新有效态。
    // 返回值表示「本次下发是否成功入队」，不是开关状态（与 nativeSetSimpleModeEnabled 同构）。
    const qol::contracts::QolCommand command{
        qol::contracts::kCommandSetConfigBool,
        "plugin.stack_limit.enabled",
        enabled == JNI_TRUE ? 1 : 0,
    };
    qol::contracts::QolCommandTicket ticket = 0;
    const QolResult result = qol::infra::commands_port().post(&ticket, &command);
    return qol::contracts::succeeded(result) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetMoveMergeEnabled(JNIEnv*, jclass, jboolean enabled) {
    // 只做参数转换：开关下发为一条命令（kCommandSetConfigBool），由主线程在下一逻辑帧
    // 写配置键 moveMergeEnabled；extension_bag 插件订阅 kEventConfigChanged 应用
    // set_move_merge_enabled（extension source protection 联动逐字保持）。
    // 返回值表示「本次下发是否成功入队」，不是开关状态（与 nativeSetExtensionBagEnabled 同构）。
    const qol::contracts::QolCommand command{
        qol::contracts::kCommandSetConfigBool,
        "moveMergeEnabled",
        enabled == JNI_TRUE ? 1 : 0,
    };
    qol::contracts::QolCommandTicket ticket = 0;
    const QolResult result = qol::infra::commands_port().post(&ticket, &command);
    return qol::contracts::succeeded(result) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetExtensionBagEnabled(JNIEnv*, jclass, jboolean enabled) {
    // 只做参数转换：开关下发为一条命令（kCommandSetConfigBool），由主线程在下一逻辑帧
    // 写配置键 extensionBagEnabled；extension_bag 插件门禁随宿主 sync_gates 安装/还原
    // 资源并驱动 on_enabled/on_disabled。
    // 返回值表示「本次下发是否成功入队」，不是开关状态（与 nativeSetSimpleModeEnabled 同构）。
    const qol::contracts::QolCommand command{
        qol::contracts::kCommandSetConfigBool,
        "extensionBagEnabled",
        enabled == JNI_TRUE ? 1 : 0,
    };
    qol::contracts::QolCommandTicket ticket = 0;
    const QolResult result = qol::infra::commands_port().post(&ticket, &command);
    return qol::contracts::succeeded(result) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetCustomRecipeEnabled(JNIEnv*, jclass, jboolean enabled) {
    // 迁移为配置门禁单一写者（名称/签名冻结）：写 customRecipeEnabled + 广播；宿主帧步进
    // sync_gates 据此安装/还原 craft_ui 的 UIMix hook 与 custom_recipe 的表注入所有权。
    return qol::infra::config_set_bool("customRecipeEnabled", enabled == JNI_TRUE) ? JNI_TRUE
                                                                                   : JNI_FALSE;
}

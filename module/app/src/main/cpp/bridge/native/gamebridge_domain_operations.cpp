#include "gamebridge_internal.h"

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpJewel(JNIEnv* env, jclass, jint role, jint bag, jint slot, jint equipSlot) {
    return op_result(env, "op_jewel", ("role=" + str_of(role) + " " + "bag=" + str_of(bag) + " " + "slot=" + str_of(slot) + " " + "equipSlot=" + str_of(equipSlot)), data_op_jewel(static_cast<int>(role), static_cast<int>(bag),
        static_cast<int>(slot), static_cast<int>(equipSlot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpEnchant(JNIEnv* env, jclass, jint role, jint bag, jint slot, jint equipSlot) {
    return op_result(env, "op_enchant", ("role=" + str_of(role) + " " + "bag=" + str_of(bag) + " " + "slot=" + str_of(slot) + " " + "equipSlot=" + str_of(equipSlot)), data_op_enchant(static_cast<int>(role), static_cast<int>(bag),
        static_cast<int>(slot), static_cast<int>(equipSlot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetAutoAttack(JNIEnv* env, jclass, jint role, jint onoff) {
    return op_result(env, "op_set_auto_attack", ("role=" + str_of(role) + " " + "onoff=" + str_of(onoff)), data_op_set_auto_attack(static_cast<int>(role), static_cast<int32_t>(onoff)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetSkillUsage(JNIEnv* env, jclass, jint role, jint onoff) {
    return op_result(env, "op_set_skill_usage", ("role=" + str_of(role) + " " + "onoff=" + str_of(onoff)), data_op_set_skill_usage(static_cast<int>(role), static_cast<int32_t>(onoff)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpEquip(JNIEnv* env, jclass, jint role, jint bag, jint slot) {
    return op_result(env, "op_equip", ("role=" + str_of(role) + " " + "bag=" + str_of(bag) + " " + "slot=" + str_of(slot)), data_op_equip(static_cast<int>(role), static_cast<int>(bag), static_cast<int>(slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpUnequip(JNIEnv* env, jclass, jint role, jint slot) {
    return op_result(env, "op_unequip", ("role=" + str_of(role) + " " + "slot=" + str_of(slot)), data_op_unequip(static_cast<int>(role), static_cast<int32_t>(slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSwitchPlayer(JNIEnv* env, jclass, jint slot) {
    return op_result(env, "op_switch_player", ("slot=" + str_of(slot)), data_op_switch_player(static_cast<int32_t>(slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpPartySwap(JNIEnv* env, jclass, jint a, jint b) {
    return op_result(env, "op_party_swap", ("a=" + str_of(a) + " " + "b=" + str_of(b)), data_op_party_swap(static_cast<int32_t>(a), static_cast<int32_t>(b)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpTeleport(JNIEnv* env, jclass, jint mapId, jint x, jint y) {
    return op_result(env, "op_teleport", ("mapId=" + str_of(mapId) + " " + "x=" + str_of(x) + " " + "y=" + str_of(y)), data_op_teleport(static_cast<int32_t>(mapId), static_cast<int32_t>(x), static_cast<int32_t>(y)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpRemoveItem(JNIEnv* env, jclass, jint category) {
    return op_result(env, "op_remove_item", ("category=" + str_of(category)), data_op_remove_item(static_cast<int32_t>(category)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpLearnAction(JNIEnv* env, jclass, jint role, jint actionId, jint level) {
    return op_result(env, "op_learn_action", ("role=" + str_of(role) + " " + "actionId=" + str_of(actionId) + " " + "level=" + str_of(level)), data_op_learn_action(static_cast<int>(role), static_cast<int32_t>(actionId), static_cast<int32_t>(level)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeGetEventsJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(data_events_json().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpMove(JNIEnv* env, jclass, jint x, jint y) {
    return op_result(env, "op_move", ("x=" + str_of(x) + " " + "y=" + str_of(y)), data_op_move(static_cast<int32_t>(x), static_cast<int32_t>(y)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpWalk(JNIEnv* env, jclass, jint direction) {
    return op_result(env, "op_walk", ("direction=" + str_of(direction)), data_op_walk(static_cast<int32_t>(direction)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpWalkStop(JNIEnv* env, jclass) {
    return op_result(env, "op_walk_stop", (std::string("")), data_op_walk_stop());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpInteract(JNIEnv* env, jclass) {
    return op_result(env, "op_interact", (std::string("")), data_op_interact());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpAttack(JNIEnv* env, jclass, jint role, jint target_slot) {
    return op_result(env, "op_attack", ("role=" + str_of(role) + " " + "target_slot=" + str_of(target_slot)), data_op_attack(static_cast<int32_t>(role), static_cast<int32_t>(target_slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpStopCombat(JNIEnv* env, jclass, jint role) {
    return op_result(env, "op_stop_combat", ("role=" + str_of(role)), data_op_stop_combat(static_cast<int32_t>(role)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpUseItem(JNIEnv* env, jclass, jint bag, jint slot) {
    return op_result(env, "op_use_item", ("bag=" + str_of(bag) + " " + "slot=" + str_of(slot)), data_op_use_item(static_cast<int>(bag), static_cast<int>(slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpDiceAccept(JNIEnv* env, jclass) {
    return op_result(env, "op_dice_accept", (std::string("")), data_op_dice_accept());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpDiceReject(JNIEnv* env, jclass) {
    return op_result(env, "op_dice_reject", (std::string("")), data_op_dice_reject());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpDiscardItem(JNIEnv* env, jclass, jint bag, jint slot) {
    return op_result(env, "op_discard_item", ("bag=" + str_of(bag) + " " + "slot=" + str_of(slot)), data_op_discard_item(static_cast<int>(bag), static_cast<int>(slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSellItem(JNIEnv* env, jclass, jint bag, jint slot) {
    return op_result(env, "op_sell_item", ("bag=" + str_of(bag) + " " + "slot=" + str_of(slot)), data_op_sell_item(static_cast<int>(bag), static_cast<int>(slot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpMoveItem(JNIEnv* env, jclass, jint bag, jint slot, jint count, jint toBag, jint toSlot) {
    return op_result(env, "op_move_item", ("bag=" + str_of(bag) + " " + "slot=" + str_of(slot) + " " + "count=" + str_of(count) + " " + "toBag=" + str_of(toBag) + " " + "toSlot=" + str_of(toSlot)), data_op_move_item(static_cast<int>(bag), static_cast<int>(slot),
        static_cast<int>(count), static_cast<int>(toBag), static_cast<int>(toSlot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpIncludeParty(JNIEnv* env, jclass, jint mercSlot) {
    return op_result(env, "op_include_party", ("mercSlot=" + str_of(mercSlot)), data_op_include_party(static_cast<int>(mercSlot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpExcludeParty(JNIEnv* env, jclass, jint mercSlot) {
    return op_result(env, "op_exclude_party", ("mercSlot=" + str_of(mercSlot)), data_op_exclude_party(static_cast<int>(mercSlot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpDischarge(JNIEnv* env, jclass, jint mercSlot) {
    return op_result(env, "op_discharge", ("mercSlot=" + str_of(mercSlot)), data_op_discharge(static_cast<int>(mercSlot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpWithdraw(JNIEnv* env, jclass, jint mercSlot, jint equipSlot) {
    return op_result(env, "op_withdraw", ("mercSlot=" + str_of(mercSlot) + " " + "equipSlot=" + str_of(equipSlot)), data_op_withdraw(static_cast<int>(mercSlot), static_cast<int32_t>(equipSlot)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetHp(JNIEnv* env, jclass, jint role, jint hp) {
    return op_result(env, "op_set_hp", ("role=" + str_of(role) + " " + "hp=" + str_of(hp)), data_op_set_hp(static_cast<int>(role), static_cast<int32_t>(hp)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetMp(JNIEnv* env, jclass, jint role, jint mp) {
    return op_result(env, "op_set_mp", ("role=" + str_of(role) + " " + "mp=" + str_of(mp)), data_op_set_mp(static_cast<int>(role), static_cast<int32_t>(mp)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpSetAttr(JNIEnv* env, jclass, jint role, jint attrIndex, jint value) {
    return op_result(env, "op_set_attr", ("role=" + str_of(role) + " " + "attrIndex=" + str_of(attrIndex) + " " + "value=" + str_of(value)), data_op_set_attr(static_cast<int>(role), static_cast<int32_t>(attrIndex), static_cast<int32_t>(value)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeOpAddItem(JNIEnv* env, jclass, jint category, jint count, jint socket,
                                                   jint enhance_remaining, jint rarity) {
    return op_result(env, "op_add_item", ("category=" + str_of(category) + " " + "count=" + str_of(count) + " " + "socket=" + str_of(socket) + " " + "enhanceRemaining=" + str_of(enhance_remaining) + " " + "rarity=" + str_of(rarity)), data_op_add_item(static_cast<int32_t>(category), static_cast<int32_t>(count), static_cast<int32_t>(socket), static_cast<int32_t>(enhance_remaining), static_cast<int32_t>(rarity)));
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetStackLimitEnabled(JNIEnv*, jclass, jboolean enabled) {
    return set_stack_limit_enabled(enabled == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetMoveMergeEnabled(JNIEnv*, jclass, jboolean enabled) {
    return set_move_merge_enabled(enabled == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetExtensionBagEnabled(JNIEnv*, jclass, jboolean enabled) {
    return set_virtual_bag_enabled(enabled == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetGemCraftOptimizeEnabled(JNIEnv*, jclass, jboolean enabled) {
    // 开关已合并：原 gemcraftEnabled 与 customRecipeEnabled 共用同一条通路，
    // 界面行为归 feature/craft_ui，配方规则归 feature/custom_recipe。此导出保留为兼容 stub。
    (void)enabled;
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inotia4_qol_NativeBridge_nativeSetCustomRecipeEnabled(JNIEnv*, jclass, jboolean enabled) {
    return custom_recipe::set_enabled(enabled == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}


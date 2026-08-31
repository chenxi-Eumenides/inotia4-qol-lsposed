package com.inotia4.qol

private const val NATIVE_TAG = "Inotia4Export"

object NativeBridge {

    @Volatile
    var ready: Boolean = false
        private set

    init {
        try {
            System.loadLibrary("gamebridge")
        } catch (t: Throwable) {
            android.util.Log.e(NATIVE_TAG, "loadLibrary(gamebridge) failed", t)
        }
    }

    fun init(): Boolean {
        if (ready) return true
        ready = try {
            nativeRegisterConfigBridge(ModuleConfigUiBridge::class.java)
        nativeRegisterExtensionBagUiBridge(ExtensionBagUiBridge::class.java)
            nativeInit()
        } catch (t: Throwable) {
            android.util.Log.e(NATIVE_TAG, "nativeInit failed", t)
            false
        }
        return ready
    }

    external fun nativeInit(): Boolean
    external fun nativeRegisterConfigBridge(bridge: Class<*>)
    external fun nativeRegisterExtensionBagUiBridge(bridge: Class<*>)
    external fun nativeGetInitReport(): String
    external fun nativeGetBaseAddr(): Long
    external fun nativeGetFrameCount(): Long
    external fun nativeGetActiveQuest(): Int
    external fun nativeQuestList(): String
    external fun nativeQuestCompleted(): String
    external fun nativeQuestActive(): String
    external fun nativeCurrentSaveSlot(): String
    external fun nativeGetPlayerJson(): String
    external fun nativeGetPartyJson(): String
    external fun nativeGetInventoryJson(): String
    external fun nativeGetMapJson(): String
    external fun nativeGetTilesJson(): String
    external fun nativeSetTilesData(json: String): Boolean
    external fun nativeGetUnitsJson(): String
    external fun nativeGetEnemiesJson(): String
    external fun nativeGetInteractivesJson(): String
    external fun nativeGetGamestateJson(): String
    external fun nativeGetDebugUiJson(): String
    external fun nativeGetSnapshotJson(): String
    external fun nativeGetSkillsJson(): String
    external fun nativeGetMercenariesJson(): String
    external fun nativeGetDropsJson(): String
    external fun nativeGetPathJson(tx: Int, ty: Int): String
    external fun nativeDistanceJson(tx: Int, ty: Int): String
    external fun nativeDebugPathJson(tx: Int, ty: Int): String

    external fun nativeOpSetMoney(money: Long): String
    external fun nativeOpAddMoney(delta: Long): String
    external fun nativeOpMinusMoney(delta: Long): String
    external fun nativeOpSetExperience(role: Int, exp: Long): String
    external fun nativeOpSetLevel(role: Int, level: Int, force: Boolean): String
    external fun nativeOpAddExperience(role: Int, delta: Long): String
    external fun nativeOpSetStatusPoint(role: Int, points: Int): String
    external fun nativeOpSetHp(role: Int, hp: Int): String
    external fun nativeOpSetMp(role: Int, mp: Int): String
    external fun nativeOpSetAttr(role: Int, attrIndex: Int, value: Int): String
    external fun nativeOpAddItem(category: Int, count: Int): String
    external fun nativeOpAddStat(role: Int, attr: Int): String
    external fun nativeOpStatReset(role: Int): String
    external fun nativeOpSkillReset(role: Int): String
    external fun nativeOpCast(role: Int, actionId: Int): String
    external fun nativeOpQuestQuit(questId: Int): String
    external fun nativeOpSave(): String
    external fun nativeOpMainMenu(): String
    external fun nativeOpEnterSlot(slot: Int): String
    external fun nativeOpCreateSlot(slot: Int, classIdx: Int): String
    external fun nativeOpPanelClose(): String
    external fun nativeOpPanelOpen(panel: String): String
    external fun nativeRecoverAfterHiveBlock(): String
    external fun nativeExp1BtnBehavior(): String
    external fun nativeExp2AddControl(): String
    external fun nativeExp3CustomDialog(text: String): String
    external fun nativeExp4TextAppearance(): String
    external fun nativeExp5NewPanel(): String
    external fun nativeExpRestoreAll(): String
    external fun nativeExpStatus(): String
    external fun nativeSaveSlotsJson(): String
    external fun nativeOpNpcInteract(): String
    external fun nativeNpcDialogOptions(): String
    external fun nativeShopItems(): String
    external fun nativeOpShopBuy(slot: Int): String
    external fun nativeDialogContent(): String
    external fun nativeOpDialogSelect(action: String, index: Int): String
    external fun nativeOpJewel(role: Int, bag: Int, slot: Int, equipSlot: Int): String
    external fun nativeOpEnchant(role: Int, bag: Int, slot: Int, equipSlot: Int): String
    external fun nativeOpSetAutoAttack(role: Int, onoff: Int): String
    external fun nativeOpSetSkillUsage(role: Int, onoff: Int): String
    external fun nativeOpEquip(role: Int, bag: Int, slot: Int): String
    external fun nativeOpUnequip(role: Int, slot: Int): String
    external fun nativeOpSwitchPlayer(slot: Int): String
    external fun nativeOpTeleport(mapId: Int, x: Int, y: Int): String
    external fun nativeOpRemoveItem(category: Int): String
    external fun nativeOpLearnAction(role: Int, actionId: Int, level: Int): String
    external fun nativeGetEventsJson(): String

    external fun nativeOpMove(x: Int, y: Int): String
    external fun nativeOpWalk(direction: Int): String
    external fun nativeOpWalkStop(): String
    external fun nativeOpInteract(): String
    external fun nativeOpAttack(role: Int, targetSlot: Int): String
    external fun nativeOpStopCombat(role: Int): String
    external fun nativeOpUseItem(bag: Int, slot: Int): String
    external fun nativeOpDiceAccept(): String
    external fun nativeOpDiceReject(): String
    external fun nativeOpDiscardItem(bag: Int, slot: Int): String
    external fun nativeOpSellItem(bag: Int, slot: Int): String
    external fun nativeOpMoveItem(bag: Int, slot: Int, count: Int, toBag: Int, toSlot: Int): String
    external fun nativeOpIncludeParty(mercSlot: Int): String
    external fun nativeOpExcludeParty(mercSlot: Int): String
    external fun nativeOpDischarge(mercSlot: Int): String
    external fun nativeOpWithdraw(mercSlot: Int, equipSlot: Int): String
    external fun nativeOpPartySwap(a: Int, b: Int): String
    external fun nativeSetStackLimitEnabled(enabled: Boolean): Boolean
    external fun nativeSetMoveMergeEnabled(enabled: Boolean): Boolean
    external fun nativeSetExtensionBagEnabled(enabled: Boolean): Boolean
    external fun nativeSettingsUiInject(): String
    external fun nativeSettingsUiStatus(): String
    external fun nativeSettingsUiRestore(): String
    external fun nativeSettingsUiOpenOption(): String
    external fun nativeSettingsUiOpenPanel(): String
    external fun nativeExtensionBagUiStatus(): String
    external fun nativeExtensionBagTestEquip(index: Int, bagType: Int): String
    external fun nativeExtensionBagTestItem(index: Int, slot: Int, category: Int, count: Int): String
    external fun nativeExtensionBagStatusJson(): String
    external fun nativeOpExtensionBagEnterView(bag: Int): String
    external fun nativeOpExtensionBagUnequip(bag: Int): String
    external fun nativeOpExtensionBagExitView(): String
    external fun nativeOpExtensionBagSelectBag(bag: Int): String
    external fun nativeOpExtensionBagClickItem(bag: Int, slot: Int): String
    external fun nativeOpExtensionBagMoveItem(fromBag: Int, fromSlot: Int, toBag: Int, toSlot: Int): String
}

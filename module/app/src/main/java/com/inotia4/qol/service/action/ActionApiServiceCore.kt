package com.inotia4.qol.service.action

import com.inotia4.qol.service.contract.ActionApiService

/**
 * ActionApiService 接口的唯一实现；仍由 service 持有的合法写操作委托给对应 action。
 */
class ActionApiServiceCore : ActionApiService {
    override fun save(): String = SaveActions.save()

    override fun mainMenu(): String = SaveActions.mainMenu()

    override fun enterSlot(slot: Int): String = SaveActions.enterSlot(slot)

    override fun createSlot(slot: Int, classIdx: Int): String = SaveActions.createSlot(slot, classIdx)

    override fun panelClose(): String = UiActions.panelClose()

    override fun panelOpen(panel: String): String = UiActions.panelOpen(panel)

    override fun npcInteract(): String = UiActions.npcInteract()

    override fun dialogSelect(action: String, index: Int): String = UiActions.dialogSelect(action, index)
}

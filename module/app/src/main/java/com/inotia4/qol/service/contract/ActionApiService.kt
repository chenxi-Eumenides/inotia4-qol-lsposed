package com.inotia4.qol.service.contract

interface ActionApiService {
    fun save(): String
    fun mainMenu(): String
    fun enterSlot(slot: Int): String
    fun createSlot(slot: Int, classIdx: Int): String
    fun panelClose(): String
    fun panelOpen(panel: String): String
    fun npcInteract(): String
    fun dialogSelect(action: String, index: Int): String
}

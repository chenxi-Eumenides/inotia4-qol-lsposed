package com.inotia4.qol.service.enrichment

import org.json.JSONObject

object NameInjector {
    fun injectSkillNames(role: JSONObject) = NameInjectorCore.injectSkillNames(role)
    fun injectQuestFields(target: JSONObject, data: JSONObject, keys: List<String>) = NameInjectorCore.injectQuestFields(target, data, keys)
    fun injectEquipmentNames(role: JSONObject) = NameInjectorCore.injectEquipmentNames(role)
    fun restructureItem(item: JSONObject) = NameInjectorCore.restructureItem(item)
    fun injectItemName(item: JSONObject, equipOverride: Boolean? = null) = NameInjectorCore.injectItemName(item, equipOverride)
    fun injectItemOptions(item: JSONObject) = NameInjectorCore.injectItemOptions(item)
    fun injectSocketEnchant(item: JSONObject) = NameInjectorCore.injectSocketEnchant(item)
    fun injectTypeName(role: JSONObject) = NameInjectorCore.injectTypeName(role)
    fun injectClassName(role: JSONObject) = NameInjectorCore.injectClassName(role)
    fun restructureRole(role: JSONObject) = NameInjectorCore.restructureRole(role)
    fun withItemNames(json: String): String = NameInjectorCore.withItemNames(json)
}

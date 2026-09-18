package com.inotia4.qol.service.info

import com.inotia4.qol.NativeBridge
import com.inotia4.qol.util.JsonUtil

/** Thin system/debug query endpoints kept out of the aggregate information service. */
internal object InfoSystemQueries {
    fun debugUi(): String = NativeBridge.nativeGetDebugUiJson()
    fun debugPath(tx: Int, ty: Int): String = NativeBridge.nativeDebugPathJson(tx, ty)
    fun debugItemRaw(bag: Int, slot: Int): String = NativeBridge.nativeDebugItemRawJson(bag, slot)
    fun settingsUiInject(): String = NativeBridge.nativeSettingsUiInject()
    fun settingsUiStatus(): String = NativeBridge.nativeSettingsUiStatus()
    fun settingsUiRestore(): String = NativeBridge.nativeSettingsUiRestore()
    fun settingsUiOpenOption(): String = NativeBridge.nativeSettingsUiOpenOption()
    fun settingsUiOpenPanel(): String = NativeBridge.nativeSettingsUiOpenPanel()
    fun extensionBagStatusJson(): String = NativeBridge.nativeExtensionBagStatusJson()
    fun extensionBagTestEquip(index: Int, bagType: Int): String = NativeBridge.nativeExtensionBagTestEquip(index, bagType)
    fun extensionBagTestItem(index: Int, slot: Int, category: Int, count: Int): String =
        NativeBridge.nativeExtensionBagTestItem(index, slot, category, count)
    fun health(): String = JsonUtil.wrap(
        "ok" to true,
        "ready" to NativeBridge.ready,
        "variant" to JsonUtil.parseObj(NativeBridge.nativeGetVariantJson())
    )
}

package com.inotia4.qol.service.info

import com.inotia4.qol.NativeBridge
import com.inotia4.qol.util.JsonUtil

/** Thin system/debug query endpoints kept out of the aggregate information service. */
internal object InfoSystemQueries {
    fun debugUi(): String = NativeBridge.nativeGetDebugUiJson()
    fun debugPath(tx: Int, ty: Int): String = NativeBridge.nativeDebugPathJson(tx, ty)
    fun exp1BtnBehavior(): String = NativeBridge.nativeExp1BtnBehavior()
    fun exp2AddControl(): String = NativeBridge.nativeExp2AddControl()
    fun exp3CustomDialog(text: String): String = NativeBridge.nativeExp3CustomDialog(text)
    fun exp4TextAppearance(): String = NativeBridge.nativeExp4TextAppearance()
    fun exp5NewPanel(): String = NativeBridge.nativeExp5NewPanel()
    fun expRestoreAll(): String = NativeBridge.nativeExpRestoreAll()
    fun expStatus(): String = NativeBridge.nativeExpStatus()
    fun settingsUiInject(): String = NativeBridge.nativeSettingsUiInject()
    fun settingsUiStatus(): String = NativeBridge.nativeSettingsUiStatus()
    fun settingsUiRestore(): String = NativeBridge.nativeSettingsUiRestore()
    fun settingsUiOpenOption(): String = NativeBridge.nativeSettingsUiOpenOption()
    fun settingsUiOpenPanel(): String = NativeBridge.nativeSettingsUiOpenPanel()
    fun extensionBagStatusJson(): String = NativeBridge.nativeExtensionBagStatusJson()
    fun extensionBagTestEquip(index: Int, bagType: Int): String = NativeBridge.nativeExtensionBagTestEquip(index, bagType)
    fun extensionBagTestItem(index: Int, slot: Int, category: Int, count: Int): String =
        NativeBridge.nativeExtensionBagTestItem(index, slot, category, count)
    fun health(): String = JsonUtil.wrap("ok" to true)
}

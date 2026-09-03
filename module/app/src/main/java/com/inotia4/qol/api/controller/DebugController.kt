package com.inotia4.qol.api.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.JsonUtil
import com.inotia4.qol.util.ControllerGuard
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RequestParam
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.StatusCode

/**
 * 调试端点：/api/debug/ui、/api/debug/path、/api/debug/exp（architecture §9.1 登记）。
 * 路由统一全路径写法（无类级 RequestMapping，见 architecture §3）。
 * v0.5.46 收边：不再裸调 NativeBridge，经 InfoApiService（ControllerGuard 兜底 not ready/500）。
 * ui-exp v0.6.7：实验端点触发 5 种自定义 UI 方式（docs/system/ui.md §6）。
 */
@RestController
class DebugController {

    @GetMapping("/api/debug/ui")
    fun ui(): String = ControllerGuard.guard { ApiServices.info.debugUi() }

    @GetMapping("/api/debug/path")
    fun path(@RequestParam("tx") tx: Int, @RequestParam("ty") ty: Int): String =
        ControllerGuard.guard { ApiServices.info.debugPath(tx, ty) }

    @GetMapping("/api/debug/exp/status")
    fun expStatus(): String = ControllerGuard.guard { ApiServices.info.expStatus() }

    @PostMapping("/api/debug/exp/1")
    fun exp1(): String = ControllerGuard.guard { ApiServices.info.exp1BtnBehavior() }

    @PostMapping("/api/debug/exp/2")
    fun exp2(): String = ControllerGuard.guard { ApiServices.info.exp2AddControl() }

    @PostMapping("/api/debug/exp/3")
    fun exp3(@RequestBody body: String): String = ControllerGuard.guard { ApiServices.info.exp3CustomDialog(body) }

    @PostMapping("/api/debug/exp/4")
    fun exp4(): String = ControllerGuard.guard { ApiServices.info.exp4TextAppearance() }

    @PostMapping("/api/debug/exp/5")
    fun exp5(): String = ControllerGuard.guard { ApiServices.info.exp5NewPanel() }

    @PostMapping("/api/debug/exp/restore")
    fun expRestore(): String = ControllerGuard.guard { ApiServices.info.expRestoreAll() }

    @PostMapping("/api/debug/settings-ui/inject")
    fun settingsUiInject(): String = ControllerGuard.guard { ApiServices.info.settingsUiInject() }

    @GetMapping("/api/debug/settings-ui/status")
    fun settingsUiStatus(): String = ControllerGuard.guard { ApiServices.info.settingsUiStatus() }

    @PostMapping("/api/debug/settings-ui/restore")
    fun settingsUiRestore(): String = ControllerGuard.guard { ApiServices.info.settingsUiRestore() }

    @PostMapping("/api/debug/settings-ui/open-option")
    fun settingsUiOpenOption(): String = ControllerGuard.guard { ApiServices.info.settingsUiOpenOption() }

    @PostMapping("/api/debug/settings-ui/open-panel")
    fun settingsUiOpenPanel(): String = ControllerGuard.guard { ApiServices.info.settingsUiOpenPanel() }

    @GetMapping("/api/debug/extension_bag/status")
    fun extensionBagStatus(): String = ControllerGuard.guard { ApiServices.info.extensionBagStatusJson() }

    @PostMapping("/api/debug/extension_bag/enter_view")
    fun extensionBagEnterView(@RequestBody body: String): String {
        val json = JsonUtil.parseBody(body)
            ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val bag = json.optInt("bag", -1)
        if (bag < 6 || bag > 10) throw ApiException(StatusCode.SC_BAD_REQUEST, "bag required (6-10)")
        return ControllerGuard.guard { ApiServices.action.extensionBagEnterView(bag) }
    }

    @PostMapping("/api/debug/extension_bag/exit_view")
    fun extensionBagExitView(): String = ControllerGuard.guard { ApiServices.action.extensionBagExitView() }

    @PostMapping("/api/debug/extension_bag/unequip")
    fun extensionBagUnequip(@RequestBody body: String): String {
        val json = JsonUtil.parseBody(body)
            ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val bag = json.optInt("bag", -1)
        if (bag < 6 || bag > 10) throw ApiException(StatusCode.SC_BAD_REQUEST, "bag required (6-10)")
        return ControllerGuard.guard { ApiServices.action.extensionBagUnequip(bag) }
    }

    @PostMapping("/api/debug/extension_bag/select_bag")
    fun extensionBagSelectBag(@RequestBody body: String): String {
        val json = JsonUtil.parseBody(body)
            ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val bag = json.optInt("bag", -1)
        if (bag < 6 || bag > 10) throw ApiException(StatusCode.SC_BAD_REQUEST, "bag required (6-10)")
        return ControllerGuard.guard { ApiServices.action.extensionBagSelectBag(bag) }
    }

    @PostMapping("/api/debug/extension_bag/click_item")
    fun extensionBagClickItem(@RequestBody body: String): String {
        val json = JsonUtil.parseBody(body)
            ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val bag = json.optInt("bag", -1)
        val slot = json.optInt("slot", -1)
        if (bag < 6 || bag > 10) throw ApiException(StatusCode.SC_BAD_REQUEST, "bag required (6-10)")
        if (slot < 0 || slot > 15) throw ApiException(StatusCode.SC_BAD_REQUEST, "slot required (0-15)")
        return ControllerGuard.guard { ApiServices.action.extensionBagClickItem(bag, slot) }
    }

    @PostMapping("/api/debug/extension_bag/equip")
    fun extensionBagEquip(@RequestBody body: String): String {
        val json = JsonUtil.parseBody(body)
            ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        return ControllerGuard.guard {
            ApiServices.info.extensionBagTestEquip(json.optInt("index", -1), json.optInt("bagType", -1))
        }
    }

    @PostMapping("/api/debug/extension_bag/item")
    fun extensionBagItem(@RequestBody body: String): String {
        val json = JsonUtil.parseBody(body)
            ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        return ControllerGuard.guard {
            ApiServices.info.extensionBagTestItem(
                json.optInt("index", -1),
                json.optInt("slot", -1),
                json.optInt("category", -1),
                json.optInt("count", -1),
            )
        }
    }

}

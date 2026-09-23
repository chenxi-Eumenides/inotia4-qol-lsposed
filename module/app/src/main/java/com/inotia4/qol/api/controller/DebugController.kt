package com.inotia4.qol.api.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.service.info.NativeHttpCommand
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.ControllerGuard
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RequestParam
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.StatusCode

/**
 * 调试端点：/api/debug/ui、/api/debug/path（architecture §9.1 登记）。
 * 路由统一全路径写法（无类级 RequestMapping，见 architecture §3）。
 * settings-ui / extension_bag 的写端点已迁 owner（core.ui / extension_bag）：
 * Kotlin 只做传输适配（NativeHttpCommand.post），body 原样透传、参数校验与响应由 owner 承载。
 */
@RestController
class DebugController {

    @GetMapping("/api/debug/ui")
    fun ui(): String = ControllerGuard.guard { ApiServices.info.debugUi() }

    @GetMapping("/api/debug/path")
    fun path(@RequestParam("tx") tx: Int, @RequestParam("ty") ty: Int): String =
        ControllerGuard.guard { ApiServices.info.debugPath(tx, ty) }

    /** 仅测试/调试：读槽位物品原始字节 + 位分解（纯读取，bag 0..10 含扩展袋） */
    @GetMapping("/api/debug/item/raw")
    fun itemRaw(@RequestParam("bag") bag: Int, @RequestParam("slot") slot: Int): String {
        if (bag < 0 || bag > 10) throw ApiException(StatusCode.SC_BAD_REQUEST, "bag required (0-10)")
        if (slot < 0 || slot > 15) throw ApiException(StatusCode.SC_BAD_REQUEST, "slot required (0-15)")
        return ControllerGuard.guard { ApiServices.info.debugItemRaw(bag, slot) }
    }

    @PostMapping("/api/debug/settings-ui/inject")
    fun settingsUiInject(): String =
        ControllerGuard.guard { NativeHttpCommand.post("/api/debug/settings-ui/inject", null) }

    @GetMapping("/api/debug/settings-ui/status")
    fun settingsUiStatus(): String = ControllerGuard.guard { ApiServices.info.settingsUiStatus() }

    @PostMapping("/api/debug/settings-ui/restore")
    fun settingsUiRestore(): String =
        ControllerGuard.guard { NativeHttpCommand.post("/api/debug/settings-ui/restore", null) }

    @PostMapping("/api/debug/settings-ui/open-option")
    fun settingsUiOpenOption(): String =
        ControllerGuard.guard { NativeHttpCommand.post("/api/debug/settings-ui/open-option", null) }

    @PostMapping("/api/debug/settings-ui/open-panel")
    fun settingsUiOpenPanel(): String =
        ControllerGuard.guard { NativeHttpCommand.post("/api/debug/settings-ui/open-panel", null) }

    @GetMapping("/api/debug/extension_bag/status")
    fun extensionBagStatus(): String = ControllerGuard.guard { ApiServices.info.extensionBagStatusJson() }

    @PostMapping("/api/debug/extension_bag/enter_view")
    fun extensionBagEnterView(@RequestBody body: String): String =
        ControllerGuard.guard { postEb("/api/debug/extension_bag/enter_view", body) }

    @PostMapping("/api/debug/extension_bag/exit_view")
    fun extensionBagExitView(): String =
        ControllerGuard.guard { NativeHttpCommand.post("/api/debug/extension_bag/exit_view", null) }

    @PostMapping("/api/debug/extension_bag/unequip")
    fun extensionBagUnequip(@RequestBody body: String): String =
        ControllerGuard.guard { postEb("/api/debug/extension_bag/unequip", body) }

    @PostMapping("/api/debug/extension_bag/select_bag")
    fun extensionBagSelectBag(@RequestBody body: String): String =
        ControllerGuard.guard { postEb("/api/debug/extension_bag/select_bag", body) }

    @PostMapping("/api/debug/extension_bag/click_item")
    fun extensionBagClickItem(@RequestBody body: String): String =
        ControllerGuard.guard { postEb("/api/debug/extension_bag/click_item", body) }

    @PostMapping("/api/debug/extension_bag/equip")
    fun extensionBagEquip(@RequestBody body: String): String =
        ControllerGuard.guard { postEb("/api/debug/extension_bag/equip", body) }

    @PostMapping("/api/debug/extension_bag/item")
    fun extensionBagItem(@RequestBody body: String): String =
        ControllerGuard.guard { postEb("/api/debug/extension_bag/item", body) }

    /** 迁移前 Kotlin parseBody 的 400 语义保留在传输层：非法 body 拒绝，合法 body 归一化透传。 */
    private fun postEb(path: String, body: String): String {
        val json = JsonUtil.parseBody(body)
            ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        return NativeHttpCommand.post(path, json.toString())
    }
}

package com.inotia4.qol.api.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.service.info.NativeHttpCommand
import com.inotia4.qol.service.info.NativeHttpQuery
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.ControllerGuard
import com.inotia4.qol.util.withNativeRouteLease
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RequestParam
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.HttpRequest
import com.yanzhenjie.andserver.http.StatusCode

/**
 * 调试端点：/api/debug/ui、/api/debug/path（architecture §9.1 登记）。
 * 路由统一全路径写法（无类级 RequestMapping，见 architecture §3）。
 * settings-ui 的写端点已迁 owner（core.ui）：
 * Kotlin 只做传输适配（NativeHttpCommand.post），body 原样透传、参数校验与响应由 owner 承载。
 */
@RestController
class DebugController {

    @GetMapping("/api/debug/ui")
    fun ui(request: HttpRequest): String =
        withNativeRouteLease(request) { ControllerGuard.guard { ApiServices.info.debugUi() } }

    @GetMapping("/api/debug/path")
    fun path(@RequestParam("tx") tx: Int, @RequestParam("ty") ty: Int, request: HttpRequest): String =
        withNativeRouteLease(request) { ControllerGuard.guard { ApiServices.info.debugPath(tx, ty) } }

    /** 仅测试/调试：读槽位物品原始字节 + 位分解（纯读取，bag 0..10 含扩展袋） */
    @GetMapping("/api/debug/item/raw")
    fun itemRaw(@RequestParam("bag") bag: Int, @RequestParam("slot") slot: Int, request: HttpRequest): String =
        withNativeRouteLease(request) {
            if (bag < 0 || bag > 10) throw ApiException(StatusCode.SC_BAD_REQUEST, "bag required (0-10)")
            if (slot < 0 || slot > 15) throw ApiException(StatusCode.SC_BAD_REQUEST, "slot required (0-15)")
            ControllerGuard.guard { ApiServices.info.debugItemRaw(bag, slot) }
        }

    @PostMapping("/api/debug/settings-ui/inject")
    fun settingsUiInject(): String =
        ControllerGuard.guard { NativeHttpCommand.post("/api/debug/settings-ui/inject", null) }

    @GetMapping("/api/debug/settings-ui/status")
    fun settingsUiStatus(request: HttpRequest): String =
        withNativeRouteLease(request) { ControllerGuard.guard { ApiServices.info.settingsUiStatus() } }

    @PostMapping("/api/debug/settings-ui/restore")
    fun settingsUiRestore(): String =
        ControllerGuard.guard { NativeHttpCommand.post("/api/debug/settings-ui/restore", null) }

    @PostMapping("/api/debug/settings-ui/open-option")
    fun settingsUiOpenOption(): String =
        ControllerGuard.guard { NativeHttpCommand.post("/api/debug/settings-ui/open-option", null) }

    @PostMapping("/api/debug/settings-ui/open-panel")
    fun settingsUiOpenPanel(): String =
        ControllerGuard.guard { NativeHttpCommand.post("/api/debug/settings-ui/open-panel", null) }

    @PostMapping("/api/debug/custom-ui/components")
    fun customUiComponents(@RequestBody body: String): String =
        ControllerGuard.guard { NativeHttpCommand.post("/api/debug/custom-ui/components", body) }

    @PostMapping("/api/debug/custom-ui/scroll")
    fun customUiScroll(@RequestBody body: String): String =
        ControllerGuard.guard { NativeHttpCommand.post("/api/debug/custom-ui/scroll", body) }

    @PostMapping("/api/debug/custom-ui/help")
    fun customUiHelp(@RequestBody body: String): String =
        ControllerGuard.guard { NativeHttpCommand.post("/api/debug/custom-ui/help", body) }

}

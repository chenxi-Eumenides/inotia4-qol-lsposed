package com.inotia4.qol.api.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ControllerGuard
import com.inotia4.qol.util.withNativeRouteLease
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.HttpRequest

/**
 * 界面状态：/api/ui（api-reference §6.1）。复合 + screen/panel/dialog 子端点。
 * 路由统一全路径写法（无类级 RequestMapping，见 architecture §3）。
 *
 * /api/ui 为 Kotlin legacy 复合（无 native handler），按 direct 路由获取 native route lease；
 * screen/panel/dialog 经 nativeHttpDispatch 由 owner 处理，不重复包 lease。
 */
@RestController
class UiController {

    @GetMapping("/api/ui")
    fun composite(request: HttpRequest): String =
        withNativeRouteLease(request) { ControllerGuard.guard(ApiServices.info::ui) }

    @GetMapping("/api/ui/screen")
    fun screen(): String = ControllerGuard.guard(ApiServices.info::uiScreen)

    @GetMapping("/api/ui/panel")
    fun panel(): String = ControllerGuard.guard(ApiServices.info::uiPanel)

    @GetMapping("/api/ui/dialog")
    fun dialog(): String = ControllerGuard.guard(ApiServices.info::uiDialog)
}

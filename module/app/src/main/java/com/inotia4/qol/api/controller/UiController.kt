package com.inotia4.qol.api.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ControllerGuard
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.RestController

/**
 * 统一界面状态查询。
 * 路由统一全路径写法（无类级 RequestMapping，见 architecture §3）。
 */
@RestController
class UiController {

    @GetMapping("/api/ui/state")
    fun state(): String = ControllerGuard.guard(ApiServices.info::uiState)

    @GetMapping("/api/ui/state/table")
    fun stateTable(): String = ControllerGuard.guard(ApiServices.info::uiStateTable)
}

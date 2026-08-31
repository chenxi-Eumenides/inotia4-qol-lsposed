package com.inotia4.qol.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ControllerGuard
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.RequestParam
import com.yanzhenjie.andserver.annotation.RestController

/**
 * 事件流：/api/system/events（api-reference §7.2）。轮询差异检测，since 参数预留。
 * 路由统一全路径写法（无类级 RequestMapping，见 architecture §3）。
 */
@RestController
class EventsController {

    @GetMapping("/api/system/events")
    fun events(@RequestParam("since", required = false) since: Long?): String =
        ControllerGuard.guard { ApiServices.info.events(since) }
}

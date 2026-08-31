package com.inotia4.qol.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ControllerGuard
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PathVariable
import com.yanzhenjie.andserver.annotation.RestController

/**
 * 任务：/api/quest（api-reference §0.2）。复合 + active/list/{id}/completed。
 * 方法级路径首段必须静态（AndServer 处理器约束，architecture §3）。
 */
@RestController
class QuestController {

    @GetMapping("/api/quest")
        fun composite(): String = ControllerGuard.guard(ApiServices.info::quest)

    @GetMapping("/api/quest/active")
    fun active(): String = ControllerGuard.guard(ApiServices.info::questActive)

    @GetMapping("/api/quest/details")
    fun details(): String = ControllerGuard.guard(ApiServices.info::questDetails)

    @GetMapping("/api/quest/{id}")
    fun questId(@PathVariable("id") id: Int): String =
        ControllerGuard.guard { ApiServices.info.questListId(id) }

    @GetMapping("/api/quest/completed")
    fun completed(): String = ControllerGuard.guard(ApiServices.info::questCompleted)
}

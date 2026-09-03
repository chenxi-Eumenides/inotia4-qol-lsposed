package com.inotia4.qol.api.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ControllerGuard
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PathVariable
import com.yanzhenjie.andserver.annotation.RestController

@RestController
class MercenaryController {

    @GetMapping("/api/character/mercenary")
    fun composite(): String = ControllerGuard.guard(ApiServices.info::mercenary)

    @GetMapping("/api/character/mercenary/list")
    fun list(): String = ControllerGuard.guard(ApiServices.info::mercenaryList)

    @GetMapping("/api/character/mercenary/{slot}")
    fun slot(@PathVariable("slot") slot: Int): String =
        ControllerGuard.guard { ApiServices.info.mercenarySlot(slot) }
}

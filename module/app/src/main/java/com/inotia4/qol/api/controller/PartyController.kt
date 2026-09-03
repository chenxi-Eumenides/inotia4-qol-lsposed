package com.inotia4.qol.api.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ControllerGuard
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PathVariable
import com.yanzhenjie.andserver.annotation.RestController

@RestController
class PartyController {

    @GetMapping("/api/character/party")
    fun composite(): String = ControllerGuard.guard(ApiServices.info::party)

    @GetMapping("/api/character/party/count")
    fun count(): String = ControllerGuard.guard(ApiServices.info::partyCount)

    @GetMapping("/api/character/leader")
    fun leader(): String = ControllerGuard.guard(ApiServices.info::partyLeader)

    @GetMapping("/api/character/party/{slot}")
    fun member(@PathVariable("slot") slot: Int): String =
        ControllerGuard.guard { ApiServices.info.partyMember(slot) }

    @GetMapping("/api/character/party/{slot}/id")
    fun memberId(@PathVariable("slot") slot: Int): String =
        ControllerGuard.guard { ApiServices.info.partyMemberId(slot) }

    @GetMapping("/api/character/party/{slot}/name")
    fun memberName(@PathVariable("slot") slot: Int): String =
        ControllerGuard.guard { ApiServices.info.partyMemberName(slot) }

    @GetMapping("/api/character/party/{slot}/level")
    fun memberLevel(@PathVariable("slot") slot: Int): String =
        ControllerGuard.guard { ApiServices.info.partyMemberLevel(slot) }

    @GetMapping("/api/character/party/{slot}/status")
    fun memberStatus(@PathVariable("slot") slot: Int): String =
        ControllerGuard.guard { ApiServices.info.partyMemberStatus(slot) }

    @GetMapping("/api/character/party/{slot}/stats")
    fun memberStats(@PathVariable("slot") slot: Int): String =
        ControllerGuard.guard { ApiServices.info.partyMemberStats(slot) }

    @GetMapping("/api/character/party/{slot}/equipment")
    fun memberEquipment(@PathVariable("slot") slot: Int): String =
        ControllerGuard.guard { ApiServices.info.partyMemberEquipment(slot) }

    @GetMapping("/api/character/party/{slot}/equipment/{equip_slot}")
    fun memberEquip(@PathVariable("slot") slot: Int, @PathVariable("equip_slot") equipSlot: Int): String =
        ControllerGuard.guard { ApiServices.info.partyMemberEquip(slot, equipSlot) }

    @GetMapping("/api/character/party/{slot}/skills")
    fun memberSkills(@PathVariable("slot") slot: Int): String =
        ControllerGuard.guard { ApiServices.info.partyMemberSkills(slot) }
}

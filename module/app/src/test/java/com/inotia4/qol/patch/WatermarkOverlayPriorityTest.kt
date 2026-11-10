package com.inotia4.qol.patch

/** Standalone pure checks for module-health text, color priority, and copied names. */
object WatermarkOverlayPriorityTest {
    @JvmStatic
    fun main(args: Array<String>) {
        check(MODULE_HEALTH_WATERMARK_COLOR == 0xCCFF0000.toInt())

        val source = mutableListOf("Native", "帧钩子", "Native", " ")
        val names = copyModuleHealthNames(source)
        source.clear()
        check(names == listOf("Native", "帧钩子"))
        check(runCatching { (names as MutableList<String>).add("模块初始化") }.isFailure)

        check(moduleHealthRuleActive(visible = true, unavailableNames = names))
        check(!moduleHealthRuleActive(visible = false, unavailableNames = names))
        check(!moduleHealthRuleActive(visible = true, unavailableNames = emptyList()))
        check(moduleHealthWatermarkText(true, names) == "qol · Native、帧钩子")
        check(moduleHealthWatermarkText(false, names) == "qol")

        check(firstActiveWatermarkRuleIndex(listOf(true, true)) == 0)
        check(firstActiveWatermarkRuleIndex(listOf(false, true)) == 1)
        check(firstActiveWatermarkRuleIndex(listOf(false, false)) == -1)

        fun direction(
            damageIn: Int = 100,
            enemyHp: Int = 100,
            damageOut: Int = 100,
            potionCooldown: Int = 100,
            skillCooldown: Int = 100,
        ): WorldTuningWatermarkDirection = worldTuningWatermarkDirection(
            WorldTuningRates(damageIn, enemyHp, damageOut, potionCooldown, skillCooldown)
        )

        check(worldTuningWatermarkDirection(null) == WorldTuningWatermarkDirection.NEUTRAL)
        check(direction() == WorldTuningWatermarkDirection.NEUTRAL)
        check(direction(damageIn = 50) == WorldTuningWatermarkDirection.EASY)
        check(direction(enemyHp = 50) == WorldTuningWatermarkDirection.EASY)
        check(direction(damageOut = 150) == WorldTuningWatermarkDirection.EASY)
        check(direction(potionCooldown = 50) == WorldTuningWatermarkDirection.EASY)
        check(direction(skillCooldown = 50) == WorldTuningWatermarkDirection.EASY)
        check(direction(damageIn = 150) == WorldTuningWatermarkDirection.HARD)
        check(direction(enemyHp = 150) == WorldTuningWatermarkDirection.HARD)
        check(direction(damageOut = 50) == WorldTuningWatermarkDirection.HARD)
        check(direction(potionCooldown = 150) == WorldTuningWatermarkDirection.HARD)
        check(direction(skillCooldown = 150) == WorldTuningWatermarkDirection.HARD)
        check(direction(damageIn = 50, damageOut = 50) == WorldTuningWatermarkDirection.EASY)
    }
}

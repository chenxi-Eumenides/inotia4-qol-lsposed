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

        check(worldTuningWatermarkDirection(null) == WorldTuningWatermarkDirection.NEUTRAL)
        check(
            worldTuningWatermarkDirection(WorldTuningRates(damageIn = 100, enemyHp = 100, damageOut = 100)) ==
                WorldTuningWatermarkDirection.NEUTRAL
        )
        check(
            worldTuningWatermarkDirection(WorldTuningRates(damageIn = 50, enemyHp = 100, damageOut = 100)) ==
                WorldTuningWatermarkDirection.EASY
        )
        check(
            worldTuningWatermarkDirection(WorldTuningRates(damageIn = 100, enemyHp = 50, damageOut = 100)) ==
                WorldTuningWatermarkDirection.EASY
        )
        check(
            worldTuningWatermarkDirection(WorldTuningRates(damageIn = 100, enemyHp = 100, damageOut = 150)) ==
                WorldTuningWatermarkDirection.EASY
        )
        check(
            worldTuningWatermarkDirection(WorldTuningRates(damageIn = 150, enemyHp = 100, damageOut = 100)) ==
                WorldTuningWatermarkDirection.HARD
        )
        check(
            worldTuningWatermarkDirection(WorldTuningRates(damageIn = 100, enemyHp = 150, damageOut = 100)) ==
                WorldTuningWatermarkDirection.HARD
        )
        check(
            worldTuningWatermarkDirection(WorldTuningRates(damageIn = 100, enemyHp = 100, damageOut = 50)) ==
                WorldTuningWatermarkDirection.HARD
        )
        check(
            worldTuningWatermarkDirection(WorldTuningRates(damageIn = 50, enemyHp = 100, damageOut = 50)) ==
                WorldTuningWatermarkDirection.EASY
        )
    }
}

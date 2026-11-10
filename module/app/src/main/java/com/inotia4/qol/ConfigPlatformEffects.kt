package com.inotia4.qol

import com.inotia4.qol.patch.PopupBlockers
import com.inotia4.qol.patch.WatermarkOverlay
import com.inotia4.qol.patch.WorldTuningRates
import com.inotia4.qol.patch.worldTuningWatermarkDirection
import org.json.JSONObject

/** Applies plugin desired feature projections to platform-only effects. */
object ConfigPlatformEffects {
    @Volatile private var latestPopupBlockerRevision = Long.MIN_VALUE
    @Volatile private var latestWorldTuningRevision = Long.MIN_VALUE

    fun onPopupBlockerRuntimeEffect(eventRevision: Long, desiredEnabled: Boolean) {
        if (eventRevision <= latestPopupBlockerRevision) return
        latestPopupBlockerRevision = eventRevision
        PopupBlockers.setEnabled(desiredEnabled)
        LogFile.info(
            LogDomain.CONFIG,
            "popup_blocker runtime effect: java popup blocking enabled=$desiredEnabled",
        )
    }

    /** 读取 world_tuning 配置中的三项难度倍率；无 rates 对象时按全默认（中性）处理。 */
    internal fun worldTuningRatesFromConfig(configJson: String): WorldTuningRates? {
        return try {
            val rates = JSONObject(configJson).optJSONObject("rates") ?: return null
            WorldTuningRates(
                damageIn = rates.optInt("damage_in", 100),
                enemyHp = rates.optInt("enemy_hp", 100),
                damageOut = rates.optInt("damage_out", 100),
            )
        } catch (t: Throwable) {
            LogFile.error(LogDomain.CONFIG, "world_tuning runtime effect config parse failed", t)
            null
        }
    }

    fun onWorldTuningRuntimeEffect(
        eventRevision: Long,
        desiredEnabled: Boolean,
        configJson: String,
    ) {
        if (eventRevision <= latestWorldTuningRevision) return
        latestWorldTuningRevision = eventRevision
        val direction = worldTuningWatermarkDirection(worldTuningRatesFromConfig(configJson))
        WatermarkOverlay.updateWorldTuning(eventRevision, desiredEnabled, direction)
    }
}

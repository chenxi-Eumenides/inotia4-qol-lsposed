package com.inotia4.qol

import com.inotia4.qol.patch.PopupBlockers
import com.inotia4.qol.patch.WatermarkOverlay

/** Applies plugin desired feature projections to platform-only effects. */
object ConfigPlatformEffects {
    @Volatile private var latestSimpleModeRevision = Long.MIN_VALUE
    @Volatile private var latestPopupBlockerRevision = Long.MIN_VALUE

    fun onSimpleModeRuntimeEffect(eventRevision: Long, desiredEnabled: Boolean) {
        if (eventRevision <= latestSimpleModeRevision) return
        latestSimpleModeRevision = eventRevision
        WatermarkOverlay.updateSimpleMode(eventRevision, desiredEnabled)
    }

    fun onPopupBlockerRuntimeEffect(eventRevision: Long, desiredEnabled: Boolean) {
        if (eventRevision <= latestPopupBlockerRevision) return
        latestPopupBlockerRevision = eventRevision
        PopupBlockers.setEnabled(desiredEnabled)
        LogFile.info(
            LogDomain.CONFIG,
            "popup_blocker runtime effect: java popup blocking enabled=$desiredEnabled",
        )
    }
}

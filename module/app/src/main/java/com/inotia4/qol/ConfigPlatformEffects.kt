package com.inotia4.qol

import com.inotia4.qol.patch.WatermarkOverlay

/** Applies the simple-mode desired feature projection to its platform-only watermark effect. */
object ConfigPlatformEffects {
    @Volatile private var latestRevision = Long.MIN_VALUE

    fun onSimpleModeRuntimeEffect(eventRevision: Long, desiredEnabled: Boolean) {
        if (eventRevision <= latestRevision) return
        latestRevision = eventRevision
        WatermarkOverlay.updateSimpleMode(eventRevision, desiredEnabled)
    }
}

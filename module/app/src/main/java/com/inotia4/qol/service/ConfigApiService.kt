package com.inotia4.qol.service

import com.inotia4.qol.LogDomain
import com.inotia4.qol.LogFile
import com.inotia4.qol.NativeBridge
import com.inotia4.qol.StaticData

/** Static native enrichment ingress unrelated to user configuration ownership. */
interface ConfigApiService {
    fun applyTiles()
}

class ConfigApiServiceImpl : ConfigApiService {
    override fun applyTiles() {
        try {
            val tilesJson = StaticData.read("maps/tiles.json")
            if (tilesJson != null && NativeBridge.ready) {
                LogFile.info(LogDomain.CONFIG, "static tiles loaded: ${NativeBridge.nativeSetTilesData(tilesJson)}")
            } else if (tilesJson == null) {
                LogFile.info(LogDomain.CONFIG, "static tiles read failed: maps/tiles.json missing")
            }
        } catch (t: Throwable) {
            LogFile.error(LogDomain.CONFIG, "load static tiles failed", t)
        }
    }
}

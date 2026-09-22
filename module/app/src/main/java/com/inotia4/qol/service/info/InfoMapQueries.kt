package com.inotia4.qol.service.info

import com.inotia4.qol.NativeBridge
import com.inotia4.qol.util.JsonUtil

/** Map query endpoints. Keeps map data lookup separate from the service facade. */
internal object InfoMapQueries {
    fun id(): String = dispatch("/api/world/map/id")
    fun exits(): String = dispatch("/api/world/map/exits")
    fun units(): String = dispatch("/api/world/map/units")
    fun enemies(): String = dispatch("/api/world/map/enemies")
    fun interactives(): String = dispatch("/api/world/map/interactives")
    fun drops(): String = dispatch("/api/world/map/drops")
    fun distance(tx: Int, ty: Int): String = dispatch("/api/world/map/distance", "tx=$tx&ty=$ty")

    private fun dispatch(path: String, query: String? = null): String {
        val envelope = JsonUtil.parseObj(NativeBridge.nativeHttpDispatch(0, path, query, null))
            ?: return JsonUtil.NOT_READY
        return envelope.optString("body", JsonUtil.NOT_READY)
    }
}

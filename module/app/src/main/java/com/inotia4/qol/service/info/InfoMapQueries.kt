package com.inotia4.qol.service.info

/** Map query endpoints. Keeps map data lookup separate from the service facade. */
internal object InfoMapQueries {
    fun id(): String = NativeHttpQuery.get("/api/world/map/id")
    fun exits(): String = NativeHttpQuery.get("/api/world/map/exits")
    fun units(): String = NativeHttpQuery.get("/api/world/map/units")
    fun enemies(): String = NativeHttpQuery.get("/api/world/map/enemies")
    fun interactives(): String = NativeHttpQuery.get("/api/world/map/interactives")
    fun drops(): String = NativeHttpQuery.get("/api/world/map/drops")
    fun distance(tx: Int, ty: Int): String =
        NativeHttpQuery.get("/api/world/map/distance", "tx=$tx&ty=$ty")
}

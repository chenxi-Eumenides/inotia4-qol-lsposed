package com.inotia4.qol.service.info

/**
 * item 域查询端点（inventory + shop GET）：owner = item_facts。
 * Kotlin 只构造路径与查询串，响应体（含名称注入/字段重排）完全由 native owner 生成。
 */
internal object InfoItemQueries {
    fun inventory(): String = NativeHttpQuery.get("/api/item/inventory")
    fun money(): String = NativeHttpQuery.get("/api/item/inventory/money")
    fun items(): String = NativeHttpQuery.get("/api/item/inventory/items")
    fun bagInfo(bag: Int): String = NativeHttpQuery.get("/api/item/inventory/bag/$bag/info")
    fun bagSlot(bag: Int, slot: Int): String = NativeHttpQuery.get("/api/item/inventory/bag/$bag/$slot")
    fun shopItems(): String = NativeHttpQuery.get("/api/item/shop/items")
}

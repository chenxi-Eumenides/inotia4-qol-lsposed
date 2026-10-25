package com.inotia4.qol.service.info

/**
 * item 域查询端点（inventory + shop GET）：owner = core.item。
 * Kotlin 只构造路径与查询串，响应体（含名称注入/字段重排/分页游标）完全由 native owner 生成。
 *
 * 袋编号空间：space 省略时 bag 按扁平袋号解析（原版→改版→注册源顺序），
 * 给出 space 时 bag 按该空间内索引解析。分页游标对调用方不透明。
 */
internal object InfoItemQueries {
    private fun encode(value: String): String = java.net.URLEncoder.encode(value, "UTF-8")

    private fun query(vararg parts: Pair<String, String?>): String? {
        val encoded = parts.mapNotNull { (key, value) ->
            value?.let { "$key=${encode(it)}" }
        }
        return if (encoded.isEmpty()) null else encoded.joinToString("&")
    }

    fun inventory(space: Int?, cursor: String?, limit: Int?): String =
        NativeHttpQuery.get(
            "/api/item/inventory",
            query("space" to space?.toString(), "cursor" to cursor, "limit" to limit?.toString()),
        )

    fun money(): String = NativeHttpQuery.get("/api/item/inventory/money")

    fun items(space: Int?, cursor: String?, limit: Int?): String =
        NativeHttpQuery.get(
            "/api/item/inventory/items",
            query("space" to space?.toString(), "cursor" to cursor, "limit" to limit?.toString()),
        )

    fun bagInfo(bag: Int, space: Int?): String =
        NativeHttpQuery.get(
            "/api/item/inventory/bag/$bag/info",
            query("space" to space?.toString()),
        )

    fun bagSlot(bag: Int, slot: Int, space: Int?): String =
        NativeHttpQuery.get(
            "/api/item/inventory/bag/$bag/$slot",
            query("space" to space?.toString()),
        )

    fun shopItems(): String = NativeHttpQuery.get("/api/item/shop/items")
}

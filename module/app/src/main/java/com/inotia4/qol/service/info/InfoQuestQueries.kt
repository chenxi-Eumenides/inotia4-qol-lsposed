package com.inotia4.qol.service.info

/**
 * quest 域查询端点：owner = quest。
 * Kotlin 只构造路径，响应体（静态字段注入与目标数组拼装）完全由 native owner 生成。
 */
internal object InfoQuestQueries {
    fun quest(): String = NativeHttpQuery.get("/api/quest")
    fun active(): String = NativeHttpQuery.get("/api/quest/active")
    fun details(): String = NativeHttpQuery.get("/api/quest/details")
    fun completed(): String = NativeHttpQuery.get("/api/quest/completed")
}

package com.inotia4.qol.util

import org.json.JSONArray
import org.json.JSONObject

/**
 * 通用 JSON 工具：解析容错 + 错误响应构造。
 * controller/service 层复用的公共函数，避免各处重复 try-catch 与手写错误串。
 *
 * v0.5.45（P1）：错误响应统一格式 A `{"ok":false,"error":"..."}`，常量同步升级；
 * [err] 工厂统一构造错误串（controller 侧配合 [ApiException] 设置 HTTP 状态码）；
 * [parseBody] 成为唯一 body 解析入口（替代各 controller 私有拷贝）。
 */
object JsonUtil {

    const val NOT_FOUND = """{"ok":false,"error":"not found"}"""
    const val NOT_READY = """{"ok":false,"error":"not ready"}"""
    const val BAD_REQUEST = """{"ok":false,"error":"bad request"}"""

    /** 构造错误响应（格式 A）：{"ok":false,"error":"$msg"}；code 用于 controller 侧设置 HTTP 状态码场景 */
    fun err(msg: String, code: Int = 400): String =
        """{"ok":false,"error":"${msg.replace("\"", "\\\"")}"}"""

    /** 公共 body 解析入口：非法/空 body 返回 null（不抛异常），替代各 controller 私有 parseBody 拷贝 */
    fun parseBody(body: String?): JSONObject? = try {
        if (body.isNullOrBlank()) null else JSONObject(body)
    } catch (e: Exception) {
        null
    }

    /** 解析对象，失败返回 null（不抛异常） */
    fun parseObj(json: String?): JSONObject? = try {
        if (json.isNullOrBlank()) null else JSONObject(json)
    } catch (e: Exception) {
        null
    }

    /** 解析数组，失败返回 null（不抛异常） */
    fun parseArr(json: String?): JSONArray? = try {
        if (json.isNullOrBlank()) null else JSONArray(json)
    } catch (e: Exception) {
        null
    }

    /** 构造 {"key": value} 简单对象 */
    fun wrap(key: String, value: Any?): String =
        JSONObject().put(key, value ?: JSONObject.NULL).toString()

    /** 构造 {"key": value, ...} 多字段对象 */
    fun wrap(vararg pairs: Pair<String, Any?>): String {
        val o = JSONObject()
        for ((k, v) in pairs) o.put(k, v ?: JSONObject.NULL)
        return o.toString()
    }
}

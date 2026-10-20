package com.inotia4.qol.api.controller

import com.inotia4.qol.NativeBridge
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.JsonUtil
import com.inotia4.qol.util.withNativeRouteLease
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.HttpRequest
import com.yanzhenjie.andserver.http.StatusCode
import org.json.JSONArray
import org.json.JSONObject

/**
 * 自动出售 feature API（开发期入口，见 auto-sell.md）。
 *
 * - GET  /api/feature/autosell/config  读取当前存档的配置与运行状态
 * - POST /api/feature/autosell/config  设置当前存档配置（写运行时 + 直写 sidecar）
 *
 * 规则「值即开关」：0=关闭；正整数为 1-based 档位
 * （rarity 1..5 / enhance 1..32 / socket 1..16 / gemTier 1..5 / gemRange 1..5）。
 * special 为特殊类型名数组（合法名 backpack / normalSeal / dice；字段类型与元素严格校验）。
 * 请求体示例：{"enabled":true,"rarity":1,"enhance":0,"socket":0,"gemTier":0,"special":["dice"],"gemRange":0}
 *
 * 两条路由由 Kotlin 直接处理（无 native handler），按 direct 路由获取 native route lease。
 */
@RestController
class AutoSellFeatureController {

    @GetMapping("/api/feature/autosell/config")
    fun getConfig(request: HttpRequest): String =
        withNativeRouteLease(request) { NativeBridge.nativeAutoSellStatusJson() }

    @PostMapping("/api/feature/autosell/config")
    fun setConfig(@RequestBody body: String, request: HttpRequest): String =
        withNativeRouteLease(request) {
            val json = JsonUtil.parseObj(body)
                ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
            val enabled = if (json.has("enabled")) {
                json.get("enabled") as? Boolean
                    ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "enabled must be boolean")
            } else {
                false
            }
            val rarity = readInteger(json, "rarity", 0, 5)
            val enhance = readInteger(json, "enhance", 0, 32)
            val socket = readInteger(json, "socket", 0, 16)
            val gemTier = readInteger(json, "gemTier", 0, 5)
            val gemRange = readInteger(json, "gemRange", 0, 5)
            // special 数组文本原样传给 native，名字->位换算在 native 唯一表完成。
            val special = if (json.has("special")) {
                val array = json.get("special") as? JSONArray
                    ?: throw ApiException(
                        StatusCode.SC_BAD_REQUEST,
                        "special must be array of backpack|normalSeal|dice"
                    )
                for (index in 0 until array.length()) {
                    val value = array.get(index)
                    if (value !is String || value !in SPECIAL_TYPES) {
                        throw ApiException(
                            StatusCode.SC_BAD_REQUEST,
                            "special must be array of backpack|normalSeal|dice"
                        )
                    }
                }
                array.toString()
            } else {
                "[]"
            }
            val accepted = NativeBridge.nativeSetAutoSellConfig(
                enabled,
                rarity,
                enhance,
                socket,
                gemTier,
                special,
                gemRange
            )
            if (!accepted) {
                throw ApiException(409, "config rejected (no active save session)")
            }
            NativeBridge.nativeAutoSellStatusJson()
        }

    private fun readInteger(json: JSONObject, key: String, min: Int, max: Int): Int {
        if (!json.has(key)) return 0
        val error = "$key must be integer in $min-$max"
        val number = json.get(key) as? Number ?: throw ApiException(StatusCode.SC_BAD_REQUEST, error)
        val integer = try {
            number.toString().toBigDecimal()
        } catch (_: NumberFormatException) {
            throw ApiException(StatusCode.SC_BAD_REQUEST, error)
        }
        if (integer.stripTrailingZeros().scale() > 0 ||
            integer < min.toBigDecimal() || integer > max.toBigDecimal()
        ) {
            throw ApiException(StatusCode.SC_BAD_REQUEST, error)
        }
        return integer.toInt()
    }

    private companion object {
        val SPECIAL_TYPES = setOf("backpack", "normalSeal", "dice")
    }
}

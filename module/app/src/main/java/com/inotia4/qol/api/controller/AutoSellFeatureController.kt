package com.inotia4.qol.api.controller

import com.inotia4.qol.NativeBridge
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.StatusCode

/**
 * 自动出售 feature API（开发期入口，见 auto-sell.md）。
 *
 * - GET  /api/feature/autosell/config  读取当前存档的配置与运行状态
 * - POST /api/feature/autosell/config  设置当前存档配置（写运行时 + 直写 sidecar）
 *
 * 规则「值即开关」：0=关闭；正整数为 1-based 档位
 * （rarity 1..5 / enhance 1..32 / socket 1..16 / gemTier 1..5 / gemRange 1..5）。
 * special 为特殊类型名数组（合法名 backpack / normalSeal / dice；缺省或坏值按空集）。
 * 请求体示例：{"enabled":true,"rarity":1,"enhance":0,"socket":0,"gemTier":0,"special":["dice"],"gemRange":0}
 */
@RestController
class AutoSellFeatureController {

    @GetMapping("/api/feature/autosell/config")
    fun getConfig(): String = NativeBridge.nativeAutoSellStatusJson()

    @PostMapping("/api/feature/autosell/config")
    fun setConfig(@RequestBody body: String): String {
        val json = JsonUtil.parseObj(body)
            ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        // special：JSON 数组文本原样传给 native（缺省/非数组按空集 []），名字->位换算在 native 唯一表完成。
        val special = json.optJSONArray("special")?.toString() ?: "[]"
        NativeBridge.nativeSetAutoSellConfig(
            json.optBoolean("enabled", false),
            json.optInt("rarity", 0),
            json.optInt("enhance", 0),
            json.optInt("socket", 0),
            json.optInt("gemTier", 0),
            special,
            json.optInt("gemRange", 0)
        )
        return NativeBridge.nativeAutoSellStatusJson()
    }
}

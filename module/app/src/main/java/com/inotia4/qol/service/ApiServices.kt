package com.inotia4.qol.service

import com.inotia4.qol.service.action.ActionApiServiceImpl
import com.inotia4.qol.service.info.InfoApiServiceImpl

/**
 * 服务注册中心（v0.4.0，P0-3 重构）：controller/调用层从这里获取 Service 实例。
 * 多调用通道预留：未来 Binder/LocalSocket 调用方同样从这里取服务，不依赖 HTTP。
 */
object ApiServices {

    val info: InfoApiService by lazy { InfoApiServiceImpl() }

    val action: ActionApiService by lazy { ActionApiServiceImpl() }

    val op: OpApiService by lazy { OpApiServiceImpl() }

    val config: ConfigApiService by lazy { ConfigApiServiceImpl() }
}

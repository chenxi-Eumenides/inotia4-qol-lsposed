package com.inotia4.qol

/**
 * 构建期包名列表是 LSPosed 的推荐作用域，也是框架上报宿主包名之前的回退值。
 *
 * 运行时不按该列表过滤：改包名的游戏变体 applicationId 会变化，但 Java 类与资源表
 * namespace 保持原样，宿主是否为目标游戏由 [HostIdentity] 判定；存储路径和 PackageManager
 * 查询一律使用框架上报的实际包名。
 */
object TargetPackages {
    private val configuredPackages: Set<String> = BuildConfig.TARGET_PACKAGES
        .split(',')
        .map(String::trim)
        .filter(String::isNotEmpty)
        .toSet()

    @Volatile
    private var activePackage: String? = null

    fun isConfigured(packageName: String): Boolean = configuredPackages.contains(packageName)

    fun activate(packageName: String) {
        if (packageName.isNotEmpty()) activePackage = packageName
    }

    fun current(): String = activePackage ?: configuredPackages.first()

    fun externalFilesPath(fileName: String): String =
        "/sdcard/Android/data/${current()}/files/$fileName"

    fun qolFilesPath(fileName: String): String =
        "/sdcard/Android/data/${current()}/files/qol/$fileName"
}

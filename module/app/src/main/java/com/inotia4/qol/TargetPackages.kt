package com.inotia4.qol

/** Build-time target package list shared by LSPosed scope and runtime guards. */
object TargetPackages {
    private val configuredPackages: Set<String> = BuildConfig.TARGET_PACKAGES
        .split(',')
        .map(String::trim)
        .filter(String::isNotEmpty)
        .toSet()

    @Volatile
    private var activePackage: String? = null

    fun contains(packageName: String): Boolean = configuredPackages.contains(packageName)

    fun activate(packageName: String) {
        if (contains(packageName)) activePackage = packageName
    }

    fun current(): String = activePackage ?: configuredPackages.first()

    fun externalFilesPath(fileName: String): String =
        "/sdcard/Android/data/${current()}/files/$fileName"
}

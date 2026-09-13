plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.kapt")
}

val defaultTargetPackages =
    "com.com2us.inotia4.normal.freefull.google.global.android.common,com.com2us.inotia4.qol.patched"
val targetPackages = providers.gradleProperty("targetPackages").orElse(defaultTargetPackages)
    .map { raw ->
        raw.split(',')
            .map(String::trim)
            .filter(String::isNotEmpty)
            .distinct()
            .also { packages ->
                require(packages.isNotEmpty()) { "targetPackages must contain at least one package" }
                packages.forEach { packageName ->
                    require(packageName.matches(Regex("[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z][A-Za-z0-9_]*)+"))) {
                        "Invalid Android package name: $packageName"
                    }
                }
            }
            .joinToString(",")
    }

val generateXposedScope = tasks.register("generateXposedScope") {
    val scopeFile = layout.buildDirectory.file("generated/xposed-scope/META-INF/xposed/scope.list")
    inputs.property("targetPackages", targetPackages)
    outputs.file(scopeFile)
    doLast {
        val packages = targetPackages.get().split(',')
        scopeFile.get().asFile.apply {
            parentFile.mkdirs()
            writeText(packages.joinToString("\n") + "\n")
        }
    }
}

tasks.configureEach {
    if (name.matches(Regex("process.*JavaRes"))) {
        dependsOn(generateXposedScope)
    }
}

apply(plugin = "com.yanzhenjie.andserver")

android {
    namespace = "com.inotia4.qol"
    compileSdk = 34
    buildToolsVersion = "37.0.0"
    ndkVersion = "26.3.11579264"

    defaultConfig {
        applicationId = "com.inotia4.qol"
        minSdk = 30
        targetSdk = 34
    versionCode = 184
    versionName = "0.7.5"

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
            }
        }
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }

        buildConfigField("String", "TARGET_PACKAGES", "\"${targetPackages.get()}\"")
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

        buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }

    }
}

android.sourceSets["main"].resources.srcDir(layout.buildDirectory.dir("generated/xposed-scope"))

dependencies {
    // LSPosed 现代 Xposed API（compileOnly：由框架提供，不打进 APK）
    compileOnly("io.github.libxposed:api:101.0.1")
    // AndServer 2.x：进程内嵌入式 HTTP 服务器（api + 注解处理器）
    implementation("com.yanzhenjie.andserver:api:2.1.12")
    implementation("com.yanzhenjie.andserver:annotation:2.1.12")
    kapt("com.yanzhenjie.andserver:processor:2.1.12")
}

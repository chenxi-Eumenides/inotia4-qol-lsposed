plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.kapt")
}

apply(plugin = "com.yanzhenjie.andserver")

android {
    namespace = "com.inotia4.export"
    compileSdk = 34
    buildToolsVersion = "37.0.0"
    ndkVersion = "26.3.11579264"

    defaultConfig {
        applicationId = "com.inotia4.export"
        minSdk = 30
        targetSdk = 34
    versionCode = 176
    versionName = "0.6.18"

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
            }
        }
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }
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

dependencies {
    // LSPosed 现代 Xposed API（compileOnly：由框架提供，不打进 APK）
    compileOnly("io.github.libxposed:api:101.0.1")
    // AndServer 2.x：进程内嵌入式 HTTP 服务器（api + 注解处理器）
    implementation("com.yanzhenjie.andserver:api:2.1.12")
    implementation("com.yanzhenjie.andserver:annotation:2.1.12")
    kapt("com.yanzhenjie.andserver:processor:2.1.12")
}

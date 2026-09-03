# Android 工程规则

- 构建优先使用项目根目录的 `scripts/build-debug.sh`，它会自动发现 `.gradle/wrapper/dists/` 中的 Gradle 8.11.1 缓存。
- 正式版使用项目根目录的 `scripts/build-release.sh`；它执行 `assembleRelease`，读取 `versionName`，并将当前未配置签名的 Release APK 复制为 `output/inotia4-qol-lsposed-v<version>-release-unsigned.apk`。
- Debug 构建会将 APK 复制为 `output/inotia4-qol-lsposed-debug-<sha256前12位>.apk`，并只保留最新 3 份 Debug APK；Release 构建只保留最新 2 份 Release APK。
- 禁止直接执行 `./gradlew`、系统 `gradle` 或手写 Gradle 二进制路径；需要传递 Gradle 参数时追加到对应构建脚本末尾。
- 不要根据 `./gradlew` 查找 `tools/gradle-8.11.1-bin.zip` 的失败结果判断 Gradle 缺失；当前 Wrapper 的本地压缩包地址不是缓存目录。
- Gradle 缓存使用项目根目录 `.gradle/`，不要移动到 `tools/`，不要将缓存或构建产物加入源码变更。
- 构建产物位于 `module/app/build/`；交付 APK 复制到项目根目录 `output/`。
- 临时构建日志、诊断输出和一次性文件写入项目根目录 `.tmp/<task-name>/`，任务结束后清理；不要写入 `scripts/`、`tools/` 或 `.gradle/`。
- 修改 Kotlin、Native 或构建配置后，执行 `git diff --check`、相关测试和 Debug 构建。

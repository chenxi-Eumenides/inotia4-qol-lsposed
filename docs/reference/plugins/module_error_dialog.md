# 模块错误兜底提示

## 1. 作用与 owner 定位

模块错误提示是 Kotlin 平台层的兜底诊断 UI，不是可启停的业务插件。它将模块通过 Kotlin `LogFile.error` 明确记录的错误显示在 Android 原生可关闭窗口中，便于用户看到错误详情；不负责修复错误、改变插件状态或保证失败功能恢复。

此窗口独立于 QoL 水印。它也不同于 `module_health`：后者发布 Host 健康事实并投影固定类别，错误窗口显示的是 Kotlin 日志调用提供的错误文本。

## 2. 触发范围与错误内容

- 唯一提示入口是 Kotlin `LogFile.error(domain, message, throwable?)`。日志写入原流程后，错误详情进入提示队列。
- 详情包含 `domain`、原始 `message`，以及可选的 `throwable` 压缩栈摘要。没有 Throwable 时不显示栈字段。
- `warn`、`info`、`debug` 不触发窗口。Native/C++ `QOL_LOG_ERROR` 直接写入 native 日志，不会因此自动进入 Kotlin 错误提示队列。
- 因而，该功能提示的是经 Kotlin `LogFile.error` 上报的模块错误，不是对全部 native 错误日志或 `module_health` 位的通用转译。

## 3. 显示与队列行为

- `MainActivityStartupHook` 在 MainActivity 创建或获焦时提供 Activity；没有 Activity 时错误先留在内存队列，待 Activity 可用后再尝试显示。
- 使用 Android 原生 `AlertDialog`，详情文本可滚动、可选择；窗口有“关闭”按钮，也可取消关闭。每次显示一条，关闭后继续处理队列中的下一条。
- 队列容量为 8；队列满时丢弃最早的待显示错误。Activity 以弱引用持有，并在显示前检查是否已结束或销毁。
- UI 创建或显示失败不会递归产生错误窗口；失败原因只尝试以 UI 域 `warn` 写入日志。窗口错误不会替代原始 `LogFile.error` 日志。

## 4. 职责边界

- 这是兜底提示，不是错误恢复机制、崩溃拦截保证或插件健康状态 API。
- 不创建或管理 patch、hook、资源、插件生命周期、HTTP 路由或 capability；错误详情按 `LogFile.error` 调用方提供的文本展示。
- 对话框需要可用 Activity 才能显示；在进程无可用 Activity、Activity 窗口已分离或系统拒绝窗口操作时，错误仍由原日志路径保留，窗口可能跳过。

## 5. 文件地图

| 子职责 | 文件 | 职责 |
|---|---|---|
| 错误日志与详情格式 | `module/app/src/main/java/com/inotia4/qol/LogFile.kt` | 保留原日志出口，并将 Kotlin error 级日志格式化后排队 |
| 队列与原生窗口 | `module/app/src/main/java/com/inotia4/qol/ModuleErrorDialog.kt` | 有界队列、主线程调度、Activity 检查和可关闭 AlertDialog |
| Activity 提供入口 | `module/app/src/main/java/com/inotia4/qol/MainActivityStartupHook.kt` | Activity 创建/获焦时通知提示器 |
| 队列与格式测试 | `module/app/src/test/java/com/inotia4/qol/ModuleErrorDialogQueueTest.kt` | 覆盖队列上限、并发入队和详情格式 |

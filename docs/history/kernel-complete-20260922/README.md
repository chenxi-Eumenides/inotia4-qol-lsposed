# Kernel-complete 真机证据（2026-09-22）

设备：`192.168.3.54:5555`；游戏：monster v29；包名：
`com.com2us.inotia4.normal.freefull.google.global.android.common`。

## VM-K1-01：kernel-only Debug

```bash
scripts/build-debug.sh -PqolKernelOnly=true
scripts/patch-apk.sh output/inotia4_qol_lsposed_debug_2609220140_35e937afdebb.apk
adb -s 192.168.3.54:5555 install -r output/Inotia4_v1.3.2_monster_v29_npatched.apk
adb -s 192.168.3.54:5555 shell am force-stop com.com2us.inotia4.normal.freefull.google.global.android.common
adb -s 192.168.3.54:5555 shell monkey -p com.com2us.inotia4.normal.freefull.google.global.android.common -c android.intent.category.LAUNCHER 1
curl -s http://192.168.3.54:8088/api/health
adb -s 192.168.3.54:5555 logcat -d -v time | grep 'Inotia4Qol' > docs/history/kernel-complete-20260922/logcat-kernel-only.txt
```

实际输出：`/api/health` 为
`{"ok":true,"ready":true,"variant":{"series":"monster","version":"v29","known":true,"capabilities":3}}`。
`logcat-kernel-only.txt` 包含
`assembly: registered=0 install_failed=0 kernel_only=true` 与业务 hook 跳过日志。
`kernel-only-agreement.png` 为原版协议页，`kernel-only-main-menu.png` 为同意后的原版主菜单。

## VM-K1-02：Debug device probe

```bash
scripts/build-debug.sh -PqolKernelOnly=true -PqolDeviceProbe=true
scripts/patch-apk.sh output/inotia4_qol_lsposed_debug_2609220141_5b3e73cd61f0.apk
adb -s 192.168.3.54:5555 install -r output/Inotia4_v1.3.2_monster_v29_npatched.apk
adb -s 192.168.3.54:5555 shell am force-stop com.com2us.inotia4.normal.freefull.google.global.android.common
adb -s 192.168.3.54:5555 shell monkey -p com.com2us.inotia4.normal.freefull.google.global.android.common -c android.intent.category.LAUNCHER 1
curl -s http://192.168.3.54:8088/api/health
adb -s 192.168.3.54:5555 logcat -d -v time | grep 'Inotia4Qol' > docs/history/kernel-complete-20260922/logcat-device-probe.txt
```

实际输出：`/api/health` 同为 `ok=true`、`ready=true`。
`logcat-device-probe.txt` 包含 provider 的 capability published、consumer 的 capability injected/dependency resolved，以及
`assembly: registered=2 install_failed=0 kernel_only=true`。

本轮未执行 release 构建；因此上述证据不宣称 release 隔离通过。

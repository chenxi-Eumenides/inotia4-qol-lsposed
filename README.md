# 艾诺迪亚4 QOL 改动 LSPosed 模块

《艾诺迪亚4》（Inotia 4）的 LSPosed 模块，提供多项体验优化（QOL）改动，并把游戏运行时数据与操作通过 HTTP API 导出，供外部程序或 AI 读取与操控。

游戏本体零修改：模块以 Xposed 形式注入；另提供免 root 的 NPatch 集成包。

## 功能

- **扩展背包**：在原版背包之外新增 5 个扩展背包，支持拖放移动、同类合并，并与商店、合成器互通。
- **堆叠上限提升**：可堆叠物品上限由 99 提升至 999。
- **商店页扩展背包**：购买、出售时可使用扩展背包内的物品。
- **合成器页扩展背包**：合成选材与产物入库支持扩展背包物品。
- **数据导出 API**：运行时状态（血量、金币、等级、坐标、背包装备、技能、佣兵等）与静态数据（数值表、道具、地图等）可通过 HTTP API 读取。
- **操作 API**：移动、使用道具、装备、队伍等游戏内操作可通过 API 触发。

## 安装

### 方式一：LSPosed 模块（需 root）

1. 从 Releases 下载模块 APK（`inotia4-qol-lsposed-vX.Y.Z-*.apk`）。
2. 安装后在 LSPosed 中启用模块，作用域勾选游戏包名
   `com.com2us.inotia4.normal.freefull.google.global.android.common`。
3. 重启游戏。

### 方式二：免 root 集成包（NPatch）

1. 从 Releases 下载与你的游戏版本对应的 `*-npatched.apk`。
2. 安装（与原版同签名可覆盖；不同签名需先卸载原版，请注意备份存档）。
3. 启动游戏即可。

## 使用

- 模块启动后在本机 `http://<设备IP>:8088` 提供 HTTP API（局域网内可访问）。
- 端点、请求/返回格式与玩法示例：
  - `docs/reference/api-reference.md` —— API 规格
  - `docs/guides/game-guide.md` —— 游玩与 API 操作手册

## 兼容版本

| 游戏版本 | 状态 |
|---|---|
| 大修 20260830 | ✅ 已验证 |
| monster v23 | ✅ 已验证 |
| 原版 v1.3.2 | 🔶 集成包已提供，未验证 |

## 下载

GitHub Releases：<https://github.com/chenxi-Eumenides/inotia4-qol-lsposed/releases>

## 已知限制

- 仅支持 arm64-v8a 设备。
- 免 root 集成包在更换模块版本后需重新打包。
- API 为明文 HTTP，仅建议在可信局域网内使用。

## 开发者

- 代码架构与规范：`docs/development/architecture.md`
- 构建、设备与部署：`docs/guides/build-and-deploy.md`
- 开发待办：`docs/development/planning/backlog.md`
- 文档索引：`docs/INDEX.md`
- 贡献与代理工作规则：`AGENTS.md`

## 致谢

- 感谢 QQ 群友 **苦尽苦来** 授权的大修版。
- 感谢 QQ 群友 **历战王大黄猫** 授权的 monster 改版。

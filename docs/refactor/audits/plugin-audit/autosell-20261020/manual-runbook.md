# `autosell` 阶段 5 人工目视卡

前置：设备 `192.168.3.56:5555`、monster v29、模块 sha256 `108a3d1e50bdcf405918cb871b024f2aa6bf54be21887e2afd32d0a9e460a37a`。以下命令在项目根目录执行。

## 卡 AS-M1：背包入口按钮与配置面板

**目的**：确认入口按钮可见、面板可打开、文案/规则/特殊类型与布局正常、关闭后配置提交不崩溃。

**准备（约 1 分钟）**：

```bash
# 1. 开启功能（若已开启可跳过）
curl -s -X POST http://192.168.3.56:8088/api/config/set \
  -H 'Content-Type: application/json' \
  -d '{"plugin_id":"autosell","field":"feature","key":"autosell.auto_sell","value":true}'

# 2. 确保存档配置关闭出售（只观察 UI，不触发自动出售）
curl -s -X POST http://192.168.3.56:8088/api/feature/autosell/config \
  -H 'Content-Type: application/json' \
  -d '{"enabled":false,"rarity":1,"enhance":1,"socket":1,"gemTier":0,"gemRange":0,"special":[]}'

# 3. 确认世界态
curl -s http://192.168.3.56:8088/api/ui/screen   # 期望 {"screen":"world"}
```

若非 world：先在游戏主菜单进档，或在 API 中执行 `POST /api/system/enter_slot {"slot":0}`（必要时先 `POST /api/ui/dialog/select {"action":"ok"}` 关同意页）。

**操作**：

1. 在游戏中打开背包/装备页（原版袋列表）。
2. 观察袋列表右侧行 5 是否出现齿轮入口按钮（57×57，`unit 0x16 / loc 0`）。
3. 点击齿轮，打开「自动出售」面板。
4. 核对：
   - 第一行「自动出售」+ 总开关；
   - 说明文案为「按规则定时出售物品。已强化或已镶嵌物品不会被出售。」且不截断；
   - 5 行规则：装备品质 / 装备强化耐久度 / 装备总孔数 / 宝石档位 / 宝石属性范围；
   - 3 个特殊类型 chip：背包 / 普通徽章 / 骰子；
   - 底部「保存并关闭」按钮。
5. 切换任意规则档位与特殊类型选中态，点「保存并关闭」，确认面板关闭、游戏不崩溃；再次打开核对选择已保留。

**预期**：入口按钮可见且可点击；面板无重叠/超框；文案准确；关闭后配置持久化；无 FATAL/SIGSEGV、无控件空指针日志。

**取证**：

```bash
adb -s 192.168.3.56:5555 exec-out screencap -p > .tmp/autosell-audit/manual-panel.png
adb -s 192.168.3.56:5555 logcat -d -t 800 -s Inotia4Qol:V | grep -iE "autosell|FATAL|SIGSEGV"
```

**失败回滚**：

```bash
# 关闭功能
curl -s -X POST http://192.168.3.56:8088/api/config/set \
  -H 'Content-Type: application/json' \
  -d '{"plugin_id":"autosell","field":"feature","key":"autosell.auto_sell","value":false}'
# 如需恢复存档（测试前备份）
curl -s -X POST http://192.168.3.56:8088/api/system/backup/import \
  -H 'Content-Type: application/json' \
  -d '{"checksum":"0b7136443c74","slot":0}'
```

预计 3–5 分钟。结果请回复「AS-M1 通过」或附现象/截图路径。

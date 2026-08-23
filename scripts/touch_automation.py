#!/usr/bin/env python3
"""adb 触摸自动化：注入点击序列（执行模式）+ 实时检测触摸（检测模式）。

用法示例：
  # 执行模式：注入一串操作，每操作 = <click|down|up> <x,y> <间隔秒>，可无限拼接
  uv run python scripts/touch_automation.py click 100,200 0.5 down 300,400 0.2 up 300,400 0.1
  uv run python scripts/touch_automation.py wait 2.0 click 500,500 0.3

  # 检测模式：不传参数，实时打印屏幕上的每个触摸动作（时刻/位置/间隔）
  uv run python scripts/touch_automation.py

操作类型：
  click x,y  按下并抬起（按下保持 --hold 秒，默认 0.05）
  down  x,y  只按下，配合后续 up 做长按
  up    x,y  抬起
  wait  秒   纯等待
间隔秒 = 该操作执行完后到下一操作的等待时间。

坐标空间：输入与检测输出的坐标统一为当前方向的应用窗口坐标（如横屏 3168x1440），
脚本自动读取系统显示旋转（mRotation）与窗口尺寸，将触摸屏 raw 原生坐标
（如 23040x50688）双向换算，横竖屏无需任何手动参数。

注入方式（自动选择，时序最精确优先）：
  sendevent-su : su -c sendevent 直写触摸屏节点（root 设备，毫秒级）
  sendevent    : shell 直写（SELinux 放行的设备）
  input        : input tap / motionevent（无 root 回退）
"""
from __future__ import annotations

import argparse
import datetime as _dt
import re
import signal
import subprocess
import sys
import time

ADB = "adb"

# Linux input event codes（sendevent 参数: type code value）
EV_SYN = 0
SYN_REPORT = 0
EV_KEY = 1
BTN_TOUCH = 330
EV_ABS = 3
ABS_MT_SLOT = 47
ABS_MT_TRACKING_ID = 57
ABS_MT_POSITION_X = 53
ABS_MT_POSITION_Y = 54
TRACKING_ID_UP = 4294967295  # 0xffffffff 表示抬起

DEFAULT_HOLD = 0.05  # click 按下保持秒数

VALID_EVENTS = ("click", "down", "up", "wait")

# getevent 单行: [ 1234.567890] EV_ABS       ABS_MT_POSITION_X   000001f4
GEVENT_LINE = re.compile(
    r"\[\s*(\d+)\.(\d+)\]\s+(?:/dev/input/event\d+:\s+)?EV_(\w+)\s+(\S+)\s+(\S+)\s*$"
)


# ---------------------------------------------------------------- adb 基础
def adb(*args: str, timeout: int = 30) -> subprocess.CompletedProcess:
    return subprocess.run([ADB, *args], capture_output=True, text=True, timeout=timeout)


def require_device() -> None:
    r = adb("devices")
    lines = [l for l in r.stdout.splitlines()[1:] if l.strip() and "device" in l]
    if not lines:
        sys.exit("错误: 未检测到 adb 设备，请先连接（adb connect <IP>）")


def detect_touch_device() -> str:
    """从 /proc/bus/input/devices 探测触摸屏事件节点（立即返回，不挂起）。"""
    r = adb("shell", "cat /proc/bus/input/devices")
    keywords = (
        "touchpanel", "touchscreen", "synaptics", "goodix", "ft5x", "fts", "msg",
        "mtk-tpd", "axs_ts",
    )
    fallback: str | None = None
    cur_name = ""
    for line in r.stdout.splitlines():
        line = line.strip()
        m = re.match(r'N: Name="([^"]+)"', line)
        if m:
            cur_name = m.group(1)
            continue
        m = re.match(r"H: Handlers=.*(event\d+)", line)
        if m:
            name = cur_name.lower()
            if any(k in name for k in keywords):
                return f"/dev/input/{m.group(1)}"
            if "touch" in name and fallback is None:
                fallback = f"/dev/input/{m.group(1)}"
    if fallback:
        return fallback
    r = adb("shell", "getevent", "-pl")
    blocks = re.split(r"(?=add device \d+: /dev/input/event\d+)", r.stdout)
    for block in blocks:
        device = re.search(r"add device \d+: (/dev/input/event\d+)", block)
        if device is None:
            continue
        has_multitouch_position = "ABS_MT_POSITION_X" in block and "ABS_MT_POSITION_Y" in block
        if has_multitouch_position and "INPUT_PROP_DIRECT" in block:
            return device.group(1)
    sys.exit("错误: 未找到触摸屏事件节点；请使用 --device /dev/input/eventN 指定")


def detect_touch_devices() -> list[str]:
    """探测所有具备多点坐标轴的触摸节点，避免同机双触摸驱动漏事件。"""
    r = adb("shell", "getevent", "-pl")
    devices: list[str] = []
    blocks = re.split(r"(?=add device \d+: /dev/input/event\d+)", r.stdout)
    for block in blocks:
        device = re.search(r"add device \d+: (/dev/input/event\d+)", block)
        if device is None:
            continue
        if "ABS_MT_POSITION_X" in block and "ABS_MT_POSITION_Y" in block and "INPUT_PROP_DIRECT" in block:
            devices.append(device.group(1))
    if not devices:
        sys.exit("错误: 未找到触摸屏事件节点；请使用 --device /dev/input/eventN 指定")
    return devices


def get_screen_size() -> tuple[int, int]:
    r = adb("shell", "wm size")
    m = re.search(r"(\d+)x(\d+)", r.stdout)
    if m:
        return int(m.group(1)), int(m.group(2))
    return 1440, 3168


def get_absinfo(dev: str) -> dict[int, tuple[int, int]]:
    """读取触摸屏 abs 范围 min/max（getevent -i），返回 {ABS 码: (min, max)}。"""
    r = adb("shell", "getevent", "-i", dev)
    info: dict[int, tuple[int, int]] = {}
    for line in r.stdout.splitlines():
        m = re.match(r"\s*([0-9a-fA-F]+)\s*:\s*value\s+\d+,\s*min\s+(-?\d+),\s*max\s+(-?\d+)", line)
        if m:
            info[int(m.group(1), 16)] = (int(m.group(2)), int(m.group(3)))
    return info


def get_rotation_and_bounds() -> tuple[int, int, int]:
    """读取当前显示旋转(0-3)与应用逻辑窗口尺寸。mRotation 0=竖屏,1=横屏90°。"""
    r = adb("shell", "dumpsys window displays")
    m = re.search(r"mRotation=(\d)", r.stdout)
    rotation = int(m.group(1)) if m else 0
    m = re.search(r"mAppBounds=Rect\(0, 0 - (\d+), (\d+)\)", r.stdout)
    if m:
        return rotation, int(m.group(1)), int(m.group(2))
    w, h = get_screen_size()
    return rotation, (h, w) if rotation in (1, 3) else (w, h)


class Calib:
    """raw(触摸屏原生) → 应用逻辑坐标：absinfo 归一化 → 按显示旋转变换 → 窗口缩放。

    旋转方向取自系统 mRotation，横竖屏自动适配，无需手动 flip。
    """

    def __init__(self, rotation: int, logical_w: int, logical_h: int,
                 absinfo: dict[int, tuple[int, int]]):
        xr = absinfo.get(ABS_MT_POSITION_X)
        yr = absinfo.get(ABS_MT_POSITION_Y)
        ok = bool(xr and yr and xr[1] > xr[0] and yr[1] > yr[0])
        self.xmax = xr[1] if ok else logical_w
        self.ymax = yr[1] if ok else logical_h
        self.rot = rotation % 4
        self.lw, self.lh = logical_w, logical_h

    def to_screen(self, rx: int, ry: int) -> tuple[int, int]:
        nx, ny = rx / self.xmax, ry / self.ymax
        if self.rot == 1:
            lx, ly = ny, 1 - nx
        elif self.rot == 2:
            lx, ly = 1 - nx, 1 - ny
        elif self.rot == 3:
            lx, ly = 1 - ny, nx
        else:
            lx, ly = nx, ny
        return round(lx * self.lw), round(ly * self.lh)

    def to_raw(self, sx: int, sy: int) -> tuple[int, int]:
        nx, ny = sx / self.lw, sy / self.lh
        if self.rot == 1:
            rx, ry = 1 - ny, nx
        elif self.rot == 2:
            rx, ry = 1 - nx, 1 - ny
        elif self.rot == 3:
            rx, ry = ny, 1 - nx
        else:
            rx, ry = nx, ny
        return round(rx * self.xmax), round(ry * self.ymax)


# ------------------------------------------------------------ 注入方式探测
def detect_inject_mode(dev: str) -> str:
    if adb("shell", f"su -c 'sendevent {dev} 0 0 0'").returncode == 0:
        return "sendevent-su"
    if adb("shell", f"sendevent {dev} 0 0 0").returncode == 0:
        return "sendevent"
    return "input"


# ------------------------------------------------------------ 事件帧构造
def frame_down(dev: str, x: int, y: int, tid: int) -> str:
    """按下帧: slot -> tracking_id -> 坐标 -> BTN_TOUCH DOWN -> 同步"""
    return "; ".join(
        f"sendevent {dev} {t} {c} {v}"
        for t, c, v in (
            (EV_ABS, ABS_MT_SLOT, 0),
            (EV_ABS, ABS_MT_TRACKING_ID, tid),
            (EV_ABS, ABS_MT_POSITION_X, x),
            (EV_ABS, ABS_MT_POSITION_Y, y),
            (EV_KEY, BTN_TOUCH, 1),
            (EV_SYN, SYN_REPORT, 0),
        )
    )


def frame_up(dev: str) -> str:
    """抬起帧: slot -> tracking_id=ffffffff -> BTN_TOUCH UP -> 同步"""
    return "; ".join(
        f"sendevent {dev} {t} {c} {v}"
        for t, c, v in (
            (EV_ABS, ABS_MT_SLOT, 0),
            (EV_ABS, ABS_MT_TRACKING_ID, TRACKING_ID_UP),
            (EV_KEY, BTN_TOUCH, 0),
            (EV_SYN, SYN_REPORT, 0),
        )
    )


def build_script(events: list[tuple[str, int, int, float]], dev: str,
                 mode: str, hold: float, calib: Calib) -> str:
    """把事件序列编译成一段 shell 脚本，一次 adb 连接内串行执行（时序精确）。"""
    parts: list[str] = []
    tid = 1
    for kind, x, y, delay in events:
        if kind == "wait":
            pass
        elif mode == "input":
            if kind == "click":
                parts.append(f"input tap {x} {y}")
            elif kind == "down":
                parts.append(f"input motionevent DOWN {x} {y}")
            elif kind == "up":
                parts.append(f"input motionevent UP {x} {y}")
        else:
            rx, ry = calib.to_raw(x, y)  # sendevent 需要 raw 坐标
            if kind == "click":
                parts.append(frame_down(dev, rx, ry, tid))
                tid += 1
                parts.append(f"sleep {hold}")
                parts.append(frame_up(dev))
            elif kind == "down":
                parts.append(frame_down(dev, rx, ry, tid))
                tid += 1
            elif kind == "up":
                parts.append(frame_up(dev))
        if delay > 0:
            parts.append(f"sleep {delay}")
    return "; ".join(parts)


def run_inject(script: str, mode: str, est_seconds: float) -> None:
    if mode == "sendevent-su":
        full = f"su -c '{script}'"
    else:
        full = script
    t0 = time.monotonic()
    r = adb("shell", full, timeout=int(est_seconds) + 60)
    elapsed = time.monotonic() - t0
    if r.returncode != 0 or r.stderr.strip():
        sys.exit(f"注入失败: {r.stderr.strip() or r.stdout.strip()}")
    print(f"完成，实际耗时 {elapsed:.2f}s")


# ------------------------------------------------------------ 执行模式
def parse_events(argv: list[str]) -> list[tuple[str, int, int, float]]:
    events: list[tuple[str, int, int, float]] = []
    i = 0
    while i < len(argv):
        kind = argv[i]
        if kind not in VALID_EVENTS:
            sys.exit(f"错误: 未知事件类型 '{kind}'，支持 {VALID_EVENTS}")
        if kind == "wait":
            if i + 1 >= len(argv):
                sys.exit("错误: wait 后缺少延迟秒数")
            events.append((kind, 0, 0, float(argv[i + 1])))
            i += 2
            continue
        if i + 2 >= len(argv):
            sys.exit(f"错误: '{kind}' 后缺少 <x,y> 或 <延迟秒>")
        m = re.fullmatch(r"(\d+),(\d+)", argv[i + 1])
        if not m:
            sys.exit(f"错误: 坐标格式应为 'x,y'，收到 '{argv[i + 1]}'")
        delay = float(argv[i + 2])
        if delay < 0:
            sys.exit("错误: 延迟秒数不能为负")
        events.append((kind, int(m.group(1)), int(m.group(2)), delay))
        i += 3
    return events


def run_mode(args: argparse.Namespace) -> None:
    require_device()
    dev = args.device or detect_touch_device()
    events = parse_events(args.events)
    mode = args.inject
    if mode == "auto":
        mode = detect_inject_mode(dev)
    rotation, lw, lh = get_rotation_and_bounds()
    calib = Calib(rotation, lw, lh, get_absinfo(dev))
    print(f"注入方式: {mode} | 触摸设备: {dev} | 窗口: {lw}x{lh} | 旋转: {rotation * 90}°")
    for kind, x, y, delay in events:
        if kind == "wait":
            print(f"  wait {delay}s")
            continue
        if not (0 <= x <= lw and 0 <= y <= lh):
            print(f"  ⚠️ 坐标 ({x},{y}) 超出窗口 {lw}x{lh}")
        extra = f" → {delay}s" if delay > 0 else ""
        print(f"  {kind:<5} ({x:>4},{y:>4}){extra}")

    est = sum(d for _, _, _, d in events) + sum(
        args.hold for k, _, _, _ in events if k == "click"
    )
    script = build_script(events, dev, mode, args.hold, calib)
    run_inject(script, mode, est)


# ------------------------------------------------------------ 检测模式
def record_mode(args: argparse.Namespace) -> None:
    require_device()
    devices = [args.device] if args.device else detect_touch_devices()
    dev = devices[0] if args.device else "all-touch-devices"
    rotation, lw, lh = get_rotation_and_bounds()
    calib = Calib(rotation, lw, lh, get_absinfo(devices[0]))
    print(f"检测模式：监听 {dev}（窗口 {lw}x{lh}，旋转 {rotation * 90}°），Ctrl+C 结束")
    signal.signal(signal.SIGTERM, lambda _s, _f: (_ for _ in ()).throw(KeyboardInterrupt()))
    remote_getevent = "exec getevent -lt" + (f" {devices[0]}" if args.device else "")
    proc = subprocess.Popen(
        [ADB, "shell", "su", "-c", remote_getevent],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        bufsize=1,
    )
    cur_x = cur_y = 0
    cur_tid: int | None = None   # 本帧内 tracking_id（None=未出现, UP=ffffffff）
    touched = False              # 当前是否有手指按下
    last_x = last_y = 0
    last_ts: float | None = None
    count = 0

    def emit(action: str, x: int, y: int, ts: float) -> None:
        nonlocal last_ts, count
        sx, sy = calib.to_screen(x, y)  # raw → 逻辑坐标
        dt = f"{(ts - last_ts) * 1000:+.0f}ms" if last_ts is not None else "+0ms"
        last_ts = ts
        wall = _dt.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        print(f"  {ts:>14.6f} {action:<5} ({sx:>4},{sy:>4})  {dt:>8}  {wall}", flush=True)
        count += 1

    try:
        for raw in proc.stdout:
            m = GEVENT_LINE.match(raw.rstrip())
            if not m:
                continue
            sec, usec, ev, code, val = m.groups()
            ts = float(sec) + float(usec) / 1e6
            if ev == "ABS":
                if code == "ABS_MT_TRACKING_ID":
                    cur_tid = int(val, 16)
                elif code == "ABS_MT_POSITION_X":
                    cur_x = int(val, 16)
                elif code == "ABS_MT_POSITION_Y":
                    cur_y = int(val, 16)
            elif ev == "SYN" and code == "SYN_REPORT":
                # 一帧结束：tracking_id 新出现 = DOWN，ffffffff = UP
                if cur_tid is not None:
                    if cur_tid == TRACKING_ID_UP:
                        if touched:
                            emit("UP", last_x, last_y, ts)
                            touched = False
                    elif not touched:
                        last_x, last_y = cur_x, cur_y
                        emit("DOWN", cur_x, cur_y, ts)
                        touched = True
                    cur_tid = None
    except KeyboardInterrupt:
        pass
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
    if proc.returncode not in (0, -signal.SIGTERM):
        sys.exit(f"监听异常退出（getevent 返回 {proc.returncode}）；请检查设备节点权限: {dev}")
    print(f"\n检测结束，共 {count} 个动作")


# ---------------------------------------------------------------- 入口
def main() -> None:
    p = argparse.ArgumentParser(
        description="adb 触摸自动化：注入点击序列 / 实时记录触摸",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "事件序列（执行模式）: <click|down|up> <x,y> <延迟秒> ... 或 wait <秒>\n"
            "例: click 100,200 0.5 down 300,400 0.2 up 300,400 0.1\n"
            "无事件参数 → 检测模式（记录真实触摸到文件）"
        ),
    )
    p.add_argument("--hold", type=float, default=DEFAULT_HOLD,
                   help=f"click 按下保持秒数（默认 {DEFAULT_HOLD}）")
    p.add_argument("--device", default=None, help="触摸设备节点（默认自动探测）")
    p.add_argument("--inject", choices=["auto", "sendevent-su", "sendevent", "input"],
                   default="auto", help="注入方式（默认 auto 自动选择最优）")
    p.add_argument("events", nargs="*", help="事件序列，见上")
    args = p.parse_args()

    if args.events:
        run_mode(args)
    else:
        record_mode(args)


if __name__ == "__main__":
    main()

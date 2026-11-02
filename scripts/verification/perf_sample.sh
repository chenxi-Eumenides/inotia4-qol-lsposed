#!/usr/bin/env bash
# 真机性能采样：simpleperf（CPU 热点/调用链）+ perfetto（sched/帧时间线）
# + SurfaceFlinger 帧间隔 + gfxinfo + 模块日志。
#
# 用法：
#   scripts/verification/perf_sample.sh -n <场景名> [-d 秒] [-s 设备序列号] [--wait]
#
# 输出：.tmp/perf-sampling/<时间戳>-<场景名>/
#   perf.data / perfetto.trace / report-*.txt / sf-latency.txt /
#   gfxinfo-framestats.txt / logcat.txt / qol.log / summary.md
#
# 设备要求：adb 已连接、shell 为 root（LSPosed/LSPatch 设备通常满足）、
#           游戏已启动（脚本只查找 pid，不负责启动游戏）。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
SIMPLEPERF_HOST="$ROOT_DIR/tools/ndk/android-ndk-r26d/simpleperf/bin/linux/x86_64/simpleperf"
DEFAULT_PKG="com.com2us.inotia4.normal.freefull.google.global.android.common"

SCENARIO=""
DURATION=20
SERIAL="${ANDROID_SERIAL:-}"
PKG="$DEFAULT_PKG"
WAIT_ENTER=0
DO_PERFETTO=1
DO_SIMPLEPERF=1

usage() {
    sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

选项：
  -n <name>    场景名（必填，用于输出目录）
  -d <sec>     采样秒数，默认 20
  -s <serial>  设备序列号；默认取 ANDROID_SERIAL 或唯一在线设备
  -p <pkg>     游戏包名，默认原版包
  --wait       开始前等待回车（先手动进入目标场景）
  --no-perfetto    跳过 perfetto
  --no-simpleperf  跳过 simpleperf
  -h, --help   显示帮助

推荐场景（每个场景单独跑一次，20-30 秒）：
  main_menu        主菜单静置（基线）
  world_idle       进入游戏后站在地图上不动
  world_move       持续移动/跑图
  bag_open         打开背包页（扩展背包）静置
  bag_scroll       背包页持续上下滚动
  bag_drag         背包页拖动物品
  store_open       打开商店页静置
  mix_open         打开合成页静置
  combat           战斗/连续攻击
  settings_open    打开模块设置页静置
  char_panel       打开角色属性面板静置
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -n) SCENARIO="$2"; shift 2 ;;
        -d) DURATION="$2"; shift 2 ;;
        -s) SERIAL="$2"; shift 2 ;;
        -p) PKG="$2"; shift 2 ;;
        --wait) WAIT_ENTER=1; shift ;;
        --no-perfetto) DO_PERFETTO=0; shift ;;
        --no-simpleperf) DO_SIMPLEPERF=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$SCENARIO" ]]; then
    echo "缺少 -n <场景名>" >&2
    usage >&2
    exit 2
fi
if ! [[ "$DURATION" =~ ^[0-9]+$ ]] || [[ "$DURATION" -lt 3 ]]; then
    echo "-d 必须是 >=3 的整数" >&2
    exit 2
fi

ADB=(adb)
if [[ -z "$SERIAL" ]]; then
    mapfile -t DEVICES < <(adb devices | awk 'NR>1 && $2=="device" {print $1}')
    if [[ ${#DEVICES[@]} -ne 1 ]]; then
        echo "在线设备数为 ${#DEVICES[@]}，请用 -s <序列号> 指定" >&2
        adb devices >&2
        exit 2
    fi
    SERIAL="${DEVICES[0]}"
fi
ADB=(adb -s "$SERIAL")

if ! "${ADB[@]}" get-state >/dev/null 2>&1; then
    echo "设备 $SERIAL 不可用" >&2
    exit 2
fi
if ! "${ADB[@]}" shell id 2>/dev/null | grep -q 'uid=0'; then
    echo "设备 shell 不是 root；simpleperf/perfetto 无法采样游戏进程" >&2
    exit 2
fi

if ! "${ADB[@]}" shell 'command -v simpleperf >/dev/null 2>&1'; then
    if [[ -x "$ROOT_DIR/tools/ndk/android-ndk-r26d/simpleperf/bin/android/arm64/simpleperf" ]]; then
        "${ADB[@]}" push "$ROOT_DIR/tools/ndk/android-ndk-r26d/simpleperf/bin/android/arm64/simpleperf" \
            /data/local/tmp/qol-simpleperf >/dev/null
        "${ADB[@]}" shell 'chmod 755 /data/local/tmp/qol-simpleperf'
        SIMPLEPERF_CMD=/data/local/tmp/qol-simpleperf
    else
        echo "设备无 simpleperf，且项目 NDK 中未找到 arm64 版本" >&2
        exit 2
    fi
else
    SIMPLEPERF_CMD=simpleperf
fi

PID="$("${ADB[@]}" shell pidof "$PKG" 2>/dev/null | tr -d '\r' | awk '{print $1}')"
if [[ -z "$PID" ]]; then
    echo "未找到 $PKG 进程；请先启动游戏，再重新运行本脚本" >&2
    exit 2
fi

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="$ROOT_DIR/.tmp/perf-sampling/${STAMP}-${SCENARIO}"
mkdir -p "$OUT_DIR"
REMOTE_PREFIX="/data/local/tmp/qol-perf-${STAMP}-${SCENARIO}"
REMOTE_DATA="${REMOTE_PREFIX}.data"
REMOTE_TRACE="/data/misc/perfetto-traces/qol-perf-${STAMP}-${SCENARIO}.perfetto-trace"

echo "设备: $SERIAL"
echo "游戏: $PKG (pid=$PID)"
echo "场景: $SCENARIO  时长: ${DURATION}s"
echo "输出: $OUT_DIR"

if [[ "$WAIT_ENTER" -eq 1 ]]; then
    echo
    echo "请先在设备上进入「$SCENARIO」场景并保持，然后按回车开始采样..."
    read -r _
fi

cleanup() {
    : # simpleperf/perfetto 均带 --duration，不主动 kill，避免误杀并行采样
}
trap cleanup EXIT

# 帧统计基线
"${ADB[@]}" shell "dumpsys gfxinfo $PKG reset" >/dev/null 2>&1 || true

SIMPLEPERF_PID=""

if [[ "$DO_PERFETTO" -eq 1 ]]; then
    cat > "$OUT_DIR/perfetto.cfg" <<EOF
buffers { size_kb: 65536 fill_policy: RING_BUFFER }
data_sources {
  config {
    name: "linux.ftrace"
    ftrace_config {
      ftrace_events: "sched/sched_switch"
      ftrace_events: "sched/sched_wakeup"
      ftrace_events: "power/cpu_frequency"
      ftrace_events: "power/cpu_idle"
      ftrace_events: "binder/binder_transaction"
      ftrace_events: "binder/binder_transaction_received"
      atrace_categories: "gfx"
      atrace_categories: "view"
      atrace_categories: "input"
      atrace_categories: "binder_driver"
      atrace_categories: "sched"
      atrace_apps: "$PKG"
      buffer_size_kb: 16384
      drain_period_ms: 250
    }
  }
}
data_sources { config { name: "linux.process_stats" process_stats_config { scan_all_processes_on_start: true } } }
duration_ms: $((DURATION * 1000))
EOF
    # 通过 stdin 传配置：perfetto 进程无法读取 adb push 到 /data/local/tmp 的 shell 文件
    "${ADB[@]}" shell "perfetto --txt -c - -o $REMOTE_TRACE --background" \
        < "$OUT_DIR/perfetto.cfg" >"$OUT_DIR/perfetto-start.log" 2>&1 \
        || echo "perfetto 启动失败（继续）" >&2
fi

if [[ "$DO_SIMPLEPERF" -eq 1 ]]; then
    "${ADB[@]}" shell "$SIMPLEPERF_CMD record -p $PID -g -f 1000 --duration $DURATION -o $REMOTE_DATA" \
        >"$OUT_DIR/simpleperf.log" 2>&1 &
    SIMPLEPERF_PID=$!
fi

# 采样期间不做任何设备操作，避免污染
sleep $((DURATION + 2))

if [[ -n "$SIMPLEPERF_PID" ]]; then
    wait "$SIMPLEPERF_PID" || echo "simpleperf 退出码非 0，见 simpleperf.log" >&2
fi

# 帧统计
"${ADB[@]}" shell "dumpsys gfxinfo $PKG framestats" > "$OUT_DIR/gfxinfo-framestats.txt" 2>&1 || true

LAYER="$("${ADB[@]}" shell "dumpsys SurfaceFlinger --list" 2>/dev/null | tr -d '\r' \
    | grep -F "$PKG" | grep -F 'SurfaceView' | grep -F '(BLAST)' | head -n1)"
if [[ -z "$LAYER" ]]; then
    LAYER="$("${ADB[@]}" shell "dumpsys SurfaceFlinger --list" 2>/dev/null | tr -d '\r' \
        | grep -F "$PKG" | grep -F 'SurfaceView' | head -n1)"
fi
if [[ -n "$LAYER" ]]; then
    "${ADB[@]}" shell "dumpsys SurfaceFlinger --latency '$LAYER'" > "$OUT_DIR/sf-latency.txt" 2>&1 || true
    printf '%s\n' "$LAYER" > "$OUT_DIR/sf-layer.txt"
else
    echo "未找到 SurfaceFlinger 图层，跳过帧间隔统计" >&2
fi

"${ADB[@]}" shell "dumpsys meminfo $PKG" > "$OUT_DIR/meminfo.txt" 2>&1 || true
"${ADB[@]}" shell "cat /proc/$PID/status" > "$OUT_DIR/proc-status.txt" 2>&1 || true
"${ADB[@]}" logcat -d -v threadtime --pid="$PID" > "$OUT_DIR/logcat.txt" 2>&1 || true
"${ADB[@]}" pull "/sdcard/Android/data/$PKG/files/qol/qol.log" "$OUT_DIR/qol.log" \
    >/dev/null 2>&1 || true

if [[ "$DO_SIMPLEPERF" -eq 1 ]]; then
    "${ADB[@]}" pull "$REMOTE_DATA" "$OUT_DIR/perf.data" >/dev/null 2>&1 || true
    if [[ -s "$OUT_DIR/perf.data" && -x "$SIMPLEPERF_HOST" ]]; then
        "$SIMPLEPERF_HOST" report -i "$OUT_DIR/perf.data" --sort dso,symbol \
            > "$OUT_DIR/report-flat.txt" 2>&1 || true
        "$SIMPLEPERF_HOST" report -i "$OUT_DIR/perf.data" -g --children --sort dso,symbol \
            > "$OUT_DIR/report-children.txt" 2>&1 || true
        "$SIMPLEPERF_HOST" report -i "$OUT_DIR/perf.data" -g --sort dso,symbol \
            > "$OUT_DIR/report-callgraph.txt" 2>&1 || true
    fi
fi

if [[ "$DO_PERFETTO" -eq 1 ]]; then
    "${ADB[@]}" pull "$REMOTE_TRACE" "$OUT_DIR/perfetto.trace" >/dev/null 2>&1 || true
fi

# 帧间隔摘要：SurfaceFlinger --latency 第 2 列是实际 present 时间(ns)
if [[ -s "$OUT_DIR/sf-latency.txt" ]]; then
    awk 'NR > 1 && $2 != "9223372036854775807" && $2 + 0 > 0 { if (prev > 0) { d = ($2 - prev) / 1000000.0; if (d > 0 && d < 1000) print d } prev = $2 + 0 }' \
        "$OUT_DIR/sf-latency.txt" | sort -n > "$OUT_DIR/frame-intervals.txt"
    awk '
    { v[++n] = $1; sum += $1 }
    END {
        if (n == 0) { print "no frames"; exit }
        over33 = 0; over50 = 0
        for (i = 1; i <= n; ++i) { if (v[i] > 33.34) over33++; if (v[i] > 50) over50++ }
        mean = sum / n
        printf "frames=%d mean=%.2fms fps(1/mean)=%.1f p50=%.2fms p90=%.2fms p99=%.2fms >33ms=%d(%.1f%%) >50ms=%d(%.1f%%)\n", \
            n, mean, 1000.0/mean, v[int(n*0.50)+1], v[int(n*0.90)+1], v[int(n*0.99)+1], \
            over33, 100.0*over33/n, over50, 100.0*over50/n
    }' "$OUT_DIR/frame-intervals.txt" > "$OUT_DIR/frame-summary.txt" 2>/dev/null || true
fi

{
    echo "# 性能采样：$SCENARIO"
    echo
    echo "- 时间：$STAMP"
    echo "- 设备：$SERIAL"
    echo "- 包名：$PKG（pid=$PID）"
    echo "- 时长：${DURATION}s"
    echo
    echo "## 帧间隔（SurfaceFlinger）"
    echo '```'
    cat "$OUT_DIR/frame-summary.txt" 2>/dev/null || echo "无数据"
    echo '```'
    echo
    echo "## simpleperf Top（模块内符号）"
    echo '```'
    grep -F 'libgamebridge.so' "$OUT_DIR/report-flat.txt" 2>/dev/null | head -25 || echo "无数据"
    echo '```'
    echo
    echo "## 文件"
    echo "- perf.data / report-flat.txt / report-children.txt / report-callgraph.txt"
    echo "- perfetto.trace（用 https://ui.perfetto.dev 打开）"
    echo "- sf-latency.txt / gfxinfo-framestats.txt / logcat.txt / qol.log"
} > "$OUT_DIR/summary.md"

# 清理设备端大文件（已拉回）
"${ADB[@]}" shell "rm -f $REMOTE_DATA $REMOTE_TRACE" >/dev/null 2>&1 || true

echo
echo "完成：$OUT_DIR"
echo "查看：$OUT_DIR/summary.md"
echo "perfetto trace 可在 https://ui.perfetto.dev 打开：$OUT_DIR/perfetto.trace"

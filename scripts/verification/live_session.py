#!/usr/bin/env python3
"""联调全自动会话脚本（局域网/Tailscale 通用）。

流程：等待 API 就绪 → 等待游戏世界就绪 → 连续采样（2s 间隔）→ 检测失败/超时 → 输出分析报告。
用法：uv run python scripts/verification/live_session.py [手机IP] [时长上限分钟]
"""
from __future__ import annotations

import csv
import json
import sys
import time
import urllib.request
from collections import Counter
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = ROOT / "log" / "live-test"
PORT = 8088
DEFAULT_IP = "192.168.3.54"
API_WAIT_MIN = 5
MAX_RUN_MIN = 40
INTERVAL = 2.0
MAX_CONSEC_FAIL = 10


def log(msg: str) -> None:
    ts = datetime.now().strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)


def fetch(url: str, timeout: int = 5) -> dict:
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def lead_fields(party) -> dict:
    if isinstance(party, list) and party and party[0]:
        p = party[0]
        return {
            "hp": p.get("hp"), "maxHp": p.get("max_hp"),
            "mp": p.get("mp"), "maxMp": p.get("max_mp"),
            "lv": p.get("level"), "exp": p.get("exp"),
            "nameId": p.get("name_id"), "equip": sum(1 for e in p.get("equipment", []) if e),
        }
    return {}


def bag_total(inv) -> int:
    try:
        return sum(b.get("slotCount", 0) for b in inv.get("bags", []))
    except Exception:  # noqa: BLE001
        return -1


def main() -> None:
    ip = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_IP
    max_run_min = float(sys.argv[2]) if len(sys.argv) > 2 else MAX_RUN_MIN
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    session = datetime.now().strftime("%Y%m%d-%H%M%S")
    session_dir = OUT_DIR / session
    session_dir.mkdir(parents=True, exist_ok=True)
    base = f"http://{ip}:{PORT}"

    log(f"会话 {session} 目标 {base}（最长 {max_run_min} 分钟）")
    log(f"等待 API 就绪（最多 {API_WAIT_MIN} 分钟）...")
    t0 = time.time()
    while time.time() - t0 < API_WAIT_MIN * 60:
        try:
            fetch(f"{base}/api/health")
            log("API 就绪")
            break
        except Exception:  # noqa: BLE001
            time.sleep(3)
    else:
        log("超时：API 不可达（游戏未启动或模块未初始化），结束")
        return

    log("等待游戏进入世界（角色数据非空）...")
    t0 = time.time()
    while time.time() - t0 < API_WAIT_MIN * 60:
        try:
            party = fetch(f"{base}/api/character/party")
            if isinstance(party, list) and party and party[0] and party[0].get("hp") is not None:
                log(f"游戏世界就绪：party={len(party)}，开始采样")
                break
        except Exception:  # noqa: BLE001
            pass
        time.sleep(3)
    else:
        log("超时：游戏未进入世界（可能停在标题/加载界面），结束")
        return

    csv_path = session_dir / "samples.csv"
    csv_f = open(csv_path, "w", newline="")
    writer = csv.writer(csv_f)
    writer.writerow([
        "t", "money", "mapId", "x", "y", "partyCount", "activeQuest",
        "hp", "maxHp", "mp", "maxMp", "lv", "exp", "nameId", "equip", "bagItems",
    ])
    csv_f.flush()

    prev: dict = {}
    changes = Counter()
    consec_fail = 0
    t_start = time.time()
    idx = 0
    log("采样中... 操作开始（打怪/捡钱/走动/切图/买卖装备）")
    while time.time() - t_start < max_run_min * 60:
        try:
            p = fetch(f"{base}/api/system/game").get("snapshot", {})
            party = fetch(f"{base}/api/character/party")
            inv = fetch(f"{base}/api/item/inventory")
            consec_fail = 0
        except Exception as e:  # noqa: BLE001
            consec_fail += 1
            if consec_fail == 1:
                log(f"采样失败（第1次）：{e}")
            if consec_fail >= MAX_CONSEC_FAIL:
                log(f"连续 {MAX_CONSEC_FAIL} 次失败，结束（手机离线或游戏退出）")
                break
            time.sleep(INTERVAL)
            continue

        lf = lead_fields(party)
        row = {
            "money": p.get("money"), "mapId": p.get("map_id"),
            "x": p.get("x"), "y": p.get("y"),
            "partyCount": sum(1 for member in party if member), "activeQuest": p.get("activeQuest"),
            "hp": lf.get("hp"), "maxHp": lf.get("maxHp"),
            "mp": lf.get("mp"), "maxMp": lf.get("maxMp"),
            "lv": lf.get("lv"), "exp": lf.get("exp"),
            "nameId": lf.get("nameId"), "equip": lf.get("equip"),
            "bagItems": bag_total(inv),
        }
        writer.writerow([datetime.now().strftime("%H:%M:%S")] + [row[k] for k in [
            "money", "mapId", "x", "y", "partyCount", "activeQuest",
            "hp", "maxHp", "mp", "maxMp", "lv", "exp", "nameId", "equip", "bagItems",
        ]])
        csv_f.flush()

        changed = [k for k in row if prev.get(k) != row[k]]
        if changed:
            for k in changed:
                changes[k] += 1
            mark = " ⚠️变:" + ",".join(changed)
        else:
            mark = ""
        if idx % 15 == 0 or changed:
            log(
                f"[{idx:03d}] money={row['money']} map={row['mapId']} "
                f"pos=({row['x']},{row['y']}) party={row['partyCount']} "
                f"hp={row['hp']}/{row['maxHp']} lv={row['lv']} bag={row['bagItems']}{mark}"
            )
        prev = dict(row)
        idx += 1
        time.sleep(INTERVAL)

    csv_f.close()
    log(f"采样结束：共 {idx} 次，字段变化统计：")
    for k, c in changes.most_common():
        log(f"  {k}: {c} 次变化")
    summary = {
        "session": session, "target": base, "samples": idx,
        "start": t_start, "duration_sec": round(time.time() - t_start, 1),
        "field_changes": dict(changes),
        "samples_csv": str(csv_path),
    }
    (session_dir / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1))
    log(f"报告：{session_dir}/summary.json")


if __name__ == "__main__":
    main()

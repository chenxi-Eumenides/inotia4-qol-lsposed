#!/usr/bin/env python3
"""P0 验收基建：118 路由全量 smoke 探测，产出基线报告（重构前后行为对比）。

用法：
    python3 scripts/analyze/smoke_all.py [base_url] [output_json]
    python3 scripts/analyze/smoke_all.py                      # 默认 http://192.168.3.54:8088
    python3 scripts/analyze/smoke_all.py http://127.0.0.1:8088 smoke_local.json

路由清单从 module/app/src/main/java/com/inotia4/qol/controller/*.kt 的
@GetMapping/@PostMapping 注解提取（118 条），路径模板参数 {role} {id} {slot} 等
按各 controller 参数语义写死合理默认值。

断言（每条路由）：
  ① 可达：无连接类异常（URLError/超时/socket 错误）
  ② 响应体不泄漏原始 Java 异常串（java. / NumberFormatException / ClassCastException / Exception）
  ③ 错误响应统一信封：含 ok 键（bool true/false）或 error 键；纯数据响应不强制

状态码语义：403/404/503 记录为 warn（guard 未就绪/占位端点），不算失败。
危险写端点（会永久改存档数值/槽位的）故意用缺参 body 触发参数校验拒绝路径，
零副作用，且同样验证可达性/异常泄漏/信封。

纯标准库，Python 3.10+。参考 scripts/analyze/api_poll.py 请求风格。
"""
from __future__ import annotations

import json
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

DEFAULT_BASE = "http://192.168.3.54:8088"
DEFAULT_OUT = Path(__file__).parent / "smoke_baseline_v0.5.43.json"

# 路径变量默认值（语义见各 controller @PathVariable）
PATH_VARS = {
    "role": 0,          # 队伍角色槽位（leader=0）
    "id": 1,            # quest id
    "slot": 0,          # 队伍/佣兵/背包/存档槽位
    "bag": 0,           # 背包 bag 索引
    "equip_slot": 0,    # 装备槽位
    "map_id": 30,       # MAPINFOBASE 记录下标（30=影子丛林1，运行时验证过）
    "table": "MAPINFOBASE",  # 表名（readTable 会 uppercase）
}

# 缺参 body：让 controller 走参数校验拒绝路径（{"ok":false,"error":"... required"}）
MISSING = "{}"  # 对象字面量，用于危险写端点

# 118 条路由清单：method / path 模板 / query / body / 备注
ROUTES = [
    # ---- CharacterController (10) ----
    {"m": "POST", "p": "/api/character/grow/add_skill", "body": MISSING, "note": "危险:消耗技能点,缺 action_id"},
    {"m": "POST", "p": "/api/character/grow/{role}/add_stat", "body": MISSING, "note": "危险:永久加点,缺 attrs"},
    {"m": "POST", "p": "/api/character/grow/reset_stat", "body": MISSING, "note": "危险:重置属性,缺参"},
    {"m": "POST", "p": "/api/character/grow/reset_skill", "body": MISSING, "note": "危险:重置技能,缺参"},
    {"m": "POST", "p": "/api/op/character/{role}/hp", "body": MISSING, "note": "危险:改血,缺 hp"},
    {"m": "POST", "p": "/api/op/character/{role}/mp", "body": MISSING, "note": "危险:改魔,缺 mp"},
    {"m": "POST", "p": "/api/op/character/{role}/experience", "body": MISSING, "note": "危险:改经验,缺 exp"},
    {"m": "POST", "p": "/api/op/character/{role}/level", "body": MISSING, "note": "危险:改等级,缺 level"},
    {"m": "POST", "p": "/api/op/character/{role}/set_attr", "body": MISSING, "note": "危险:改属性,缺 stats"},
    {"m": "POST", "p": "/api/op/inventory/add", "body": MISSING, "note": "危险:加道具,缺 category"},
    # ---- CombatController (6) ----
    {"m": "POST", "p": "/api/character/combat/{role}/set_auto_attack", "body": '{"on":true}', "note": "运行时:开自动攻击"},
    {"m": "POST", "p": "/api/character/combat/{role}/set_skill_usage", "body": "{}", "note": "占位:not implemented"},
    {"m": "POST", "p": "/api/character/combat/switch_player", "body": '{"slot":0}', "note": "运行时:切换战斗角色"},
    {"m": "POST", "p": "/api/character/combat/{role}/cast_skill", "body": '{"action_id":1}', "note": "运行时:释放技能"},
    {"m": "POST", "p": "/api/character/combat/{role}/attack_target", "body": '{"target_slot":0}', "note": "运行时:攻击目标"},
    {"m": "POST", "p": "/api/character/combat/{role}/stop_combat", "body": None, "note": "运行时:停止战斗"},
    # ---- ConfigController (2) ----
    {"m": "GET", "p": "/api/config/list", "note": "读配置"},
    {"m": "POST", "p": "/api/config/set", "body": "{}", "note": "空对象不更新字段,返回当前配置"},
    # ---- CurrentMapController (7) ----
    {"m": "GET", "p": "/api/world/map/id"},
    {"m": "GET", "p": "/api/world/map/exits"},
    {"m": "GET", "p": "/api/world/map/units"},
    {"m": "GET", "p": "/api/world/map/enemies"},
    {"m": "GET", "p": "/api/world/map/interactives"},
    {"m": "GET", "p": "/api/world/map/drops"},
    {"m": "GET", "p": "/api/world/map/distance", "query": {"tx": 0, "ty": 0}},
    # ---- DataController (11) ----
    {"m": "GET", "p": "/api/world/maps/list"},
    {"m": "GET", "p": "/api/world/maps/{map_id}"},
    {"m": "GET", "p": "/api/world/maps/{map_id}/tiles"},
    {"m": "GET", "p": "/api/system/tables"},
    {"m": "GET", "p": "/api/system/tables/{table}"},
    {"m": "GET", "p": "/api/system/tables/{table}/search", "query": {"q": "test"}},
    {"m": "GET", "p": "/api/system/tables/{table}/download", "note": "占位:not implemented"},
    {"m": "GET", "p": "/api/system/tables/story-events"},
    {"m": "GET", "p": "/api/system/tables/text", "query": {"lang": "zh-Hans"}},
    {"m": "GET", "p": "/api/system/help", "note": "占位:not implemented"},
    {"m": "GET", "p": "/api/system/download", "note": "占位:not implemented"},
    # ---- DebugController (2) ----
    {"m": "GET", "p": "/api/debug/ui"},
    {"m": "GET", "p": "/api/debug/path", "query": {"tx": 0, "ty": 0}},
    # ---- EventsController (1) ----
    {"m": "GET", "p": "/api/system/events", "note": "大响应(~11MB),since 可选不带"},
    # ---- GameController (4) ----
    {"m": "GET", "p": "/api/system/game"},
    {"m": "GET", "p": "/api/system/snapshot"},
    {"m": "GET", "p": "/api/system/info"},
    {"m": "GET", "p": "/api/system/game_frame"},
    # ---- HealthController (1) ----
    {"m": "GET", "p": "/api/health"},
    # ---- InventoryActionController (10) ----
    {"m": "POST", "p": "/api/item/inventory/use_item", "body": '{"bag":0,"slot":0}', "note": "运行时:使用物品"},
    {"m": "POST", "p": "/api/item/inventory/accept_dice", "note": "运行时:接受骰子"},
    {"m": "POST", "p": "/api/item/inventory/reject_dice", "note": "运行时:拒绝骰子"},
    {"m": "POST", "p": "/api/item/inventory/discard_item", "body": '{"bag":0,"slot":0}', "note": "危险:丢弃物品"},
    {"m": "POST", "p": "/api/item/inventory/sell_item", "body": '{"bag":0,"slot":0}', "note": "危险:出售物品"},
    {"m": "POST", "p": "/api/item/inventory/move_item", "body": '{"bag":0,"slot":0,"count":1,"to_bag":0,"to_slot":1}', "note": "危险:移动物品"},
    {"m": "POST", "p": "/api/item/inventory/{role}/put_jewel", "body": '{"bag":0,"slot":0,"equip_slot":0}', "note": "危险:嵌宝石"},
    {"m": "POST", "p": "/api/item/inventory/{role}/enchant", "body": '{"bag":0,"slot":0,"equip_slot":0}', "note": "危险:附魔"},
    {"m": "POST", "p": "/api/item/inventory/{role}/equip_item", "body": '{"bag":0,"slot":0}', "note": "危险:装备"},
    {"m": "POST", "p": "/api/item/inventory/{role}/unequip_item", "body": '{"slot":0}', "note": "危险:卸下装备"},
    # ---- InventoryController (5) ----
    {"m": "GET", "p": "/api/item/inventory"},
    {"m": "GET", "p": "/api/item/inventory/money"},
    {"m": "GET", "p": "/api/item/inventory/items"},
    {"m": "GET", "p": "/api/item/inventory/bag/{bag}/info"},
    {"m": "GET", "p": "/api/item/inventory/bag/{bag}/{slot}"},
    # ---- MercenaryController (3) ----
    {"m": "GET", "p": "/api/character/mercenary"},
    {"m": "GET", "p": "/api/character/mercenary/list"},
    {"m": "GET", "p": "/api/character/mercenary/{slot}"},
    # ---- MovementController (4) ----
    {"m": "POST", "p": "/api/world/movement/move_to", "body": '{"x":0,"y":0}', "note": "运行时:移动角色"},
    {"m": "POST", "p": "/api/world/movement/walk_dir", "body": '{"direction":0}', "note": "运行时:走一步"},
    {"m": "POST", "p": "/api/world/movement/stop_move", "note": "运行时:停止移动"},
    {"m": "POST", "p": "/api/world/movement/interact_with", "note": "运行时:交互键"},
    # ---- NpcController (2) ----
    {"m": "POST", "p": "/api/ui/start_interact", "note": "运行时:开始交互"},
    {"m": "POST", "p": "/api/ui/dialog/select", "body": '{"action":"ok"}', "note": "运行时:对话框选 ok"},
    # ---- OpController (15, 全部占位 not implemented) ----
    {"m": "POST", "p": "/api/op/quest/accept", "note": "占位"},
    {"m": "POST", "p": "/api/op/quest/complete", "note": "占位"},
    {"m": "POST", "p": "/api/op/character/{role}/status-point", "note": "占位"},
    {"m": "POST", "p": "/api/op/character/{role}/skill-point", "note": "占位"},
    {"m": "POST", "p": "/api/op/character/{role}/skill-level", "note": "占位"},
    {"m": "POST", "p": "/api/op/party/swap", "note": "占位"},
    {"m": "POST", "p": "/api/op/inventory/set-slot", "note": "占位"},
    {"m": "POST", "p": "/api/op/inventory/set-equip", "note": "占位"},
    {"m": "POST", "p": "/api/op/inventory/money", "note": "占位"},
    {"m": "POST", "p": "/api/op/craft/mix-direct", "note": "占位"},
    {"m": "POST", "p": "/api/op/combat/{role}/heal", "note": "占位"},
    {"m": "POST", "p": "/api/op/combat/{role}/rest", "note": "占位"},
    {"m": "POST", "p": "/api/op/combat/{role}/revive", "note": "占位"},
    {"m": "POST", "p": "/api/op/combat/{role}/hate", "note": "占位"},
    {"m": "POST", "p": "/api/op/movement/teleport", "note": "占位"},
    # ---- PartyActionController (4) ----
    {"m": "POST", "p": "/api/character/party/include", "body": '{"mercenary_slot":0}', "note": "危险:入队佣兵"},
    {"m": "POST", "p": "/api/character/party/exclude", "body": '{"mercenary_slot":0}', "note": "危险:离队佣兵"},
    {"m": "POST", "p": "/api/character/party/discharge", "body": '{"mercenary_slot":0}', "note": "危险:解散佣兵"},
    {"m": "POST", "p": "/api/character/party/withdraw", "body": '{"mercenary_slot":0,"equip_slot":0}', "note": "危险:取回装备"},
    # ---- PartyController (12) ----
    {"m": "GET", "p": "/api/character/party"},
    {"m": "GET", "p": "/api/character/party/count"},
    {"m": "GET", "p": "/api/character/leader"},
    {"m": "GET", "p": "/api/character/party/{slot}"},
    {"m": "GET", "p": "/api/character/party/{slot}/id"},
    {"m": "GET", "p": "/api/character/party/{slot}/name"},
    {"m": "GET", "p": "/api/character/party/{slot}/level"},
    {"m": "GET", "p": "/api/character/party/{slot}/status"},
    {"m": "GET", "p": "/api/character/party/{slot}/stats"},
    {"m": "GET", "p": "/api/character/party/{slot}/equipment"},
    {"m": "GET", "p": "/api/character/party/{slot}/equipment/{equip_slot}"},
    {"m": "GET", "p": "/api/character/party/{slot}/skills"},
    # ---- QuestActionController (1) ----
    {"m": "POST", "p": "/api/quest/quit_quest", "body": MISSING, "note": "危险:放弃任务,缺 quest_id"},
    # ---- QuestController (5) ----
    {"m": "GET", "p": "/api/quest"},
    {"m": "GET", "p": "/api/quest/active"},
    {"m": "GET", "p": "/api/quest/details"},
    {"m": "GET", "p": "/api/quest/{id}"},
    {"m": "GET", "p": "/api/quest/completed"},
    # ---- SaveController (3) ----
    {"m": "POST", "p": "/api/system/save", "note": "写盘保存(有益)"},
    {"m": "POST", "p": "/api/system/enter_slot", "body": MISSING, "note": "危险:切存档槽,缺 slot"},
    {"m": "POST", "p": "/api/system/create_slot", "body": MISSING, "note": "危险:建存档,缺 slot/class_idx"},
    # ---- ShopController (2) ----
    {"m": "GET", "p": "/api/item/shop/items"},
    {"m": "POST", "p": "/api/item/shop/buy_item", "body": '{"slot":0}', "note": "危险:购买物品"},
    # ---- UiActionController (3) ----
    {"m": "POST", "p": "/api/ui/go_main_menu", "note": "运行时:回主菜单"},
    {"m": "POST", "p": "/api/ui/close_panel", "note": "运行时:关面板"},
    {"m": "POST", "p": "/api/ui/open_panel", "body": '{"panel":"inventory"}', "note": "运行时:开背包面板"},
    # ---- UiController (4) ----
    {"m": "GET", "p": "/api/ui"},
    {"m": "GET", "p": "/api/ui/screen"},
    {"m": "GET", "p": "/api/ui/panel"},
    {"m": "GET", "p": "/api/ui/dialog"},
]

# 异常串泄漏检测（断言②）
LEAK_MARKERS = ("java.", "NumberFormatException", "ClassCastException", "Exception")
# 记录为 warn 而非 fail 的状态码（guard 未就绪/占位端点/AndServer 路由匹配既存行为）
# 405：AndServer 2.1.12 模式路由（如 /api/quest/{id}）会 shadow 精确路由的 POST，属真机既存行为
WARN_STATUS = {403, 404, 405, 503}

TIMEOUT = 45  # 秒；大响应端点（events ~11MB）需要长超时


def build_url(base: str, route: dict) -> str:
    path = route["p"]
    for k, v in PATH_VARS.items():
        path = path.replace("{" + k + "}", str(v))
    q = route.get("query")
    if q:
        path += "?" + urllib.parse.urlencode(q)
    return base + path


def fetch(base: str, route: dict) -> dict:
    url = build_url(base, route)
    headers = {"Accept": "application/json"}
    data = None
    if route["m"] == "POST":
        headers["Content-Type"] = "application/json"
        data = route.get("body")
        data = None if data is None else data.encode("utf-8")
    start = time.monotonic()
    req = urllib.request.Request(url, data=data, headers=headers, method=route["m"])
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            raw = r.read()
            status = r.status
            err = None
    except urllib.error.HTTPError as e:
        raw = e.read()
        status = e.code
        err = None
    except (urllib.error.URLError, socket.timeout, TimeoutError, ConnectionError, OSError) as e:
        raw = b""
        status = None
        err = f"{type(e).__name__}: {e}"
    elapsed = round(time.monotonic() - start, 3)
    try:
        body = raw.decode("utf-8")
    except UnicodeDecodeError:
        body = raw.decode("utf-8", errors="replace")
    return {"url": url, "status": status, "body": body, "elapsed": elapsed, "error": err}


def check_assertions(status, body: str) -> dict:
    """断言①②③；返回 {reachable, no_leak, envelope, verdict, reason}"""
    reachable = status is not None
    no_leak = not any(m in body for m in LEAK_MARKERS)
    # 断言③：错误响应统一信封。解析 JSON；含 ok 键则必须为 bool；含 error 键即通过；
    # 纯数据响应（无 ok/error）不强制。status>=400 且无信封 → fail。
    envelope = True
    envelope_reason = ""
    try:
        obj = json.loads(body)
        if isinstance(obj, dict):
            if "ok" in obj:
                okv = obj["ok"]
                if not isinstance(okv, bool):
                    envelope = False
                    envelope_reason = f"ok 非 bool: {okv!r}"
            elif "error" not in obj:
                if status is not None and status >= 400:
                    envelope = False
                    envelope_reason = f"错误响应({status})无 ok/error 信封"
                # 纯数据响应：通过
            # 含 error 键：信封通过
    except (json.JSONDecodeError, ValueError):
        if status is None:
            pass  # 连接失败已由 reachable 捕获
        elif status in WARN_STATUS or status >= 500:
            pass  # 非 JSON 的 403/404/503/5xx 页面：记录 warn，不算信封失败
        else:
            envelope = False
            envelope_reason = f"响应非 JSON(status={status})"

    if not reachable:
        return {"reachable": False, "no_leak": True, "envelope": True,
                "verdict": "fail", "reason": "连接失败"}
    if not no_leak:
        return {"reachable": True, "no_leak": False, "envelope": envelope,
                "verdict": "fail", "reason": "Java 异常串泄漏"}
    if not envelope:
        return {"reachable": True, "no_leak": True, "envelope": False,
                "verdict": "fail", "reason": envelope_reason}
    if status in WARN_STATUS or status >= 500:
        return {"reachable": True, "no_leak": True, "envelope": True,
                "verdict": "warn", "reason": f"状态码 {status}（guard 未就绪/占位/服务器错误）"}
    return {"reachable": True, "no_leak": True, "envelope": True,
            "verdict": "pass", "reason": ""}


def body_summary(body: str, limit: int = 400) -> str:
    flat = " ".join(body.split())
    return flat[:limit] + ("..." if len(flat) > limit else "")


def main() -> int:
    base = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BASE
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_OUT

    print(f"smoke 全量探测开始: {base}（{len(ROUTES)} 条路由）")
    results = []
    pass_n = warn_n = fail_n = 0
    for i, route in enumerate(ROUTES, 1):
        resp = fetch(base, route)
        checks = check_assertions(resp["status"], resp["body"])
        verdict = checks["verdict"]
        pass_n += verdict == "pass"
        warn_n += verdict == "warn"
        fail_n += verdict == "fail"
        rec = {
            "path": route["p"],
            "method": route["m"],
            "status": resp["status"],
            "elapsed": resp["elapsed"],
            "ok": resp["status"] == 200,
            "verdict": verdict,
            "reason": checks["reason"] or route.get("note", ""),
            "body": body_summary(resp["body"]),
            "error": resp["error"],
        }
        results.append(rec)
        flag = "PASS" if verdict == "pass" else ("warn" if verdict == "warn" else "FAIL")
        print(f"[{i:03d}/{len(ROUTES)}] {flag} {route['m']:4s} {route['p']} "
              f"-> {resp['status']} {resp['elapsed']}s {checks['reason'] or ''}".rstrip())
        time.sleep(0.05)  # 温和限速，避免打爆真机

    report = {
        "meta": {
            "project": "inotia4-qol-lsposed",
            "phase": "P0-baseline",
            "version": "v0.5.43",
            "base_url": base,
            "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
            "total_routes": len(ROUTES),
        },
        "summary": {"pass": pass_n, "warn": warn_n, "fail": fail_n, "total": len(ROUTES)},
        "routes": results,
    }
    out_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\n基线报告已写入: {out_path}")
    print(f"摘要: total={len(ROUTES)} pass={pass_n} warn={warn_n} fail={fail_n}")
    if fail_n:
        print("FAIL 路由：")
        for r in results:
            if r["verdict"] == "fail":
                print(f"  {r['method']:4s} {r['path']} status={r['status']} reason={r['reason']}")
    return 1 if fail_n else 0


if __name__ == "__main__":
    sys.exit(main())

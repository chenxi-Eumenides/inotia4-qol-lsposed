#!/usr/bin/env python3
from __future__ import annotations

import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
JSON_DIR = ROOT / "apk" / "static-data" / "json"
ASSET_DIR = ROOT / "module" / "app" / "src" / "main" / "assets" / "static-data"

# 打包进 APK 的子集（控制体积：全量 22MB 过大）
INCLUDE_TABLES = [
    "CHARCLASSBASE", "ATTRINITBASE", "MAXLEVELBASE",
    "ITEMDATABASE", "ITEMCLASSBASE", "ITEMSTATICBASE", "ITEMDESCBASE",
    "ITEMRARITYGRADEBASE", "ITEMGRADEBASE", "ITEMENCHANTBASE",
    "ITEMSTATICOPTBASE",
    "SKILLDESCBASE", "SKILLTRAINBASE", "SKILLTRAINPOINTBASE",
    "MERCENARYINFOBASE", "MERCENARYSKILLBASE", "MERCENARYGROUPSKILLBASE",
    "MAPINFOBASE", "MAPFEATUREINFOBASE", "PORTALINFOBASE",
    "MONDATABASE", "MONSKILLBASE", "MONSTERDROPBASE", "MONAIINFOBASE",
    "QUESTINFOBASE", "QUESTGROUPBASE", "QUESTREWARDBASE", "QUESTS",
    "NPCINFOBASE", "NPCDESCBASE",
    "ITEMOPTINFOBASE",
]
TEXT_LANGS = ["zh-Hans", "en"]


def main() -> None:
    if ASSET_DIR.exists():
        shutil.rmtree(ASSET_DIR)
    (ASSET_DIR / "tables").mkdir(parents=True)
    (ASSET_DIR / "text").mkdir()
    (ASSET_DIR / "reverse").mkdir()
    (ASSET_DIR / "maps").mkdir()

    for name in INCLUDE_TABLES:
        src = JSON_DIR / "tables" / f"{name}.json"
        if src.exists():
            shutil.copy(src, ASSET_DIR / "tables" / f"{name}.json")

    for lang in TEXT_LANGS:
        src = JSON_DIR / "text" / f"{lang}.json"
        if src.exists():
            shutil.copy(src, ASSET_DIR / "text" / f"{lang}.json")

    events_src = JSON_DIR / "reverse" / "events.json"
    if events_src.exists():
        shutil.copy(events_src, ASSET_DIR / "reverse" / "events.json")

    # P0#瓦片矩阵（2026-08-12）：416 图通行矩阵（2.2MB，native 从静态读替代运行时读内存）
    tiles_src = JSON_DIR / "maps" / "tiles.json"
    if tiles_src.exists():
        shutil.copy(tiles_src, ASSET_DIR / "maps" / "tiles.json")

    summary = {
        "tables": INCLUDE_TABLES,
        "text_langs": TEXT_LANGS,
        "maps": ["tiles.json"],
        "note": "API 静态数据内嵌子集，完整版见 apk/static-data/json/",
    }
    (ASSET_DIR / "manifest.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1))

    total = sum(f.stat().st_size for f in ASSET_DIR.rglob("*") if f.is_file())
    print(f"assets packaged: {len(INCLUDE_TABLES)} tables + {len(TEXT_LANGS)} langs, total {total:,} bytes")


if __name__ == "__main__":
    main()

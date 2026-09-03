#!/usr/bin/env python3
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "vendor"))
from inotia_resources import (  # noqa: E402
    TABLE_NAMES,
    parse_excel_tables,
    parse_memorytext_blob,
    parse_table_records,
)

ROOT = Path(__file__).resolve().parents[2]
RAW_DIR = ROOT / "apk" / "static-data" / "raw"
OUT_DIR = ROOT / "apk" / "static-data" / "json" / "tables"

# 已知文本 id 字段（来自 IDA 调用点逆向 + vendor 工具已验证偏移）
TEXT_ID_FIELDS: dict[str, list[int]] = {
    "CHARCLASSBASE": [2],
    "ITEMDATABASE": [0],
    "SKILLDESCBASE": [2],
    "MERCENARYINFOBASE": [2],
    "MAPINFOBASE": [0],
    "MONDATABASE": [0],
    "QUESTINFOBASE": [2, 14, 16, 18],
    "NPCINFOBASE": [0],
    "NPCDESCBASE": [1],
    "HELPTEXTBASE": [1],
    "TEXTDATABASE": [0],
    "TIPBASE": [1],
    "ITEMDESCBASE": [2],
    "ACTDATABASE": [2],
    "STATUSBASE": [1],
    "CONDITIONBASE": [1],
    "BUFFDATABASE": [1],
    "CHOICEBASE": [0],
}


def decode_record(rec: bytes) -> dict:
    u16 = []
    for off in range(0, len(rec) - 1, 2):
        u16.append(struct.unpack_from("<H", rec, off)[0])
    return {"hex": rec.hex(), "u16": u16}


def load_text_records() -> list[str]:
    blob = (RAW_DIR / "memorytext_zhhans.dat.bin").read_bytes()
    return parse_memorytext_blob(blob)


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    text_records = load_text_records()
    blob = (RAW_DIR / "game.dat.bin").read_bytes()
    tables = parse_excel_tables(blob)
    summary: list[dict] = []
    for index, name in TABLE_NAMES.items():
        parsed = parse_table_records(tables[index])
        if parsed is None:
            summary.append({"index": index, "name": name, "record_count": 0})
            continue
        record_count, record_size, body = parsed
        records: list[dict] = []
        text_offsets = TEXT_ID_FIELDS.get(name, [])
        for i in range(min(record_count, len(body) // record_size)):
            rec = body[i * record_size:(i + 1) * record_size]
            entry = decode_record(rec)
            for off in text_offsets:
                tid = struct.unpack_from("<H", rec, off)[0] if off + 2 <= len(rec) else -1
                if 0 <= tid < len(text_records) and text_records[tid]:
                    entry[f"text_{off}"] = text_records[tid]
            records.append(entry)
        payload = {
            "table": name,
            "index": index,
            "record_count": len(records),
            "record_size": record_size,
            "records": records,
        }
        (OUT_DIR / f"{name}.json").write_text(
            json.dumps(payload, ensure_ascii=False)
        )
        summary.append({"index": index, "name": name, "record_count": len(records), "record_size": record_size})
    (OUT_DIR / "_summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1))
    total = sum(s["record_count"] for s in summary)
    print(f"tables={len(summary)} records_total={total:,}")


if __name__ == "__main__":
    main()

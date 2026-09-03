#!/usr/bin/env python3
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "vendor"))
from inotia_resources import parse_memorytext_blob  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
RAW_DIR = ROOT / "apk" / "static-data" / "raw"
OUT_DIR = ROOT / "apk" / "static-data" / "json" / "text"

LANG_MAP = {
    "memorytext": "en",
    "memorytext_en": "en",
    "memorytext_zhhans": "zh-Hans",
    "memorytext_zhhant": "zh-Hant",
    "memorytext_jp": "ja",
    "memorytext_de": "de",
    "memorytext_fr": "fr",
    "memorytext_e": "formula-e",
}


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    summary: list[dict] = []
    for bin_name, lang in LANG_MAP.items():
        src = RAW_DIR / f"{bin_name}.dat.bin"
        if not src.exists():
            print(f"missing: {bin_name}")
            continue
        records = parse_memorytext_blob(src.read_bytes())
        non_empty = sum(1 for r in records if r)
        (OUT_DIR / f"{lang}.json").write_text(
            json.dumps(records, ensure_ascii=False)
        )
        summary.append({"lang": lang, "source": bin_name, "count": len(records), "non_empty": non_empty})
        print(f"{lang}: {len(records)} records ({non_empty} non-empty)")
    (OUT_DIR / "_summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()

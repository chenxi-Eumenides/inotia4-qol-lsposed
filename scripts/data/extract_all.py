#!/usr/bin/env python3
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "vendor"))
from inotia_resources import decode_standard_outer  # noqa: E402

GAME_RES = Path(__file__).resolve().parents[2] / "apk" / "decoded" / "assets" / "common" / "game_res"
RAW_DIR = Path(__file__).resolve().parents[2] / "apk" / "static-data" / "raw"


def main() -> None:
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    manifest: list[dict] = []
    ok = skip = fail = 0
    for f in sorted(GAME_RES.glob("*.dat.jpg")):
        data = f.read_bytes()
        entry: dict = {"file": f.name, "size": len(data)}
        if len(data) < 16 or data[:4] != b"\x01\x00\x5d\x00":
            entry["type"] = "non-container"
            skip += 1
        else:
            try:
                out = decode_standard_outer(data)
                out_name = f.name[: -len(".jpg")] + ".bin"
                (RAW_DIR / out_name).write_bytes(out)
                entry["decoded_size"] = len(out)
                entry["type"] = "lzma-container"
                ok += 1
            except Exception as e:  # noqa: BLE001
                entry["type"] = "error"
                entry["error"] = str(e)[:120]
                fail += 1
        manifest.append(entry)
    (RAW_DIR / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=1)
    )
    print(f"OK={ok} skip={skip} fail={fail} total={ok + skip + fail}")


if __name__ == "__main__":
    main()

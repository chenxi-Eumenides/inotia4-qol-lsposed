#!/usr/bin/env python3
from __future__ import annotations

import lzma
import shutil
import struct
import zipfile
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
APK_INPUT_DIR = ROOT / "apk"
# 项目适配：apk/decoded/ 已是 apktool 解码产物，find_assets_dir 通过
# WORKDIR_DIR.glob("*/assets/common/game_res") 命中 apk/decoded/assets/common/game_res
WORKDIR_DIR = ROOT / "apk"
RESOURCE_RELATIVE_DIR = Path("assets") / "common" / "game_res"
TARGET_MEMORYTEXT_STEM = "memorytext_zhhans"
TARGET_LANGUAGE = "zh-Hans"

# Table index -> name mapping (extracted from EXCELDATA_Create in libgame.so)
TABLE_NAMES: dict[int, str] = {
    0: "CHARCLASSBASE",
    1: "STATUSINFOBASE",
    2: "STATUSASSIGNBASE",
    3: "STATUSDICEBASE",
    4: "ATTRINITBASE",
    5: "PORTRAITBASE",
    6: "PORTRAITCLASSBASE",
    7: "CHARACTERSTATEBASE",
    8: "CHARACTERSTATECHANGEBASE",
    9: "CONDITIONBASE",
    10: "EXPRESSBASE",
    11: "CONSTBASE",
    12: "STATUSBASE",
    13: "ITEMDATABASE",
    14: "ITEMCLASSBASE",
    15: "ITEMMIXLINKBASE",
    16: "MANAGEMBASE",
    17: "ITEMRARITYGRADEBASE",
    18: "ITEMGRADEBASE",
    19: "ITEMENCHANTBASE",
    20: "ITEMSTATICBASE",
    21: "ITEMRECOVERBASE",
    22: "ITEMBUFFBASE",
    23: "ITEMOPTINFOBASE",
    24: "ITEMSTATICOPTBASE",
    25: "DEALINFOBASE",
    26: "RECIPEBASE",
    27: "MIXTUREBASE",
    28: "ACTDATABASE",
    29: "ACTUNITBASE",
    30: "ACTTRANSMITBASE",
    31: "BUFFDATABASE",
    32: "BUFFUNITBASE",
    33: "ACTTRANSMIT_ADDBASE",
    34: "SKILLDESCBASE",
    35: "SKILLTRAINPOINTBASE",
    36: "SKILLTRAINBASE",
    37: "CHARACTERCOSTUMEGROUPBASE",
    38: "CHARACTERCOSTUMEBASE",
    39: "CHARACTERCOSTUMEPALETTEBASE",
    40: "IMAGEFILEBASE",
    41: "ANIMATIONAREABASE",
    42: "ANIMATIONTYPEBASE",
    43: "ANIMATIONDEFINEBASE",
    44: "MONSTERCOSTUMEBASE",
    45: "MONSKILLTYPEBASE",
    46: "MONSKILLBASE",
    47: "MONDATABASE",
    48: "MONAIINFOBASE",
    49: "QUESTDROPBASE",
    50: "NPCFUNCBASE",
    51: "NPCINFOBASE",
    52: "NPCFUNCLINKBASE",
    53: "NPCDESCBASE",
    54: "NPCABILITYBASE",
    55: "NPCCOSTUMEBASE",
    56: "MAPFEATUREINFOBASE",
    57: "MAPCOLORBASE",
    58: "MAPINFOBASE",
    59: "EVTINFOBASE",
    60: "EVTCONDBASE",
    61: "EVTCMDBASE",
    62: "EFFECTINFOBASE",
    63: "CHOICEBASE",
    64: "QUESTGROUPBASE",
    65: "QUESTINFOBASE",
    66: "QUESTLINKBASE",
    67: "QUESTCOMPLETEBASE",
    68: "QUESTOBJECTCHANGEBASE",
    69: "QUESTPREPAREBASE",
    70: "QUESTREWARDBASE",
    71: "QUESTGENBASE",
    72: "SYMBOLBASE",
    73: "TEXTDATABASE",
    74: "TIPBASE",
    75: "MONSTERDROPBASE",
    76: "OPENITEMBOXBASE",
    77: "DROPINFOBASE",
    78: "DROPDETAILINFOBASE",
    79: "DROPEVTBASE",
    80: "SOUNDINFOBASE",
    81: "HELPTEXTBASE",
    82: "MAXLEVELBASE",
    83: "INSTALLBASE",
    84: "CASHITEMGROUPBASE",
    85: "CASHITEMBASE",
    86: "ITEMPACKBASE",
    87: "PORTALINFOBASE",
    88: "ITEMDESCBASE",
    89: "COLORRATEBASE",
    90: "COLORRATEDATABASE",
    91: "MERCENARYINFOBASE",
    92: "MERCENARYSKILLBASE",
    93: "MERCENARYGROUPSKILLBASE",
    94: "CHEATCHARBASE",
    95: "CHEATCHARITEMBASE",
    96: "SOUNDEFFECTBASE",
    97: "SOUNDBGMBASE",
    98: "CHARGEDITEMBASE",
    99: "CHARGEDITEMPRODUCTBASE",
}
TABLE_INDEX_BY_NAME: dict[str, int] = {name: idx for idx, name in TABLE_NAMES.items()}


def _props_to_lclppb(prop: int) -> tuple[int, int, int] | None:
    if prop >= 225:
        return None
    pb = prop // 45
    rem = prop % 45
    lp = rem // 9
    lc = rem % 9
    return lc, lp, pb


def _decode_raw_with_limit(comp: bytes, filters: list[dict[str, Any]], out_size: int) -> bytes:
    dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filters)
    out = bytearray()
    data = comp
    while len(out) < out_size:
        chunk = dec.decompress(data, max_length=out_size - len(out))
        out.extend(chunk)
        data = b""
        if len(out) >= out_size:
            break
        if not chunk and dec.needs_input:
            break
    return bytes(out)


def decode_standard_outer(data: bytes) -> bytes:
    if len(data) < 16 or data[:4] != b"\x01\x00\x5d\x00":
        raise ValueError("resource is not a standard 0x01 00 5d 00 container")
    props = _props_to_lclppb(data[2])
    if props is None:
        raise ValueError(f"invalid outer LZMA property byte: {data[2]:#x}")
    lc, lp, pb = props
    dict_size = int.from_bytes(data[3:7], "little")
    out_size = int.from_bytes(data[7:11], "little")
    decoded = _decode_raw_with_limit(
        data[15:],
        [{
            "id": lzma.FILTER_LZMA1,
            "dict_size": max(dict_size, 4096),
            "lc": lc,
            "lp": lp,
            "pb": pb,
        }],
        out_size,
    )
    if len(decoded) != out_size:
        raise ValueError(f"short outer decode: {len(decoded)} != {out_size}")
    return decoded


def parse_excel_tables(blob: bytes) -> list[bytes]:
    if len(blob) < 5:
        raise ValueError("excel blob too small")
    table_count = int.from_bytes(blob[:2], "little")
    header_size = 2 + (table_count + 1) * 3
    if header_size > len(blob):
        raise ValueError("excel header extends beyond blob size")
    offsets = [
        int.from_bytes(blob[2 + index * 3: 5 + index * 3], "little")
        for index in range(table_count + 1)
    ]
    if offsets[0] != 0:
        raise ValueError("excel table offsets do not start at zero")
    return [
        blob[header_size + offsets[index]: header_size + offsets[index + 1]]
        for index in range(table_count)
    ]


def parse_table_records(table_blob: bytes) -> tuple[int, int, bytes] | None:
    if len(table_blob) < 6:
        return None
    record_count = struct.unpack_from("<I", table_blob, 0)[0]
    record_size = struct.unpack_from("<H", table_blob, 4)[0]
    if record_count == 0 or record_size == 0:
        return None
    return record_count, record_size, table_blob[6:]


def parse_flat_record_blob(blob: bytes) -> tuple[int, int, bytes]:
    if len(blob) < 6:
        raise ValueError("record blob is too small")
    record_count = struct.unpack_from("<I", blob, 0)[0]
    record_size = struct.unpack_from("<H", blob, 4)[0]
    if record_count == 0 or record_size == 0:
        raise ValueError("record blob has zero-sized header")
    body = blob[6:]
    expected_size = record_count * record_size
    if expected_size > len(body):
        raise ValueError(
            f"record blob body is truncated: expected {expected_size} bytes, got {len(body)}"
        )
    return record_count, record_size, body[:expected_size]


def read_u8(blob: bytes, offset: int) -> int:
    return blob[offset]


def read_s8(blob: bytes, offset: int) -> int:
    return struct.unpack_from("<b", blob, offset)[0]


def read_u16(blob: bytes, offset: int) -> int:
    return struct.unpack_from("<H", blob, offset)[0]


def read_s16(blob: bytes, offset: int) -> int:
    return struct.unpack_from("<h", blob, offset)[0]


def read_u32(blob: bytes, offset: int) -> int:
    return struct.unpack_from("<I", blob, offset)[0]


def get_record_slice(body: bytes, record_size: int, index: int) -> bytes | None:
    start = index * record_size
    end = start + record_size
    if start < 0 or end > len(body):
        return None
    return body[start:end]


def iter_record_slices(record_count: int, record_size: int, body: bytes) -> list[tuple[int, bytes]]:
    actual_rc = min(record_count, len(body) // record_size)
    return [
        (index, body[index * record_size: (index + 1) * record_size])
        for index in range(actual_rc)
    ]


def get_text(records: list[str], text_id: int) -> str:
    if 0 <= text_id < len(records):
        return records[text_id]
    return ""


def parse_memorytext_blob(blob: bytes) -> list[str]:
    if len(blob) < 4:
        raise ValueError("memorytext blob is too small")
    record_count = struct.unpack_from("<I", blob, 0)[0]
    table_end = 4 + record_count * 3
    if table_end > len(blob):
        raise ValueError("memorytext offset table extends beyond blob size")
    offsets: list[int] = []
    for index in range(record_count):
        offset = int.from_bytes(blob[4 + index * 3: 7 + index * 3], "little")
        offsets.append(offset)
    records: list[str] = []
    for offset in offsets:
        end = blob.find(b"\x00", offset)
        if end == -1:
            end = len(blob)
        records.append(blob[offset:end].decode("utf-8", errors="replace"))
    return records


def discover_single_apk() -> Path:
    APK_INPUT_DIR.mkdir(parents=True, exist_ok=True)
    apk_paths = sorted(APK_INPUT_DIR.glob("*.apk"))
    if not apk_paths:
        raise FileNotFoundError(
            f"expected exactly one APK in {APK_INPUT_DIR}, found none; place your APK there and rerun"
        )
    if len(apk_paths) > 1:
        joined = ", ".join(path.name for path in apk_paths)
        raise ValueError(f"expected exactly one APK in {APK_INPUT_DIR}, found {len(apk_paths)}: {joined}")
    return apk_paths[0]


def extract_assets_dir(apk_path: Path, *, clear_existing: bool = True) -> Path:
    if clear_existing and WORKDIR_DIR.exists():
        shutil.rmtree(WORKDIR_DIR)
    extract_root = WORKDIR_DIR / apk_path.stem
    extract_root.mkdir(parents=True, exist_ok=True)
    print(f"Extracting {apk_path.name} -> {extract_root}")
    with zipfile.ZipFile(apk_path) as archive:
        archive.extractall(extract_root)
    assets_dir = extract_root / RESOURCE_RELATIVE_DIR
    if not assets_dir.is_dir():
        raise FileNotFoundError(f"Missing resource directory after extraction: {assets_dir}")
    return assets_dir


def find_assets_dir() -> Path:
    candidates = list(WORKDIR_DIR.glob("*/assets/common/game_res"))
    if not candidates:
        return extract_assets_dir(discover_single_apk())
    if len(candidates) != 1:
        raise FileNotFoundError(
            f"Expected exactly 1 game_res directory under {WORKDIR_DIR}, found {len(candidates)}"
        )
    return candidates[0]


def load_resource_blob(assets_dir: Path, name: str) -> bytes:
    return decode_standard_outer((assets_dir / f"{name}.dat.jpg").read_bytes())


def load_memorytext_records(
    assets_dir: Path,
    resource_name: str = TARGET_MEMORYTEXT_STEM,
) -> list[str]:
    return parse_memorytext_blob(load_resource_blob(assets_dir, resource_name))


def load_game_tables(assets_dir: Path) -> list[bytes]:
    return parse_excel_tables(load_resource_blob(assets_dir, "game"))


def get_game_table(
    tables: list[bytes],
    table_name: str,
) -> tuple[int, int, int, bytes]:
    table_index = TABLE_INDEX_BY_NAME[table_name]
    parsed = parse_table_records(tables[table_index])
    if parsed is None:
        raise ValueError(f"{table_name} is empty or malformed")
    record_count, record_size, body = parsed
    return table_index, record_count, record_size, body

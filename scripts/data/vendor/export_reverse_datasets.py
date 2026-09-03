#!/usr/bin/env python3
from __future__ import annotations

import json
import struct
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from consolidate_texts import (
    EVENT_OPCODE_KINDS,
    build_choice_relations,
    build_monster_name_map,
    build_npc_name_map,
    build_quest_text_relations,
    resolve_event_speaker,
    strip_text_markup,
)
from inotia_resources import (
    ROOT,
    TARGET_LANGUAGE,
    TARGET_MEMORYTEXT_STEM,
    find_assets_dir,
    get_game_table,
    get_record_slice,
    get_text,
    iter_record_slices,
    load_game_tables,
    load_memorytext_records,
    load_resource_blob,
    read_s8,
    read_u8,
    read_u16,
    read_u32,
)


OUTPUT_DIR = ROOT / "apk" / "static-data" / "json" / "reverse"
FORMULA_MEMORYTEXT_STEM = "memorytext_e"

FIELD_CONFIDENCE_HIGH = "high"
FIELD_CONFIDENCE_MEDIUM = "medium"
FIELD_CONFIDENCE_LOW = "low"

EVENT_INFO_FIELDS: list[dict[str, Any]] = [
    {
        "scope": "EVTINFOBASE",
        "offset": 0,
        "width": 2,
        "name": "condition_start",
        "confidence": FIELD_CONFIDENCE_HIGH,
        "source_function": "EVTSYSTEM_CheckCondition",
        "read_kind": "u16",
        "notes": "Start index into EVTCONDBASE.",
    },
    {
        "scope": "EVTINFOBASE",
        "offset": 2,
        "width": 1,
        "name": "condition_count",
        "confidence": FIELD_CONFIDENCE_HIGH,
        "source_function": "EVTSYSTEM_CheckCondition",
        "read_kind": "u8",
        "notes": "Number of EVTCONDBASE rows attached to the event.",
    },
    {
        "scope": "EVTINFOBASE",
        "offset": 3,
        "width": 2,
        "name": "data_start",
        "confidence": FIELD_CONFIDENCE_HIGH,
        "source_function": "EVTSYSTEM_LoadEvent",
        "read_kind": "u16",
        "notes": "Start index into eventdata.dat records.",
    },
    {
        "scope": "EVTINFOBASE",
        "offset": 5,
        "width": 2,
        "name": "command_count",
        "confidence": FIELD_CONFIDENCE_HIGH,
        "source_function": "EVTSYSTEM_LoadEvent",
        "read_kind": "u16",
        "notes": "Number of eventdata.dat records consumed by the event.",
    },
    {
        "scope": "EVTINFOBASE",
        "offset": 7,
        "width": 1,
        "name": "flags",
        "confidence": FIELD_CONFIDENCE_HIGH,
        "source_function": "EVTSYSTEM_LoadEvent",
        "read_kind": "u8",
        "notes": "Event flags byte; bit 2 is checked when marking completion.",
    },
]

EVENT_COMMAND_FIELDS: list[dict[str, Any]] = [
    {
        "scope": "eventdata.dat",
        "offset": 0,
        "width": 1,
        "name": "opcode",
        "confidence": FIELD_CONFIDENCE_HIGH,
        "source_function": "EVTSYSTEM_Process",
        "read_kind": "u8",
        "notes": "Dispatches into the 78-case event opcode switch.",
    },
    {
        "scope": "eventdata.dat",
        "offset": 1,
        "width": 1,
        "name": "arg0",
        "confidence": FIELD_CONFIDENCE_HIGH,
        "source_function": "EVTSYSTEM_Process",
        "read_kind": "u8",
        "notes": "Runtime argument byte 0; meaning depends on opcode.",
    },
    {
        "scope": "eventdata.dat",
        "offset": 2,
        "width": 2,
        "name": "arg1",
        "confidence": FIELD_CONFIDENCE_HIGH,
        "source_function": "EVTSYSTEM_Process",
        "read_kind": "u16",
        "notes": "Runtime argument word 1; meaning depends on opcode.",
    },
    {
        "scope": "eventdata.dat",
        "offset": 4,
        "width": 4,
        "name": "param",
        "confidence": FIELD_CONFIDENCE_HIGH,
        "source_function": "EVTSYSTEM_Process",
        "read_kind": "u32",
        "notes": "Runtime parameter dword.",
    },
    {
        "scope": "eventdata.dat",
        "offset": 8,
        "width": 2,
        "name": "text_id",
        "confidence": FIELD_CONFIDENCE_HIGH,
        "source_function": "EVTSYSTEM_LoadEvent",
        "read_kind": "u16",
        "notes": "Memorytext id resolved into the runtime text pointer.",
    },
]

EVENT_CONDITION_TYPE_CATALOG: dict[int, dict[str, Any]] = {
    0: {
        "name": "map_region",
        "confidence": FIELD_CONFIDENCE_HIGH,
        "source_function": "EVTSYSTEM_CheckCondition",
        "notes": "Checks map id and player tile bounds packed into raw_u32.",
    },
    1: {
        "name": "event_state",
        "confidence": FIELD_CONFIDENCE_HIGH,
        "source_function": "EVTSYSTEM_CheckCondition",
        "notes": "Checks a target event's completion bit against the expected state in raw_u16.",
    },
    2: {
        "name": "quest_state_bits",
        "confidence": FIELD_CONFIDENCE_HIGH,
        "source_function": "EVTSYSTEM_CheckCondition",
        "notes": "Checks QUESTSYSTEM state byte against the allowed bitset in raw_u16.",
    },
    3: {
        "name": "near_npc",
        "confidence": FIELD_CONFIDENCE_HIGH,
        "source_function": "EVTSYSTEM_CheckCondition",
        "notes": "Requires caller kind 2 and the current near-NPC id to match raw_u32.",
    },
    4: {
        "name": "interaction_object",
        "confidence": FIELD_CONFIDENCE_HIGH,
        "source_function": "EVTSYSTEM_CheckCondition",
        "notes": "Requires caller kind 7 plus matching object type/id from EVTSYSTEM_nObjectType/nObjectID.",
    },
    6: {
        "name": "event_object",
        "confidence": FIELD_CONFIDENCE_HIGH,
        "source_function": "EVTSYSTEM_CheckCondition",
        "notes": "Requires caller kind 5 plus matching EVTSYSTEM_pObject type/id.",
    },
    7: {
        "name": "chain",
        "confidence": FIELD_CONFIDENCE_LOW,
        "source_function": "EVTSYSTEM_CheckCondition",
        "notes": "Only observed to pass when caller kind == 6; likely a chained condition marker.",
    },
    8: {
        "name": "choice_result",
        "confidence": FIELD_CONFIDENCE_HIGH,
        "source_function": "EVTSYSTEM_CheckCondition",
        "notes": "Checks UICHOICE focus index and choice id after a branch choice.",
    },
}

EVENT_COMMAND_FLAG_BITS: list[dict[str, Any]] = [
    {
        "scope": "EVTCMDBASE",
        "offset": 0,
        "width": 1,
        "name": "flag_bit_0",
        "confidence": FIELD_CONFIDENCE_LOW,
        "source_function": "EVTSYSTEM_Process",
        "read_kind": "bit",
        "notes": "Consulted inside the alternate pre-dispatch path entered when flag_bit_1 is set.",
    },
    {
        "scope": "EVTCMDBASE",
        "offset": 0,
        "width": 1,
        "name": "flag_bit_1",
        "confidence": FIELD_CONFIDENCE_MEDIUM,
        "source_function": "EVTSYSTEM_Process",
        "read_kind": "bit",
        "notes": "Routes opcode handling through the alternate pre-dispatch path when EVT info bit0 is set.",
    },
]

TEXT_OPCODE_BASELINES = {
    "dialogue": 4329,
    "narration": 101,
    "choice": 69,
    "overlay_text": 14,
}


def raw_record_payload(record: bytes) -> dict[str, Any]:
    return {
        "raw_bytes": list(record),
        "raw_u16": [
            struct.unpack_from("<H", record, offset)[0]
            for offset in range(0, len(record) - (len(record) % 2), 2)
        ],
        "raw_u32": [
            struct.unpack_from("<I", record, offset)[0]
            for offset in range(0, len(record) - 3, 4)
        ],
    }


def field_def(
    table_name: str,
    offset: int,
    width: int,
    name: str,
    confidence: str,
    source_function: str,
    read_kind: str,
    notes: str,
) -> dict[str, Any]:
    return {
        "scope": table_name,
        "offset": offset,
        "width": width,
        "name": name,
        "confidence": confidence,
        "source_function": source_function,
        "read_kind": read_kind,
        "notes": notes,
    }


def command_flag_lookup(table_export: dict[str, Any]) -> dict[int, int]:
    return {entry["opcode"]: entry["flags"] for entry in table_export["commands"]}


def decode_condition_record(condition_id: int, record: bytes) -> dict[str, Any]:
    cond_type = read_u8(record, 0)
    raw_u16 = read_u16(record, 1)
    raw_u32 = read_u32(record, 3)
    catalog = EVENT_CONDITION_TYPE_CATALOG.get(cond_type)
    entry: dict[str, Any] = {
        "condition_id": condition_id,
        "cond_type": cond_type,
        "raw_u16": raw_u16,
        "raw_u32": raw_u32,
        "raw_bytes": list(record),
        "verified_by": catalog["source_function"] if catalog else None,
    }
    if catalog:
        entry["type_name"] = catalog["name"]
        entry["confidence"] = catalog["confidence"]
        entry["notes"] = catalog["notes"]

    if cond_type == 0:
        entry["decoded"] = {
            "map_id": raw_u16,
            "min_x": (raw_u32 >> 24) & 0xFF,
            "min_y": (raw_u32 >> 16) & 0xFF,
            "max_x": (raw_u32 >> 8) & 0xFF,
            "max_y": raw_u32 & 0xFF,
        }
    elif cond_type == 1:
        entry["decoded"] = {"expected_state": raw_u16, "event_id": raw_u32}
    elif cond_type == 2:
        entry["decoded"] = {"allowed_state_bits": raw_u16, "quest_id": raw_u32}
    elif cond_type == 3:
        entry["decoded"] = {"required_caller_kind": 2, "object_type": raw_u16, "npc_id": raw_u32}
    elif cond_type == 4:
        entry["decoded"] = {"required_caller_kind": 7, "object_type": raw_u16, "object_id": raw_u32}
    elif cond_type == 6:
        entry["decoded"] = {
            "required_caller_kind": 5,
            "object_type": raw_u16 & 0xFF,
            "object_id": raw_u32 & 0xFFFF,
        }
    elif cond_type == 7:
        entry["decoded"] = {"required_caller_kind": 6}
    elif cond_type == 8:
        entry["decoded"] = {"required_caller_kind": 8, "focus_index": raw_u16, "choice_id": raw_u32}
    return entry


def build_event_command_flags(tables: list[bytes]) -> dict[str, Any]:
    _, record_count, record_size, body = get_game_table(tables, "EVTCMDBASE")
    entries: list[dict[str, Any]] = []
    for opcode, record in iter_record_slices(record_count, record_size, body):
        flags = read_u8(record, 0)
        entries.append({
            "opcode": opcode,
            "flags": flags,
            "bits": {f"bit_{bit}": bool(flags & (1 << bit)) for bit in range(8)},
            "verified_bits": EVENT_COMMAND_FLAG_BITS,
            "verified_kind": EVENT_OPCODE_KINDS.get(opcode),
        })
    return {
        "description": "Raw EVTCMDBASE opcode flag bytes with verified runtime bit usage",
        "source_files": ["game.dat.jpg"],
        "record_count": record_count,
        "record_size": record_size,
        "commands": entries,
    }


def event_preview_text(commands: list[dict[str, Any]]) -> str:
    for command in commands:
        if command.get("choice"):
            prompt = command["choice"].get("prompt")
            if prompt:
                return prompt
        plain_text = command.get("plain_text")
        if plain_text:
            return plain_text
        text = command.get("text")
        if text:
            return text
    return ""


def build_event_exports(
    assets_dir: Path,
    tables: list[bytes],
    text_records: list[str],
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    flag_payload = build_event_command_flags(tables)
    flag_map = command_flag_lookup(flag_payload)
    _, event_count, event_size, event_body = get_game_table(tables, "EVTINFOBASE")
    _, cond_count, cond_size, cond_body = get_game_table(tables, "EVTCONDBASE")
    choice_sets = build_choice_relations(tables, text_records)
    choice_lookup = {entry["choice_id"]: entry for entry in choice_sets["entries"]}
    npc_name_map = build_npc_name_map(tables, text_records)
    monster_name_map = build_monster_name_map(tables, text_records)

    event_blob = load_resource_blob(assets_dir, "eventdata")
    data_count = read_u32(event_blob, 0)
    data_size = read_u16(event_blob, 4)
    data_body = event_blob[6:]
    if data_size != 10:
        raise ValueError(f"unexpected eventdata record size: {data_size}")

    opcode_counts: Counter[int] = Counter()
    verified_kind_counts: Counter[str] = Counter()
    condition_type_counts: Counter[str] = Counter()
    flattened_conditions: list[dict[str, Any]] = []
    events: list[dict[str, Any]] = []

    for event_index, event_record in iter_record_slices(event_count, event_size, event_body):
        condition_start = read_u16(event_record, 0)
        condition_count_value = read_u8(event_record, 2)
        data_start = read_u16(event_record, 3)
        command_count = read_u16(event_record, 5)
        flags = read_u8(event_record, 7)

        conditions: list[dict[str, Any]] = []
        actual_condition_count = min(condition_count_value, max(0, cond_count - condition_start))
        for local_index in range(actual_condition_count):
            condition_id = condition_start + local_index
            record = get_record_slice(cond_body, cond_size, condition_id)
            if record is None:
                break
            condition = decode_condition_record(condition_id, record)
            condition["event_index"] = event_index
            condition["event_local_index"] = local_index
            conditions.append(condition)
            flattened_conditions.append(condition)
            condition_type_counts[condition.get("type_name", f"unknown_{condition['cond_type']}")] += 1

        commands: list[dict[str, Any]] = []
        actual_command_count = min(command_count, max(0, data_count - data_start))
        for sequence in range(actual_command_count):
            command_index = data_start + sequence
            record = get_record_slice(data_body, data_size, command_index)
            if record is None:
                break
            opcode = read_u8(record, 0)
            arg0 = read_u8(record, 1)
            arg1 = read_u16(record, 2)
            param = read_u32(record, 4)
            text_id = read_u16(record, 8)
            text = get_text(text_records, text_id)
            verified_kind = EVENT_OPCODE_KINDS.get(opcode)
            command: dict[str, Any] = {
                "sequence": sequence,
                "command_index": command_index,
                "opcode": opcode,
                "cmd_flags": flag_map.get(opcode),
                "arg0": arg0,
                "arg1": arg1,
                "param": param,
                "text_id": text_id,
                "text": text,
                "plain_text": strip_text_markup(text) if text else "",
                "verified_kind": verified_kind,
            }
            if opcode == 2:
                command["speaker"] = resolve_event_speaker(arg0, arg1, npc_name_map, monster_name_map)
            if opcode == 52:
                command["choice_id"] = arg0
                command["choice"] = choice_lookup.get(arg0)
            commands.append(command)
            opcode_counts[opcode] += 1
            if verified_kind:
                verified_kind_counts[verified_kind] += 1

        events.append({
            "event_index": event_index,
            "condition_start": condition_start,
            "condition_count": condition_count_value,
            "data_start": data_start,
            "command_count": command_count,
            "flags": flags,
            "conditions": conditions,
            "commands": commands,
            "preview_text": event_preview_text(commands),
        })

    events_payload = {
        "description": "Reverse-engineered event metadata, conditions, and command streams",
        "language": TARGET_LANGUAGE,
        "source_files": ["game.dat.jpg", "eventdata.dat.jpg", f"{TARGET_MEMORYTEXT_STEM}.dat.jpg"],
        "event_table_count": event_count,
        "event_record_size": event_size,
        "condition_record_count": cond_count,
        "condition_record_size": cond_size,
        "eventdata_record_count": data_count,
        "eventdata_record_size": data_size,
        "opcode_counts": dict(sorted(opcode_counts.items())),
        "verified_kind_counts": dict(sorted(verified_kind_counts.items())),
        "events_with_commands": sum(1 for event in events if event["commands"]),
        "events": events,
    }
    conditions_payload = {
        "description": "Flattened EVTCONDBASE records referenced by events",
        "source_files": ["game.dat.jpg"],
        "record_count": len(flattened_conditions),
        "condition_type_counts": dict(sorted(condition_type_counts.items())),
        "type_catalog": {
            str(cond_type): {
                **catalog,
                "cond_type": cond_type,
            }
            for cond_type, catalog in EVENT_CONDITION_TYPE_CATALOG.items()
        },
        "conditions": flattened_conditions,
    }
    return events_payload, conditions_payload, flag_payload


TABLE_FIELD_DEFS: dict[str, list[dict[str, Any]]] = {
    "STATUSDICEBASE": [
        {
            "offset": offset,
            "width": 4,
            "name": f"dice_slot_{index}",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "STATUSDICE_Roll",
            "read_kind": "u32",
            "notes": "One of the six dice value slots consumed by STATUSDICE_Roll*.",
        }
        for index, offset in enumerate((0, 4, 8, 12, 16, 20))
    ],
    "MONDATABASE": [
        {
            "offset": 5,
            "width": 1,
            "name": "monster_u8_0x05",
            "confidence": FIELD_CONFIDENCE_MEDIUM,
            "source_function": "CHAR_UpdateAttrFromMonster",
            "read_kind": "u8",
            "notes": "Consumed directly from MONDATABASE+0x05.",
        },
        {
            "offset": 6,
            "width": 1,
            "name": "monster_u8_0x06",
            "confidence": FIELD_CONFIDENCE_MEDIUM,
            "source_function": "CHAR_UpdateAttrFromMonster",
            "read_kind": "u8",
            "notes": "Consumed directly from MONDATABASE+0x06.",
        },
        {
            "offset": 7,
            "width": 4,
            "name": "monster_u32_0x07",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "CHAR_UpdateAttrFromMonster",
            "read_kind": "u32",
            "notes": "Consumed directly from MONDATABASE+0x07.",
        },
        {
            "offset": 11,
            "width": 2,
            "name": "monster_u16_0x0B",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "CHAR_UpdateAttrFromMonster",
            "read_kind": "u16",
            "notes": "Consumed directly from MONDATABASE+0x0B.",
        },
        {
            "offset": 15,
            "width": 1,
            "name": "monster_scaled_u8_0x0F",
            "confidence": FIELD_CONFIDENCE_MEDIUM,
            "source_function": "CHAR_UpdateAttrFromMonster",
            "read_kind": "u8",
            "notes": "Converted into stat output by CHAR_UpdateAttrFromMonster.",
        },
        {
            "offset": 16,
            "width": 1,
            "name": "monster_scaled_u8_0x10",
            "confidence": FIELD_CONFIDENCE_MEDIUM,
            "source_function": "CHAR_UpdateAttrFromMonster",
            "read_kind": "u8",
            "notes": "Converted into stat output by CHAR_UpdateAttrFromMonster.",
        },
        {
            "offset": 17,
            "width": 1,
            "name": "monster_scaled_u8_0x11",
            "confidence": FIELD_CONFIDENCE_MEDIUM,
            "source_function": "CHAR_UpdateAttrFromMonster",
            "read_kind": "u8",
            "notes": "Converted into stat output by CHAR_UpdateAttrFromMonster.",
        },
        {
            "offset": 18,
            "width": 1,
            "name": "monster_scaled_u8_0x12",
            "confidence": FIELD_CONFIDENCE_MEDIUM,
            "source_function": "CHAR_UpdateAttrFromMonster",
            "read_kind": "u8",
            "notes": "Converted into stat output by CHAR_UpdateAttrFromMonster.",
        },
        {
            "offset": 19,
            "width": 1,
            "name": "monster_scaled_u8_0x13",
            "confidence": FIELD_CONFIDENCE_MEDIUM,
            "source_function": "CHAR_UpdateAttrFromMonster",
            "read_kind": "u8",
            "notes": "Converted into stat output by CHAR_UpdateAttrFromMonster.",
        },
        {
            "offset": 20,
            "width": 1,
            "name": "monster_scaled_u8_0x14",
            "confidence": FIELD_CONFIDENCE_MEDIUM,
            "source_function": "CHAR_UpdateAttrFromMonster",
            "read_kind": "u8",
            "notes": "Converted into stat output by CHAR_UpdateAttrFromMonster.",
        },
        {
            "offset": 34,
            "width": 1,
            "name": "mon_skill_start",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "MONSTERAI_Init",
            "read_kind": "s8",
            "notes": "Start index into MONSKILLBASE for monster AI skills.",
        },
        {
            "offset": 35,
            "width": 1,
            "name": "mon_skill_count",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "MONSTERAI_Init",
            "read_kind": "s8",
            "notes": "Number of MONSKILLBASE rows linked from the monster record.",
        },
        {
            "offset": 36,
            "width": 1,
            "name": "monster_u8_0x24",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "CHAR_UpdateAttrFromMonster",
            "read_kind": "u8",
            "notes": "Consumed directly from MONDATABASE+0x24.",
        },
    ],
    "MONSKILLBASE": [
        {
            "offset": 1,
            "width": 1,
            "name": "action_id",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "CHAR_LearnMonSkillSheet",
            "read_kind": "u8",
            "notes": "Action/skill id copied into the learned monster skill entry.",
        },
    ],
    "BUFFDATABASE": [
        {
            "offset": 4,
            "width": 1,
            "name": "unit_start",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "CHAR_UpdateAttrFromBuff",
            "read_kind": "u8",
            "notes": "Start index into BUFFUNITBASE.",
        },
        {
            "offset": 5,
            "width": 1,
            "name": "unit_count",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "CHAR_UpdateAttrFromBuff",
            "read_kind": "u8",
            "notes": "Number of BUFFUNITBASE rows referenced by the buff.",
        },
    ],
    "BUFFUNITBASE": [
        {
            "offset": 0,
            "width": 1,
            "name": "target_kind",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "CHAR_UpdateAttrFromBuff",
            "read_kind": "s8",
            "notes": "Filter byte checked before applying the buff-unit effect.",
        },
        {
            "offset": 1,
            "width": 2,
            "name": "target_attr_id",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "CHAR_UpdateAttrFromBuff",
            "read_kind": "s16",
            "notes": "Attribute id matched against the requested attribute update.",
        },
        {
            "offset": 3,
            "width": 1,
            "name": "operation",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "CHAR_UpdateAttrFromBuff",
            "read_kind": "s8",
            "notes": "Operation selector forwarded into UTIL_Calculate.",
        },
        {
            "offset": 4,
            "width": 2,
            "name": "formula_text_id_e",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "BUFFUNIT_GetValue",
            "read_kind": "u16",
            "notes": "Expression text id resolved through MEMORYTEXT_GetText_E.",
        },
    ],
    "ACTDATABASE": [
        {
            "offset": 0,
            "width": 2,
            "name": "name_text_id",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "UIDesc_MakeSkill",
            "read_kind": "u16",
            "notes": "Primary skill/action name text id.",
        },
        {
            "offset": 2,
            "width": 1,
            "name": "action_kind",
            "confidence": FIELD_CONFIDENCE_MEDIUM,
            "source_function": "MONSTERAI_RunAIProc",
            "read_kind": "u8",
            "notes": "Read before AI-specific action filtering.",
        },
        {
            "offset": 11,
            "width": 1,
            "name": "flags",
            "confidence": FIELD_CONFIDENCE_MEDIUM,
            "source_function": "UIDesc_MakeSkill",
            "read_kind": "u8",
            "notes": "Bit 2 is checked while composing the skill description UI.",
        },
        {
            "offset": 19,
            "width": 1,
            "name": "act_unit_count",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "MONSTERAI_RunAIProc",
            "read_kind": "u8",
            "notes": "Loop count forwarded to ACTDATA_GetActUnit.",
        },
        {
            "offset": 31,
            "width": 1,
            "name": "desc_style",
            "confidence": FIELD_CONFIDENCE_LOW,
            "source_function": "UIDesc_MakeSkill",
            "read_kind": "s8",
            "notes": "Signed style byte used while assembling description text.",
        },
    ],
    "QUESTINFOBASE": [
        {
            "offset": 26,
            "width": 2,
            "name": "reward_start",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "QUESTSYSTEM_ApplyReward",
            "read_kind": "u16",
            "notes": "Start index into QUESTREWARDBASE.",
        },
        {
            "offset": 28,
            "width": 1,
            "name": "reward_count",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "QUESTSYSTEM_ApplyReward",
            "read_kind": "u8",
            "notes": "Number of QUESTREWARDBASE rows linked to the quest.",
        },
    ],
    "QUESTREWARDBASE": [
        {
            "offset": 0,
            "width": 2,
            "name": "item_id",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "QUESTSYSTEM_ApplyReward",
            "read_kind": "u16",
            "notes": "Item id awarded by the reward entry.",
        },
        {
            "offset": 2,
            "width": 2,
            "name": "quantity",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "QUESTSYSTEM_ApplyReward",
            "read_kind": "u16",
            "notes": "Item quantity awarded by the reward entry.",
        },
        {
            "offset": 4,
            "width": 1,
            "name": "class_mask",
            "confidence": FIELD_CONFIDENCE_HIGH,
            "source_function": "QUESTSYSTEM_ApplyReward",
            "read_kind": "u8",
            "notes": "Bit mask checked against the main player's class before granting the reward.",
        },
    ],
}


def add_field_catalog(entries: list[dict[str, Any]], table_name: str) -> list[dict[str, Any]]:
    catalog = list(entries)
    for item in TABLE_FIELD_DEFS.get(table_name, []):
        catalog.append(field_def(table_name, **item))
    return catalog


def build_statusdice_export(tables: list[bytes]) -> dict[str, Any]:
    _, record_count, record_size, body = get_game_table(tables, "STATUSDICEBASE")
    records: list[dict[str, Any]] = []
    for record_id, record in iter_record_slices(record_count, record_size, body):
        records.append({
            "record_id": record_id,
            **raw_record_payload(record),
            "verified_values": {
                f"dice_slot_{slot}": read_u32(record, offset)
                for slot, offset in enumerate((0, 4, 8, 12, 16, 20))
            },
        })
    return {
        "table_name": "STATUSDICEBASE",
        "record_count": record_count,
        "record_size": record_size,
        "source_functions": [
            "STATUSDICE_Roll",
            "STATUSDICE_RollRevision",
            "STATUSDICE_RollForMercenary",
        ],
        "verified_fields": TABLE_FIELD_DEFS["STATUSDICEBASE"],
        "records": records,
    }


def build_monster_exports(tables: list[bytes], text_records: list[str]) -> list[dict[str, Any]]:
    monster_exports: list[dict[str, Any]] = []

    _, record_count, record_size, body = get_game_table(tables, "MONDATABASE")
    monster_records: list[dict[str, Any]] = []
    for monster_id, record in iter_record_slices(record_count, record_size, body):
        monster_records.append({
            "record_id": monster_id,
            "name_text_id": read_u16(record, 0),
            "name": get_text(text_records, read_u16(record, 0)),
            **raw_record_payload(record),
            "verified_values": {
                "monster_u8_0x05": read_u8(record, 5),
                "monster_u8_0x06": read_u8(record, 6),
                "monster_u32_0x07": read_u32(record, 7),
                "monster_u16_0x0B": read_u16(record, 11),
                "monster_scaled_u8_0x0F": read_u8(record, 15),
                "monster_scaled_u8_0x10": read_u8(record, 16),
                "monster_scaled_u8_0x11": read_u8(record, 17),
                "monster_scaled_u8_0x12": read_u8(record, 18),
                "monster_scaled_u8_0x13": read_u8(record, 19),
                "monster_scaled_u8_0x14": read_u8(record, 20),
                "mon_skill_start": read_s8(record, 34),
                "mon_skill_count": read_s8(record, 35),
                "monster_u8_0x24": read_u8(record, 36),
            },
        })
    monster_exports.append({
        "table_name": "MONDATABASE",
        "record_count": record_count,
        "record_size": record_size,
        "source_functions": [
            "CHAR_UpdateAttrFromMonster",
            "MONSTERAI_Init",
            "CHAR_LearnMonSkillSheet",
        ],
        "verified_fields": TABLE_FIELD_DEFS["MONDATABASE"],
        "records": monster_records,
    })

    _, record_count, record_size, body = get_game_table(tables, "MONSKILLBASE")
    monskill_records: list[dict[str, Any]] = []
    for record_id, record in iter_record_slices(record_count, record_size, body):
        monskill_records.append({
            "record_id": record_id,
            **raw_record_payload(record),
            "verified_values": {
                "action_id": read_u8(record, 1),
            },
        })
    monster_exports.append({
        "table_name": "MONSKILLBASE",
        "record_count": record_count,
        "record_size": record_size,
        "source_functions": [
            "MONSTERAI_Init",
            "CHAR_LearnMonSkillSheet",
        ],
        "verified_fields": TABLE_FIELD_DEFS["MONSKILLBASE"],
        "records": monskill_records,
    })
    return monster_exports


def build_buff_exports(
    tables: list[bytes],
    formula_records_e: list[str],
) -> list[dict[str, Any]]:
    buff_exports: list[dict[str, Any]] = []

    _, record_count, record_size, body = get_game_table(tables, "BUFFDATABASE")
    buff_records: list[dict[str, Any]] = []
    for buff_id, record in iter_record_slices(record_count, record_size, body):
        buff_records.append({
            "record_id": buff_id,
            **raw_record_payload(record),
            "verified_values": {
                "unit_start": read_u8(record, 4),
                "unit_count": read_u8(record, 5),
            },
        })
    buff_exports.append({
        "table_name": "BUFFDATABASE",
        "record_count": record_count,
        "record_size": record_size,
        "source_functions": [
            "CHAR_UpdateAttrFromBuff",
            "CHAR_CreateBuff",
            "CHAR_InitializeFromBuff",
        ],
        "verified_fields": TABLE_FIELD_DEFS["BUFFDATABASE"],
        "records": buff_records,
    })

    _, record_count, record_size, body = get_game_table(tables, "BUFFUNITBASE")
    buff_unit_records: list[dict[str, Any]] = []
    for unit_id, record in iter_record_slices(record_count, record_size, body):
        formula_text_id = read_u16(record, 4)
        buff_unit_records.append({
            "record_id": unit_id,
            **raw_record_payload(record),
            "verified_values": {
                "target_kind": read_s8(record, 0),
                "target_attr_id": struct.unpack_from("<h", record, 1)[0],
                "operation": read_s8(record, 3),
                "formula_text_id_e": formula_text_id,
                "formula_text_e": get_text(formula_records_e, formula_text_id),
            },
        })
    buff_exports.append({
        "table_name": "BUFFUNITBASE",
        "record_count": record_count,
        "record_size": record_size,
        "source_functions": [
            "CHAR_UpdateAttrFromBuff",
            "BUFFUNIT_GetValue",
        ],
        "verified_fields": TABLE_FIELD_DEFS["BUFFUNITBASE"],
        "records": buff_unit_records,
    })
    return buff_exports


def build_act_export(tables: list[bytes], text_records: list[str]) -> dict[str, Any]:
    _, record_count, record_size, body = get_game_table(tables, "ACTDATABASE")
    records: list[dict[str, Any]] = []
    for record_id, record in iter_record_slices(record_count, record_size, body):
        name_text_id = read_u16(record, 0)
        records.append({
            "record_id": record_id,
            "name_text_id": name_text_id,
            "name": get_text(text_records, name_text_id),
            **raw_record_payload(record),
            "verified_values": {
                "name_text_id": name_text_id,
                "action_kind": read_u8(record, 2),
                "flags": read_u8(record, 11),
                "act_unit_count": read_u8(record, 19),
                "desc_style": read_s8(record, 31),
            },
        })
    return {
        "table_name": "ACTDATABASE",
        "record_count": record_count,
        "record_size": record_size,
        "source_functions": [
            "UIDesc_MakeSkill",
            "MONSTERAI_RunAIProc",
        ],
        "verified_fields": TABLE_FIELD_DEFS["ACTDATABASE"],
        "records": records,
    }


def build_item_name_map(tables: list[bytes], text_records: list[str]) -> dict[int, str]:
    _, record_count, record_size, body = get_game_table(tables, "ITEMDATABASE")
    item_name_map: dict[int, str] = {}
    for item_id, record in iter_record_slices(record_count, record_size, body):
        item_name_map[item_id] = get_text(text_records, read_u16(record, 0))
    return item_name_map


def build_quest_reward_export(tables: list[bytes], text_records: list[str]) -> list[dict[str, Any]]:
    item_name_map = build_item_name_map(tables, text_records)
    quest_texts = {
        entry["quest_id"]: entry
        for entry in build_quest_text_relations(tables, text_records)["entries"]
    }

    _, quest_count, quest_size, quest_body = get_game_table(tables, "QUESTINFOBASE")
    reward_links: dict[int, list[dict[str, Any]]] = defaultdict(list)
    quest_reward_sets: list[dict[str, Any]] = []
    for quest_id, record in iter_record_slices(quest_count, quest_size, quest_body):
        reward_start = read_u16(record, 26)
        reward_count = read_u8(record, 28)
        if reward_count == 0:
            continue
        rewards = []
        for reward_index in range(reward_start, reward_start + reward_count):
            reward_links[reward_index].append({
                "quest_id": quest_id,
                "title": quest_texts.get(quest_id, {}).get("title", ""),
            })
            rewards.append(reward_index)
        quest_reward_sets.append({
            "quest_id": quest_id,
            "title": quest_texts.get(quest_id, {}).get("title", ""),
            "reward_start": reward_start,
            "reward_count": reward_count,
            "reward_record_ids": rewards,
        })

    _, reward_count, reward_size, reward_body = get_game_table(tables, "QUESTREWARDBASE")
    reward_records: list[dict[str, Any]] = []
    for reward_id, record in iter_record_slices(reward_count, reward_size, reward_body):
        item_id = read_u16(record, 0)
        quantity = read_u16(record, 2)
        class_mask = read_u8(record, 4)
        reward_records.append({
            "record_id": reward_id,
            "item_id": item_id,
            "item_name": item_name_map.get(item_id, ""),
            "quantity": quantity,
            "class_mask": class_mask,
            "class_mask_bits": [bit for bit in range(8) if class_mask & (1 << bit)],
            "related_quests": reward_links.get(reward_id, []),
            **raw_record_payload(record),
            "verified_values": {
                "item_id": item_id,
                "quantity": quantity,
                "class_mask": class_mask,
            },
        })

    quest_info_records: list[dict[str, Any]] = []
    for quest_id, record in iter_record_slices(quest_count, quest_size, quest_body):
        quest_info_records.append({
            "record_id": quest_id,
            "title_text_id": read_u16(record, 2),
            "title": quest_texts.get(quest_id, {}).get("title", ""),
            **raw_record_payload(record),
            "verified_values": {
                "reward_start": read_u16(record, 26),
                "reward_count": read_u8(record, 28),
            },
        })

    return [
        {
            "table_name": "QUESTINFOBASE",
            "record_count": quest_count,
            "record_size": quest_size,
            "source_functions": [
                "QUESTSYSTEM_ApplyReward",
                "QUESTSYTEM_GetRewardCount",
            ],
            "verified_fields": TABLE_FIELD_DEFS["QUESTINFOBASE"],
            "records": quest_info_records,
            "quest_reward_sets": quest_reward_sets,
        },
        {
            "table_name": "QUESTREWARDBASE",
            "record_count": reward_count,
            "record_size": reward_size,
            "source_functions": [
                "QUESTSYSTEM_ApplyReward",
                "QUESTSYTEM_GetRewardCount",
            ],
            "verified_fields": TABLE_FIELD_DEFS["QUESTREWARDBASE"],
            "records": reward_records,
        },
    ]


def build_game_values_core(
    tables: list[bytes],
    text_records: list[str],
    formula_records_e: list[str],
) -> dict[str, Any]:
    table_exports: list[dict[str, Any]] = []
    table_exports.append(build_statusdice_export(tables))
    table_exports.extend(build_monster_exports(tables, text_records))
    table_exports.extend(build_buff_exports(tables, formula_records_e))
    table_exports.append(build_act_export(tables, text_records))
    table_exports.extend(build_quest_reward_export(tables, text_records))
    return {
        "description": "Core gameplay value tables recovered from static resources and verified against IDA call sites",
        "language": TARGET_LANGUAGE,
        "source_files": [
            "game.dat.jpg",
            f"{TARGET_MEMORYTEXT_STEM}.dat.jpg",
            f"{FORMULA_MEMORYTEXT_STEM}.dat.jpg",
        ],
        "tables": table_exports,
    }


def build_field_catalog() -> dict[str, Any]:
    fields = list(EVENT_INFO_FIELDS)
    fields.extend(EVENT_COMMAND_FIELDS)
    fields.extend(EVENT_COMMAND_FLAG_BITS)
    for table_name, entries in TABLE_FIELD_DEFS.items():
        for item in entries:
            fields.append(field_def(table_name, **item))
    return {
        "description": "Verified field catalog collected from static tables and IDA-backed call-site analysis",
        "fields": fields,
    }


def verify_event_offsets(tables: list[bytes], events_payload: dict[str, Any]) -> None:
    _, _, record_size, body = get_game_table(tables, "EVTINFOBASE")
    for event_index in [0, 1, 2, 3, 4]:
        record = get_record_slice(body, record_size, event_index)
        if record is None:
            raise ValueError(f"missing EVTINFOBASE sample record {event_index}")
        exported = events_payload["events"][event_index]
        expected = {
            "condition_start": read_u16(record, 0),
            "condition_count": read_u8(record, 2),
            "data_start": read_u16(record, 3),
            "command_count": read_u16(record, 5),
            "flags": read_u8(record, 7),
        }
        actual = {key: exported[key] for key in expected}
        if actual != expected:
            raise ValueError(f"event offset verification failed for event {event_index}: {actual} != {expected}")


def verify_condition_decoding(conditions_payload: dict[str, Any]) -> None:
    per_type_seen: Counter[int] = Counter()
    for condition in conditions_payload["conditions"]:
        cond_type = condition["cond_type"]
        if cond_type not in EVENT_CONDITION_TYPE_CATALOG or cond_type == 7:
            continue
        if per_type_seen[cond_type] >= 3:
            continue
        if condition.get("decoded") is None:
            raise ValueError(f"condition type {cond_type} missing decoded payload")
        per_type_seen[cond_type] += 1
    for cond_type in (0, 1, 2, 3, 4, 6, 8):
        if per_type_seen[cond_type] == 0:
            raise ValueError(f"missing sample verification for condition type {cond_type}")


def verify_game_value_tables(values_payload: dict[str, Any]) -> None:
    tables = {table["table_name"]: table for table in values_payload["tables"]}
    statusdice = tables["STATUSDICEBASE"]
    if statusdice["record_count"] != 30:
        raise ValueError(f"unexpected STATUSDICEBASE count: {statusdice['record_count']}")
    if any(len(record["verified_values"]) != 6 for record in statusdice["records"]):
        raise ValueError("STATUSDICEBASE export does not contain 6 dice slots per record")

    mondatabase_fields = {field["offset"] for field in tables["MONDATABASE"]["verified_fields"]}
    required_mon_offsets = {5, 6, 7, 11, 15, 16, 17, 18, 19, 20, 36}
    if not required_mon_offsets.issubset(mondatabase_fields):
        raise ValueError("MONDATABASE verified field coverage is incomplete")

    buffdatabase = tables["BUFFDATABASE"]
    buffunit = tables["BUFFUNITBASE"]
    max_buffunit_index = buffunit["record_count"]
    for record in buffdatabase["records"]:
        start = record["verified_values"]["unit_start"]
        count = record["verified_values"]["unit_count"]
        if start + count > max_buffunit_index:
            raise ValueError(f"BUFFDATABASE range out of bounds for record {record['record_id']}")

    questreward = tables["QUESTREWARDBASE"]
    if questreward["record_count"] != 394:
        raise ValueError(f"unexpected QUESTREWARDBASE count: {questreward['record_count']}")


def verify_field_catalog(field_catalog: dict[str, Any]) -> None:
    for field in field_catalog["fields"]:
        if not field.get("source_function"):
            raise ValueError(f"field catalog entry missing source_function: {field}")


def verify_event_text_regression(events_payload: dict[str, Any]) -> None:
    counts = events_payload["verified_kind_counts"]
    for kind, minimum in TEXT_OPCODE_BASELINES.items():
        actual = counts.get(kind, 0)
        if actual < minimum:
            raise ValueError(f"text opcode regression for {kind}: {actual} < {minimum}")


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def main() -> None:
    assets_dir = find_assets_dir()
    print(f"Assets directory: {assets_dir}")

    print(f"Loading {TARGET_MEMORYTEXT_STEM}.dat.jpg...")
    text_records = load_memorytext_records(assets_dir, TARGET_MEMORYTEXT_STEM)
    print(f"  zh-Hans records={len(text_records):,}")

    print(f"Loading {FORMULA_MEMORYTEXT_STEM}.dat.jpg...")
    formula_records_e = load_memorytext_records(assets_dir, FORMULA_MEMORYTEXT_STEM)
    print(f"  formula records={len(formula_records_e):,}")

    print("Loading game.dat.jpg...")
    tables = load_game_tables(assets_dir)
    print(f"  tables={len(tables)}")

    print("Building event exports...")
    events_payload, conditions_payload, flag_payload = build_event_exports(assets_dir, tables, text_records)
    print(
        "  events="
        f"{events_payload['event_table_count']}, commands={events_payload['eventdata_record_count']}, "
        f"text_kinds={events_payload['verified_kind_counts']}"
    )

    print("Building core gameplay value exports...")
    values_payload = build_game_values_core(tables, text_records, formula_records_e)
    print(f"  exported tables={len(values_payload['tables'])}")

    print("Building field catalog...")
    field_catalog = build_field_catalog()
    print(f"  verified fields={len(field_catalog['fields'])}")

    print("Running validation...")
    verify_event_offsets(tables, events_payload)
    verify_condition_decoding(conditions_payload)
    verify_game_value_tables(values_payload)
    verify_field_catalog(field_catalog)
    verify_event_text_regression(events_payload)
    print("  validation passed")

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    events_path = OUTPUT_DIR / "events.json"
    conditions_path = OUTPUT_DIR / "event_conditions.json"
    flag_path = OUTPUT_DIR / "event_command_flags.json"
    values_path = OUTPUT_DIR / "game_values_core.json"
    field_catalog_path = OUTPUT_DIR / "field_catalog.json"

    print("Writing datasets...")
    write_json(events_path, events_payload)
    write_json(conditions_path, conditions_payload)
    write_json(flag_path, flag_payload)
    write_json(values_path, values_payload)
    write_json(field_catalog_path, field_catalog)
    print(f"  {events_path}")
    print(f"  {conditions_path}")
    print(f"  {flag_path}")
    print(f"  {values_path}")
    print(f"  {field_catalog_path}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Generate the extension bag type-capacity table from ITEMSTATICBASE."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "apk/static-data/json/tables/ITEMSTATICBASE.json"
OUTPUT = (ROOT / "module/app/src/main/cpp/plugins/extension_bag/model/bag_catalog.inc")


def build_table() -> list[int]:
    data = json.loads(SOURCE.read_text(encoding="utf-8"))
    records = data.get("records")
    if not isinstance(records, list):
        raise ValueError("ITEMSTATICBASE.records must be an array")

    capacities = {0: 0}
    for position, record in enumerate(records):
        values = record.get("u16") if isinstance(record, dict) else None
        if not isinstance(values, list) or len(values) < 2:
            raise ValueError(f"ITEMSTATICBASE.records[{position}].u16 must contain BagType and capacity")
        bag_type, capacity = values[0], values[1]
        if not isinstance(bag_type, int) or not isinstance(capacity, int):
            raise ValueError(f"ITEMSTATICBASE.records[{position}] values must be integers")
        if bag_type <= 0 or bag_type > 4 or capacity < 0 or capacity > 16:
            raise ValueError(f"invalid bag record at index {position}: {bag_type} -> {capacity}")
        if bag_type in capacities:
            raise ValueError(f"duplicate BagType {bag_type}")
        capacities[bag_type] = capacity

    expected_types = list(range(5))
    if sorted(capacities) != expected_types:
        raise ValueError("ITEMSTATICBASE must define BagType values 1..4")
    return [capacities[bag_type] for bag_type in expected_types]


def main() -> None:
    values = ", ".join(str(value) for value in build_table())
    contents = f"inline constexpr std::array<uint8_t, 5> kBagTypeCapacities = {{{values}}};\n"
    OUTPUT.write_text(contents, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()

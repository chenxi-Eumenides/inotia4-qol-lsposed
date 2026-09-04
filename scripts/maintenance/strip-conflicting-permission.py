#!/usr/bin/env python3
"""Remove a target APK's self-owned custom permission from binary AndroidManifest.xml."""

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


ANDROID_MANIFEST = "AndroidManifest.xml"
CHUNK_STRING_POOL = 0x0001
CHUNK_START_ELEMENT = 0x0102
CHUNK_END_ELEMENT = 0x0103


def read_string_pool(data: bytes, chunk_offset: int) -> list[str]:
    _, header_size, chunk_size = struct.unpack_from("<HHI", data, chunk_offset)
    if chunk_size < header_size or chunk_offset + chunk_size > len(data):
        raise ValueError("invalid AndroidManifest string pool")
    string_count, _, flags, strings_start, _ = struct.unpack_from(
        "<IIIII", data, chunk_offset + 8
    )
    offsets_start = chunk_offset + header_size
    strings_base = chunk_offset + strings_start
    utf8 = bool(flags & 0x00000100)
    result: list[str] = []
    for index in range(string_count):
        relative = struct.unpack_from("<I", data, offsets_start + index * 4)[0]
        cursor = strings_base + relative
        if utf8:
            byte_length, cursor = read_utf8_length(data, cursor)
            _, cursor = read_utf8_length(data, cursor)
            result.append(data[cursor : cursor + byte_length].decode("utf-8"))
        else:
            char_length = struct.unpack_from("<H", data, cursor)[0]
            cursor += 2
            result.append(data[cursor : cursor + char_length * 2].decode("utf-16le"))
    return result


def read_utf8_length(data: bytes, cursor: int) -> tuple[int, int]:
    first = data[cursor]
    cursor += 1
    if first & 0x80:
        return ((first & 0x7F) << 8) | data[cursor], cursor + 1
    return first, cursor


def remove_permission_node(data: bytes, permission_name: str) -> bytes:
    root_type, root_header, root_size = struct.unpack_from("<HHI", data, 0)
    if root_type != 0x0003 or root_header != 8 or root_size != len(data):
        raise ValueError("invalid binary AndroidManifest.xml")

    cursor = root_header
    strings: list[str] | None = None
    start_offset: int | None = None
    end_offset: int | None = None
    while cursor < root_size:
        chunk_type, header_size, chunk_size = struct.unpack_from("<HHI", data, cursor)
        if chunk_size < header_size or cursor + chunk_size > root_size:
            raise ValueError("invalid AndroidManifest chunk")
        if chunk_type == CHUNK_STRING_POOL:
            strings = read_string_pool(data, cursor)
        elif chunk_type == CHUNK_START_ELEMENT and strings is not None:
            name_index = struct.unpack_from("<I", data, cursor + 20)[0]
            if name_index < len(strings) and strings[name_index] == "permission":
                attribute_count = struct.unpack_from("<H", data, cursor + 28)[0]
                attribute_size = struct.unpack_from("<H", data, cursor + 26)[0]
                name_value: str | None = None
                attributes = cursor + 36
                for index in range(attribute_count):
                    attribute = attributes + index * attribute_size
                    value_string = struct.unpack_from("<I", data, attribute + 8)[0]
                    value_type = struct.unpack_from("<H", data, attribute + 14)[0]
                    value_data = struct.unpack_from("<I", data, attribute + 16)[0]
                    if value_string < len(strings):
                        name_string = strings[struct.unpack_from("<I", data, attribute + 4)[0]]
                        if name_string == "name":
                            name_value = strings[value_string]
                    if name_value is None and value_type == 0x0003 and value_data < len(strings):
                        name_value = strings[value_data]
                if name_value == permission_name:
                    start_offset = cursor
        elif (
            chunk_type == CHUNK_END_ELEMENT
            and start_offset is not None
            and end_offset is None
        ):
            start_name = struct.unpack_from("<I", data, start_offset + 20)[0]
            end_name = struct.unpack_from("<I", data, cursor + 20)[0]
            if start_name == end_name:
                end_offset = cursor + chunk_size
                break
        cursor += chunk_size

    if start_offset is None or end_offset is None:
        raise ValueError(f"permission not found: {permission_name}")
    result = bytearray(data[:start_offset] + data[end_offset:])
    struct.pack_into("<I", result, 4, len(result))
    return result


def patch_apk(input_apk: Path, output_apk: Path, permission_name: str) -> None:
    input_apk = input_apk.resolve()
    output_apk = output_apk.resolve()
    shutil.copy2(input_apk, output_apk)
    with tempfile.TemporaryDirectory(prefix="manifest-patch-") as work_dir:
        manifest_path = Path(work_dir) / ANDROID_MANIFEST
        extracted = subprocess.run(
            ["unzip", "-p", str(input_apk), ANDROID_MANIFEST],
            check=True,
            stdout=subprocess.PIPE,
        )
        manifest_path.write_bytes(remove_permission_node(extracted.stdout, permission_name))
        subprocess.run(
            ["zip", "-q", "-d", str(output_apk), ANDROID_MANIFEST],
            check=True,
        )
        subprocess.run(
            ["zip", "-q", str(output_apk), ANDROID_MANIFEST],
            cwd=work_dir,
            check=True,
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input_apk", type=Path)
    parser.add_argument("output_apk", type=Path)
    parser.add_argument("permission_name")
    args = parser.parse_args()
    patch_apk(args.input_apk, args.output_apk, args.permission_name)


if __name__ == "__main__":
    main()

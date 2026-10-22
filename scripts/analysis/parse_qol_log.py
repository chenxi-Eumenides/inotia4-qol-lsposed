#!/usr/bin/env python3
"""汇总 qol.log 中的警告、错误和格式异常行。"""

import argparse
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
import re
import sys


LOG_LINE = re.compile(
    r"^(?P<timestamp>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}) "
    r"f=(?P<frame>-?\d+) (?P<level>[A-Z]) (?P<domain>\S+) (?P<content>.*)$"
)
START_LINE = re.compile(r"^=== Inotia4Qol log start .+ ===$")
ISSUE_TEXT = re.compile(
    r"\b(?:error|failed|failure|exception|unavailable)\b|"
    r"[\"']ok[\"']\s*:\s*(?:false|0)",
    re.IGNORECASE,
)
ISSUE_LEVELS = {"W", "E", "F"}


@dataclass(frozen=True)
class LogEntry:
    line_number: int
    timestamp: str
    frame: int
    level: str
    domain: str
    content: str


def parse_entry(line: str, line_number: int) -> LogEntry | None:
    """解析一行日志；非日志行返回 None。"""
    match = LOG_LINE.fullmatch(line)
    if match is None:
        return None
    return LogEntry(
        line_number=line_number,
        timestamp=match.group("timestamp"),
        frame=int(match.group("frame")),
        level=match.group("level"),
        domain=match.group("domain"),
        content=match.group("content"),
    )


def print_entry(entry: LogEntry) -> None:
    print(
        f"  L{entry.line_number} {entry.timestamp} f={entry.frame} "
        f"{entry.level} [{entry.domain}] {entry.content}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="汇总 qol.log 中的警告、错误和格式异常行")
    parser.add_argument(
        "log_file",
        nargs="?",
        type=Path,
        default=Path(".tmp/qol.log"),
        help="日志路径（默认：.tmp/qol.log）",
    )
    args = parser.parse_args()

    try:
        lines = args.log_file.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        print(f"无法读取日志 {args.log_file}: {error}", file=sys.stderr)
        return 2

    entries: list[LogEntry] = []
    malformed: list[tuple[int, str]] = []
    for line_number, line in enumerate(lines, start=1):
        if not line or START_LINE.fullmatch(line):
            continue
        entry = parse_entry(line, line_number)
        if entry is None:
            malformed.append((line_number, line))
        else:
            entries.append(entry)

    level_counts = Counter(entry.level for entry in entries)
    issues = [entry for entry in entries if entry.level in ISSUE_LEVELS]
    info_signals = [
        entry
        for entry in entries
        if entry.level == "I" and ISSUE_TEXT.search(entry.content)
    ]

    print(f"日志：{args.log_file}")
    print(f"解析日志：{len(entries)} 行；级别统计：")
    print("  " + (", ".join(f"{level}={count}" for level, count in sorted(level_counts.items())) or "无"))

    print(f"\n警告/错误（W/E/F）：{len(issues)} 条")
    if issues:
        for entry in issues:
            print_entry(entry)
    else:
        print("  无")

    print(f"\n信息级异常关键词（需人工确认）：{len(info_signals)} 条")
    if info_signals:
        for entry in info_signals:
            print_entry(entry)
    else:
        print("  无")

    print(f"\n格式异常：{len(malformed)} 行")
    for line_number, line in malformed:
        print(f"  L{line_number} {line}")

    return 1 if malformed else 0


if __name__ == "__main__":
    sys.exit(main())

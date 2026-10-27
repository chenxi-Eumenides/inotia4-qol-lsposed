#!/usr/bin/env python3
"""统一构建前静态预检入口。

默认依次更新游戏变体表，再运行全部静态检查，并汇总失败退出码。变体表生成器会
有意写回生成的 ``game_variant_table.inc``；使用 ``--only`` 可只运行指定步骤。

步骤顺序：
1. build_game_variant_table
2. check_layers、check_offsets、check_jni、check_resources
3. check_symbols、check_http_routes、check_log_policy

用法：
    uv run python scripts/maintenance/check_arch.py
    uv run python scripts/maintenance/check_arch.py --only check_jni --all
    uv run python scripts/maintenance/check_arch.py --list

`--verbose` 与 `-v` 是 `--all` 的兼容别名；总入口按子脚本能力精确转发参数。
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
ARCH_CHECKS = ("check_layers", "check_offsets", "check_jni", "check_resources")
SCRIPTS = (
    "build_game_variant_table",
    *ARCH_CHECKS,
    "check_symbols",
    "check_http_routes",
    "check_log_policy",
)
SCRIPT_PATHS = {
    **{name: HERE / f"{name}.py" for name in SCRIPTS if name != "check_log_policy"},
    "check_log_policy": ROOT / "scripts" / "verification" / "check_log_policy.py",
}
ALL_CAPABLE = frozenset(("build_game_variant_table", *ARCH_CHECKS, "check_symbols", "check_http_routes"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--freeze", action="store_true", help="把四个架构基线检查器的当前违规写入基线")
    parser.add_argument("--json", action="store_true", help="仅透传给四个架构基线检查器")
    parser.add_argument(
        "--all", "--verbose", "-v", dest="verbose", action="store_true",
        help="透传 --all，显示各脚本完整明细",
    )
    parser.add_argument("--only", default="", help="逗号分隔的脚本名子集（不含 check_ 前缀亦可）")
    parser.add_argument("--list", action="store_true", help="列出可用脚本后退出")
    args = parser.parse_args()

    if args.list:
        for name in SCRIPTS:
            print(name)
        return 0

    selected = SCRIPTS
    if args.only:
        wanted = {item.strip().removeprefix("check_") for item in args.only.split(",") if item.strip()}
        available = {name.removeprefix("check_"): name for name in SCRIPTS}
        unknown = wanted - set(available)
        if unknown:
            print(f"未知脚本：{sorted(unknown)}；可用：{list(SCRIPTS)}")
            return 2
        selected = tuple(name for name in SCRIPTS if name.removeprefix("check_") in wanted)

    failures: list[str] = []
    for name in selected:
        script = SCRIPT_PATHS[name]
        if not script.exists():
            failures.append(name)
            print(f"== {name} ==\n  缺失：{script}")
            continue
        print(f"\n########## {name} ##########", flush=True)
        passthrough: list[str] = []
        if name in ARCH_CHECKS:
            if args.freeze:
                passthrough.append("--freeze")
            if args.json:
                passthrough.append("--json")
        if args.verbose and name in ALL_CAPABLE:
            passthrough.append("--all")
        try:
            result = subprocess.run([sys.executable, str(script), *passthrough], cwd=ROOT)
        except OSError as exc:
            failures.append(name)
            print(f"  无法启动：{exc}")
            continue
        if result.returncode != 0:
            failures.append(name)

    print("\n===== 静态预检汇总 =====")
    for name in selected:
        mark = "❌" if name in failures else "✅"
        print(f"  {mark} {name}")
    if failures:
        print(f"\n失败 {len(failures)} 项：{failures}")
        if args.freeze:
            print("注意：--freeze 模式下失败通常意味着脚本自身执行异常。")
        return 1
    print("\n全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())

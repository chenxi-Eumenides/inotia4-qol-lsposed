#!/usr/bin/env python3
"""架构校验总入口（ARCHITECTURE.md §9.2 / §9.4）。

依次运行校验脚本族，汇总退出码：
- check_layers    层依赖方向 + include 环
- check_offsets   裸地址与符号头越权
- check_jni       Kotlin external ↔ native 导出
- check_resources 资源安装点与重复安装

check_symbols.py（符号 VMA 对 .so 校验）需要 APK/SO 输入且耗时，不纳入默认入口，
需要时单独运行。

用法：
    uv run python scripts/maintenance/check_arch.py                     # 全部校验（棘轮）
    uv run python scripts/maintenance/check_arch.py --freeze            # 冻结全部基线（只允许收缩）
    uv run python scripts/maintenance/check_arch.py --only check_jni --all
    uv run python scripts/maintenance/check_arch.py --list              # 仅列出脚本

`--verbose` 与 `-v` 是 `--all` 的兼容别名；总入口向子脚本传递 canonical `--all`。
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SCRIPTS = ("check_layers", "check_offsets", "check_jni", "check_resources")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--freeze", action="store_true", help="把当前违规写入共享基线快照")
    parser.add_argument("--json", action="store_true", help="透传 --json 给各脚本")
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

    passthrough: list[str] = []
    if args.freeze:
        passthrough.append("--freeze")
    if args.json:
        passthrough.append("--json")
    if args.verbose:
        passthrough.append("--all")

    failures: list[str] = []
    for name in selected:
        script = HERE / f"{name}.py"
        if not script.exists():
            failures.append(name)
            print(f"== {name} ==\n  缺失：{script}")
            continue
        print(f"\n########## {name} ##########")
        result = subprocess.run([sys.executable, str(script), *passthrough], cwd=ROOT)
        if result.returncode != 0:
            failures.append(name)

    print("\n===== 架构校验汇总 =====")
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

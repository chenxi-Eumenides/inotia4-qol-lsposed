#!/usr/bin/env python3
"""Kotlin `external fun` 与 native JNI 导出符号一致性校验（ARCHITECTURE.md §7.2）。

规则：
- jni_kotlin_missing_native：Kotlin `external fun` 在 native 侧找不到对应导出；
- jni_native_missing_kotlin：native `Java_...` 导出在 Kotlin 侧没有对应 `external fun`。

本阶段只比对方法名维度：native 导出名先按 JNI 规则把 `_1` 还原为 `_`（Kotlin 方法名里的
字面下划线在导出名中编码为 `_1`），再与 `external fun` 名比较；类名维度仅作为 detail 与
汇总输出，暂不做强校验。描述符 `jni` 表尚未落地，第三向留待后续。

用法：
    uv run python scripts/maintenance/check_jni.py            # 棘轮校验
    uv run python scripts/maintenance/check_jni.py -v --json  # 明细 + JSON 汇总
"""
from __future__ import annotations

import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import arch_common as ac  # noqa: E402

# Kotlin 侧唯一 JNI 声明文件（external fun 只在此声明，见 docs/development/architecture.md）。
KOTLIN_BRIDGE = (
    ac.ROOT / "module" / "app" / "src" / "main" / "java" / "com" / "inotia4" / "qol"
    / "NativeBridge.kt"
)
KOTLIN_BRIDGE_REL = KOTLIN_BRIDGE.relative_to(ac.ROOT).as_posix()

# JNI 导出名固定包前缀；类名与方法名以 `_` 连接，方法名内字面下划线被编码为 `_1`。
_PACKAGE = "com_inotia4_qol"
_NATIVE_SUFFIXES = {".cpp", ".inc"}

_KOTLIN_OWNER_RE = re.compile(r"\b(?:object|class)\s+([A-Za-z_][A-Za-z0-9_]*)")
_KOTLIN_EXTERNAL_RE = re.compile(r"\bexternal\s+fun\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")
_JAVA_SYMBOL_RE = re.compile(r"\bJava_([A-Za-z0-9_$]+)\s*\(")
_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)


def _strip_comments(text: str) -> str:
    """剥掉块注释与行注释（块注释替换为等量换行，保持行号可算）。"""
    text = _BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


def _is_definition(code: str, open_paren: int) -> bool:
    """从 `(` 起按括号配平前进；`)` 之后首个非空白字符为 `{` 才是函数定义。

    调用/前置声明在 `)` 之后是 `;` 或 `,`，据此排除，避免把调用误判为导出。
    """
    depth = 0
    i = open_paren
    while i < len(code):
        ch = code[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                j = i + 1
                while j < len(code) and code[j].isspace():
                    j += 1
                return j < len(code) and code[j] == "{"
        i += 1
    return False


def _split_class_method(tail: str, known_classes: set[str]) -> tuple[str, str]:
    """把去掉 `Java_` 前缀的符号（如 com_inotia4_qol_NativeBridge_nativeInit）拆成类名与方法名。

    类名优先按 Kotlin 侧已知类名匹配（JNI 类名无嵌套、无下划线时成立）；无法匹配时退化为
    首个下划线切分，仅影响 detail 文本。
    """
    rest = tail[len(_PACKAGE) + 1:] if tail.startswith(_PACKAGE + "_") else tail
    for cls in sorted(known_classes, key=len, reverse=True):
        if rest.startswith(cls + "_"):
            return cls, rest[len(cls) + 1:]
    cls, _, method = rest.partition("_")
    return cls, method


def _unmangle(method: str) -> str:
    """JNI 名称还原：导出名中的 `_1` 对应 Kotlin 方法名里的字面下划线。"""
    return method.replace("_1", "_")


def read_kotlin_externals() -> list[tuple[str, str, int]]:
    """返回 (方法名, 所属类, 行号)，按声明顺序。"""
    text = ac.read_text(KOTLIN_BRIDGE_REL, KOTLIN_BRIDGE)
    out: list[tuple[str, str, int]] = []
    owner = "?"
    for lineno, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.split("//", 1)[0]
        match_owner = _KOTLIN_OWNER_RE.search(line)
        if match_owner:
            owner = match_owner.group(1)
        match_external = _KOTLIN_EXTERNAL_RE.search(line)
        if match_external:
            out.append((match_external.group(1), owner, lineno))
    return out


def read_native_exports(known_classes: set[str]) -> list[tuple[str, str, str, str, int]]:
    """返回 (文件相对路径, 完整导出符号, 类名, 方法名(仍编码), 行号)。"""
    out: list[tuple[str, str, str, str, int]] = []
    for rel, path in ac.iter_sources():
        if Path(rel).suffix not in _NATIVE_SUFFIXES:
            continue
        code = _strip_comments(ac.read_text(rel, path))
        for match in _JAVA_SYMBOL_RE.finditer(code):
            if not _is_definition(code, match.end() - 1):
                continue
            tail = match.group(1)
            cls, method = _split_class_method(tail, known_classes)
            line = code.count("\n", 0, match.start()) + 1
            out.append((rel, "Java_" + tail, cls, method, line))
    return out


def main() -> int:
    args = ac.parse_common_args(__doc__ or "JNI 导出一致性校验")

    externals = read_kotlin_externals()
    known_classes = {owner for _, owner, _ in externals if owner != "?"}
    exports = read_native_exports(known_classes)

    kotlin_lines: dict[str, int] = {}
    for name, _, line in externals:
        kotlin_lines.setdefault(name, line)

    native_methods = {_unmangle(method) for _, _, _, method, _ in exports}

    violations: list[ac.Violation] = []

    for name, owner, line in externals:
        if name in native_methods:
            continue
        mangled = name.replace("_", "_1")
        expected = f"Java_{_PACKAGE}_{owner}_{mangled}" if owner != "?" else f"Java_{_PACKAGE}_{mangled}"
        violations.append(
            ac.Violation("jni_kotlin_missing_native", KOTLIN_BRIDGE_REL, expected, f"行 {line}")
        )

    for rel, symbol, cls, method, line in exports:
        if _unmangle(method) in kotlin_lines:
            continue
        violations.append(
            ac.Violation("jni_native_missing_kotlin", rel, symbol, f"行 {line} 类 {cls}")
        )

    class_counts = Counter(cls for _, _, cls, _, _ in exports)
    class_summary = "、".join(f"{cls}={count}" for cls, count in sorted(class_counts.items()))
    summary = [
        f"  Kotlin external={len(externals)}，native 导出={len(exports)}，"
        f"类名分布=[{class_summary}]"
    ]

    return ac.emit(
        "check_jni",
        violations,
        args,
        "Kotlin external ↔ native JNI 导出",
        summary,
        rules=("jni_kotlin_missing_native", "jni_native_missing_kotlin"),
    )


if __name__ == "__main__":
    sys.exit(main())

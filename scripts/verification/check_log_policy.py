#!/usr/bin/env python3
"""统一日志系统合规检查（P4）。

权威规范：`docs/development/logging.md`。本脚本静态检查代码是否满足该规范：

  R1 唯一出口    native 中除 `core/native/qol_log.cpp` 与 host 桩外不得出现 __android_log_print；
                 Kotlin 中除 `LogFile.kt` 外不得出现 android.util.Log
  R2 旧 tag 清零  字符串字面量中不得出现已废弃 logcat tag（含 Inotia4Export/ApiServer/
                 OpApiService/OpController）；`inotia4-export.log` 文件名允许保留
  R3 domain 词表  Kotlin `LogDomain` token 集合 == native `qol_domain_token` token 集合
  R4 逐帧无高级别 `*_tick|*_process|*_draw*|*_render*`（含 frame_host 的
                 `render_pre_wrapper`/`logic_pre_wrapper`）函数体、`frame_task_add(` 回调与
                 内联回调内不得有 I/W/E；别名宏按映射级别判定（软告警）
  R5 级别与定位  `WARN|ERROR`（含别名）格式串需含 `=`；INFO/DEBUG 正文不得以 `ERROR ` 开头（软告警）

R2 只在字符串字面量中匹配 “整串等于 tag” 或 “以 `tag:` 开头” 两种旧用法，避免把
`ApiServer`/`OpController` 等同名 Kotlin 类/标识符误判为废弃 tag；`Inotia4Export` 与文件名
常量 `inotia4-export.log` 大小写不同，不会互相命中。

输出：每条给出 `文件:行: 规则号: 说明`，末尾汇总计数。
退出码：默认为 0；存在 R1/R2/R3 违规时非 0；`--warn-only` 恒返回 0。
R4/R5 为软告警，不影响退出码。

无第三方依赖，纯标准库。用法：
    uv run python scripts/verification/check_log_policy.py [--warn-only]
"""
from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parents[2]
CPP_ROOT = ROOT / "module" / "app" / "src" / "main" / "cpp"
JAVA_ROOT = ROOT / "module" / "app" / "src" / "main" / "java"

QOL_LOG_CPP = CPP_ROOT / "core" / "native" / "qol_log.cpp"
LOG_STUB = CPP_ROOT / "tests" / "stubs" / "android" / "log.h"

CPP_SUFFIXES = {".cpp", ".h", ".hpp", ".cc", ".inc"}
JAVA_SUFFIXES = {".kt", ".java"}

# R1：Kotlin 侧唯一允许使用 android.util.Log 的兜底通道。
KOTLIN_LOG_FILE_NAME = "LogFile.kt"

# R2：已废弃的 logcat tag（Inotia4Qol / inotia4-export.log 不在其列，允许保留）。
BANNED_TAGS = (
    "Inotia4Move",
    "Inotia4Tiles",
    "Inotia4VirtBag",
    "Inotia4AutoSell",
    "Inotia4AutoSellUI",
    "Inotia4UISaveBackup",
    "Inotia4UISettings",
    "Inotia4UICustom",
    "Inotia4UIExp",
    "Inotia4SaveBackup",
    "Inotia4SaveEnter",
    "Inotia4SaveExit",
    "Inotia4Transition",
    "Inotia4CallPatch",
    "Inotia4FrameHost",
    "Inotia4ModuleSave",
    "Inotia4GemCraft",
    "Inotia4AttrRange",
    "Inotia4NativeHook",
    "Inotia4Export",
    "ApiServer",
    "OpApiService",
    "OpController",
)

# R1：允许直接使用 __android_log_print 的 native 文件（唯一出口 + host 桩）。
EXEMPT_ANDROID_LOG = {QOL_LOG_CPP.resolve(), LOG_STUB.resolve()}

# R4：逐帧/高频命名启发式（函数定义体）。
#
# 只匹配真正的逐帧上下文：
#   - `*_tick`    frame_task 回调；
#   - `*_process` 面板每帧处理；
#   - 名字含 `draw`/`render` 的绘制/渲染路径（含 `*_drawing` 与预/后绘制 wrapper）；
#   - `render_pre_wrapper`/`logic_pre_wrapper` 两个 frame_host 相位 wrapper。
#
# 已移除通用 `*_wrapper`、`*_gate` 后缀：`*_wrapper` 是 hook 包装函数，仅在用户操作/hook
# 触发时执行，并非逐帧，作为逐帧线索会产生大量误报。
#
# 注：仍属**仅命名启发式**；门控型回调（如 `save_enter_tick` 用 exchange-once 保证每次
# 进程只输出一次）可能仍命中，属已知基线，需逐点复核而不是机械降级（见 logging.md §7）。
PER_FRAME_NAME_PATTERNS = (
    r"\w*_tick",
    r"\w*_process",
    r"\w*draw\w*",
    r"\w*render\w*",
    r"render_pre_wrapper",
    r"logic_pre_wrapper",
)

_ANDROID_LOG_RE = re.compile(r"\bandroid\.util\.Log\b")
_NATIVE_DOMAIN_RE = re.compile(r"\{\s*QolDomain::\w+\s*,\s*\"([a-z_]+)\"\s*\}")
_KOTLIN_DECL_RE = re.compile(r"\b(?:enum\s+class|object|class)\s+LogDomain\b")
_KOTLIN_TOKEN_RE = re.compile(r"\"([a-z][a-z0-9_]*)\"")
_STR_LITERAL_RE = re.compile(r'"(?:[^"\\]|\\.)*"')

# 别名宏定义：`#define NAME(...) QOL_LOG_LEVEL(...)`（同一行），用于展开 R4/R5 判定级别。
_ALIAS_DEF_RE = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)[ \t]*\([^\n]*?\bQOL_LOG_(DEBUG|INFO|WARN|ERROR)\b",
    re.MULTILINE,
)


class Finding(NamedTuple):
    """一条检查结果。"""

    path: str
    line: int
    rule: str
    message: str
    soft: bool = False


def iter_source_files(root: Path, suffixes: set[str]):
    if not root.exists():
        return
    for path in sorted(root.rglob("*")):
        if path.is_file() and path.suffix in suffixes:
            yield path


def rel(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def _masked(text: str) -> str:
    """把字符串/字符字面量与注释替换为空格，保持长度与换行，用于括号配对。"""
    out = list(text)
    n = len(text)
    i = 0

    def blank(start: int, end: int) -> None:
        for k in range(start, end):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            i = j
        elif c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    j += 1
                    break
                j += 1
            blank(i, min(j, n))
            i = j
        elif c == "'":
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == "'":
                    j += 1
                    break
                j += 1
            blank(i, min(j, n))
            i = j
        else:
            i += 1
    return "".join(out)


def _line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def _matching(masked: str, open_pos: int, open_ch: str, close_ch: str) -> int:
    depth = 0
    for i in range(open_pos, len(masked)):
        c = masked[i]
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
    return -1


def _split_top_level(text: str, sep: str = ",") -> list[str]:
    parts: list[str] = []
    depth = 0
    masked = _masked(text)
    last = 0
    for i, c in enumerate(masked):
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == sep and depth == 0:
            parts.append(text[last:i])
            last = i + 1
    parts.append(text[last:])
    return parts


# ------------------------------------------------------------------- 宏别名


def macro_alias_levels() -> dict[str, str]:
    """扫描 native 源，收集 `#define ALIAS(...) QOL_LOG_LEVEL(...)` 的别名→级别映射。"""
    aliases: dict[str, str] = {}
    for path in iter_source_files(CPP_ROOT, CPP_SUFFIXES):
        text = path.read_text(errors="replace")
        for m in _ALIAS_DEF_RE.finditer(text):
            name = m.group(1)
            if name.startswith("QOL_LOG_"):
                continue
            aliases.setdefault(name, m.group(2))
    return aliases


def _macro_call_re(aliases: dict[str, str]) -> re.Pattern[str]:
    direct = r"QOL_LOG_(?:DEBUG|INFO|WARN|ERROR)"
    alts = [direct] + [re.escape(n) for n in sorted(aliases, key=len, reverse=True)]
    return re.compile(r"\b(" + "|".join(alts) + r")\s*\(")


def _macro_level(name: str, aliases: dict[str, str]) -> str | None:
    if name.startswith("QOL_LOG_"):
        return name[len("QOL_LOG_"):]
    return aliases.get(name)


def _macro_format_index(name: str) -> int:
    """直接宏 `QOL_LOG_X(dom, fmt, ...)` 的格式串下标为 1；别名宏 `ALIAS(fmt, ...)` 为 0。"""
    return 1 if name.startswith("QOL_LOG_") else 0


def _iter_qol_macro_calls(text: str, aliases: dict[str, str]):
    """产出 (起始位置, 宏名, 级别, 顶层实参列表)。级别为 DEBUG/INFO/WARN/ERROR。"""
    masked = _masked(text)
    for m in _macro_call_re(aliases).finditer(masked):
        name = m.group(1)
        level = _macro_level(name, aliases)
        if level is None:
            continue
        close = _matching(masked, m.end() - 1, "(", ")")
        if close < 0:
            continue
        yield m.start(), name, level, _split_top_level(text[m.end():close])


# --------------------------------------------------------------------------- R1


def check_unique_outlet(findings: list[Finding]) -> None:
    for path in iter_source_files(CPP_ROOT, CPP_SUFFIXES):
        if path.resolve() in EXEMPT_ANDROID_LOG:
            continue
        text = path.read_text(errors="replace")
        for m in re.finditer(r"\b__android_log_print\b", text):
            findings.append(
                Finding(rel(path), _line_of(text, m.start()), "R1",
                        "禁止直接调用 __android_log_print，统一走 qol_log_write()")
            )
    for path in iter_source_files(JAVA_ROOT, JAVA_SUFFIXES):
        if path.name == KOTLIN_LOG_FILE_NAME:
            continue
        text = path.read_text(errors="replace")
        for m in _ANDROID_LOG_RE.finditer(text):
            findings.append(
                Finding(rel(path), _line_of(text, m.start()), "R1",
                        "Kotlin 禁止直接使用 android.util.Log，统一走 LogFile")
            )


# --------------------------------------------------------------------------- R2


def check_banned_tags(findings: list[Finding]) -> None:
    tag_patterns = {
        tag: re.compile(r"(?<![A-Za-z0-9_])" + re.escape(tag) + r"(?![A-Za-z0-9_])")
        for tag in BANNED_TAGS
    }
    for root in (CPP_ROOT, JAVA_ROOT):
        suffixes = CPP_SUFFIXES if root == CPP_ROOT else JAVA_SUFFIXES
        for path in iter_source_files(root, suffixes):
            text = path.read_text(errors="replace")
            for lit in _STR_LITERAL_RE.finditer(text):
                content = lit.group(0)[1:-1].strip()
                for tag, pattern in tag_patterns.items():
                    if not pattern.search(content):
                        continue
                    # 仅旧用法：“整串等于 tag” 或 “以 `tag:` 开头的旧前缀”。
                    if content == tag or content.startswith(tag + ":"):
                        findings.append(
                            Finding(rel(path), _line_of(text, lit.start()), "R2",
                                    f"已废弃 logcat tag `{tag}`，改用单一 tag `Inotia4Qol`")
                        )


# --------------------------------------------------------------------------- R3


def native_domain_tokens() -> list[str]:
    if not QOL_LOG_CPP.exists():
        return []
    return _NATIVE_DOMAIN_RE.findall(QOL_LOG_CPP.read_text(errors="replace"))


def kotlin_domain_tokens() -> tuple[set[str], Path | None]:
    """返回 (Kotlin LogDomain token 集合, 声明所在文件)。"""
    tokens: set[str] = set()
    declared_at: Path | None = None
    if not JAVA_ROOT.exists():
        return tokens, declared_at
    for path in sorted(JAVA_ROOT.rglob("*.kt")):
        text = path.read_text(errors="replace")
        # 仅解析 LogDomain 的声明块，避免把同文件/引用文件里的其它字符串当成 token。
        decl = _KOTLIN_DECL_RE.search(text)
        if decl is None:
            continue
        brace = text.find("{", decl.end())
        if brace < 0:
            continue
        end = _matching(_masked(text), brace, "{", "}")
        body = text[brace:end] if end > 0 else text[brace:]
        found = _KOTLIN_TOKEN_RE.findall(body)
        if found:
            declared_at = path
            tokens.update(found)
    return tokens, declared_at


def check_domain_vocabulary(findings: list[Finding]) -> None:
    native = set(native_domain_tokens())
    kotlin, kotlin_path = kotlin_domain_tokens()
    if not native:
        findings.append(
            Finding(rel(QOL_LOG_CPP), 1, "R3", "未能在 qol_log.cpp 解析到 native domain token 表")
        )
        return
    if kotlin_path is None:
        findings.append(
            Finding("module/app/src/main/java", 1, "R3",
                    "未找到 Kotlin LogDomain（期望 token 集合与 native 一致）")
        )
        return
    location = rel(kotlin_path)
    for token in sorted(native - kotlin):
        findings.append(
            Finding(location, 1, "R3", f"Kotlin LogDomain 缺少 native token `{token}`")
        )
    for token in sorted(kotlin - native):
        findings.append(
            Finding(location, 1, "R3", f"Kotlin LogDomain 多出 native 未定义 token `{token}`")
        )


# --------------------------------------------------------------------------- R4


def _function_body_ranges(
    text: str, extra_names: set[str] | None = None
) -> list[tuple[int, int, str]]:
    """返回逐帧/高频命名函数定义的 (起, 止, 名) 字符区间。"""
    masked = _masked(text)
    ranges: list[tuple[int, int, str]] = []
    alternation = "(?:" + "|".join(PER_FRAME_NAME_PATTERNS) + ")"
    if extra_names:
        alternation = "(?:" + alternation + "|" + "|".join(
            re.escape(n) for n in sorted(extra_names)
        ) + ")"
    pattern = re.compile(r"\b(" + alternation + r")\s*\(")
    for m in pattern.finditer(masked):
        close = _matching(masked, m.end() - 1, "(", ")")
        if close < 0:
            continue
        k = close + 1
        while k < len(masked) and masked[k] in " \t\r\n":
            k += 1
        if masked[k:k + 5] == "const":
            k += 5
            while k < len(masked) and masked[k] in " \t\r\n":
                k += 1
        if k >= len(masked) or masked[k] != "{":
            continue  # 函数调用，不是定义
        end = _matching(masked, k, "{", "}")
        if end > 0:
            ranges.append((m.start(), end, m.group(1)))
    return ranges


def _frame_task_spans(text: str) -> list[tuple[int, int]]:
    masked = _masked(text)
    spans: list[tuple[int, int]] = []
    for m in re.finditer(r"\bframe_task_add\s*\(", masked):
        close = _matching(masked, m.end() - 1, "(", ")")
        if close > 0:
            spans.append((m.start(), close))
    return spans


def _frame_task_callback_names(text: str, masked: str) -> set[str]:
    """收集 `frame_task_add(...)` 实参中出现的标识符（含回调函数名）。"""
    names: set[str] = set()
    for m in re.finditer(r"\bframe_task_add\s*\(", masked):
        close = _matching(masked, m.end() - 1, "(", ")")
        if close < 0:
            continue
        for arg in _split_top_level(text[m.end():close]):
            names.update(re.findall(r"\b[A-Za-z_]\w*\b", _masked(arg)))
    return names


_HIGH_LEVELS = {"INFO", "WARN", "ERROR"}


def check_per_frame_levels(findings: list[Finding]) -> None:
    aliases = macro_alias_levels()
    for path in iter_source_files(CPP_ROOT, CPP_SUFFIXES):
        text = path.read_text(errors="replace")
        if "QOL_LOG_" not in text and not any(name in text for name in aliases):
            continue
        masked = _masked(text)
        suffix_ranges = _function_body_ranges(text)
        callback_ranges = _function_body_ranges(text, _frame_task_callback_names(text, masked))
        spans = _frame_task_spans(text)
        for pos, name, level, _args in _iter_qol_macro_calls(text, aliases):
            if level not in _HIGH_LEVELS:
                continue
            context = None
            for start, end, fname in suffix_ranges:
                if start <= pos <= end:
                    context = f"逐帧函数 `{fname}`"
                    break
            if context is None:
                for start, end, fname in callback_ranges:
                    if start <= pos <= end:
                        context = f"frame_task_add 回调 `{fname}`"
                        break
            if context is None:
                for start, end in spans:
                    if start <= pos <= end:
                        context = "frame_task_add 内联回调"
                        break
            if context is not None:
                findings.append(
                    Finding(rel(path), _line_of(text, pos), "R4",
                            f"{context} 内出现 {name}（级别 {level}）；非 debug 级别禁止逐帧",
                            soft=True)
                )


# --------------------------------------------------------------------------- R5


def check_localizing_vars(findings: list[Finding]) -> None:
    aliases = macro_alias_levels()
    for path in iter_source_files(CPP_ROOT, CPP_SUFFIXES):
        text = path.read_text(errors="replace")
        if "QOL_LOG_" not in text and not any(name in text for name in aliases):
            continue
        for _pos, name, level, args in _iter_qol_macro_calls(text, aliases):
            if level not in {"WARN", "ERROR"}:
                continue
            fmt_idx = _macro_format_index(name)
            if len(args) <= fmt_idx:
                continue
            literals = _STR_LITERAL_RE.findall(args[fmt_idx])
            if not literals:
                continue  # 格式串非字面量，无法静态判断
            if not any("=" in lit for lit in literals):
                findings.append(
                    Finding(rel(path), _line_of(text, _pos), "R5",
                            f"{name} 格式串缺少定位变量 `key=value`", soft=True)
                )


def check_level_mismatch(findings: list[Finding]) -> None:
    """R5 加固：正文以 `ERROR ` 开头却使用 INFO/DEBUG 级别即级别错配。"""
    aliases = macro_alias_levels()
    for path in iter_source_files(CPP_ROOT, CPP_SUFFIXES):
        text = path.read_text(errors="replace")
        if "QOL_LOG_" not in text and not any(name in text for name in aliases):
            continue
        for pos, name, level, args in _iter_qol_macro_calls(text, aliases):
            if level not in {"INFO", "DEBUG"}:
                continue
            fmt_idx = _macro_format_index(name)
            if len(args) <= fmt_idx:
                continue
            literals = _STR_LITERAL_RE.findall(args[fmt_idx])
            if not literals:
                continue
            first = literals[0][1:-1]
            if first.startswith("ERROR "):
                findings.append(
                    Finding(rel(path), _line_of(text, pos), "R5",
                            f"级别错配：{name}（{level}）正文以 `ERROR ` 开头", soft=True)
                )


# --------------------------------------------------------------------------- main


def main() -> None:
    warn_only = "--warn-only" in sys.argv[1:]
    findings: list[Finding] = []
    check_unique_outlet(findings)
    check_banned_tags(findings)
    check_domain_vocabulary(findings)
    check_per_frame_levels(findings)
    check_localizing_vars(findings)
    check_level_mismatch(findings)

    for f in sorted(findings, key=lambda x: (x.rule, x.path, x.line)):
        severity = "warn" if f.soft else "error"
        print(f"{f.path}:{f.line}: {f.rule}: [{severity}] {f.message}")

    counts: dict[str, int] = {}
    for f in findings:
        counts[f.rule] = counts.get(f.rule, 0) + 1
    hard = sum(1 for f in findings if not f.soft)
    soft = sum(1 for f in findings if f.soft)

    print()
    print("== 统一日志系统合规检查汇总 ==")
    for rule in ("R1", "R2", "R3", "R4", "R5"):
        print(f"{rule}: {counts.get(rule, 0)}")
    print(f"违规(error): {hard} ｜ 软告警(warn): {soft} ｜ 合计: {len(findings)}")

    if warn_only or hard == 0:
        print("结果: PASS" if hard == 0 else "结果: WARN-ONLY（存在违规但按 --warn-only 返回 0）")
        sys.exit(0)
    print("结果: FAIL（存在 R1/R2/R3 违规）")
    sys.exit(1)


if __name__ == "__main__":
    main()

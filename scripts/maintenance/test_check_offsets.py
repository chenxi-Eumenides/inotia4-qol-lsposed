#!/usr/bin/env python3
"""`check_offsets.py` 的裸地址规则回归测试。

覆盖三类判定：
1. 插件目录出现模块基址引用（`g_base`）→ `offsets_plugin_base`；
2. 基址加十进制 VMA（`g_base + 3152304`）→ `offsets_bare_address`（此形态曾漏检）；
3. 反例不得误报：game 层同一形态、注释行、`kWh4FnvOffsetBasis` 一类哈希常量。

fixture 用 FakeGraph + mock read_text，不读真实源码树、不写临时文件。
"""

import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_offsets as co  # noqa: E402


class FakeGraph:
    """最小 IncludeGraph 替身：只提供 collect_violations 用到的三个面。"""

    def __init__(self, texts: dict[str, str]) -> None:
        self.texts = texts
        self.paths = {rel: Path("/nonexistent") / rel for rel in texts}

    def layer(self, rel: str) -> str:
        return rel.split("/", 1)[0]

    def includes_of(self, rel: str) -> list[str]:
        return []


def scan(texts: dict[str, str]) -> list[co.ac.Violation]:
    graph = FakeGraph(texts)
    with mock.patch.object(co.ac, "read_text", side_effect=lambda rel, path: texts[rel]):
        return co.collect_violations(graph)


def rule_hits(violations: list[co.ac.Violation], rule: str) -> list[co.ac.Violation]:
    return [v for v in violations if v.rule == rule]


class PluginBaseRuleTest(unittest.TestCase):
    def test_plugin_declaring_g_base_is_reported(self) -> None:
        violations = scan({"plugins/demo/table.cpp": "extern uintptr_t g_base;\n"})
        assert len(rule_hits(violations, "offsets_plugin_base")) == 1

    def test_plugin_using_g_base_is_reported(self) -> None:
        violations = scan({"plugins/demo/table.cpp": "auto* p = (void*)(g_base + 0x10);\n"})
        assert len(rule_hits(violations, "offsets_plugin_base")) == 1

    def test_comment_only_g_base_in_plugin_is_not_reported(self) -> None:
        violations = scan({"plugins/demo/table.cpp": "// g_base 未就绪时不得读取槽位\n"})
        assert rule_hits(violations, "offsets_plugin_base") == []

    def test_game_layer_may_declare_g_base(self) -> None:
        violations = scan({"game/adapter_ports/port.cpp": "extern uintptr_t g_base;\n"})
        assert rule_hits(violations, "offsets_plugin_base") == []


class BareAddressRuleTest(unittest.TestCase):
    def test_decimal_vma_add_is_reported(self) -> None:
        violations = scan({"plugins/demo/table.cpp": "bss(kRecipe = g_base + 3152304);\n"})
        assert len(rule_hits(violations, "offsets_bare_address")) == 1

    def test_hex_vma_add_is_reported(self) -> None:
        violations = scan({"host/demo.cpp": "auto p = g_base + 0x3019b0;\n"})
        assert len(rule_hits(violations, "offsets_bare_address")) == 1

    def test_game_layer_is_exempt(self) -> None:
        violations = scan({"game/demo.cpp": "auto p = g_base + 0x3019b0;\n"})
        assert violations == []

    def test_hash_constant_is_not_reported(self) -> None:
        violations = scan({"infra/hash.cpp": "constexpr uint64_t kWh4FnvOffsetBasis = 0xcbf29ce4;\n"})
        assert violations == []

    def test_comment_line_is_not_reported(self) -> None:
        violations = scan({"plugins/demo/table.cpp": "// g_base + 3152304 旧写法\n"})
        assert rule_hits(violations, "offsets_bare_address") == []


if __name__ == "__main__":
    unittest.main()

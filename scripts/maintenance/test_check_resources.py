#!/usr/bin/env python3
"""`check_resources.py` 的机制识别单元测试。

覆盖两类回归：
1. ptr slot 机制**不做子串匹配**：`recipe_ptr_slot` / `mixture_ptr_slot` 一类业务字段名
   （声明、null 校验、解引用读取、结构赋值、bridge 装配）不得被算成 kPtrSlot 安装点；
2. 真实机制标识/调用形态仍被识别：`PtrHook`、旧标识 `GamePtrHook`（以及 snake_case
   `ptr_hook` / `game_ptr_hook`）、独立 `ptr_slot` token、`ptr_slot_write_raw(...)` /
   `ptr_slot_restore_raw(...)` 调用；同时保留 entry hook / call patch 既有行为、机制本体
   与注入排除、未知符号仍报 `resource_unknown_symbol`、跨文件同机制同 F_* 仍报
   `resource_duplicate_owner`。

fixture 用 mock 注入源码字符串（`ac.iter_sources` / `ac.read_text`），不创建任何临时文件、
不读真实源码树。
"""

import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import arch_common as ac  # noqa: E402
import check_resources as cr  # noqa: E402


def scan_texts(texts: dict[str, str]) -> list[cr.InstallPoint]:
    """把 {相对路径: 文本} 注入 scan()：不写磁盘、不读真实源码树。"""
    sources = [(rel, Path("/nonexistent") / rel) for rel in sorted(texts)]
    with mock.patch.object(ac, "iter_sources", return_value=sources), mock.patch.object(
        ac, "read_text", side_effect=lambda rel, path: texts[rel]
    ):
        return cr.scan()


def only_point(texts: dict[str, str]) -> cr.InstallPoint:
    points = scan_texts(texts)
    assert len(points) == 1, points
    return points[0]


class PtrSlotBusinessFieldTests(unittest.TestCase):
    """`*_ptr_slot` 业务字段名不是 kPtrSlot 机制安装点（误报回归）。"""

    BUSINESS_LINES = (
        "    void** recipe_ptr_slot = nullptr;",
        "    void** mixture_ptr_slot = nullptr;",
        "    if (state == nullptr || access.recipe_ptr_slot == nullptr ||",
        "        access.mixture_ptr_slot == nullptr || access.recipe_size_slot == nullptr ||",
        "    probe.recipe_ptr_present = *access.recipe_ptr_slot != nullptr;",
        "    if (state->recipe_buffer != nullptr && *access.recipe_ptr_slot == state->recipe_buffer) {",
        "    const uint8_t* recipe_data = static_cast<const uint8_t*>(*access.recipe_ptr_slot);",
        "    *access.recipe_ptr_slot = new_recipe;",
        "    *access.mixture_ptr_slot = new_mixture;",
        "    access.recipe_ptr_slot = reinterpret_cast<void**>(bss(kRecipeDataSlot));",
        "    access.mixture_ptr_slot = reinterpret_cast<void**>(bss(kMixtureDataSlot));",
        "    recipe_ptr_slot_initialized = true;",
        "    const void** ptr_slot_backup = &access.recipe_ptr_slot;",
    )

    def test_business_fields_produce_no_install_point(self) -> None:
        text = "\n".join(self.BUSINESS_LINES) + "\n"
        points = scan_texts({"plugins/business/table.cpp": text})
        self.assertEqual(points, [])

    def test_business_fields_produce_no_violation(self) -> None:
        text = "\n".join(self.BUSINESS_LINES) + "\n"
        violations = cr.build_violations(scan_texts({"plugins/business/table.cpp": text}))
        self.assertEqual(violations, [])


class PtrSlotRealFormTests(unittest.TestCase):
    """真实机制标识/调用形态仍被识别为 ptr slot 安装点。"""

    def test_ptr_hook_type_usage_is_detected_with_symbol(self) -> None:
        point = only_point(
            {
                "plugins/save_backup/plugin.cpp": (
                    "void install() {\n"
                    '    PtrHook hook;\n'
                    '    hook.install(fn_resolve("F_SAVESLOT_DELETE_GOT_VMA"), &wrapper);\n'
                    "}\n"
                )
            }
        )
        self.assertEqual(point.mechanism, "ptr slot")
        self.assertEqual(point.symbol, "F_SAVESLOT_DELETE_GOT_VMA")

    def test_game_ptr_hook_legacy_identifier_is_detected(self) -> None:
        point = only_point(
            {
                "plugins/legacy/plugin.cpp": (
                    '// F_GAMEPTR_LEGACY_VMA 归属见 resource claim\n'
                    "GamePtrHook slot_hook;\n"
                )
            }
        )
        self.assertEqual(point.mechanism, "ptr slot")
        self.assertEqual(point.symbol, "F_GAMEPTR_LEGACY_VMA")

    def test_snake_case_legacy_identifiers_are_detected(self) -> None:
        for line in ("game_ptr_hook.install(addr, &wrapper);", "ptr_hook = {};"):
            with self.subTest(line=line):
                point = only_point(
                    {"plugins/legacy/plugin.cpp": 'F_LEGACY_PTR_VMA\n' + line + "\n"}
                )
                self.assertEqual(point.mechanism, "ptr slot")
                self.assertEqual(point.symbol, "F_LEGACY_PTR_VMA")

    def test_standalone_ptr_slot_token_is_detected(self) -> None:
        point = only_point(
            {
                "plugins/other/plugin.cpp": (
                    'const char* name = "ptrslot:demo";\n'
                    "bool ok = ptr_slot(name, &site);\n"
                    "F_DEMO_PTR_VMA\n"
                )
            }
        )
        self.assertEqual(point.mechanism, "ptr slot")
        self.assertEqual(point.symbol, "F_DEMO_PTR_VMA")

    def test_raw_write_calls_are_detected(self) -> None:
        for call in ("ptr_slot_write_raw(addr, value);", "ptr_slot_restore_raw(addr, value);"):
            with self.subTest(call=call):
                point = only_point(
                    {"plugins/other/plugin.cpp": "F_RAW_PTR_VMA\n" + call + "\n"}
                )
                self.assertEqual(point.mechanism, "ptr slot")
                self.assertEqual(point.symbol, "F_RAW_PTR_VMA")

    def test_mechanism_wiring_injection_is_still_excluded(self) -> None:
        text = (
            "const Primitives primitives{\n"
            "    &qol::patch::ptr_slot_write_raw,\n"
            "    &qol::patch::ptr_slot_restore_raw,\n"
            "};\n"
            "void dispatch() { ptr_slot_site(\"ptrslot:demo\", &site); }\n"
        )
        self.assertEqual(scan_texts({"host/assembly.cpp": text}), [])


class MechanismAndViolationSemanticsTests(unittest.TestCase):
    """既有行为保持：机制本体排除、entry hook/call patch、unknown 与 duplicate。"""

    def test_mechanism_files_api_lines_and_comments_are_excluded(self) -> None:
        texts = {
            # 机制本体文件（_MECHANISM_FILES）：其中的命中是机制定义而非资源安装点。
            "patch/ptr_slot.cpp": "bool ok = PtrHook{};\n",
            "patch/call_patch.cpp": "void call_patch_install_bl() {}\n",
            "patch/game_ptr_hook.h": "struct PtrHook { void** slot; };\n",
            "game/access/ptr_slots.cpp": "PtrSlotSite site{};\n",
            # 机制取函数指针的 API 声明/定义（_MECH_API_LINE）不算安装点。
            "infra/wrapper.cpp": "NativeHookFunType native_hook_func();\n",
            # 纯注释行不算安装点。
            "plugins/demo/plugin.cpp": "// PtrHook 只在注释里出现\n",
        }
        self.assertEqual(scan_texts(texts), [])

    def test_entry_hook_and_call_patch_still_detected(self) -> None:
        points = scan_texts(
            {
                "plugins/demo/hook.cpp": (
                    'native_hook_func("hook:DEMO", fn_resolve("F_DEMO_HOOK_VMA"));\n'
                ),
                "plugins/demo/patches.inc": (
                    "instr_patch(kDemoSite);\n"
                    'static constexpr char kDemoSymbol[] = "F_DEMO_PATCH_VMA";\n'
                ),
            }
        )
        mechanisms = sorted((point.mechanism, point.symbol) for point in points)
        self.assertEqual(
            mechanisms,
            [("call patch", "F_DEMO_PATCH_VMA"), ("entry hook", "F_DEMO_HOOK_VMA")],
        )

    def test_unknown_symbol_is_still_reported(self) -> None:
        violations = cr.build_violations(
            scan_texts({"plugins/demo/plugin.cpp": "PtrHook hook;\nhook.install(addr, &wrapper);\n"})
        )
        self.assertEqual(len(violations), 1)
        self.assertEqual(violations[0].rule, "resource_unknown_symbol")

    def test_duplicate_owner_across_files_is_still_reported(self) -> None:
        body = 'PtrHook hook;\nhook.install(fn_resolve("F_SHARED_PTR_VMA"), &wrapper);\n'
        violations = cr.build_violations(
            scan_texts({"plugins/a/plugin.cpp": body, "plugins/b/plugin.cpp": body})
        )
        self.assertEqual(
            sorted((item.rule, item.src) for item in violations),
            [
                ("resource_duplicate_owner", "plugins/a/plugin.cpp"),
                ("resource_duplicate_owner", "plugins/b/plugin.cpp"),
            ],
        )


if __name__ == "__main__":
    unittest.main()

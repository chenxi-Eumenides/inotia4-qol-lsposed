"""Source-level checks for feature categories and the settings catalog contract."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
PLUGINS = ROOT / "module/app/src/main/cpp/plugins"
FEATURE = re.compile(
    r'\{"([^"]+)",\s*([01]),\s*(?:qol::)?contracts::FeatureCategory::(\w+),\s*'
    r'(?:qol::)?contracts::FeatureType::(\w+),\s*"([^"]*)",\s*"([^"]*)"'
)
CATEGORY = re.compile(
    r'\{(?:qol::)?contracts::FeatureCategory::(\w+),\s*"([^"]*)",\s*(-?\d+)\s*\}'
)


def feature_initializer(source: str, feature_id: str) -> str:
    start = source.index('{"' + feature_id + '"')
    next_item = re.search(r'\n\s*\{"[^\"]+"', source[start + 2:])
    return source[start:] if next_item is None else source[start:start + 2 + next_item.start()]


class CoreFeatureExposureTests(unittest.TestCase):
    def test_all_core_features_are_hidden_and_have_no_player_description(self):
        sources = sorted(PLUGINS.glob("core.*/plugin.cpp"))
        self.assertEqual(len(sources), 12)
        for source in sources:
            with self.subTest(plugin=source.parent.name):
                features = FEATURE.findall(source.read_text())
                self.assertTrue(features)
                for feature_id, _, category, _, _, description in features:
                    expected_category = "kGeneral" if (
                        source.parent.name == "core.api" and feature_id == "api.enabled"
                    ) else "kHidden"
                    self.assertEqual(category, expected_category)
                    self.assertEqual(description, "")
        api_features = FEATURE.findall((PLUGINS / "core.api" / "plugin.cpp").read_text())
        self.assertEqual(api_features, [(
            "api.enabled", "1", "kGeneral", "kToggle", "API服务", "",
        )])

    def test_removed_catalog_opt_in_is_absent_from_current_sources_and_docs(self):
        removed_field = "player_" + "visible"
        roots = [ROOT / "module/app/src/main/cpp", ROOT / "scripts"]
        source_paths = [
            path for root in roots for path in root.rglob("*")
            if path.is_file() and path.suffix in {".h", ".hpp", ".cpp", ".cc", ".c", ".inc", ".py"}
        ]
        source_paths.append(ROOT / "ARCHITECTURE.md")
        source_paths.extend(
            path for path in (ROOT / "docs").rglob("*.md")
            if "archive" not in path.relative_to(ROOT / "docs").parts
        )
        for path in source_paths:
            with self.subTest(path=path.relative_to(ROOT)):
                self.assertNotIn(removed_field, path.read_text())

        descriptor = (ROOT / "module/app/src/main/cpp/contracts/plugin.h").read_text()
        self.assertNotIn(removed_field, descriptor)

    def test_regular_public_switches_keep_their_metadata(self):
        expected = {
            "autosell.auto_sell": ("0", "kGameplay", "kToggle", "自动出售"),
            "move_merge.drag_merge": ("1", "kBasicOptimization", "kToggle", "拖拽合并"),
            "push_all.push": ("0", "kGameplay", "kToggle", "推怪"),
            "simple_mode.combat": ("0", "kGameplay", "kToggle", "简单模式"),
            "custom_recipe.recipes": ("0", "kGameplay", "kToggle", "自定义合成"),
            "extension_bag.extended_bag": ("1", "kGameplay", "kToggle", "扩展背包"),
            "stack_limit.limit_999": ("0", "kGameplay", "kToggle", "堆叠上限"),
            "debug.logging": ("0", "kGeneral", "kToggle", "debug日志"),
            "attribute_range_display.ranges": ("1", "kBasicOptimization", "kToggle", "属性范围显示"),
            "character_panel_i18n.translation": ("1", "kBasicOptimization", "kToggle", "角色属性汉化"),
            "world_teleport.map_jump": ("1", "kBasicOptimization", "kToggle", "更好的地图传送对话框"),
        }
        sources = [source for source in PLUGINS.glob("*/plugin.cpp")
                   if not source.parent.name.startswith("core.")]
        sources.append(ROOT / "module/app/src/main/cpp/host/features/platform.cpp")
        actual = {
            feature_id: (default, category, kind, name)
            for source in sources
            for feature_id, default, category, kind, name, _ in FEATURE.findall(source.read_text())
            if category != "kHidden" and kind == "kToggle" and
            feature_id not in {"api.enabled", "op.enabled"}
        }
        self.assertEqual(actual, expected)
        self.assertEqual(len(actual), 11)

    def test_basic_optimization_metadata_and_defaults(self):
        expected = {
            "attribute_range_display": ("attribute_range_display.ranges", "属性范围显示"),
            "character_panel_i18n": ("character_panel_i18n.translation", "角色属性汉化"),
            "world_teleport": ("world_teleport.map_jump", "更好的地图传送对话框"),
            "move_merge": ("move_merge.drag_merge", "拖拽合并"),
        }
        for plugin, (feature_id, label) in expected.items():
            with self.subTest(plugin=plugin):
                features = FEATURE.findall((PLUGINS / plugin / "plugin.cpp").read_text())
                self.assertEqual(len(features), 1)
                self.assertEqual(features[0][:4],
                                 (feature_id, "1", "kBasicOptimization", "kToggle"))
                self.assertEqual(features[0][4], label)

    def test_hidden_feature_categories_remain_registered(self):
        features = FEATURE.findall((PLUGINS / "special_equip" / "plugin.cpp").read_text())
        self.assertEqual(len(features), 1)
        self.assertEqual(features[0][0], "special_equip.equipment_rules")
        self.assertEqual(features[0][2], "kHidden")
        module_health_source = (PLUGINS / "module_health" / "plugin.cpp").read_text()
        module_health = FEATURE.findall(module_health_source)
        self.assertEqual(module_health, [(
            "module_health.enabled", "1", "kHidden", "kToggle", "Module health", "",
        )])

    def test_core_custom_ui_debug_entry_remains_conditionally_registered(self):
        source = (PLUGINS / "core.custom_ui" / "plugin.cpp").read_text()
        entry = [feature for feature in FEATURE.findall(source)
                 if feature[0] == "core.custom_ui.open_debug_components"]
        self.assertEqual(entry, [(
            "core.custom_ui.open_debug_components", "1", "kHidden", "kEntry",
            "打开调试面板", "",
        )])
        self.assertRegex(
            source,
            re.compile(
                r'#if defined\(QOL_DEBUG_COMPONENTS\) && QOL_DEBUG_COMPONENTS\s*'
                r'\{"core\.custom_ui\.open_debug_components",\s*1,\s*'
                r'contracts::FeatureCategory::kHidden,\s*contracts::FeatureType::kEntry,.*?'
                r'nullptr,\s*&open_debug_components_feature,\s*"打开",\s*-5\s*\},?\s*#endif',
                re.DOTALL,
            ),
        )
        production = (ROOT / "module/app/src/main/cpp/host/features/production.cpp").read_text()
        self.assertIn('plugin.plugin_id == "settings"', production)
        self.assertNotIn('"core.api"', production)
        self.assertNotIn('"core.custom_ui"', production)

    def test_save_backup_entry_remains_in_regular_public_catalog(self):
        source = (PLUGINS / "save_backup" / "plugin.cpp").read_text()
        self.assertRegex(
            source,
            re.compile(
                r'\{"save_backup\.open_manager",\s*1,.*?FeatureType::kEntry,.*?'
                r'nullptr,\s*&open_manager_feature,\s*"打开",\s*-2\s*\}',
                re.DOTALL,
            ),
        )
        feature = FEATURE.findall(source)
        self.assertEqual(len(feature), 1)
        self.assertEqual(feature[0][:5], (
            "save_backup.open_manager", "1", "kTools", "kEntry", "存档备份导出",
        ))

    def test_platform_controls_remain_public(self):
        source = ROOT / "module/app/src/main/cpp/host/features/platform.cpp"
        features = FEATURE.findall(source.read_text())
        self.assertEqual({feature[0] for feature in features},
                         {"debug.logging", "op.enabled"})
        debug_logging = next(feature for feature in features if feature[0] == "debug.logging")
        security_op = next(feature for feature in features if feature[0] == "op.enabled")
        self.assertEqual(debug_logging[:4],
                         ("debug.logging", "0", "kGeneral", "kToggle"))
        self.assertEqual(debug_logging[4], "debug日志")
        self.assertEqual(security_op[:4],
                         ("op.enabled", "0", "kGeneral", "kToggle"))
        self.assertEqual(security_op[4], "操作权限")
        self.assertIn("kPlatformConfigOwner", source.read_text())
        self.assertIn("kSecurityConfigOwner", source.read_text())

    def test_host_feature_display_categories_and_priorities(self):
        production = (ROOT / "module/app/src/main/cpp/host/features/production.cpp").read_text()
        self.assertEqual(CATEGORY.findall(production), [
            ("kGeneral", "基础设置", "-9"),
            ("kBasicOptimization", "基础优化设置", "-8"),
            ("kGameplay", "更多优化功能", "-5"),
            ("kTools", "更多工具", "-3"),
            ("kHidden", "调试开关", "-10"),
        ])
        cases = {
            ("core.api/plugin.cpp", "api.enabled"): -5,
            ("host/features/platform.cpp", "op.enabled"): -4,
            ("host/features/platform.cpp", "debug.logging"): -3,
            ("extension_bag/plugin.cpp", "extension_bag.extended_bag"): -2,
            ("autosell/plugin.cpp", "autosell.auto_sell"): -1,
            ("custom_recipe/plugin.cpp", "custom_recipe.recipes"): -1,
            ("save_backup/plugin.cpp", "save_backup.open_manager"): -2,
            ("core.custom_ui/plugin.cpp", "core.custom_ui.open_debug_components"): -5,
        }
        for (relative, feature_id), priority in cases.items():
            path = (ROOT / "module/app/src/main/cpp" / (
                relative if relative.startswith("host/") else "plugins/" + relative
            ))
            with self.subTest(feature=feature_id):
                item = feature_initializer(path.read_text(), feature_id)
                self.assertRegex(item, rf",\s*{priority}\s*\}}")
        self.assertIn("feature display priority out of range",
                      (ROOT / "module/app/src/main/cpp/host/core_settings_config_store.cpp").read_text())

    def test_api_and_op_gates_have_separate_host_owners(self):
        api = (PLUGINS / "core.api" / "plugin.cpp").read_text()
        self.assertIn('"api.enabled"', api)
        self.assertNotIn('"core.api.http_registry"', api)
        store = (ROOT / "module/app/src/main/cpp/host/core_settings_config_store.cpp").read_text()
        self.assertIn('has_feature(definitions, "core.api", "api.enabled")', store)
        self.assertIn('has_feature(definitions, "module.security", "op.enabled")', store)
        api_migration = store.split(
            'if (has_feature(definitions, "core.api", "api.enabled")) {', 1
        )[1].split(
            '\n    if (has_feature(definitions, "module.security", "op.enabled")) {', 1
        )[0]
        self.assertIn('"module.platform", "api.enabled"', api_migration)
        self.assertIn('"core.api", "core.api.http_registry"', api_migration)
        self.assertIn('saved_root_value(*unknown, "apiEnabled")', api_migration)
        self.assertIn("platform_api && registry_api", api_migration)

        op_migration = store.split(
            'if (has_feature(definitions, "module.security", "op.enabled")) {', 1
        )[1].split("return true;", 1)[0]
        self.assertIn('"module.platform",\n                                                     "op.enabled"', op_migration)
        self.assertIn('saved_root_value(*unknown, "opEnabled")', op_migration)
        self.assertIn('"core.settings", "opEnabled"', op_migration)
        self.assertIn('"module.security",\n                                                      "op.enabled"', op_migration)
        self.assertNotIn("api_enabled", op_migration)


if __name__ == "__main__":
    unittest.main()

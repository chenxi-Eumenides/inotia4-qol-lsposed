"""Source-level checks for core visibility and player feature metadata."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
PLUGINS = ROOT / "module/app/src/main/cpp/plugins"
FEATURE = re.compile(
    r'\{"([^"]+)",\s*([01]),\s*(?:qol::)?contracts::FeatureCategory::(\w+),\s*'
    r'(?:qol::)?contracts::FeatureType::(\w+),\s*"([^"]*)",\s*"([^"]*)"'
)


class CoreFeatureExposureTests(unittest.TestCase):
    def test_every_core_feature_is_hidden_and_has_no_player_description(self):
        sources = sorted(PLUGINS.glob("core.*/plugin.cpp"))
        self.assertEqual(len(sources), 13)
        for source in sources:
            with self.subTest(plugin=source.parent.name):
                features = FEATURE.findall(source.read_text())
                self.assertTrue(features)
                for _, _, category, _, _, description in features:
                    self.assertEqual(category, "kHidden")
                    self.assertEqual(description, "")

    def test_public_switch_set_includes_basic_optimizations(self):
        # Keep the historical public switches and expose the newly categorized
        # basic optimizations without reclassifying unrelated metadata.
        expected_historical = {
            "autosell.auto_sell", "move_merge.drag_merge", "push_all.push",
            "simple_mode.combat", "custom_recipe.recipes",
            "extension_bag.extended_bag", "stack_limit.limit_999",
            "api.enabled", "debug.logging",
        }
        expected = expected_historical | {
            "attribute_range_display.ranges",
            "character_panel_i18n.translation",
            "world_teleport.map_jump",
        }
        sources = [source for source in PLUGINS.glob("*/plugin.cpp")
                   if not source.parent.name.startswith("core.")]
        sources.append(ROOT / "module/app/src/main/cpp/host/features/platform.cpp")
        actual = {
            feature_id
            for source in sources
            for feature_id, _, category, kind, _, _ in FEATURE.findall(source.read_text())
            if category != "kHidden" and kind == "kToggle"
        }
        self.assertEqual(actual, expected)

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

    def test_unexposed_switch_remains_hidden(self):
        features = FEATURE.findall((PLUGINS / "special_equip" / "plugin.cpp").read_text())
        self.assertEqual(len(features), 1)
        self.assertEqual(features[0][0], "special_equip.equipment_rules")
        self.assertEqual(features[0][2], "kHidden")

    def test_core_custom_ui_only_has_debug_hidden_entry_exception(self):
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
                r'nullptr,\s*&open_debug_components_feature,\s*"打开"\s*\},?\s*#endif',
                re.DOTALL,
            ),
        )
        production = (ROOT / "module/app/src/main/cpp/host/features/production.cpp").read_text()
        self.assertRegex(
            production,
            re.compile(
                r'feature\.id != nullptr &&\s*'
                r'std::strcmp\(feature\.id, "core\.custom_ui\.open_debug_components"\) == 0 &&\s*'
                r'feature\.type == contracts::FeatureType::kEntry &&\s*'
                r'feature\.category == contracts::FeatureCategory::kHidden &&\s*'
                r'feature\.on_entry != nullptr'
            ),
        )

    def test_save_backup_entry_action_label_is_registered(self):
        source = (PLUGINS / "save_backup" / "plugin.cpp").read_text()
        self.assertRegex(
            source,
            re.compile(
                r'\{"save_backup\.open_manager",\s*1,.*?FeatureType::kEntry,.*?'
                r'nullptr,\s*&open_manager_feature,\s*"打开"\s*\}',
                re.DOTALL,
            ),
        )

    def test_platform_controls_remain_public(self):
        source = ROOT / "module/app/src/main/cpp/host/features/platform.cpp"
        features = FEATURE.findall(source.read_text())
        self.assertEqual({feature[0] for feature in features},
                         {"api.enabled", "debug.logging"})
        for _, _, category, kind, _, _ in features:
            self.assertEqual(category, "kGeneral")
            self.assertEqual(kind, "kToggle")
        self.assertEqual({feature[0]: feature[4] for feature in features},
                         {"api.enabled": "API服务", "debug.logging": "调试日志"})


if __name__ == "__main__":
    unittest.main()

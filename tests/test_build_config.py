import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))


class BuildConfigTests(unittest.TestCase):
    def load_config(self):
        self.assertIsNotNone(importlib.util.find_spec('build_config'),
                             'Cross-platform build configuration is not implemented')
        import build_config
        return build_config

    def test_android_selection_preserves_available_abis(self):
        configuration = self.load_config()
        self.assertEqual(configuration.select_abis('android', 'all'),
                         ['arm64-v8a', 'armeabi-v7a', 'x86'])
        self.assertEqual(configuration.select_abis('ios', 'all'), ['arm64-v8a'])
        self.assertEqual(configuration.select_abis('harmony', 'all'), ['arm64-v8a'])
        with self.assertRaises(ValueError):
            configuration.select_abis('android', 'mips')

    def test_crc_patch_is_fail_closed_and_does_not_duplicate(self):
        configuration = self.load_config()
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            manifest = source / 'meson.build'
            manifest.write_text("absl_dep = [\n  dependency('absl_bad_optional_access'),\n]\n")
            configuration.patch_crc_dependency(source)
            self.assertEqual(manifest.read_text().count("dependency('absl_crc')"), 1)
            with self.assertRaises(ValueError):
                configuration.patch_crc_dependency(source)
            manifest.write_text('unexpected upstream source')
            with self.assertRaises(ValueError):
                configuration.patch_crc_dependency(source)

    def test_android_cross_file_uses_target_compilers_not_host(self):
        configuration = self.load_config()
        with patch.object(configuration.sys, 'platform', 'linux'):
            text = configuration.android_cross_file(Path('/sdk/ndk'), 'arm64-v8a', 28)
        self.assertIn('aarch64-linux-android28-clang++', text)
        self.assertIn("system = 'android'", text)
        self.assertIn('needs_exe_wrapper = true', text)
        self.assertIn('max-page-size=16384', text)
        self.assertIn('pkg_config_libdir = []', text)

    def test_harmony_cross_file_uses_ohos_target_and_sysroot(self):
        configuration = self.load_config()
        text = configuration.harmony_cross_file(Path('/sdk/native'))
        self.assertIn('--target=aarch64-linux-ohos', text)
        self.assertIn('--sysroot=/sdk/native/sysroot', text)
        self.assertNotIn('android', text)
        self.assertIn('needs_exe_wrapper = true', text)

    def test_ios_cross_file_targets_device_and_minimum_os(self):
        configuration = self.load_config()
        text = configuration.ios_cross_file('/Xcode/iPhoneOS.sdk', '14.0')
        self.assertIn('arm64-apple-ios14.0', text)
        self.assertIn('/Xcode/iPhoneOS.sdk', text)
        self.assertIn("system = 'darwin'", text)
        self.assertNotIn('simulator', text)

    def test_unsafe_cross_file_paths_are_rejected(self):
        configuration = self.load_config()
        with self.assertRaises(ValueError):
            configuration.harmony_cross_file(Path('/sdk/bad\npath'))


if __name__ == '__main__':
    unittest.main()

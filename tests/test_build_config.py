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

    def test_additive_statistics_exports_retain_existing_abi(self):
        configuration = self.load_config()
        old_exports = {
            'aozora_apm_create', 'aozora_apm_destroy', 'aozora_apm_process_reverse',
            'aozora_apm_process_capture', 'aozora_apm_set_stream_delay_ms',
            'aozora_apm_get_voice_detected', 'aozora_apm_frame_samples',
        }
        self.assertTrue(old_exports <= configuration.C_EXPORTS)
        for platform in ('host', 'android', 'ios', 'harmony'):
            with self.subTest(platform=platform):
                _, export_map = configuration.wrapper_meson(platform)
                self.assertIn('    aozora_apm_get_build_revision;\n', export_map)
                self.assertIn('    aozora_apm_get_statistics;\n', export_map)
        _, android_exports = configuration.wrapper_meson('android')
        for method in ('nativeGetBuildRevision', 'nativeGetStatistics'):
            self.assertIn('Java_com_aozora_audio_AozoraWebRtcApm_' + method + ';',
                          android_exports)

    def test_host_build_explicitly_compiles_all_registered_tests(self):
        import build
        commands = []
        with patch.object(build, 'run', side_effect=lambda *args, **kwargs: commands.append(args)):
            build.configure_build('source', 'build', None, 3, {}, 'host')
        compile_command = commands[-1]
        for target in ('apm_smoke', 'apm_statistics', 'apm_echo_regression'):
            self.assertIn(target, compile_command)
        configuration = self.load_config()
        meson, _ = configuration.wrapper_meson('host')
        self.assertIn("test('native-statistics-contract'", meson)
        self.assertIn("test('native-echo-regression'", meson)
        self.assertNotIn('should_fail:', meson)
        for fault in ('disconnect-reference', 'zero-output'):
            self.assertRegex(meson, r"test\('native-echo-" + fault +
                             r"', mutation_python[^\n]+mutation_runner, echo_regression")

    def test_cross_platform_builds_do_not_compile_host_test_programs(self):
        import build
        configuration = self.load_config()
        for platform in ('android', 'ios', 'harmony'):
            with self.subTest(platform=platform):
                commands = []
                with patch.object(build, 'run', side_effect=lambda *args, **kwargs: commands.append(args)):
                    build.configure_build('source', 'build', 'cross', 3, {}, platform)
                self.assertEqual(commands[-1][-1], 'aozora_apm')
                meson, _ = configuration.wrapper_meson(platform)
                self.assertNotIn('executable(', meson)
                self.assertNotIn('test(', meson)


if __name__ == '__main__':
    unittest.main()

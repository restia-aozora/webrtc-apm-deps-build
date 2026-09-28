import os
import stat
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))

from support import LOCK, copy_file, extract_native_sdk, use_abseil_release_mirror, verify_abseil_lock


class SupportTests(unittest.TestCase):
    def test_same_file_copy_preserves_existing_header(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'header.h'
            source.write_bytes(b'header')
            link = source.with_name('link.h')
            link.hardlink_to(source)
            copy_file(source, link)
            self.assertEqual(source.read_bytes(), b'header')
            self.assertTrue(source.samefile(link))

    def test_missing_source_copy_is_not_hidden(self):
        with tempfile.TemporaryDirectory() as temporary:
            missing = Path(temporary) / 'missing'
            with self.assertRaises(FileNotFoundError):
                copy_file(missing, missing)

    def test_extracts_only_native_sdk(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / 'sdk.zip'
            prefix = 'command-line-tools/sdk/default/openharmony/native'
            with zipfile.ZipFile(archive, 'w') as bundle:
                bundle.writestr(prefix + '/build/cmake/ohos.toolchain.cmake', 'toolchain')
                bundle.writestr(prefix + '/llvm/bin/clang', 'compiler')
                bundle.writestr('command-line-tools/other/large-file', 'not required')
            native = extract_native_sdk(archive, root / 'sdk')
            self.assertEqual((native / 'llvm/bin/clang').read_text(), 'compiler')
            self.assertFalse((root / 'sdk/command-line-tools/other').exists())

    def test_rejects_missing_or_ambiguous_sdk(self):
        for prefixes in ([], ['one/native', 'two/native']):
            with self.subTest(prefixes=prefixes), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                with zipfile.ZipFile(root / 'sdk.zip', 'w') as bundle:
                    for prefix in prefixes:
                        bundle.writestr(prefix + '/build/cmake/ohos.toolchain.cmake', 'toolchain')
                with self.assertRaises(ValueError):
                    extract_native_sdk(root / 'sdk.zip', root / 'sdk')

    def test_rejects_traversal_and_escaping_symlinks(self):
        for unsafe_link in (False, True):
            with self.subTest(symlink=unsafe_link), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                with zipfile.ZipFile(root / 'sdk.zip', 'w') as bundle:
                    bundle.writestr('native/build/cmake/ohos.toolchain.cmake', 'toolchain')
                    if unsafe_link:
                        entry = zipfile.ZipInfo('native/compiler')
                        entry.create_system = 3
                        entry.external_attr = (stat.S_IFLNK | 0o777) << 16
                        bundle.writestr(entry, '../../outside')
                    else:
                        bundle.writestr('../outside', 'escape')
                with self.assertRaises(ValueError):
                    extract_native_sdk(root / 'sdk.zip', root / 'sdk')
                self.assertFalse((root / 'outside').exists())

    @unittest.skipIf(os.name == 'nt', 'Linux CI verifies executable and symbolic link permissions')
    def test_preserves_sdk_symlinks_and_executable_bits(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with zipfile.ZipFile(root / 'sdk.zip', 'w') as bundle:
                bundle.writestr('native/build/cmake/ohos.toolchain.cmake', 'toolchain')
                compiler = zipfile.ZipInfo('native/llvm/bin/clang-15')
                compiler.create_system = 3
                compiler.external_attr = (stat.S_IFREG | 0o755) << 16
                bundle.writestr(compiler, 'compiler')
                link = zipfile.ZipInfo('native/llvm/bin/clang')
                link.create_system = 3
                link.external_attr = (stat.S_IFLNK | 0o777) << 16
                bundle.writestr(link, 'clang-15')
            native = extract_native_sdk(root / 'sdk.zip', root / 'sdk')
            self.assertTrue((native / 'llvm/bin/clang').is_symlink())
            self.assertTrue(os.access(native / 'llvm/bin/clang', os.X_OK))

    def test_rejects_drifted_abseil_wrap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'subprojects').mkdir()
            (root / 'subprojects/abseil-cpp.wrap').write_text('[wrap-file]\ndirectory = changed\n')
            with self.assertRaises(ValueError):
                verify_abseil_lock(root)

    def test_official_patch_mirror_keeps_locked_dependency_hashes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'subprojects').mkdir()
            manifest = root / 'subprojects/abseil-cpp.wrap'
            manifest.write_text('[wrap-file]\n' + ''.join(
                f'{key} = {value}\n' for key, value in LOCK['abseil'].items()) +
                'patch_url = https://wrapdb.mesonbuild.com/v2/abseil-cpp_20230125.1-4/get_patch\n')
            verify_abseil_lock(root)
            use_abseil_release_mirror(root)
            verify_abseil_lock(root)
            self.assertIn('github.com/mesonbuild/wrapdb/releases/download/', manifest.read_text())
            with self.assertRaises(ValueError):
                use_abseil_release_mirror(root)


if __name__ == '__main__':
    unittest.main()

import itertools
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))

from build_config import select_abis

ABI_INPUTS = {
    'android_arm64': ('arm64-v8a', 'ANDROID_ARM64'),
    'android_armv7': ('armeabi-v7a', 'ANDROID_ARMV7'),
    'android_x86': ('x86', 'ANDROID_X86'),
}


class DispatchTests(unittest.TestCase):
    def setUp(self):
        self.workflow = yaml.load(
            (ROOT / '.github/workflows/build.yml').read_text(encoding='utf-8'),
            Loader=yaml.BaseLoader)

    def test_checkbox_inputs_default_to_arm64_only(self):
        inputs = self.workflow['on']['workflow_dispatch']['inputs']
        self.assertNotIn('android_abis', inputs)
        for name in ABI_INPUTS:
            self.assertEqual(inputs[name]['type'], 'boolean')
            self.assertEqual(inputs[name]['default'], 'true' if name == 'android_arm64' else 'false')

    def test_cli_accepts_every_nonempty_abi_subset(self):
        available = [entry[0] for entry in ABI_INPUTS.values()]
        for size in range(1, len(available) + 1):
            for selected in itertools.combinations(available, size):
                with self.subTest(selected=selected):
                    self.assertEqual(select_abis('android', list(selected)), list(selected))

    def test_empty_android_selection_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'at least one'):
            select_abis('android', [])

    def test_all_remains_available_without_mixing_with_explicit_abis(self):
        self.assertEqual(select_abis('android', ['all']),
                         ['arm64-v8a', 'armeabi-v7a', 'x86'])
        with self.assertRaises(ValueError):
            select_abis('android', ['all', 'x86'])

    def test_other_platforms_ignore_android_selection(self):
        for platform in ('ios', 'harmony', 'host'):
            self.assertEqual(select_abis(platform, []), ['arm64-v8a'])

    def test_cli_parses_multiple_abis_before_validating_jobs(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / 'scripts/build.py'), 'android',
             '--android-abis', 'arm64-v8a', 'x86', '--jobs', '0'],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('jobs must be positive', result.stderr)
        self.assertNotIn('BUILD DIRECTORY', result.stdout)

    def run_workflow_step(self, job_name, step_name, environment):
        step = next(step for step in self.workflow['jobs'][job_name]['steps']
                    if step.get('name') == step_name)
        bash = shutil.which('bash')
        if os.name == 'nt':
            git_executable = shutil.which('git')
            candidates = [Path('C:/Program Files/Git/bin/bash.exe'),
                          Path('C:/Program Files/Git/usr/bin/bash.exe')]
            if git_executable:
                git_root = Path(git_executable).resolve().parent.parent
                candidates.extend((git_root / 'bin/bash.exe', git_root / 'usr/bin/bash.exe'))
            bash = next((str(candidate) for candidate in candidates if candidate.is_file()), None)
        if not bash:
            self.skipTest('Bash is required for workflow shell integration tests')
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'output.txt'
            env = os.environ.copy()
            env.update(environment)
            env['GITHUB_OUTPUT'] = output.as_posix()
            env['GITHUB_REPOSITORY'] = 'restia-aozora/webrtc-apm-deps-build'
            result = subprocess.run([bash, '-e', '-c', step['run']], env=env,
                                    capture_output=True, text=True, timeout=15)
            return result, output.read_text() if output.exists() else ''

    def test_checkboxes_reach_build_command_for_every_combination(self):
        steps = self.workflow['jobs']['build-android']['steps']
        build = next(step for step in steps if step.get('name') == 'Build Android libraries')
        self.assertEqual(build['env']['SELECTED_ABIS'], '${{ steps.android_abis.outputs.abis }}')
        self.assertIn('"${selected_abis[@]}"', build['run'])
        for flags in itertools.product((False, True), repeat=3):
            selected = [entry[0] for entry, enabled in zip(ABI_INPUTS.values(), flags) if enabled]
            environment = {entry[1]: str(enabled).lower()
                           for entry, enabled in zip(ABI_INPUTS.values(), flags)}
            with self.subTest(selected=selected):
                result, output = self.run_workflow_step(
                    'build-android', 'Resolve Android ABI selection', environment)
                if selected:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(output.strip(), 'abis=' + ' '.join(selected))
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('Select at least one Android ABI', result.stdout)
                    self.assertEqual(output, '')

    def test_harmony_diagnostics_identify_missing_url_without_leaking_it(self):
        valid_hash = 'a' * 64
        cases = [
            ('', valid_hash, 'Missing HARMONY_SDK_URL'),
            ('https://example.invalid/sdk.zip?token=do-not-log', '', 'Invalid HARMONY_SDK_SHA256'),
            ('http://example.invalid/sdk.zip?token=do-not-log', valid_hash, 'must use HTTPS'),
        ]
        for url, checksum, message in cases:
            with self.subTest(message=message):
                result, _ = self.run_workflow_step('build-harmony', 'Verify Harmony SDK configuration', {
                    'HARMONY_SDK_URL': url, 'HARMONY_SDK_SHA256': checksum})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stdout)
                self.assertNotIn('do-not-log', result.stdout + result.stderr)
        result, _ = self.run_workflow_step('build-harmony', 'Verify Harmony SDK configuration', {
            'HARMONY_SDK_URL': 'https://example.invalid/sdk.zip?token=do-not-log',
            'HARMONY_SDK_SHA256': valid_hash})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('do-not-log', result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()

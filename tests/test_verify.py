import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))

from build_config import C_EXPORTS
from verify import verify_ios


class IosVerificationTests(unittest.TestCase):
    def verify_fixture(self, minimum='14.0', platform='IOS', dependency='/usr/lib/libc++.1.dylib'):
        responses = [
            'arm64\n',
            f'platform {platform}\nminos {minimum}\nsdk 18.0\n',
            'AozoraApm:\n\t@rpath/AozoraApm.framework/AozoraApm (compatibility version 0.0.0)\n'
            f'\t{dependency} (compatibility version 1.0.0)\n',
            '\n'.join('_' + symbol for symbol in C_EXPORTS),
        ]
        with patch('verify.run', side_effect=responses):
            return verify_ios(Path('AozoraApm'), '14.0')

    def test_equivalent_minimum_versions_are_accepted(self):
        for minimum in ('14', '14.0', '14.0.0'):
            with self.subTest(minimum=minimum):
                self.assertEqual(self.verify_fixture(minimum)['architecture'], 'arm64')

    def test_newer_minimum_os_is_rejected(self):
        for minimum in ('14.0.1', '14.1', '15.0'):
            with self.subTest(minimum=minimum), self.assertRaises(ValueError):
                self.verify_fixture(minimum)

    def test_simulator_is_not_a_device_framework(self):
        with self.assertRaises(ValueError):
            self.verify_fixture(platform='IOSSIMULATOR')

    def test_local_build_dependency_is_rejected(self):
        with self.assertRaises(ValueError):
            self.verify_fixture(dependency='/tmp/build/libwebrtc.dylib')


if __name__ == '__main__':
    unittest.main()

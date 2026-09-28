import re
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]


class WorkflowTests(unittest.TestCase):
    def test_native_build_is_manual_only_and_routes_each_selection(self):
        workflow = yaml.load((ROOT / '.github/workflows/build.yml').read_text(encoding='utf-8'),
                             Loader=yaml.BaseLoader)
        self.assertEqual(set(workflow['on']), {'workflow_dispatch'})
        platform = workflow['on']['workflow_dispatch']['inputs']['platform']
        self.assertEqual(platform['default'], 'all')
        self.assertEqual(set(platform['options']), {'all', 'android', 'ios', 'harmony'})
        platform_jobs = {name: job for name, job in workflow['jobs'].items() if name.startswith('build-')}
        for selection in platform['options']:
            selected = set()
            for name, job in platform_jobs.items():
                self.assertNotIn('needs', job)
                terms = [re.fullmatch(r"inputs\.platform == '([^']+)'", term)
                         for term in job['if'].split(' || ')]
                self.assertTrue(all(terms))
                if any(selection == term.group(1) for term in terms):
                    selected.add(name)
            expected = set(platform_jobs) if selection == 'all' else {'build-' + selection}
            self.assertEqual(selected, expected)

    def test_native_artifacts_are_required_and_do_not_upload_worktrees(self):
        workflow = yaml.load((ROOT / '.github/workflows/build.yml').read_text(encoding='utf-8'),
                             Loader=yaml.BaseLoader)
        names = []
        for job in workflow['jobs'].values():
            for step in job['steps']:
                if step.get('uses', '').startswith('actions/upload-artifact@'):
                    configuration = step['with']
                    self.assertEqual(configuration['if-no-files-found'], 'error')
                    self.assertTrue(configuration['path'].startswith('dist/webrtc-apm-'))
                    self.assertNotIn('.work', configuration['path'])
                    names.append(configuration['name'])
        self.assertEqual(len(names), 3)
        self.assertEqual(len(set(names)), 3)

    def test_automatic_checks_do_not_compile_native_libraries(self):
        text = (ROOT / '.github/workflows/checks.yml').read_text(encoding='utf-8')
        self.assertNotIn('scripts/build.py', text)
        self.assertIn('unittest discover', text)


if __name__ == '__main__':
    unittest.main()

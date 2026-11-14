import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

from native_echo_mutation_runner import run_mutation, run_rejection_control


class MutationRunnerTests(unittest.TestCase):
    def run_child(self, program, predicate='echo-attenuation', timeout=5):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            accepted = run_mutation([sys.executable, '-c', program], predicate, timeout)
        return accepted, output.getvalue()

    def test_actual_child_prerequisite_failure_is_not_detection(self):
        with tempfile.TemporaryDirectory() as directory:
            missing_input = str(Path(directory) / 'required-input-not-created')
            accepted, output = self.run_child(
                f'from pathlib import Path; Path({missing_input!r}).read_bytes()')
        self.assertFalse(accepted)
        self.assertIn('FileNotFoundError', output)

    def test_successful_child_without_detected_predicate_is_rejected(self):
        accepted, output = self.run_child("print('PASS: normal predicates all satisfied')")
        self.assertFalse(accepted)
        self.assertIn('missing expected detection', output)

    def test_undetected_mutation_failure_is_not_success(self):
        accepted, _ = self.run_child(
            "import sys; print('FAIL: mutation was not detected'); sys.exit(1)")
        self.assertFalse(accepted)

    def test_wrong_predicate_cannot_satisfy_requested_mutation(self):
        accepted, _ = self.run_child("print('PASS: detected mutation: near-preservation')")
        self.assertFalse(accepted)

    def test_nonzero_child_cannot_pass_even_with_detection_marker(self):
        accepted, _ = self.run_child(
            "import sys; print('PASS: detected mutation: echo-attenuation'); sys.exit(1)")
        self.assertFalse(accepted)

    def test_both_explicit_positive_detection_markers_are_accepted(self):
        for predicate in ('echo-attenuation', 'near-preservation'):
            with self.subTest(predicate=predicate):
                accepted, output = self.run_child(
                    f"print('measurement=1.25'); print('PASS: detected mutation: {predicate}')",
                    predicate)
                self.assertTrue(accepted)
                self.assertIn('measurement=1.25', output)

    def test_actual_child_timeout_is_failure(self):
        accepted, output = self.run_child('import time; time.sleep(10)', timeout=0.1)
        self.assertFalse(accepted)
        self.assertIn('timed out', output)

    def test_missing_executable_is_failure(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertFalse(run_mutation(['nonexistent-native-echo-executable'],
                                          'echo-attenuation', 5))

    def test_rejection_control_requires_its_specific_failure_not_any_failure(self):
        expected_failure = 'FAIL: mutation was not detected'
        cases = [
            ("import sys; print('FAIL: mutation was not detected', file=sys.stderr); sys.exit(1)", True),
            ("import sys; print('FAIL: unrelated processing failure', file=sys.stderr); sys.exit(1)", False),
            ("print('PASS: detected mutation: echo-attenuation')", False),
            ("import sys; print('FAIL: mutation was not detected', file=sys.stderr)", False),
        ]
        for program, expected_result in cases:
            with self.subTest(program=program):
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(run_rejection_control(
                        [sys.executable, '-c', program], expected_failure, 5), expected_result)


if __name__ == '__main__':
    unittest.main()

"""Run a native mutation process and validate its detection result."""

import argparse
import subprocess
import sys


def run_process(command, timeout):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f'FAIL: mutation process: {error}', file=sys.stderr)
        return None
    print(result.stdout, end='')
    print(result.stderr, end='', file=sys.stderr)
    return result


def run_mutation(command, predicate, timeout=180):
    result = run_process(command, timeout)
    if result is None:
        return False
    if result.returncode != 0:
        print(f'FAIL: mutation process exited {result.returncode}', file=sys.stderr)
        return False
    expected_marker = f'PASS: detected mutation: {predicate}'
    if expected_marker not in result.stdout.splitlines():
        print(f'FAIL: missing expected detection: {predicate}', file=sys.stderr)
        return False
    return True


def run_rejection_control(command, expected_failure, timeout=180):
    result = run_process(command, timeout)
    if result is None:
        return False
    if (result.returncode != 1 or expected_failure not in result.stderr.splitlines() or
            any(line.startswith('PASS: detected mutation:') for line in result.stdout.splitlines())):
        print('FAIL: rejection control did not fail at the intended prerequisite/predicate',
              file=sys.stderr)
        return False
    print(f'PASS: rejection control: {expected_failure}')
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable')
    parser.add_argument('fault', choices=('disconnect-reference', 'zero-output'))
    arguments = parser.parse_args()
    predicate = ('echo-attenuation' if arguments.fault == 'disconnect-reference'
                 else 'near-preservation')
    command = [arguments.executable, '--' + arguments.fault]
    if not run_rejection_control(command + ['--invalid-configuration'],
                                 'FAIL: create actual AEC3 with NS=0 and AGC=0'):
        return 1
    undetected_failure = (
        'FAIL: disconnect-reference mutation was not detected by echo attenuation predicate'
        if arguments.fault == 'disconnect-reference' else
        'FAIL: zero-output mutation was not detected by both near preservation predicates')
    if not run_rejection_control(command + ['--disable-mutation'], undetected_failure):
        return 1
    return 0 if run_mutation(command, predicate) else 1


if __name__ == '__main__':
    sys.exit(main())

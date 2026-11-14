# Native API implementation report - 2026-11-14

## Scope and status

Implementation is present in the working tree based on HEAD 55aa915 on
fix/native-aec-evidence-20261114. No commit, staging, push, GitHub query,
workflow dispatch, recording, playback, audio download/upload, or device use
was performed by this task. No application files were changed.

The original README changes and tests/native_echo_probe.cpp were not edited.
Existing unrelated CRLF-only changes were not cleaned. The upstream lock,
AEC3/NS/AGC/VAD configuration, delay bounds and audio processing order are
unchanged. The existing cache and upstream repository were read only.

Local compilation and Python checks passed as detailed below. Native host
execution, effect thresholds, positive mutation self-validation, JNI runtime
behavior, final packaging and ELF verification remain NOT RUN for this
implementation. These are explicit remaining gates, not claimed passes.

## Modified and added files

- native/aozora_apm.cpp: seed the reverse destination from PCM input only on
  the reverse branch. Cache accepted-capture statistics under process_mutex.
- native/aozora_apm.h: additive revision/statistics APIs and fixed-width v1
  structure; all previous declarations and error values are retained.
- native/aozora_apm_jni.cpp: additive String revision and nullable 14-element
  DoubleArray statistics. A live-JNI-handle registry protects the new getter
  from null, unknown and already-released handles and serializes it against
  JNI release. Existing processing entry points retain their audio path.
- scripts/build_config.py: additive C/JNI export sets and generated version
  map; three real linked host executables and five Meson test registrations.
- scripts/build.py: copy all host test sources, explicitly compile all three
  host targets, and include wrapperCommit in package manifests. Cross-platform
  builds still compile only the wrapper, not host test programs.
- tests/native_statistics.cpp: real-library frozen-snapshot contract test.
- tests/native_echo_regression.cpp: real-library synthetic effect controls and
  test-only reference-disconnection/all-zero-output fault injection.
- tests/native_echo_mutation_runner.py: positive mutation validation and
  specific real-native rejection controls, host tests only.
- tests/test_native_echo_mutation_runner.py: executable subprocess protocol
  checks for prerequisite failures, detection, undetected faults and timeouts.
- tests/test_build_config.py: export/ABI preservation and host/cross target
  selection regression checks.
- docs/superpowers/plans/2026-11-14-native-aec-evidence.md: implementation
  checkpoint and local evidence ledger, leaving native execution unchecked.
- This report.

tests/native_smoke.cpp already contained the nonzero reverse regression at
HEAD 55aa915; this task did not replace or edit that regression.

## C and JNI contract

aozora_apm_get_build_revision() returns native-apm-v2-stats1.

AozoraApmStatistics is 88 bytes, counters start at byte 16, and doubles at
byte 56. Compile-time checks cover ARM64, ARMv7 and x86. Field order is:

    uint32: struct_size, version, valid_fields, sample_rate_hz
    uint64: capture_frames, reverse_frames
    int32: stream_delay_ms, voice_detected, estimated_delay_ms,
           noise_suppression_enabled, gain_control_enabled
    uint32: reserved
    double: echo_return_loss_db, echo_return_loss_enhancement_db,
            residual_echo_likelihood, residual_echo_likelihood_recent_max

Valid bits are voice=1, ERL=2, ERLE=4, internal delay=8, residual=16,
recent maximum=32. Missing doubles remain NaN; missing voice/internal delay
remain -1 with clear bits. A finite zero with a set bit is valid data.
Non-finite upstream optional doubles are not advertised as valid.

Before the first accepted capture, both published counters and the mask are
zero, while rate and creation-time NS/AGC flags are known. Successful reverse
calls increment an internal counter but do not publish a new snapshot. The
next accepted capture publishes both counters and its actual submitted delay.
A setter or a later reverse cannot change that frozen snapshot. GetStatistics
is called once at the existing accepted-capture observation point, never by
the getter. Null/short destination errors do not write bytes or consume stats;
larger destinations retain their tail beyond the v1 structure.

The new JNI array contract is unchanged from the approved 14-field schema:

    [1, validMask, captureFrames, reverseFrames, streamDelayMs, voiceDetected,
     sampleRateHz, estimatedDelayMs, erlDb, erleDb, residualLikelihood,
     residualLikelihoodRecentMax, noiseSuppressionEnabled, gainControlEnabled]

JNI allocation failures retain Java's pending exception behavior and return
null. The new getter does not dereference handles outside the live JNI set.
The C pointer ABI still requires a live instance and caller-serialized
destruction, exactly as documented previously; arbitrary stale C pointers are
not made safe by the new getter. Existing audio JNI entry points also retain
their established lifetime requirements. No Harmony ArkTS API was added.

## Focused red evidence observed before implementation

### Reverse regression

Accepted existing real-library evidence from the on-disk plan:

    HEAD 55aa915
    GitHub run 43577044665, host-smoke job 132393440492
    FAIL: analysis-only reverse must preserve nonzero reference PCM

That host job built the pinned real upstream library and failed the behavioral
assertion before the repair. This task did not rerun or operate GitHub.

### New statistics API

Created tests/native_statistics.cpp before adding the type or implementations.
Ran the actual NDK compiler against the old public header (arguments below;
compiler denotes NDK r27c windows-x86_64/bin/clang++.exe):

    & $compiler --target=aarch64-linux-android28 -std=c++17 -fsyntax-only \
      -ferror-limit=4 -I native tests/native_statistics.cpp

Observed exit 1 with the expected missing API/type, not an SDK failure:

    tests/native_statistics.cpp:24:1: error: unknown type name 'AozoraApmStatistics'
    tests/native_statistics.cpp:25:3: error: unknown type name 'AozoraApmStatistics'
    tests/native_statistics.cpp:31:32: error: unknown type name 'AozoraApmStatistics'
    tests/native_statistics.cpp:32:32: error: unknown type name 'AozoraApmStatistics'
    fatal error: too many errors emitted, stopping now [-ferror-limit=]
    5 errors generated.

### Export/build registration regression

Added targeted Python checks before modifying the build scripts. Ran:

    python -m unittest discover -s tests -p test_build_config.py -v

Observed exit 1: missing aozora_apm_get_build_revision and statistics exports
in generated platform maps, missing new JNI symbols, and missing apm_statistics
in the explicit host compile command. Cross-platform wrapper-only checks
already passed. After the build changes these focused checks passed in the
full suite below.

## Fresh local verification

All commands used repository root as their working directory. Paths below use
readable variable placeholders for the same installed NDK, pinned upstream
webrtc include directory and existing cached Abseil include directory; they
are not alternate/mock headers. No command modified those directories.

1. Python regression:

       python -m unittest discover -s tests -q

   Actual output, exit 0:

       Ran 32 tests in 2.702s
       OK (skipped=1)

   The skip is the pre-existing Linux-only SDK permissions/symlink check.
   This is 31 passed plus one skipped, not 32 native tests.

2. Public contract, all three native tests, and JNI syntax, each target:

       & $compiler --target=$target -std=c++17 -Wall -Wextra -Werror \
         -fsyntax-only -I native tests/native_statistics.cpp \
         tests/native_echo_regression.cpp tests/native_smoke.cpp \
         native/aozora_apm_jni.cpp

   Targets aarch64-linux-android28, armv7a-linux-androideabi28 and
   i686-linux-android28 each returned exit 0 with no diagnostics. ARM64 was
   repeated after the final aligned test buffer and JNI exception guard edits,
   also exit 0. These commands do not link or run WebRTC.

3. Wrapper syntax against the actual pinned upstream header:

       & $compiler --target=$target -std=c++17 -Wall -Wextra -Werror \
         -Wno-unused-parameter -fsyntax-only -DWEBRTC_POSIX -DWEBRTC_ANDROID \
         -DWEBRTC_LINUX $architectureDefines -DWEBRTC_APM_DEBUG_DUMP=0 \
         -I native -I $upstream -I $abseil native/aozora_apm.cpp

   ARM64 used -DWEBRTC_ARCH_ARM64 -DWEBRTC_HAS_NEON; ARMv7 used
   -DWEBRTC_ARCH_ARM -DWEBRTC_HAS_NEON; x86 used -DWEBRTC_ARCH_X86.
   Each returned exit 0 with no diagnostics. Unused upstream parameter
   warnings were suppressed explicitly; no dependency/configuration changed.

4. Public C11 header compatibility:

       '#include "aozora_apm.h"' | & $cCompiler \
         --target=aarch64-linux-android28 -x c -std=c11 -Wall -Wextra -Werror \
         -fsyntax-only -I native -

   Actual exit 0, no diagnostics ($cCompiler is the same NDK's clang.exe).

5. Scoped whitespace review:

       git diff --check -- native/aozora_apm.cpp native/aozora_apm.h \
         native/aozora_apm_jni.cpp scripts/build.py scripts/build_config.py \
         tests/test_build_config.py

   Actual exit 0. Git printed existing CRLF-to-LF advisory warnings; no
   whitespace error or cleanup operation occurred.

6. ReadLints over changed C/C++/Python files returned totalDiagnostics=0.
   IDE diagnostics are not a substitute for native execution.

## Native tests prepared, not executed locally

The generated host Meson setup links every test with the actual wrapper and
its pinned upstream implementation; no mock WebRTC, fake statistics queue or
production fault hook exists.

- apm_smoke / native-api-smoke retains the proven nonzero reverse assertion.
- apm_statistics / native-statistics-contract covers initial unknown values,
  valid false voice vs unknown, counters, getter bytewise idempotence, reverse
  publication only on capture, capture-submitted delay vs later setter,
  rejected calls, all 16 sample-rate/NS/AGC combinations, short buffer guards,
  null errors and oversized destination tail preservation.
- apm_echo_regression / native-echo-regression generates all PCM in memory.
  NS=0 and AGC=0 are asserted through the C API. A deterministic independent
  render, wrong reference and near proxy feed a 40 ms delayed multipath echo.
  Twenty seconds of pure-echo adaptation precede eight evaluation seconds.
  Matched echo attenuation must be >=15 dB; disconnected and wrong-reference
  controls must each be <5 dB.
- Independent near-only and double-talk runs measure aligned projection gain
  and normalized correlation against the independent near proxy, not just
  total output energy. Lag search is bounded to 0..480 samples. Near-only gain
  must be 0.55..1.50 with correlation >=0.70; double-talk gain must be
  0.40..1.50, >=60% of near-only, with correlation >=0.45 and >=55% of near-only.
  This rejects muting everything as a false echo-cancellation success.
- native-echo-disconnect-reference positively verifies --disconnect-reference
  using the echo-attenuation predicate. Its matched echo path skips reverse.
- native-echo-zero-output positively verifies --zero-output using both near
  preservation predicates. Only test code overwrites near-only/double-talk
  output; the matched echo control stays unmodified. Both are ordinary Meson
  tests without should_fail. See the review-fix evidence appended below.

The normal effect test and both injected variants are syntax-checked but have
NOT been executed against the library on this Windows host. Therefore their
thresholds, runtime and expected assertion failures still require real host
evidence. Do not interpret Meson registration as proof that they passed.

## Remaining gates for the parent controller

1. Review the unstaged scoped changes; do not stage README, the manual probe,
   unrelated CRLF-only files, caches or SDK material.
2. On an approved Linux host run python scripts/build.py host. It explicitly
   compiles apm_smoke, apm_statistics and apm_echo_regression before Meson runs
   all five registrations with --verbose. Inspect the normal measurements,
   both rejection controls and positive detection markers. Mutation tests
   require success at their specific predicates, not setup/compile or unrelated
   failures. This task did not operate a remote CI service.
3. Verify JNI 14-field behavior, including null/unknown/released handles, in
   the parent's permitted runtime environment. No such runtime was available
   or used in this subtask.
4. Build/package all three Android ABIs after review. Inspect archive hashes,
   symbols, architectures, 16 KB ELF alignment and wrapperCommit. Since this
   work is deliberately uncommitted, current HEAD alone does not identify
   its dirty working tree; do not label old packages as this implementation.
5. Human/device acoustic acceptance remains outside synthetic proxy evidence.
   No claim that the reported application false onset is fixed is justified.

## Review fix round - Important and Minor

This section supersedes the original expected-failure mutation strategy.
The working-tree implementation and its execution gates are still separate:
no real host, JNI runtime, package or device validation was added in this round.

### Important: positive mutation self-validation, not inverted exits

Removed both should_fail: true registrations. All five Meson tests now expect
normal success. The three normal effect predicates are computed once and
reused without relaxing any threshold:

- nearOnlyPreserved and doubleTalkPreserved retain every original gain and
  correlation threshold and the near-only-relative checks.
- matchedEchoCancelled retains the >=15 dB threshold; disconnected/wrong
  controls retain <5 dB. Finite echo measurements are additionally required.
- --disconnect-reference mutates only the matched echo path. Unmodified
  near-only, double-talk, disconnected and wrong controls must pass. Success
  requires matchedEchoCancelled=false, then prints exactly:
  PASS: detected mutation: echo-attenuation
- --zero-output mutates only the near-only and double-talk outputs. Matched,
  disconnected and wrong echo controls must pass. Success requires both near
  preservation predicates=false, then prints exactly:
  PASS: detected mutation: near-preservation
- All creation, configuration, processing and unrelated predicate failures
  still fail the executable. If the intended mutation is not detected, the
  executable fails rather than treating the absence of detection as success.

Added tests/native_echo_mutation_runner.py, copied only for host tests. The
runner executes the real native executable and requires exit 0 AND the exact
requested predicate marker. Nonzero exit, wrong/missing marker, missing
executable and timeout all fail. It forwards native stdout/stderr. Host Meson
execution now uses --verbose, exposing normal measurements and the specific
detected predicate in CI logs even for successful tests.

Each mutation registration also executes two real-library rejection controls
before its positive mutation run:

1. --invalid-configuration passes unsupported 44100 Hz to aozora_apm_create
   from test code. The child must exit 1 at the exact create precondition;
   the control checks that this is not accepted as mutation detection.
2. --disable-mutation keeps the requested mutation verdict but omits the
   injection. The child must exit 1 at that mutation's exact "was not detected"
   predicate, not a setup or unrelated assertion. Unexpected success also
   fails this control. This proves the injected change, not a baseline defect,
   is what makes the intended predicate detect the mutation.

Only these intentional rejection controls accept a specific child failure;
the final mutation run must succeed positively. No production fault hook was
added. Each child has a 180-second timeout; each full mutation registration has
a 400-second Meson timeout. Any timeout fails, never counts as detection.

### Executable focused red and green

Added tests/test_native_echo_mutation_runner.py before implementing the new
runner. The first command failed with missing runner module, exit 1. Then a
temporary baseline runner reproduced the reviewed should_fail policy as
returncode != 0. Before replacing that policy, ran:

    python -m unittest discover -s tests -p test_native_echo_mutation_runner.py -v

Observed exit 1 with behavioral failures including:

    test_actual_child_prerequisite_failure_is_not_detection ... FAIL
    test_nonzero_child_cannot_pass_even_with_detection_marker ... FAIL
    test_both_explicit_positive_detection_markers_are_accepted ... FAIL

This baseline reproduced the inverted-exit bug in the runner; it was not an
execution of old Meson or WebRTC. The temporary policy is not in the final
code. Final tests run actual controlled child processes, not source-text
assertions or mocked subprocess calls. The precondition case now uses a real
missing required file and observes FileNotFoundError. Other cases cover no
detection, wrong predicate, nonzero exit even with the marker, positive
detection, actual timeout and missing executable. Rejection-control tests also
ensure an unrelated error or unexpected success cannot satisfy its expected
failure. These subprocess tests validate the runner protocol, not AEC effects.

Fresh focused command, exit 0:

    python -m unittest discover -s tests -p test_native_echo_mutation_runner.py -v
    Ran 9 tests in 0.631s
    OK

Fresh complete Python command, exit 0:

    python -m unittest discover -s tests -q
    Ran 41 tests in 2.417s
    OK (skipped=1)

That means 40 passed and one pre-existing Linux-only skip. The adapted build
test verifies ordinary mutation registrations; runner behavior is covered by
executable subprocess tests rather than only generated Meson text.

### Minor: optional diagnostics cannot veto an APM

nativeCreate now returns the successfully created APM even if registry insert
throws. That instance's new statistics getter returns null because it is not
registered; processing remains usable. nativeRelease erases under the registry
lock and destroys the caller-provided valid live pointer unconditionally,
including an instance whose optional registration failed. It no longer uses
erase success as an ownership test. The lock still excludes a registered
statistics getter from destruction. Valid/live-handle and caller-serialized
release requirements remain the original contract; this does not add safety
for invalid or duplicate releases or change old audio entry points.

No allocation-failure production hook was introduced. JNI allocation-failure
runtime testing is NOT RUN; this fix received source review and fresh NDK
syntax compilation only. The 14 fields, C API, APM configuration, delay and
production PCM paths are unchanged by this review round.

### Fresh syntax, diagnostics and remaining limits

Actual NDK compiler command was executed once for each target below; every
invocation returned exit 0 with no diagnostics:

    & "D:\包\webrtc-apm-deps-20260902\android-ndk-r27c\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe" --target=<target> -std=c++17 -Wall -Wextra -Werror -fsyntax-only -I native tests/native_echo_regression.cpp native/aozora_apm_jni.cpp

    aarch64-linux-android28
    armv7a-linux-androideabi28
    i686-linux-android28

Generated host Meson text was parsed with the installed Meson parser:

    python -c "import sys; sys.path.insert(0, 'scripts'); from build_config import wrapper_meson; from mesonbuild.mparser import Parser; Parser(wrapper_meson('host')[0], 'generated-host-meson.build').parse(); print('PASS: generated host Meson syntax')"

Observed exit 0: PASS: generated host Meson syntax. This parses syntax, not a
full host configure/link/run. Scoped git diff --check returned exit 0 with
only existing CRLF advisories. ReadLints over all changed code: zero diagnostics.

The real-library invalid-configuration/disabled-injection controls and positive
mutations are prepared but NOT RUN locally. Their normal effect thresholds,
positive predicate detections, runtime, JNI behavior and final artifacts still
require the parent's approved host/runtime validation. No git staging, commit,
push, GitHub/CI operation, application edit, manual-probe edit, upstream/cache
modification, audio I/O or device use occurred in this review round.

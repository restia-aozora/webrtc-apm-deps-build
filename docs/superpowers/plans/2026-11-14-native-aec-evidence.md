# Native AEC evidence and regression plan

User-approved scope: repair the proven reverse-output defect, expose compatible
native evidence, validate the real library and build Android on a feature branch.
This plan is also the execution ledger; do not repeat completed tasks.

## Constraints and baseline

- Preserve all existing work. Initial meaningful changes: README's synthetic
  Android probe section and untracked tests/native_echo_probe.cpp. Many other
  status entries are CRLF-only. Do not stage those unrelated changes.
- Do not record, play, download or upload audio. Tests generate PCM in memory.
- Do not alter VAD, suppression thresholds, APM configuration, stream delay
  formula, guard budget, or upstream source revision.
- No claim that synthetic/host success proves phone acoustic correctness.
- Branch: fix/native-aec-evidence-20261114, base bf372ed.
- Python baseline: 29 tests, 28 passed, 1 Linux-only test skipped, exit 0.
- Local Windows has no usable Linux host runtime. GitHub host execution supplies
  the real-library red/green evidence. Android builds retain all three app ABIs.

## Design and files

1. native/aozora_apm.cpp: preserve reverse PCM when the upstream integer API
   leaves its destination untouched. Seed its destination from the input; keep
   upstream processing and capture semantics unchanged.
2. native/aozora_apm.h and .cpp: add aozora_apm_get_build_revision() and
   aozora_apm_get_statistics(handle, destination, destination_size). Keep all old
   APIs. Statistics v1 uses fixed-width fields, struct size/version, valid mask,
   accepted capture/reverse counts, sample rate, NS/AGC configuration flags,
   submitted delay and voice; optional ERL, ERLE, internal delay and residual
   likelihood retain unavailable state. Cache once after successful capture
   under the existing processing mutex; getter must not consume upstream stats.
3. native/aozora_apm_jni.cpp: add nativeGetBuildRevision(): String and
   nativeGetStatistics(handle): DoubleArray. Wire schema v1 is [version,
   validMask, captureFrames, reverseFrames, streamDelayMs, voiceDetected,
   sampleRateHz, estimatedDelayMs, erlDb, erleDb, residualLikelihood,
   residualLikelihoodRecentMax, noiseSuppressionEnabled, gainControlEnabled].
   Unknown doubles use NaN, unknown integer metrics use -1 plus clear valid bit.
4. scripts/build_config.py, build.py, tests/native_smoke.cpp and a new focused
   native effect test: register and compile actual linked tests on Linux.
   Validate nonzero reverse preservation; optional statistics, counters and
   invalid calls; matched/no/wrong reference controls; independent near-only
   and double-talk preservation with alignment-aware projection. Effect cases
   run with NS and AGC disabled. Include deliberate disconnection and all-zero
   output mutations in test-only code to prove assertions can fail.
5. Existing workflow remains manual for native builds; checks stay lightweight.
   Build on the approved feature ref, inspect host and Android jobs independently,
   and verify archive SHA-256, exports, architectures and 16 KB ELF alignment.
6. Application Kotlin wrapper and coordinator: read the additive JNI interface
   only at existing periodic/onset diagnostic snapshots. Missing symbols disable
   only diagnostics, not APM. Validate array version/size/finite optional values;
   publish a frozen diagnostic string without changing any audio/VAD decision.
7. Preserve existing probe/README edits, document new behavior separately. Do not
   include caches, SDKs, credentials, logs, or binary artifacts in Git commits.

## Execution checkpoints

- [x] Inspect native/upstream/application and obtain scope approval.
- [x] Verify baseline and create feature branch without resetting existing work.
- [x] Add real-library red regression and run it in GitHub host.
- [x] Implement minimal reverse repair and additive statistics/JNI contract.
- [ ] Run native green, controls and configuration tests; independent review.
- [x] Connect low-frequency application diagnostics with compatibility tests.
- [ ] Final cross-boundary review and fresh regression run.
- [ ] Push reviewed commits and dispatch Android ARM64/ARMv7/x86 packaging.
- [ ] Verify final run and artifacts; report remaining device validation.

## Evidence and limitations

Source inspection proves the reverse destination can remain unwritten; current
application ignores its returned PCM, so fixing it is not evidence of fixing the
reported false onset. AEC3 is enabled and Initialize preserves configuration.
voice_detected is speech VAD, not a near-end identity or double-talk decision.
The application's current ARM64 binary differs from the local cached package;
hash difference alone does not identify why or prove a behavior difference.
Internal delay uses 4 ms blocks and may be 0 before useful estimation. ERLE and
residual likelihood are diagnostics, never authorization to mute user speech.

### Confirmed real-library red

Commit 55aa915, GitHub run 43577044665, host-smoke job 132393440492:
the real pinned WebRTC library built successfully, then native-api-smoke failed
with `FAIL: analysis-only reverse must preserve nonzero reference PCM`.
This is an expected test failure before the fix, not a compiler or SDK error.
The concurrent ARM64 packaging job succeeded; its old-library artifact is NOT
the final deliverable and must not be installed as the repaired version.

### Native implementation working-tree checkpoint

Reverse destination seeding, additive C statistics v1 and the approved JNI
14-field array are implemented without application/upstream/config changes.
The statistics contract test first failed actual NDK compilation because the
new API type was absent, then syntax compilation passed after implementation.
All three Android ABIs passed public-contract/JNI/test syntax checks and
wrapper syntax checks against actual upstream headers. Python: 32 tests,
31 passed and one pre-existing Linux-only skip; exit 0. ReadLints: zero issues.

Three linked host test targets and five Meson registrations now cover the
existing reverse smoke, frozen statistics and synthetic echo/independent-near
controls, plus disconnected-reference/all-zero mutations (positive
self-validation after the review fix below).
Real native execution and mutation results are NOT RUN locally, so the native
green checkpoint above remains open. No staging, commit, push, CI operation,
packaging or device use was performed. See 2026-11-14-native-api-report.md for
commands, observed red evidence, exact scope and remaining verification gates.

### Native review fix checkpoint

Important: removed inverted should_fail mutation tests. Shared unchanged
effect predicates now distinguish normal success from explicit detection of
echo attenuation failure (disconnected reference) or both near-preservation
failures (zero output); unrelated controls must still pass. A host-only Python
runner requires exit 0 and the exact named detection marker, and verifies
real-library invalid-create and disabled-injection rejection controls before
each positive mutation. Native measurements and detection markers are visible
via Meson --verbose. No production test hook was added.

Minor: optional JNI registry insertion failure no longer rejects/destroys a
valid newly created APM; release destroys a caller-provided valid live handle
regardless of registry insertion success, retaining synchronized stats reads.
JNI schema, production processing/configuration/delay and old lifetime rules
are unchanged.

Executable runner regressions first exposed the reproduced inverted-exit
policy, then passed: 9 focused tests. Fresh Python full suite: 41 tests,
40 passed, one pre-existing Linux-only skip, exit 0. ARM64/ARMv7/x86 NDK syntax
checks of changed C++/JNI passed; generated Meson parsed; ReadLints zero.
Real host controls/mutations, allocation-failure JNI runtime behavior and
packaging remain NOT RUN. No Git/CI write operation or device use occurred.
See the appended review-fix section of 2026-11-14-native-api-report.md.

### Application task validation

Application report: docs/superpowers/plans/2026-11-14-native-apm-diagnostics-report.md
in the consuming workspace. Its three focused cases passed after the documented
red; actual emitted UTS helper compiled with the real Kotlin wrapper. Directed
read-only review found specification and quality compliant, no findings.
Full application regression: 25 scripts, 228 tests passed, 0 failed/skipped/TODO,
exit 0, duration about 115 seconds. This includes existing native guard, timing,
resampling, VAD, trace and player tests plus the new optional diagnostics tests.
No real Android JNI load, full APK build or device acoustics was exercised.
User additionally requires no subagent to use a questioning tool; all subsequent
delegations explicitly carry that constraint.

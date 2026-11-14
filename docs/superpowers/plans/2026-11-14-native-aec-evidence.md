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
- [ ] Add real-library red regression and run it in GitHub host.
- [ ] Implement minimal reverse repair and additive statistics/JNI contract.
- [ ] Run native green, controls and configuration tests; independent review.
- [ ] Connect low-frequency application diagnostics with compatibility tests.
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

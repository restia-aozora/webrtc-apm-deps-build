# WebRTC APM native builds

## Approved scope

Create a separate build-only GitHub repository for Android, iOS and HarmonyOS.
Do not change the consuming application, its remotes, or the reference source
directory. Do not publish downloaded SDKs, upstream source snapshots, caches,
business code, credentials or previously compiled binaries.

## Architecture

- Pin helloooideeeeea/webrtc-audio-processing to
  f8efa8486ac0dd463fb84e4e99be2e072c28adb8, matching the reference checkout.
- Preserve the local absl_crc dependency fix, using an exact-match patch.
- Build upstream and the wrapper in the same Meson project so platform and
  architecture definitions, Abseil and transitive static libraries stay aligned.
- Expose a mono, 10 ms, signed PCM16 C API with AEC3, optional NS/AGC2, stream
  delay and voice detection, matching the reference Android processing settings.
- Keep the six existing Android JNI method names in libaozora_apm.so.
- Supply AozoraApm.xcframework for iPhoneOS ARM64 with a Swift-importable C module.
- Supply HarmonyOS ARM64 libaozora_apm.so with an N-API bridge and .d.ts types.
- Default to ARM64, with optional existing Android ARMv7/x86 ABI builds.
- Native workflows are workflow_dispatch only: all/android/ios/harmony.
  Independent jobs upload separate artifacts and do not cancel each other.

## Implementation checkpoints

- [x] Lock source/dependencies and implement exact-match patch regression tests.
- [x] Implement the shared C API, compatible JNI and Harmony N-API wrappers.
- [x] Generate cross files using actual runner SDK paths; prohibit host dependency
  lookup and use the pinned Abseil wrap fallback.
- [x] Compile static upstream dependencies into the Android ARM64 wrapper; verify ELF
  architecture, dependencies, JNI/C exports and Android 16 KB LOAD alignment.
- [x] Implement iOS device framework packaging, architecture, minimum OS and
  dependency checks, and Swift import type-checking. Actual macOS execution pending.
- [x] Add manual jobs, Linux host smoke tests, manifest hashes, license notices,
  deployment instructions and explicit limits on runtime validation.
- [x] Run local tests and available Android cross-compilation checks; report
  iOS/Harmony builds as unverified until their GitHub jobs actually succeed.
- [x] Review publishable files and create the separate public repository/remote.
  Publish via a normal initial commit and push, then verify the remote HEAD.
  Do not dispatch a native build without the user's request.

## Local validation evidence (2026-09-28)

- Python regression suite: 21 tests, 20 passed and one Linux-specific symlink
  permission test skipped on Windows. Python syntax checks passed.
- Actual Android NDK r27c ARM64 build completed all 395 compile/link steps and
  packaging. Stripped libaozora_apm.so: 720880 bytes; ZIP including notices:
  approximately 0.45 MiB. These sizes are local results, not CI guarantees.
- Export, dynamic dependency and 16 KB ELF LOAD alignment checks passed.
- Harmony N-API bridge syntax was checked against headers from the pinned SDK
  using the Android NDK compiler. This is not an OHOS cross-build or link test.
- iOS, full Harmony, Linux host runtime, real-device acoustic performance and
  UTS integration remain unverified until the corresponding tests run.
- The new repository's SDK checksum variable is configured. Its SDK URL secret
  still needs to be supplied; another repository's secret cannot be read back.

## Important boundaries

The library does not capture microphones or play audio. Consumers must feed
playback-reference and capture frames, measure path delay and serialize destroy
against processing. This repository does not claim real-device AEC convergence,
UTS compatibility, or that an application satisfies an upload quota.
The Harmony SDK URL must be configured separately for this repository: GitHub
repository secrets cannot be read back from the existing build repository.

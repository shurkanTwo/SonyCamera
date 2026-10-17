# Changes

## 1.0.1.23-device-removal-test.1 — experimental candidate

Preserve @totoantibes's original PR #16 commit on an isolated review branch, then correct nested-open removal state, notification COM lifetime and capture-failure completion. Fix pre-existing property-copy crashes for missing defaults and uninitialized owned pointers. Add real-production-object fault injection, per-device notification cookies, safe missing-event handling and callback work queued without blocking the camera worker. Reopen recovery requires a successful fresh session; this is not transparent hot reconnection.

Windows automated checks and the complete source rebuild must pass before hardware testing. Physical camera/USB teardown and ASCOM/NINA compatibility remain unverified. The upstream author reports remaining crashes; no claim is made that issue #15 is resolved. See HARDWARE-TESTING.md for acceptance and rollback. This candidate is not merged into workerThread or the published release.

## 1.0.1.22-preview.1 — 2026-10-17

Published as a regular GitHub release marked Latest. The original preview.1 tag, binary/source assets and checksums are retained; this changes release visibility and leaves the verified runtime build intact. Experimental device-removal PR #16 remains separate.

Community native-driver release maintained by @shurkanTwo, based on @dougforpres's workerThread commit `21f469f6d2e9516ec4896eaa0ec9c45c4e96d92d` (native version 1.0.1.21).

- Merge [@totoantibes's upstream PR #17](https://github.com/dougforpres/SonyCamera/pull/17), original commit `ac9bf1feb9ed6d24f587176905d7c7818ae64787`. Replace the mistaken `2 ^ 23` XOR value (21 bytes) with a named 8 MiB allocation floor, capped by reported transfer size. Larger optimal transfer sizes and Maximum Memory Mode retain their previous behavior. This is not a fixed 8 MiB upper bound.
- Publish x86/x64 native DLL and diagnostic-tool builds. Build LibRaw 0.21.0 and libjpeg-turbo 2.0.3 from pinned source matching the vendored headers; these are not claimed to reproduce the upstream installer binaries exactly.
- Add Windows DLL/export/dependency checks, seven calculation regression cases with a failing upstream baseline, matching source archives, checksums, and manual installation/rollback instructions.
- Bump DLL/tool resource versions to 1.0.1.22; preserve original copyright notices and commit authorship.

The runtime source change is limited to PR #17 plus version resources. Build configuration, dependencies, tests, packaging and documentation also change. Upstream PR #16 is not included, and disconnection crashes are not claimed fixed. Physical camera capture, WPD/USB transfers, hot unplug/reconnect and ASCOM/NINA integration remain unverified. There is no ASCOM adapter build or installer in this release.

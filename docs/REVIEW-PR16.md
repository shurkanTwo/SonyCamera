# Review of upstream PR #16 and isolated test candidate

Original review: `21f469f6d2e9516ec4896eaa0ec9c45c4e96d92d...5a26bd79e44615d5f1b21a529496e5ca02f8e556`, [@totoantibes's upstream PR #16](https://github.com/dougforpres/SonyCamera/pull/16). The original commit is preserved on maintenance/device-removal-review. This branch is separate from workerThread and the regular native 1.0.1.22 release.

## Standards

No documented-style violations or actionable baseline smells were found. The correctness review identified unsafe callback deletion on the new removal-time path, clearing device loss during a reference-counted Open, and failure completion leaving capture status Reading. Follow-up review found concurrent callback ownership transfer, incomplete multi-image success and no-data command response handling needed correction too.

The candidate addresses these with serialized notification ownership, reference-counted callback lifetime and an owned device reference through Stop, genuine-session-only removal reset, explicit capture/task failure on any interrupted image, and the direct WPD no-data response contract. Notification callbacks enqueue work without waiting for the worker mutex; existing capture/idle polling remains the fallback when a busy-worker notification cannot be queued.

## Spec

The initial review found two missing requirements: nested opens could defeat “once gone, stop reissuing,” and a missing-image loop exit did not provide terminal capture failure. Follow-up review found the same capture gap after a successful first image and a concurrent teardown gap. These paths are corrected in the candidate and covered by production-object regressions.

The author explicitly reports remaining abnormal exits in the original PR. This review and candidate do not claim to resolve upstream issue #15 or provide transparent hot reconnect. Reconnection requires a successful fresh session. The ASCOM wrapper remains outside this fork's changes.

## Runtime diagnosis and regression coverage

Tests link the same production object files as SonyMTPCamera.dll, with only the physical IPortableDevice endpoint substituted. They use real Windows WPD result collections and phase-accurate responses. Coverage includes 48 terminal error scenarios, ordinary nonterminal errors, nested/failed/successful reopen, failed Close, callbacks after failed Unadvise and camera destruction, two device cookies, 64 concurrent shutdown cycles, busy-worker callback liveness and complete first-image/partial-image failure tasks.

The expanded test also reproduced two pre-existing property-copy bugs independently of USB: [missing default value dereference](https://github.com/shurkanTwo/SonyCamera/actions/runs/39062779788) and [uninitialized ownership when copying into reused storage](https://github.com/shurkanTwo/SonyCamera/actions/runs/39062951394). Both caused access violations on x86 and x64. The candidate preserves absent defaults and initializes owned pointers. These are source-level defects found through the tests, not proof of the cause of the original physical-removal crashes.

Final results belong to the latest successful [draft PR #3 checks](https://github.com/shurkanTwo/SonyCamera/pull/3/checks) and its BUILD.json / DEVICE-REMOVAL-TESTS.json source revision. Packaging and the source-ZIP rebuild must pass before using a candidate artifact. See [HARDWARE-TESTING.md](../HARDWARE-TESTING.md) for acceptance evidence still required before merging.

Standards review and Spec review remain separate; their corrected findings are recorded above. Physical-camera, Windows USB teardown and ASCOM/NINA validation remain outstanding.

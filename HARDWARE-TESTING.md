# Device-removal test candidate

This branch prepares **1.0.1.23-device-removal-test.1**, native file version **1.0.1.23**. It preserves @totoantibes's original [PR #16](https://github.com/dougforpres/SonyCamera/pull/16) commit and adds reviewed follow-up fixes. It remains separate from workerThread and the published 1.0.1.22-preview.1 release. Do not treat it as a verified fix for upstream issue #15.

## What the automated tests cover

The test executable links the same production object files as the native DLL. It substitutes only a fake IPortableDevice endpoint, retaining real Windows WPD parameter/result collections and real MTPDevice, DeviceEventHandler, Camera and capture-task logic. It injects six terminal HRESULTs through both direct transport errors and WPD result errors at execute/read/write/end phases (48 scenarios). Other checks cover nested opens, failed/successful genuine reopen, nonterminal errors, failed close, notification ownership/cookies, late callbacks after failed Unadvise, missing event fields, five failed refreshes, partial JPEG/RAW failures, 64 concurrent shutdown cycles, callback liveness while the worker is busy, and the full failed-image download task. Property copy checks cover absent defaults and ownership when object storage is reused.

These checks do not reproduce Windows USB driver teardown, physical camera timing, image fidelity or ASCOM/NINA integration. Passing them is a prerequisite for hardware testing, not proof that removing a camera is safe. The upstream author reports remaining unexplained abnormal exits even with the original PR #16.

The callback queue uses a zero timeout while the worker is busy. A notification can be dropped in that case; existing capture/idle polling provides the fallback. The tests check this liveness behavior, while hardware acceptance must check normal capture timing and completeness.

See [the review record](docs/REVIEW-PR16.md) for findings, corrections and limits.

## Obtain and identify the candidate

Use the native-release-packages artifact from the linked draft PR's latest successful Actions run. Its ZIP names contain 1.0.1.23-device-removal-test.1; the DLL/tool version must be 1.0.1.23. Verify SHA256SUMS.txt, architecture and BUILD.json sourceCommit before testing. Pull-request workflows compile GitHub's temporary merge commit; match that sourceCommit with SOURCE_REVISION in the source ZIP and the run's checkout log, rather than expecting the PR branch's head SHA. Do not substitute packages from the older published release. Each package includes complete source and notices through the companion source ZIP. SOURCE.txt links to the Actions run that delivered all three ZIPs together.

Follow [INSTALLATION.md](INSTALLATION.md) for loaded module paths, backups, installation and rollback. Start in a dedicated test session with saved work and no important sequence running. Turn on relevant client/native logging before reproducing a disconnection. Keep original files and the existing installer available.

## Hardware acceptance record

Record camera model/firmware, Windows version, client/version, x86/x64 process architecture, loaded paths and all three DLL versions, candidate sourceCommit, test times, logs and observed behavior for each case:

1. Normal operation: repeated short and long captures, RAW/JPEG settings used by the camera, previews where supported, image dimensions/content and successful save/download. Compare with the working installation.
2. Idle removal: unplug or power off an idle connected camera. The host must survive, report failure/disconnection, and stop repeated WPD traffic. Observe it beyond the delayed crash interval reported upstream (at least 30 seconds).
3. Capture/download removal: in a disposable test capture, repeat removal during exposure and image retrieval. Capture polling must end with failure/cancellation rather than remain Reading indefinitely. Record any application crash/hang and its faulting module/dump.
4. Reconnection: reconnect/power on, perform the client's disconnect/connect flow to establish a new session, and complete repeated captures. A reference-counted Open of the dead session is deliberately rejected; this candidate does not promise transparent hot reconnection.
5. Cleanup: close the client after failed captures and failed reconnection attempts, then reopen and test another session. Repeat removal/reconnect cycles to look for retained callbacks, worker hangs and growing resources.
6. Client coverage: test the actual NINA plugin path and an existing ASCOM client where available, using matching architecture. Test two devices concurrently only if that is a supported configuration you can exercise.

Keep this candidate out of workerThread until the acceptance record and any crash evidence have been reviewed. Hardware results cannot be supplied by this Linux build environment. Report results in the fork's draft PR or issues, with personal information removed from logs.

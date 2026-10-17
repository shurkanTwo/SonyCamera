# Building the native test release

Use Windows with Visual Studio 2022 C++ build tools (Desktop development with C++, Windows SDK and ATL), PowerShell 7, Git and CMake available on PATH. The script locates Visual Studio with vswhere and imports the chosen MSVC environment. GitHub CI uses windows-2022. Toolchain updates can change the binary output; this is a pinned-source build procedure, not a claim of byte-for-byte reproducibility.

From the repository root, run:

```powershell
pwsh ./build/Build-Native.ps1 -Architecture x64
pwsh ./build/Build-Native.ps1 -Architecture x86
```

Outputs appear in `.build/output/x64` and `.build/output/x86`. The script builds both SonyMTPCamera and SonyCameraInfo, checks real DLL loading/28 exports/local dependency resolution and compares seven extracted buffer-calculation cases against the original failing baseline. These tests do not use a physical camera.

The build downloads NASM 2.16.03 from nasm.us and verifies SHA256 `3ee4782247bcb874378d02f7eab4e294a84d3d15f3f6ee2de2f47a46aa7226e6`. It builds LibRaw 0.21.0 at `979160ff13c89e1158b1995975c23ed7497b6b62` and libjpeg-turbo 2.0.3 at `5db6a6819d0f904e0b58f34ae928fea234adb1a0` with shared MSVC runtimes and JPEG SIMD enabled. Dependency versions match the vendored headers. They are deliberately not upgraded in the transfer-buffer patch.

## Build from the release source archive

Extract `SonyCamera-native-1.0.1.23-device-removal-test.1-source.zip` and run the same commands from its SonyCamera-native directory. It includes complete matching dependency source under third-party, SOURCE_REVISION markers, and a baseline source copy for the regression comparison. A Git checkout is not required for that build. NASM still needs network access on its first run; install the prerequisites above beforehand.

The historical prebuilt .dll/.lib files tracked upstream are omitted from this source archive. The maintained build compiles fresh dependencies and explicitly selects their import libraries instead.

## Package verified output

CI first completes both Windows builds. A packaging job downloads their artifacts and runs:

```bash
python build/Package-Native.py --artifacts .build/artifacts --output .build/packages
```

Python 3's standard library is sufficient. The script validates architecture, smoke/regression evidence and source revision, downloads pinned dependency source, creates two binary ZIPs and a complete matching source ZIP, then writes SHA256SUMS.txt. Packaging requires a clean Git checkout with full history. This candidate sets artifactOnly in build/Release.json: SOURCE.txt links to the current Actions run, or the review PR when packaging locally. All three ZIPs and the checksum file are delivered together in native-release-packages. A later published release must use packages built from its tagged commit and update the distribution metadata accordingly.

The experimental branch also links DeviceRemovalTests.exe from the same production objects as the native DLL. A fake COM endpoint injects 48 terminal-failure scenarios and exercises real callback, reopen and capture-task behavior. DEVICE-REMOVAL-TESTS.json records the result; no physical camera is used. Candidate/package versions are defined in build/Release.json. See HARDWARE-TESTING.md before using the resulting packages.

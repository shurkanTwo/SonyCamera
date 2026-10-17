# Manual installation and rollback

These are experimental native-driver candidate packages, not an ASCOM installer. The regular/Latest native release is 1.0.1.22; this separate candidate is 1.0.1.23-device-removal-test.1 and requires hardware validation. Keep your existing working ASCOM/NINA installation and its settings. Camera captures, USB disconnections and ASCOM/NINA integration have not yet been tested on hardware with this candidate.

## Choose and verify a download

1. Download and extract the native-release-packages artifact from the candidate draft PR's successful Actions run; see [HARDWARE-TESTING.md](HARDWARE-TESTING.md). GitHub requires signing in to download Actions artifacts. This candidate is separate from the published regular release.
2. Inside the artifact, choose `SonyCamera-native-1.0.1.23-device-removal-test.1-win-x64.zip` for a 64-bit client, or `...-win-x86.zip` for a 32-bit client. Match the application process, even on 64-bit Windows. Never mix DLLs from the two packages.
3. Use SHA256SUMS.txt from the same artifact. In PowerShell, run `Get-FileHash .\SonyCamera-native-1.0.1.23-device-removal-test.1-win-x64.zip -Algorithm SHA256` (substitute x86 as needed) and compare the complete value with SHA256SUMS.txt. Unblock the downloaded ZIP through Windows Properties, then extract it to a new folder.

Each binary ZIP contains SonyMTPCamera.dll, libraw.dll, turbojpeg.dll, the diagnostic tool, build/check evidence and license notices. The source ZIP is for rebuilding; it is not the binary installation package. The binaries are unsigned. If Windows reports missing Visual C++ runtime DLLs, install Microsoft's supported [Visual C++ v14 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist) matching the process architecture.

## Test the extracted package first

Close other camera clients. In a PowerShell terminal opened in the extracted folder, run the matching tool:

```powershell
# 64-bit package
.\SonyCameraInfo64.exe /h
.\SonyCameraInfo64.exe /s
# 32-bit package: use .\SonyCameraInfo.exe instead
```

The tool prints the paths of the loaded SonyMTPCamera.dll, libraw.dll and turbojpeg.dll. Confirm all three paths point into the extracted folder and that the native DLL is version 1.0.1.23. `/h` displays help; `/s` scans for cameras and can contact connected devices. Loading the native DLL can initialize supported camera/lens defaults in the existing registry, so this is a file-isolated test, not a registry-isolated test. A successful scan alone does not verify image capture or ASCOM/NINA integration.

## Use it with an existing client

Only proceed when you can identify the exact native DLL directory the client actually loads. Use the client's native-load log or inspect the loaded SonyMTPCamera.dll module path while it is running (for example, with Microsoft's [Process Explorer](https://learn.microsoft.com/en-us/sysinternals/downloads/process-explorer)). The diagnostic tool's own folder does not establish the client's path.

1. Record the client's architecture and the loaded paths/versions of SonyMTPCamera.dll, libraw.dll and turbojpeg.dll. Close NINA, ASCOM applications and all other clients using them.
2. Back up all three existing files to a dated folder outside the installation directory. Record the destination paths. Keep the old installer as an additional recovery option.
3. Replace those three files in the confirmed native DLL directory with all three from the matching architecture package. Administrator access may be required. Do not replace the ASCOM wrapper. Do not assume the NINA plugin folder is the correct destination. Shared native directories affect every client using those files.
4. Reopen the client and confirm its loaded native DLL path/version and dependency paths. Test connection, a short capture, a longer capture and image retrieval before normal use. Record any errors; use a dedicated disposable test session for the controlled removal cases in HARDWARE-TESTING.md.

On 64-bit Windows, System32 normally holds 64-bit system files and SysWOW64 holds 32-bit files. Installations can use other locations; use the observed module paths rather than guessing from directory names. If the three loaded DLLs live in different directories or another dependency copy takes precedence, stop and resolve that layout before replacing files.

## Roll back

Close every camera client again. Restore the three backed-up files to their recorded original destinations, replacing the test versions. Reopen the client and confirm the previous versions load. If files or registration are missing, repair/reinstall the same original driver installer you used before testing. Restoring native files does not undo registry defaults initialized while loading the DLL.

## Report results

Use [this fork's issues](https://github.com/shurkanTwo/SonyCamera/issues). Include camera model/firmware, Windows and client versions, process architecture, native DLL path/version, dependency paths, the Actions run URL and BUILD.json sourceCommit, test steps and relevant logs. Remove personal information from logs. This candidate includes upstream PR #16 and follow-up fixes, but physical removal safety is unverified. Keep it separate from your working installation until the hardware acceptance checks pass.

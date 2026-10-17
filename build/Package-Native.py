"""Package verified Windows builds and their complete matching source."""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import struct
import subprocess
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parent.parent
RELEASE = json.loads((ROOT / "build/Release.json").read_text(encoding="utf-8"))
VERSION = RELEASE["packageVersion"]
BASELINE = "21f469f6d2e9516ec4896eaa0ec9c45c4e96d92d"
DEPENDENCIES = {
    "LibRaw": ("LibRaw/LibRaw", "979160ff13c89e1158b1995975c23ed7497b6b62"),
    "libjpeg-turbo": ("libjpeg-turbo/libjpeg-turbo", "5db6a6819d0f904e0b58f34ae928fea234adb1a0"),
}


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args])


def dependency_source(repository, revision):
    url = f"https://codeload.github.com/{repository}/zip/{revision}"
    with urllib.request.urlopen(url, timeout=120) as response:
        archive = zipfile.ZipFile(io.BytesIO(response.read()))
    files = {}
    for entry in archive.infolist():
        if entry.is_dir():
            continue
        path = PurePosixPath(entry.filename)
        relative = PurePosixPath(*path.parts[1:])
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"Unsafe source archive entry: {entry.filename}")
        files[str(relative)] = archive.read(entry)
    files["SOURCE_REVISION"] = (revision + "\n").encode()
    return files


def architecture(path):
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"Not a Windows executable: {path}")
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    if data[offset:offset + 4] != b"PE\0\0":
        raise ValueError(f"Not a PE executable: {path}")
    return struct.unpack_from("<H", data, offset + 4)[0]


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def validate_build(directory, arch, revision):
    build = read_json(directory / "BUILD.json")
    smoke = read_json(directory / "SMOKE.json")
    regression = read_json(directory / "TRANSFER-TESTS.json")
    removal = read_json(directory / "DEVICE-REMOVAL-TESTS.json")
    if build["architecture"] != arch or build["sourceCommit"] != revision:
        raise ValueError(f"Build revision/architecture mismatch: {arch}")
    if build["nativeVersion"] != RELEASE["nativeVersion"] or build["diagnosticToolHelpVerified"] is not True:
        raise ValueError(f"Native version/diagnostic tool check missing: {arch}")
    if (build["libraw"]["commit"] != DEPENDENCIES["LibRaw"][1]
            or build["libjpegTurbo"]["commit"] != DEPENDENCIES["libjpeg-turbo"][1]
            or build["libjpegTurbo"]["simd"] is not True):
        raise ValueError(f"Unexpected dependency build: {arch}")
    bits = 64 if arch == "x64" else 32
    if (smoke["pointerBits"] != bits or smoke["exportsVerified"] != 28
            or smoke["localDependenciesVerified"] != 2 or smoke["lensDefinitions"] < 1):
        raise ValueError(f"Incomplete DLL smoke check: {arch}")
    if (regression["checks"] != 7 or regression["baselineRejected"] is not True
            or regression["currentPassed"] is not True):
        raise ValueError(f"Incomplete regression check: {arch}")
    if (removal["checks"] < 100 or removal["terminalFaultScenarios"] != 48
            or removal["productionObjectsLinked"] is not True or removal["physicalCamera"] is not False):
        raise ValueError(f"Incomplete device-removal regression check: {arch}")
    tool = "SonyCameraInfo64.exe" if arch == "x64" else "SonyCameraInfo.exe"
    names = ["SonyMTPCamera.dll", "libraw.dll", "turbojpeg.dll", tool]
    for name in names:
        if architecture(directory / name) != (0x8664 if arch == "x64" else 0x14C):
            raise ValueError(f"Incorrect PE architecture: {arch}/{name}")
    return names


def licenses(name, files):
    selected = {}
    for path, data in files.items():
        filename = PurePosixPath(path).name.upper()
        if filename.startswith(("LICENSE", "COPYING", "COPYRIGHT", "README")):
            selected[f"THIRD-PARTY-LICENSES/{name}/{path}"] = data
    if name == "LibRaw":
        # These files contain component BSD notices beyond the root COPYRIGHT.
        for path in ["internal/x3f_tools.h", "src/x3f/x3f_parse_process.cpp",
                     "src/x3f/x3f_utils_patched.cpp", "src/demosaic/dcb_demosaic.cpp"]:
            selected[f"THIRD-PARTY-LICENSES/{name}/notices-source/{path}"] = files[path]
    return selected


def write_zip(path, files):
    if len({name.casefold() for name in files}) != len(files):
        raise ValueError(f"Case-insensitive filename collision in {path}")
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(files.items()):
            # Stable ZIP metadata; compiler output itself need not be deterministic.
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 17, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data)
    with zipfile.ZipFile(path) as archive:
        if archive.testzip() is not None:
            raise ValueError(f"ZIP verification failed: {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if git("status", "--porcelain").strip():
        raise ValueError("Package from a clean checkout so source and build scripts match.")
    revision = git("rev-parse", "HEAD").decode().strip()
    validated = {}
    for arch in ("x86", "x64"):
        directory = args.artifacts / f"native-{arch}"
        validated[arch] = (directory, validate_build(directory, arch, revision))

    deps = {name: dependency_source(*pin) for name, pin in DEPENDENCIES.items()}
    source_name = f"SonyCamera-native-{VERSION}-source.zip"
    if RELEASE.get("artifactOnly", False):
        run_id = os.environ.get("GITHUB_RUN_ID")
        source_url = (f"https://github.com/shurkanTwo/SonyCamera/actions/runs/{run_id}"
                      if run_id else RELEASE["reviewUrl"])
        source_location = (
            "Download native-release-packages from the linked Actions run or PR checks.\n"
            "Extract the artifact;\n"
            "it contains this binary ZIP and the complete matching source ZIP.\n"
        )
    else:
        source_url = f"https://github.com/shurkanTwo/SonyCamera/releases/download/{VERSION}/{source_name}"
        source_location = "Download the source ZIP alongside this binary ZIP from the same release.\n"
    source_notice = (
        f"SonyCamera native {VERSION}\nSource commit: {revision}\n"
        f"Complete corresponding source: {source_name}\n{source_url}\n"
        f"{source_location}"
        "It includes the native project, complete LibRaw/libjpeg-turbo source, licenses,\n"
        "build scripts, pinned dependency revisions and regression baseline.\n"
        "See build/README.md in the source ZIP for build prerequisites/instructions.\n"
    ).encode()
    common = {name: (ROOT / name).read_bytes() for name in
              ("LICENSE", "NOTICE.md", "INSTALLATION.md", "RELEASE-NOTES.md", "HARDWARE-TESTING.md")}
    common["SOURCE.txt"] = source_notice
    for name, files in deps.items():
        common.update(licenses(name, files))

    args.output.mkdir(parents=True, exist_ok=True)
    outputs = []
    for arch, (directory, names) in validated.items():
        files = dict(common)
        for name in names + ["BUILD.json", "SMOKE.json", "TRANSFER-TESTS.json", "DEVICE-REMOVAL-TESTS.json"]:
            files[name] = (directory / name).read_bytes()
        path = args.output / f"SonyCamera-native-{VERSION}-win-{arch}.zip"
        write_zip(path, files)
        outputs.append(path)

    files = {}
    with zipfile.ZipFile(io.BytesIO(git("archive", "--format=zip", "HEAD"))) as native:
        for name in native.namelist():
            if not name.endswith("/") and PurePosixPath(name).suffix.lower() not in {
                    ".dll", ".exe", ".lib", ".obj", ".pdb"}:
                files[f"SonyCamera-native/{name}"] = native.read(name)
    files["SonyCamera-native/SOURCE_REVISION"] = (revision + "\n").encode()
    files["SonyCamera-native/SOURCE.txt"] = source_notice
    files["SonyCamera-native/tests/baseline-MTPDevice.cpp"] = git(
        "show", f"{BASELINE}:SonyMTPCamera/MTPDevice.cpp")
    for name, dep_files in deps.items():
        for path, data in dep_files.items():
            files[f"SonyCamera-native/third-party/{name}/{path}"] = data
    path = args.output / source_name
    write_zip(path, files)
    outputs.append(path)

    sums = "".join(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n"
                   for path in sorted(outputs))
    (args.output / "SHA256SUMS.txt").write_text(sums, encoding="utf-8")
    print(sums, end="")


if __name__ == "__main__":
    main()

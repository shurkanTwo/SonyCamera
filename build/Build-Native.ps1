param(
    [ValidateSet('x86', 'x64')][string]$Architecture = 'x64'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$depRoot = Join-Path $root ".build/dependencies/$Architecture"
$output = Join-Path $root ".build/output/$Architecture"
$librawCommit = '979160ff13c89e1158b1995975c23ed7497b6b62' # LibRaw 0.21.0, matching the vendored headers
$jpegCommit = '5db6a6819d0f904e0b58f34ae928fea234adb1a0' # libjpeg-turbo 2.0.3, matching the vendored headers

function Invoke-Checked {
    param([string]$Command, [string[]]$Arguments)
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE" }
}

# Import the architecture-specific MSVC environment into this PowerShell process.
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ build tools were not found.' }
$setup = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$environment = & $env:COMSPEC /d /s /c "`"$setup`" -no_logo -arch=$Architecture -host_arch=x64 && set"
if ($LASTEXITCODE -ne 0) { throw 'MSVC environment setup failed.' }
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
$nasmRoot = Join-Path $root '.build/tools/nasm-2.16.03'
if (!(Test-Path "$nasmRoot/nasm.exe")) {
    $archive = Join-Path $root '.build/tools/nasm-2.16.03-win64.zip'
    New-Item -ItemType Directory -Force -Path (Split-Path $archive) | Out-Null
    Invoke-WebRequest 'https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip' -OutFile $archive
    if ((Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne '3ee4782247bcb874378d02f7eab4e294a84d3d15f3f6ee2de2f47a46aa7226e6') {
        throw 'NASM archive checksum mismatch.'
    }
    Expand-Archive $archive (Split-Path $archive) -Force
}
$env:PATH = "$nasmRoot;$env:PATH"
if (!(Get-Command nasm -ErrorAction SilentlyContinue)) { throw 'Pinned NASM executable was not found.' }
New-Item -ItemType Directory -Force -Path $depRoot, $output | Out-Null

function Get-PinnedSource {
    param([string]$Name, [string]$Repository, [string]$Commit)
    $path = Join-Path $depRoot $Name
    if (!(Test-Path "$path/.git")) {
        Invoke-Checked git @('init', '--quiet', $path)
        Invoke-Checked git @('-C', $path, 'remote', 'add', 'origin', "https://github.com/$Repository.git")
        Invoke-Checked git @('-C', $path, 'fetch', '--quiet', '--depth=1', 'origin', $Commit)
        Invoke-Checked git @('-C', $path, 'checkout', '--quiet', '--detach', 'FETCH_HEAD')
    }
    $actual = & git -C $path rev-parse HEAD
    if ($actual -ne $Commit) { throw "Unexpected source revision for $Name`: $actual" }
    return $path
}

$libraw = Get-PinnedSource 'LibRaw' 'LibRaw/LibRaw' $librawCommit
$jpeg = Get-PinnedSource 'libjpeg-turbo' 'libjpeg-turbo/libjpeg-turbo' $jpegCommit
if (!(Test-Path "$libraw/bin/libraw.dll")) {
    Push-Location $libraw
    try {
        New-Item -ItemType Directory -Force -Path 'object', 'lib', 'bin' | Out-Null
        Invoke-Checked nmake @('/nologo', '/f', 'Makefile.msvc', 'bin\libraw.dll')
    } finally { Pop-Location }
}
$platform = if ($Architecture -eq 'x64') { 'x64' } else { 'Win32' }
$jpegBuild = Join-Path $depRoot 'jpeg-build'
Invoke-Checked cmake @('-S', $jpeg, '-B', $jpegBuild, '-G', 'Visual Studio 17 2022', '-A', $platform,
    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DENABLE_SHARED=ON', '-DENABLE_STATIC=OFF', '-DWITH_SIMD=ON', '-DWITH_CRT_DLL=ON', '-DBUILD=20261017')
Invoke-Checked cmake @('--build', $jpegBuild, '--config', 'Release', '--target', 'turbojpeg', '--parallel', '2')
foreach ($project in @('SonyMTPCamera/SonyMTPCamera.vcxproj', 'SonyCameraInfo/SonyCameraInfo.vcxproj')) {
    Invoke-Checked msbuild @((Join-Path $root $project), '/m:2', '/verbosity:minimal', '/p:Configuration=Release',
        "/p:Platform=$platform", "/p:SolutionDir=$root/", "/p:SonyDependencyRoot=$depRoot", "/p:SonyOutputRoot=$output")
}
Copy-Item "$libraw/bin/libraw.dll" $output
Copy-Item "$jpegBuild/Release/turbojpeg.dll" $output
@{
    architecture = $Architecture
    sourceCommit = (& git -C $root rev-parse HEAD)
    libraw = @{ version = '0.21.0'; commit = $librawCommit }
    libjpegTurbo = @{ version = '2.0.3'; commit = $jpegCommit; simd = $true }
    visualStudio = $vs
    builtAtUtc = [DateTime]::UtcNow.ToString('o')
} | ConvertTo-Json -Depth 4 | Set-Content "$output/BUILD.json" -Encoding utf8
Write-Host "Native build complete: $output"

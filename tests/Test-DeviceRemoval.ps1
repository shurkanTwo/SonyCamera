param([Parameter(Mandatory)][string]$OutputDirectory, [Parameter(Mandatory)][string]$DependencyRoot)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$executable = Join-Path $OutputDirectory 'DeviceRemovalTests.exe'
# Link the exact production objects used by SonyMTPCamera.dll; this exercises
# MTPDevice, DeviceEventHandler, Camera and the complete capture task state machine.
$objects = @(Get-ChildItem "$OutputDirectory/obj/SonyMTPCamera/*.obj" | ForEach-Object FullName)
if ($objects.Count -lt 20) { throw 'Production native objects were not found.' }
$libraries = @('kernel32.lib','user32.lib','gdi32.lib','winspool.lib','comdlg32.lib','advapi32.lib',
    'shell32.lib','ole32.lib','oleaut32.lib','uuid.lib','odbc32.lib','odbccp32.lib','cfgmgr32.lib',
    'PortableDeviceGUIDs.lib','rpcrt4.lib','Version.lib','propsys.lib','libraw.lib','turbojpeg.lib')
& cl /nologo /EHsc /MD /std:c++20 /DUNICODE /D_UNICODE "/I$root/SonyMTPCamera" "$root/tests/DeviceRemoval.cpp" "/Fe$executable" "/Fo$OutputDirectory/DeviceRemovalTests.obj" @objects /link /LTCG @libraries "/LIBPATH:$DependencyRoot/LibRaw/lib" "/LIBPATH:$DependencyRoot/jpeg-build/Release"
if ($LASTEXITCODE -ne 0) { throw 'Failed to link device-removal regression executable.' }
$result = & $executable
if ($LASTEXITCODE -ne 0) { throw 'Production device-removal regression checks failed.' }
$result | Set-Content "$OutputDirectory/DEVICE-REMOVAL-TESTS.json" -Encoding utf8
Write-Host $result

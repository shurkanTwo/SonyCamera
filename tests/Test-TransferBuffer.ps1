param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$baselineRevision = '21f469f6d2e9516ec4896eaa0ec9c45c4e96d92d'
$template = @'
#include <algorithm>
#include <cstdint>
#include <iostream>
using ULONG = std::uint32_t;
using std::min;
using std::max;
ULONG ProductionBufferSize(ULONG cbReportedDataSize, ULONG cbOptimalDataSize, bool maximumMemoryMode) {
__PRODUCTION_EXPRESSION__
return bufferSize;
}
int main() {
    constexpr ULONG MiB = 1024 * 1024;
    struct Case { const char* name; ULONG reported; ULONG optimal; bool maximum; ULONG expected; };
    const Case cases[] = {
        {"missing optimal size", 12 * MiB, 0, false, 8 * MiB},
        {"ordinary optimal size", 12 * MiB, 256 * 1024, false, 8 * MiB},
        {"small reported transfer", 1024, 0, false, 1024},
        {"tiny transfer", 8, 0, false, 8},
        {"zero reported transfer", 0, 0, false, 0},
        {"larger optimal size", 64 * MiB, 2 * MiB, false, 32 * MiB},
        {"maximum memory mode", 12 * MiB, 0, true, 12 * MiB}
    };
    int failures = 0;
    for (const auto& test : cases) {
        ULONG actual = ProductionBufferSize(test.reported, test.optimal, test.maximum);
        if (actual != test.expected) {
            std::cerr << test.name << ": got " << actual << ", expected " << test.expected << '\n';
            ++failures;
        }
    }
    std::cout << "checks=7; failures=" << failures << '\n';
    return failures ? 1 : 0;
}
'@
function Test-Expression {
    param([string]$Label, [string]$Source, [bool]$ExpectFailure)
    $expression = [regex]::Match($Source, '(?m)^\s*ULONG bufferSize = [^;]+;')
    if (!$expression.Success) { throw 'Production transfer-buffer expression was not found.' }
    $constant = [regex]::Match($Source, '(?m)^\s*static const ULONG TRANSFER_BUFFER_TARGET = [^;]+;')
    $code = $template.Replace('__PRODUCTION_EXPRESSION__', $constant.Value + "`n" + $expression.Value)
    $cpp = Join-Path $OutputDirectory "transfer-$Label.cpp"
    $exe = Join-Path $OutputDirectory "transfer-$Label.exe"
    $code | Set-Content $cpp -Encoding utf8
    & cl /nologo /EHsc /std:c++20 $cpp "/Fe$exe" "/Fo$OutputDirectory/transfer-$Label.obj"
    if ($LASTEXITCODE -ne 0) { throw "Failed to compile $Label production calculation." }
    & $exe
    $result = $LASTEXITCODE
    if (($ExpectFailure -and $result -ne 1) -or (!$ExpectFailure -and $result -ne 0)) {
        throw "Unexpected result from $Label production calculation: $result"
    }
}
$before = (& git -C $root show "${baselineRevision}:SonyMTPCamera/MTPDevice.cpp") -join "`n"
if ($LASTEXITCODE -ne 0) { throw 'Baseline source could not be read.' }
Test-Expression 'baseline' $before $true
$current = Get-Content (Join-Path $root 'SonyMTPCamera/MTPDevice.cpp') -Raw
Test-Expression 'current' $current $false
@{ checks = 7; baselineRejected = $true; currentPassed = $true; scope = 'production buffer-size calculation; no WPD/USB execution' } |
    ConvertTo-Json | Set-Content "$OutputDirectory/TRANSFER-TESTS.json" -Encoding utf8

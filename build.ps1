#requires -Version 5.1
# Builds the two DLLs of Unepic Better Mod from src\.
#
#   .\build.ps1                      release build into .\build
#   .\build.ps1 -Warnings            same, with -Wall -Wextra (two known warnings are
#                                    expected: an unused parameter in the core and an
#                                    unused variable in the loader, see README)
#   .\build.ps1 -Zig C:\path\zig.exe if zig is not on PATH
#   .\build.ps1 -Out mybuild         output folder, default .\build
#   .\build.ps1 -HistoricNames       link the core as unepic_mod_core_new.dll, the name
#                                    the shipped file was linked under, so a section
#                                    comparison with the release matches as closely as
#                                    possible (LLD embeds the output name)
#
# Only the Zig toolchain is needed (it carries clang/LLVM, the MinGW-w64 headers
# and the Windows import libraries).  Requires Zig 0.14 or newer; the shipped
# binaries were made with 0.17.0.  Nothing is downloaded.

param(
    [string]$Zig = 'zig',
    [string]$Out = 'build',
    [switch]$Warnings,
    [switch]$HistoricNames
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $PSCommandPath
$dir = Join-Path $here $Out

function Fail([string]$msg) {
    Write-Host ''
    Write-Host $msg -ForegroundColor Red
    exit 1
}

if (-not (Get-Command $Zig -ErrorAction SilentlyContinue)) {
    Fail "zig not found ('$Zig'). Install Zig or pass -Zig C:\path\to\zig.exe"
}
foreach ($f in @('src\unepic_mod_core.c', 'src\unepic_loader.c')) {
    if (-not (Test-Path -LiteralPath (Join-Path $here $f))) { Fail "missing $f" }
}
New-Item -ItemType Directory -Force -Path $dir | Out-Null

$flags = @('-target', 'x86-windows-gnu', '-shared', '-O2')
if ($Warnings) { $flags += @('-Wall', '-Wextra') }

# the loader is a proxy for the game's SDL2.dll, so its output must stay SDL2.dll.
$coreName = 'unepic_mod_core.dll'
if ($HistoricNames) { $coreName = 'unepic_mod_core_new.dll' }   # name used for the release build
$jobs = @(
    @{ out = $coreName;             src = 'src\unepic_mod_core.c'
       libs = @('-luser32', '-lgdi32', '-limm32', '-ladvapi32') },
    @{ out = 'SDL2.dll';            src = 'src\unepic_loader.c'
       libs = @('-luser32') }
)

foreach ($j in $jobs) {
    $target = Join-Path $dir $j.out
    $ccArgs = $flags + @('-o', $target, (Join-Path $here $j.src)) + $j.libs
    Write-Host ('building ' + $j.out + ' ...')
    & $Zig cc @ccArgs
    if ($LASTEXITCODE -ne 0) { Fail "$($j.out): zig cc failed with exit code $LASTEXITCODE" }
    if (-not (Test-Path -LiteralPath $target)) { Fail "$($j.out): no output produced" }
}

Write-Host ''
Write-Host 'built:'
foreach ($j in $jobs) {
    $t = Get-Item -LiteralPath (Join-Path $dir $j.out)
    Write-Host ('  {0,-22} {1,9} B  sha256 {2}' -f $t.Name, $t.Length, (Get-FileHash -LiteralPath $t.FullName).Hash.ToLower())
}
Write-Host ''
Write-Host ('files are in ' + $dir)
Write-Host 'to use them: copy the core and SDL2.dll over bin\, then'
Write-Host 'run "python -X utf8 tools\build_release.py build" to regenerate the archive.'
Write-Host ''
Write-Host 'comparing with the shipped files: LLD embeds the output name and randomises the'
Write-Host 'PE TimeDateStamp and the build-ID, so never compare whole-file hashes. Measured:'
Write-Host '  -HistoricNames core -> .text, .rdata, .data, .reloc, .tls identical (0 bytes),'
Write-Host '    only the 4-byte timestamp and the 16-byte build-ID differ.'
Write-Host '  loader            -> same, plus 388 bytes in .rdata and 29 in .text: the'
Write-Host '    embedded PDB path string differs by length and moves the pointers that'
Write-Host '    reference it. No instruction differs.'
exit 0

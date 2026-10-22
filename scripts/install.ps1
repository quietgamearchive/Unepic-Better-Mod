#Requires -Version 5.1
# Unepic mod installer.  Runs on the PowerShell that ships with Windows
# (right-click the file -> "Run with PowerShell", or: powershell -File install.ps1)
#   <game>\SDL2.dll                -> loader (from mod\SDL2.dll)
#   <game>\SDL2_orig.dll           -> the original SDL2.dll, kept as a backup
#   <game>\mod\unepic_mod_core.dll -> cheat core, hot-reloaded at runtime
#   <game>\unepic_mod.ini          -> settings (created once, never overwritten)

$ErrorActionPreference = 'Stop'

# Every way out of this script waits for a key press, so a double-clicked
# window never closes before the user has read the message.
function Stop-Here([int]$code) {
    Write-Host ""
    if (-not [Console]::IsInputRedirected) {
        Write-Host -NoNewline "Press any key to close..."
        try { [Console]::ReadKey() | Out-Null } catch { }
    }
    exit $code
}

function Fail([string]$msg) {
    Write-Host ""
    Write-Host $msg -ForegroundColor Red
    Stop-Here 1
}

trap { Fail $_.Exception.Message }

$here = Split-Path -Parent $PSCommandPath
$game = Split-Path -Parent $here
$orig = Join-Path $game 'SDL2_orig.dll'
$real = Join-Path $game 'SDL2.dll'
$loader = Join-Path $here 'SDL2.dll'
$core = Join-Path $here 'unepic_mod_core.dll'
$iniTpl = Join-Path $here 'unepic_mod.ini'
$iniLive = Join-Path $game 'unepic_mod.ini'

if (-not (Test-Path -LiteralPath (Join-Path $game 'unepic.exe'))) {
    Fail "unepic.exe not found in $game"
}
if (Get-Process -Name unepic -ErrorAction SilentlyContinue) {
    Fail "Unepic is running - close it first."
}
foreach ($f in @($loader, $core)) {
    if (-not (Test-Path -LiteralPath $f)) { Fail "missing $f" }
}

if (-not (Test-Path -LiteralPath $orig)) {
    if (-not (Test-Path -LiteralPath $real)) { Fail "SDL2.dll not found" }
    Rename-Item -LiteralPath $real -NewName 'SDL2_orig.dll'
    Write-Host "original SDL2.dll -> SDL2_orig.dll"
}

Copy-Item -LiteralPath $loader -Destination $real -Force
if (-not (Test-Path -LiteralPath $iniLive)) {
    if (Test-Path -LiteralPath $iniTpl) {
        Copy-Item -LiteralPath $iniTpl -Destination $iniLive
        Write-Host "created unepic_mod.ini"
    } else {
        Write-Host "no ini template shipped - unepic_mod.ini is created on the first change"
    }
}

Write-Host ""
Write-Host "installed."
Write-Host "  launch Unepic, then press the backquote key in game for the panel"
Write-Host "  to update the mod: replace mod\unepic_mod_core.dll (hot reload, ~1 s)"
Stop-Here 0

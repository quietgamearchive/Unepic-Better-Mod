#Requires -Version 5.1
# Unepic mod uninstaller: removes the loader and restores the original DLL.
# Runs on the PowerShell that ships with Windows (right-click -> "Run with PowerShell").

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

if (Get-Process -Name unepic -ErrorAction SilentlyContinue) {
    Fail "Unepic is running - close it first."
}

$ours = $false
if ((Test-Path -LiteralPath $real) -and (Test-Path -LiteralPath $loader)) {
    $ours = (Get-FileHash -LiteralPath $real).Hash -eq (Get-FileHash -LiteralPath $loader).Hash
}
if (Test-Path -LiteralPath $real) {
    if ($ours) {
        Remove-Item -LiteralPath $real -Force
        Write-Host "removed mod loader"
    } else {
        Write-Warning "SDL2.dll is not the installed loader - left untouched"
    }
}
if (Test-Path -LiteralPath $orig) {
    Rename-Item -LiteralPath $orig -NewName 'SDL2.dll'
    Write-Host "restored original SDL2.dll"
}
Write-Host "done. mod\ and unepic_mod.ini were kept."
Stop-Here 0

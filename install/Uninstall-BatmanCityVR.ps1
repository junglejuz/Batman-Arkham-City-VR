<#
  Batman City VR uninstaller. The installer copies it into the game folder (next to BatmanAC.exe).
  Removes the mod, puts back the game settings exactly as they were before the first install, removes the shortcuts.
  Optional: -GameDir "<folder with BatmanAC.exe>" -Yes (no questions) -RemoveSettings (also the mod's settings and logs)
#>
param([string]$GameDir, [switch]$Yes, [switch]$RemoveSettings, [switch]$KeepSelf)

$ErrorActionPreference = 'Stop'
function Say([string]$text, [string]$color = 'Gray') { Write-Host $text -ForegroundColor $color }
function Fail([string]$text) { Write-Host ''; Write-Host "  STOPPED: $text" -ForegroundColor Red; Write-Host ''; exit 1 }
$Here = [IO.Path]::GetDirectoryName($MyInvocation.MyCommand.Path)
$Marker = '[batman_city-proxy]'

Write-Host ''
Write-Host '  Batman City VR - uninstaller' -ForegroundColor White
Write-Host '  ---------------------------'
if (-not $GameDir) {
    if (Test-Path (Join-Path $Here 'BatmanAC.exe')) { $GameDir = $Here }
    else {
        foreach ($key in 'HKCU:\Software\Valve\Steam') {
            try {
                $root = ((Get-ItemProperty $key -ErrorAction Stop).SteamPath) -replace '/', '\'
                $libs = @($root)
                $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
                if (Test-Path $vdf) { foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) { $libs += ($m.Groups[1].Value -replace '\\\\', '\') } }
                foreach ($l in $libs) { $d = Join-Path $l 'steamapps\common\Batman Arkham City GOTY\Binaries\Win32'; if (Test-Path (Join-Path $d 'BatmanAC.exe')) { $GameDir = $d; break } }
            } catch { }
        }
    }
}
if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir 'BatmanAC.exe'))) { Fail 'the game was not found. Run this from the game folder (next to BatmanAC.exe).' }
if (Get-Process -Name 'BatmanAC' -ErrorAction SilentlyContinue) { Fail 'Batman: Arkham City is running. Close it first.' }
$GameRoot = [IO.Path]::GetFullPath((Join-Path $GameDir '..\..'))
$bk = Join-Path $GameRoot 'BatmanCityVR_backup'
Say "  Game: $GameDir"
if (-not $Yes) {
    $a = Read-Host '  Remove Batman City VR and put your game settings back? (y/n)'
    if ($a -notmatch '^[yY]') { Say '  Nothing changed.'; exit 0 }
}

# ---- the mod's files ------------------------------------------------------------------------------
$px = Join-Path $GameDir 'xinput1_3.dll'
if ((Test-Path $px) -and [IO.File]::ReadAllText($px, [Text.Encoding]::GetEncoding(28591)).Contains($Marker)) { Remove-Item $px -Force }
foreach ($f in 'batman_city.dll', 'Batman City VR.bat', 'Batman City VR (SteamVR).bat', 'Batman City VR (test).bat') {
    $p = Join-Path $GameDir $f; if (Test-Path $p) { Remove-Item $p -Force }
}
foreach ($d in 'BatmanCityData', 'BatmanCityDLSS') { $p = Join-Path $GameDir $d; if (Test-Path $p) { Remove-Item $p -Recurse -Force } }
if (Test-Path (Join-Path $bk 'binaries\xinput1_3.dll')) { Copy-Item (Join-Path $bk 'binaries\xinput1_3.dll') $px -Force; Say '  The xinput1_3.dll that was there before is back.' }
Say '  Mod files removed.' Green

# ---- the game's settings, exactly as they were -------------------------------------------------
if (Test-Path (Join-Path $bk 'config\BmEngine.ini')) {
    $cfgDir = (Get-Content (Join-Path $bk 'config_dir.txt') -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($cfgDir -and (Test-Path $cfgDir)) {
        foreach ($f in 'BmEngine.ini', 'UserEngine.ini') {
            $s = Join-Path $bk "config\$f"; if (Test-Path $s) { Copy-Item $s (Join-Path $cfgDir $f) -Force }
        }
    }
    $s = Join-Path $bk 'install\DefaultEngine.ini'
    if (Test-Path $s) { Copy-Item $s (Join-Path $GameRoot 'BmGame\Config\DefaultEngine.ini') -Force }
    Say '  Your game settings are back as they were before the mod.' Green
} else {
    Say '  No settings backup found: the game settings were left as they are (DirectX 11 stays on).' Yellow
}

# ---- shortcuts ----------------------------------------------------------------------------------------
$desk = [Environment]::GetFolderPath('Desktop')
foreach ($n in 'Batman City VR.lnk', 'Batman City VR (SteamVR).lnk') { $p = Join-Path $desk $n; if (Test-Path $p) { Remove-Item $p -Force } }
$start = Join-Path ([Environment]::GetFolderPath('Programs')) 'Batman City VR'
if (Test-Path $start) { Remove-Item $start -Recurse -Force }

if ($RemoveSettings -or (-not $Yes -and (Read-Host '  Also delete the mod''s own VR settings and logs? (y/n)') -match '^[yY]')) {
    $data = Join-Path $env:LOCALAPPDATA 'Batman_City'
    if (Test-Path $data) { Remove-Item $data -Recurse -Force }
    Say '  VR settings and logs deleted.'
}
if (Test-Path $bk) { Remove-Item $bk -Recurse -Force }

Write-Host ''
Write-Host '  Batman City VR is removed.' -ForegroundColor White
Write-Host ''
# Remove ourselves when run from the game folder (the .bat deletes itself after this script is gone).
if (-not $KeepSelf -and $Here -eq $GameDir) { Remove-Item $MyInvocation.MyCommand.Path -Force -ErrorAction SilentlyContinue }

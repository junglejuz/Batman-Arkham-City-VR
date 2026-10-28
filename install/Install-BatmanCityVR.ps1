<#
  Batman City VR installer (Batman: Arkham City GOTY in PC VR).

  Double-click Install-BatmanCityVR.bat. It finds the game, backs up what it changes, copies the mod into the
  game folder, switches the game to DirectX 11 with VR-friendly settings, and makes the "Batman City VR" shortcut.
  Every change is listed in FIX_CHANGES.md; Uninstall-BatmanCityVR.bat (copied into the game folder) undoes them.

  Optional: -GameDir "<folder with BatmanAC.exe>" -ConfigDir "<...\BmGame\Config in Documents>" -Yes (no questions)
#>
param([string]$GameDir, [string]$ConfigDir, [switch]$Yes, [switch]$NoShortcuts)

$Mod = @{
    Name        = 'Batman City VR'
    Exe         = 'BatmanAC.exe'
    SteamFolder = 'Batman Arkham City GOTY\Binaries\Win32'           # under steamapps\common
    DocsConfig  = 'WB Games\Batman Arkham City GOTY\BmGame\Config'   # under Documents
    ProxyMarker = '[batman_city-proxy]'                              # text inside our xinput1_3.dll
    Backup      = 'BatmanCityVR_backup'                              # in the game's top folder
}

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
function Say([string]$text, [string]$color = 'Gray') { Write-Host $text -ForegroundColor $color }
function Step([string]$text) { Write-Host ''; Write-Host "  $text" -ForegroundColor Cyan }
function Fail([string]$text) { Write-Host ''; Write-Host "  STOPPED: $text" -ForegroundColor Red; Write-Host ''; exit 1 }
$Here = [IO.Path]::GetDirectoryName($MyInvocation.MyCommand.Path)

# ---- finding things -------------------------------------------------------------------------------
function Find-SteamGameDir {
    $roots = @()
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam') {
        try {
            $p = Get-ItemProperty $key -ErrorAction Stop
            if ($p.SteamPath) { $roots += ($p.SteamPath -replace '/', '\') }
            if ($p.InstallPath) { $roots += $p.InstallPath }
        } catch { }
    }
    $libs = @()
    foreach ($r in ($roots | Select-Object -Unique)) {
        $libs += $r
        $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) { $libs += ($m.Groups[1].Value -replace '\\\\', '\') }
        }
    }
    foreach ($lib in ($libs | Select-Object -Unique)) {
        $dir = [IO.Path]::Combine($lib, 'steamapps\common\' + $Mod.SteamFolder)
        if (Test-Path -LiteralPath ([IO.Path]::Combine($dir, $Mod.Exe))) { return $dir }
    }
    return $null
}
function Select-Folder([string]$description) {
    $f = New-Object System.Windows.Forms.Form; $f.TopMost = $true
    $d = New-Object System.Windows.Forms.FolderBrowserDialog; $d.Description = $description
    if ($d.ShowDialog($f) -eq [System.Windows.Forms.DialogResult]::OK) { return $d.SelectedPath }
    return $null
}
function Resolve-GameDir([string]$picked) {
    if (-not $picked -or -not (Test-Path -LiteralPath $picked -PathType Container)) { return @() }
    if (Test-Path -LiteralPath (Join-Path $picked $Mod.Exe)) { return @($picked) }
    return @(Get-ChildItem -LiteralPath $picked -Filter $Mod.Exe -Recurse -Depth 4 -File -ErrorAction SilentlyContinue |
        ForEach-Object { $_.DirectoryName } | Select-Object -Unique)
}
# Documents can be anywhere (moved, OneDrive): Windows' own lookup first, as the game does.
function Get-ConfigCandidates {
    $docs = @([Environment]::GetFolderPath('MyDocuments'))
    try { $docs += [Environment]::ExpandEnvironmentVariables((Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders' -ErrorAction Stop).Personal) } catch { }
    $docs += (Join-Path $env:USERPROFILE 'Documents'), (Join-Path $env:USERPROFILE 'OneDrive\Documents')
    return @($docs | Where-Object { $_ } | Select-Object -Unique | ForEach-Object { Join-Path $_ $Mod.DocsConfig })
}

# ---- ini editing --------------------------------------------------------------------------------
# UE3 configs: Key=Value, no spaces. BmEngine.ini / DefaultEngine.ini are 8-bit text; UserEngine.ini is UTF-16 with a
# byte-order mark (a plain-text write breaks it). Line endings and encoding are kept exactly; never add a UTF-8 BOM.
function Read-Cfg([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $utf16 = $bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE
    $enc = if ($utf16) { New-Object Text.UnicodeEncoding($false, $true) } else { [Text.Encoding]::GetEncoding(28591) }
    $text = if ($utf16) { $enc.GetString($bytes, 2, $bytes.Length - 2) } else { $enc.GetString($bytes) }
    return @{ Path = $path; Text = $text; Utf16 = $utf16; Enc = $enc }
}
function Save-Cfg($cfg) {
    $body = $cfg.Enc.GetBytes($cfg.Text)
    if ($cfg.Utf16) { $body = [byte[]](0xFF, 0xFE) + $body }
    [IO.File]::WriteAllBytes($cfg.Path, $body)
}
# Sets Key=Value inside [section] only (adds the line at the end of the section if missing). Returns $true if changed.
function Set-CfgValue($cfg, [string]$section, [string]$key, [string]$value) {
    $nl = if ($cfg.Text -match "`r`n") { "`r`n" } else { "`n" }
    $lines = [Collections.Generic.List[string]]($cfg.Text -split "`r?`n")
    # UE3 configs can hold the same [section] more than once (DefaultEngine.ini has two [Engine.GameEngine]): look in
    # every copy; a missing key goes at the end of the last copy.
    $in = $false; $last = -1; $changed = $false; $found = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $l = $lines[$i]
        if ($l -match '^\s*\[(.+)\]\s*$') { $in = ($Matches[1] -eq $section); if ($in) { $last = $i }; continue }
        if (-not $in) { continue }
        if ($l.Trim() -ne '') { $last = $i }
        if ($l -match ('^' + [regex]::Escape($key) + '=(.*)$')) {
            $found = $true
            if ($Matches[1] -ne $value) { $lines[$i] = "$key=$value"; $changed = $true }
        }
    }
    if (-not $found) {
        if ($last -lt 0) { return $false }   # no such section: leave the file alone
        $lines.Insert($last + 1, "$key=$value"); $changed = $true
    }
    if ($changed) { $cfg.Text = [string]::Join($nl, $lines) }
    return $changed
}

# =================================================================================================
Write-Host ''
Write-Host '  Batman City VR - installer' -ForegroundColor White
Write-Host '  -------------------------'

# ---- 1. the game ---------------------------------------------------------------------------------
Step '1. Finding Batman: Arkham City'
if (-not $GameDir) { $GameDir = Find-SteamGameDir }
while (-not $GameDir -or -not (Test-Path (Join-Path $GameDir $Mod.Exe))) {
    if ($Yes) { Fail 'the game was not found. Run the installer again with -GameDir "<folder with BatmanAC.exe>".' }
    Say '  Could not find the game by itself. Please pick its folder (for example ...\steamapps\common\Batman Arkham City GOTY).' Yellow
    $pick = Select-Folder 'Pick the Batman: Arkham City folder'
    if (-not $pick) { Fail 'no game folder was picked.' }
    $found = Resolve-GameDir $pick
    if ($found.Count -eq 1) { $GameDir = $found[0] } else { Say '  BatmanAC.exe is not in that folder. Try again.' Yellow }
}
if (Get-Process -Name 'BatmanAC' -ErrorAction SilentlyContinue) { Fail 'Batman: Arkham City is running. Close it, then run the installer again.' }
$GameRoot = [IO.Path]::GetFullPath((Join-Path $GameDir '..\..'))
Say "  Game: $GameDir"

if (-not $ConfigDir) { $ConfigDir = Get-ConfigCandidates | Where-Object { Test-Path (Join-Path $_ 'BmEngine.ini') } | Select-Object -First 1 }
if (-not $ConfigDir -or -not (Test-Path (Join-Path $ConfigDir 'BmEngine.ini'))) {
    Fail ('the game has not made its settings yet. Start Batman: Arkham City once from Steam as normal, wait for the ' +
          'main menu, quit, then run this installer again.')
}
Say "  Game settings: $ConfigDir"

# ---- 2. back up what we change (first install only, so the uninstaller always gets the player's originals) --------
Step '2. Backing up'
$bk = Join-Path $GameRoot $Mod.Backup
$first = -not (Test-Path (Join-Path $bk 'config\BmEngine.ini'))
if ($first) {
    New-Item -ItemType Directory -Force (Join-Path $bk 'config'), (Join-Path $bk 'install'), (Join-Path $bk 'binaries') | Out-Null
    foreach ($f in 'BmEngine.ini', 'UserEngine.ini') {
        if (Test-Path (Join-Path $ConfigDir $f)) { Copy-Item (Join-Path $ConfigDir $f) (Join-Path $bk "config\$f") }
    }
    $def = Join-Path $GameRoot 'BmGame\Config\DefaultEngine.ini'
    if (Test-Path $def) { Copy-Item $def (Join-Path $bk 'install\DefaultEngine.ini') }
    Set-Content -Path (Join-Path $bk 'config_dir.txt') -Value $ConfigDir -Encoding ASCII
    Say "  Your game settings were saved in $bk" Green
} else {
    Say '  Already backed up by an earlier install (kept as it is: those are your originals).'
}
# Someone else's xinput1_3.dll in the game folder (another mod): keep it, the uninstaller puts it back.
$px = Join-Path $GameDir 'xinput1_3.dll'
if ((Test-Path $px) -and -not ([IO.File]::ReadAllText($px, [Text.Encoding]::GetEncoding(28591)).Contains($Mod.ProxyMarker))) {
    if (-not (Test-Path (Join-Path $bk 'binaries\xinput1_3.dll'))) { Copy-Item $px (Join-Path $bk 'binaries\xinput1_3.dll') }
    Say '  Another xinput1_3.dll was in the game folder (another mod?). It is saved and comes back on uninstall.' Yellow
}

# ---- 3. the mod ---------------------------------------------------------------------------------
Step '3. Copying the mod into the game folder'
foreach ($f in 'xinput1_3.dll', 'batman_city.dll', 'Batman City VR.bat', 'Batman City VR (SteamVR).bat',
               'Uninstall-BatmanCityVR.bat', 'Uninstall-BatmanCityVR.ps1') {
    $src = Join-Path $Here $f
    if (-not (Test-Path $src)) { Fail "$f is missing from the download. Unpack the whole zip and run the installer from there." }
    Copy-Item $src (Join-Path $GameDir $f) -Force
}
foreach ($d in 'BatmanCityData', 'BatmanCityDLSS') {
    $src = Join-Path $Here "files\$d"
    if (Test-Path $src) { Copy-Item $src $GameDir -Recurse -Force }
}
Say '  Mod files copied.' Green

# ---- 4. the mod's tested settings (first install only: never over the player's own) --------------------------
$data = Join-Path $env:LOCALAPPDATA 'Batman_City'
New-Item -ItemType Directory -Force $data | Out-Null
if (-not (Test-Path (Join-Path $data 'settings.ini'))) {
    Copy-Item (Join-Path $Here 'files\settings.ini') (Join-Path $data 'settings.ini')
    Say '  Tested VR settings copied (change them later in the in-game VR menu).' Green
} else {
    Say '  Your VR settings from an earlier install are kept.'
}
# The VR render size the launcher starts the game at, and the recorded bat transition the menus replay (otherwise only
# recorded the first time a game is started from the main menu). First install only, like the settings.
foreach ($f in 'launch.ini', 'menu_wipe.bin') {
    $src = Join-Path $Here "files\$f"
    if ((Test-Path $src) -and -not (Test-Path (Join-Path $data $f))) { Copy-Item $src (Join-Path $data $f) }
}

# ---- 5. the game's settings for VR -----------------------------------------------------------------
Step '4. Setting the game up for VR'
$bm = Read-Cfg (Join-Path $ConfigDir 'BmEngine.ini')
$n = 0
foreach ($kv in @(@('AllowD3D11', 'True'), @('MotionBlur', 'False'), @('LensFlares', 'False'), @('UseVsync', 'False'))) {
    if (Set-CfgValue $bm 'SystemSettings' $kv[0] $kv[1]) { $n++ }
}
if (Set-CfgValue $bm 'Engine.GameEngine' 'MaxSmoothedFrameRate' '90') { $n++ }
if ($n) { Save-Cfg $bm }
$ue = Join-Path $ConfigDir 'UserEngine.ini'
if (Test-Path $ue) {
    $u = Read-Cfg $ue; $m = 0
    foreach ($kv in @(@('AllowD3D11', 'True'), @('MotionBlur', 'False'), @('LensFlares', 'False'), @('UseVsync', 'False'))) {
        if (Set-CfgValue $u 'SystemSettings' $kv[0] $kv[1]) { $m++ }
    }
    if ($m) { Save-Cfg $u }
}
# The game rewrites the frame cap in BmEngine.ini from its install defaults at every start: raise it there too.
$def = Join-Path $GameRoot 'BmGame\Config\DefaultEngine.ini'
if (Test-Path $def) { $d = Read-Cfg $def; if (Set-CfgValue $d 'Engine.GameEngine' 'MaxSmoothedFrameRate' '90') { Save-Cfg $d } }
Say '  DirectX 11 on, motion blur, lens flares and V-sync off, frame cap 90.' Green

# ---- 6. shortcuts -----------------------------------------------------------------------------------
if (-not $NoShortcuts) {
    Step '5. Making the shortcuts'
    $ws = New-Object -ComObject WScript.Shell
    $places = @([Environment]::GetFolderPath('Desktop'), (Join-Path ([Environment]::GetFolderPath('Programs')) 'Batman City VR'))
    New-Item -ItemType Directory -Force $places[1] | Out-Null
    foreach ($p in $places) {
        foreach ($s in @(@('Batman City VR', 'Batman City VR.bat'), @('Batman City VR (SteamVR)', 'Batman City VR (SteamVR).bat'),
                         @('Uninstall Batman City VR', 'Uninstall-BatmanCityVR.bat'))) {
            if ($p -eq $places[0] -and $s[0] -like 'Uninstall*') { continue }   # the desktop gets the two launchers only
            $lnk = $ws.CreateShortcut((Join-Path $p ($s[0] + '.lnk')))
            $lnk.TargetPath = Join-Path $GameDir $s[1]
            $lnk.WorkingDirectory = $GameDir
            $lnk.IconLocation = (Join-Path $GameDir $Mod.Exe) + ',0'
            $lnk.Save()
        }
    }
    Say '  "Batman City VR" is on your desktop and in the Start menu.' Green
}

Write-Host ''
Write-Host '  Done. Start your PC VR software, then start the game with the "Batman City VR" shortcut.' -ForegroundColor White
Write-Host '  In the game, hold both thumbsticks down (or press F10) for the VR menu.'
Write-Host ''

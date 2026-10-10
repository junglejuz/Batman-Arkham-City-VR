# 2026-10-07 run 4 (edges: grunge feather, pause dirt mirror), no sweep: title grunge, in the room (head straight / turned), camera sweep + reticle probe in game, pause-menu dirt. Closes the game.
$ErrorActionPreference = 'Continue'
$g = "E:\Games\Steam\steamapps\common\Batman Arkham City GOTY\Binaries\Win32"
$data = "$env:LOCALAPPDATA\Batman_City"
$tools = "D:\Documents\Antigravity\VR_Modding\batman_city\tools"
$env:XR_RUNTIME_JSON = "$data\xrsim\bvr_xrsim32.json"
$env:BVR_XRSIM_DIR = "$data\xrsim"

Add-Type @"
using System; using System.Runtime.InteropServices;
public class FocusV {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
}
"@
function Focus() {
    $p = Get-Process BatmanAC -ErrorAction SilentlyContinue
    if ($p -and $p.MainWindowHandle -ne [IntPtr]::Zero) {
        [FocusV]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [FocusV]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
        [void][FocusV]::ShowWindow($p.MainWindowHandle, 5)
        [void][FocusV]::SetForegroundWindow($p.MainWindowHandle)
    }
}
function Key([byte]$vk, [byte]$scan) {
    Focus
    Start-Sleep -Milliseconds 300
    [FocusV]::keybd_event($vk, $scan, 0x8, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 120
    [FocusV]::keybd_event($vk, $scan, 0xA, [UIntPtr]::Zero)
}
function Cmd([string]$line, [int]$after = 2) {
    Set-Content "$data\command.txt" $line -Encoding ascii
    Start-Sleep -Seconds $after
}
function Shot([string]$name) {
    try { & "$tools\xrsim-shot.ps1" -Out "$env:TEMP\bc_$name" -Quiet -TimeoutSec 6 | Out-Null } catch {}
    Start-Sleep 2
    Get-ChildItem "$data\xrsim\capture\bc_$name*" -ErrorAction SilentlyContinue | ForEach-Object {
        $dst = Join-Path $_.DirectoryName ($_.Name -replace "^bc_", "vis_")
        if (Test-Path $dst) { Remove-Item $dst -Force }
        Rename-Item $_.FullName $dst }
}
$tools = "D:\Documents\Antigravity\VR_Modding\batman_city\tools"
function Head([string]$ypr) { try { & "$tools\xrsim-cmd.ps1" -Quiet "head rot $ypr" | Out-Null } catch { "head cmd failed: $_" } }
function WaitLog([string]$pat, [int]$sec) {
    for ($i = 0; $i -lt $sec * 4; $i++) {
        if (Select-String -Path "$data\batman_city.log" -Pattern $pat -Quiet -ErrorAction SilentlyContinue) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}
if (Get-Process BatmanAC -ErrorAction SilentlyContinue) { "GAME ALREADY RUNNING - abort"; exit 1 }
Copy-Item "$data\settings.ini" "$data\settings.ini.testbackup" -Force -ErrorAction SilentlyContinue
Get-ChildItem "$data\xrsim\capture\vis_*", "$data\xrsim\capture\bc_*" -ErrorAction SilentlyContinue | Remove-Item -Force
Start-Process -FilePath "$g\Batman City VR (test).bat" -WorkingDirectory $g
Start-Sleep 3
if (-not (WaitLog "movie opened" 90)) { "no logo movie opened" }
Start-Sleep 3
Head "0 0 0"
Key 0x20 0x39
function MovieCount([string]$what) { (Select-String -Path "$data\batman_city.log" -Pattern "full-screen movie $what" -ErrorAction SilentlyContinue).Count }
for ($i = 0; $i -lt 40 -and (MovieCount "ended") -lt 1; $i++) { Start-Sleep -Milliseconds 500 }
Start-Sleep 5
"title: grunge, head straight"
Head "0 0 0"; Start-Sleep 1
Shot "t1_title_ahead"
Cmd "bc capture" 4
"title: head turned 25 deg left"
Head "25 0 0"; Start-Sleep 1
Shot "t2_title_yaw25"
Cmd "bc capture" 4
Head "0 0 0"; Start-Sleep 1
Focus; Start-Sleep -Milliseconds 500
for ($i = 0; $i -lt 6 -and (MovieCount "STARTED") -lt 2; $i++) {
    Key 0x0D 0x1C
    for ($w = 0; $w -lt 8 -and (MovieCount "STARTED") -lt 2; $w++) { Start-Sleep -Milliseconds 400 }
}
"enter presses: $i"
for ($i = 0; $i -lt 60 -and (MovieCount "ended") -lt 2; $i++) { Start-Sleep -Milliseconds 500 }
Start-Sleep 6
$pp = Join-Path $PSScriptRoot "is_paused.py"
"in game"
for ($t = 0; $t -lt 3; $t++) {
    Shot "g0_game"
    $st = & python $pp "$data\xrsim\capture\vis_g0_game_left.png"
    "after load: $st"
    if ($st -notlike "PAUSED*") { break }
    Key 0x1B 0x01; Start-Sleep 3
}
"pause menu"
Key 0x1B 0x01
Start-Sleep 3
Head "0 0 0"; Start-Sleep 1
Shot "p1_pause_ahead"
Cmd "bc capture" 5
Head "25 0 0"; Start-Sleep 1
Shot "p2_pause_yaw25"
Head "0 0 0"
Get-Process BatmanAC -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
Start-Sleep 1
if (Get-Process BatmanAC -ErrorAction SilentlyContinue) { "STILL RUNNING" } else { "test game closed" }
if (Test-Path "$data\settings.ini.testbackup") { Move-Item "$data\settings.ini.testbackup" "$data\settings.ini" -Force; "settings restored" }

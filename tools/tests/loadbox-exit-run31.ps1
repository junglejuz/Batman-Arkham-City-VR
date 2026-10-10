# 2026-10-09 run 31: LOADBOX2 (loading-screen top-left piece not drawn) - loading shots; then LEAVES THE GAME RUNNING in gameplay
#  for step-by-step driving (exit to the main menu: the bat wipe). settings.ini.testbackup must be restored afterwards.
#  Logos -> menu -> Continue -> gameplay, then the shots below; closes the game, restores settings.ini.
# Logos -> title -> save -> main menu -> Continue Story -> loading -> gameplay. Closes the game, restores settings.ini.
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
    try { & "$tools\xrsim-shot.ps1" -Out "$env:TEMP\bc_$name" -Quiet -TimeoutSec 6 | Out-Null } catch { "shot $name failed: $_" }
}
function Head([string]$ypr) { try { & "$tools\xrsim-cmd.ps1" -Quiet "head rot $ypr" | Out-Null } catch { "head cmd failed: $_" } }
function WaitLog([string]$pat, [int]$sec) {
    for ($i = 0; $i -lt $sec * 4; $i++) {
        if (Select-String -Path "$data\batman_city.log" -Pattern $pat -Quiet -ErrorAction SilentlyContinue) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}
function MovieCount([string]$what) { (Select-String -Path "$data\batman_city.log" -Pattern "full-screen movie $what" -ErrorAction SilentlyContinue).Count }

if (Get-Process BatmanAC -ErrorAction SilentlyContinue) { "GAME ALREADY RUNNING - abort"; exit 1 }
Copy-Item "$data\settings.ini" "$data\settings.ini.testbackup" -Force -ErrorAction SilentlyContinue
Add-Content "$data\settings.ini" "debug_hudline_after_movie=900" -Encoding ascii
Remove-Item "$data\panelseq" -Recurse -Force -ErrorAction SilentlyContinue
Get-ChildItem "$data\xrsim\capture\vis_*", "$data\xrsim\capture\bc_*" -ErrorAction SilentlyContinue | Remove-Item -Force
Start-Process -FilePath "$g\Batman City VR (test).bat" -WorkingDirectory $g
Start-Sleep 3
if (-not (WaitLog "movie opened" 90)) { "no logo movie opened" }
Start-Sleep 3
Head "0 0 0"
Key 0x20 0x39
for ($i = 0; $i -lt 60 -and (MovieCount "ended") -lt 1; $i++) { Start-Sleep -Milliseconds 100 }
"logos ended"
Start-Sleep 4
Focus; Start-Sleep -Milliseconds 500
"Enter 1: title -> save select"
Key 0x0D 0x1C
Start-Sleep 10
"Enter 2: save slot -> main menu"
Key 0x0D 0x1C
Start-Sleep 3
$started0 = MovieCount "STARTED"; $ended0 = MovieCount "ended"
"Enter 3: Continue Story -> loading"
Key 0x0D 0x1C
for ($i = 0; $i -lt 200 -and (MovieCount "STARTED") -le $started0; $i++) { Start-Sleep -Milliseconds 100 }
"loading movie started"
Start-Sleep 2
for ($i = 0; $i -lt 4; $i++) { Shot ("b" + $i + "_loading"); Start-Sleep 2 }
for ($i = 0; $i -lt 1200 -and (MovieCount "ended") -le $ended0; $i++) { Start-Sleep -Milliseconds 100 }
"loading ended - game left running"
Start-Sleep 6
Shot "c0_gameplay"
Get-ChildItem "$data\xrsim\capture\bc_*" -ErrorAction SilentlyContinue | ForEach-Object {
    $dst = Join-Path $_.DirectoryName ($_.Name -replace "^bc_", "vis_")
    if (Test-Path $dst) { Remove-Item $dst -Force }
    Rename-Item $_.FullName $dst }

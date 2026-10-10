# 2026-10-08 run 15: DLSS at Quality with 0 / 1 / 2 extra evaluates per eye: is the 4 ms per eye DLSS itself or the
# hand-off between processes? Then DLSS off. Closes the game.
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
$set = (Get-Content "$data\settings.ini") | Where-Object { $_ -notlike "dlss_mode=*" }
$set + "dlss_mode=3" | Set-Content "$data\settings.ini" -Encoding ascii
Get-ChildItem "$data\xrsim\capture\vis_*", "$data\xrsim\capture\bc_*" -ErrorAction SilentlyContinue | Remove-Item -Force
Start-Process -FilePath "$g\Batman City VR (test).bat" -WorkingDirectory $g
Start-Sleep 3
if (-not (WaitLog "movie opened" 90)) { "no logo movie opened" }
$t0 = Get-Date
Start-Sleep 3
Head "0 0 0"; Shot "l1_logo_ahead"
Head "0 0 0"
Key 0x20 0x39
function MovieCount([string]$what) { (Select-String -Path "$data\batman_city.log" -Pattern "full-screen movie $what" -ErrorAction SilentlyContinue).Count }
# title up = the logo movie ended
for ($i = 0; $i -lt 40 -and (MovieCount "ended") -lt 1; $i++) { Start-Sleep -Milliseconds 500 }
Start-Sleep 2
Focus; Start-Sleep -Milliseconds 500
# Enter until the loading screen starts (2nd full-screen movie), never more
for ($i = 0; $i -lt 6 -and (MovieCount "STARTED") -lt 2; $i++) {
    Key 0x0D 0x1C
    for ($w = 0; $w -lt 8 -and (MovieCount "STARTED") -lt 2; $w++) { Start-Sleep -Milliseconds 400 }
}
"enter presses: $i"
for ($i = 0; $i -lt 60 -and (MovieCount "ended") -lt 2; $i++) { Start-Sleep -Milliseconds 500 }
Start-Sleep 3
"in game"
Start-Sleep 5
$pp = Join-Path $PSScriptRoot "is_paused.py"
for ($t = 0; $t -lt 3; $t++) {
    Shot "g0_game"
    $st = & python $pp "$data\xrsim\capture\vis_g0_game_left.png"
    "after load: $st"
    if ($st -notlike "PAUSED*") { break }
    Key 0x1B 0x01; Start-Sleep 3
}
$smi = "$env:TEMP\bc_smi.csv"
Remove-Item $smi -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "nvidia-smi" -ArgumentList "--query-gpu=timestamp,utilization.gpu,clocks.gr,power.draw,memory.used --format=csv,noheader -lms 500" -RedirectStandardOutput $smi -PassThru -WindowStyle Hidden
foreach ($rp in 0, 1, 2, 0) {
    Cmd "bc dlss repeat $rp" 3
    (Get-Date -Format "HH:mm:ss.fff") + " repeat $rp start"
    Start-Sleep 10
}
Cmd "bc dlss mode 0" 3
(Get-Date -Format "HH:mm:ss.fff") + " DLSS off (stretched) start"
Start-Sleep 10
(Get-Date -Format "HH:mm:ss.fff") + " end"
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Copy-Item $smi "$data\perf_smi.csv" -Force
Get-Process BatmanAC -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
Start-Sleep 1
if (Get-Process BatmanAC -ErrorAction SilentlyContinue) { "STILL RUNNING" } else { "test game closed" }
if (Test-Path "$data\settings.ini.testbackup") { Move-Item "$data\settings.ini.testbackup" "$data\settings.ini" -Force; "settings restored" }

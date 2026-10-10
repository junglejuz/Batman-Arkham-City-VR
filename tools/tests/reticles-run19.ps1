# 2026-10-08 run 19: aim reticles - batarang (1) and gadget 2 aimed with the right mouse button held, captures
# while aiming (targets.txt = every world->screen placement + caller). Closes the game.
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
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint f);
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
# settings unchanged
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
function Aim([string]$key, [byte]$vk, [byte]$scan, [string]$name) {
    for ($t = 0; $t -lt 3; $t++) {
        Shot "pre_$name"
        $st = & python $pp "$data\xrsim\capture\vis_pre_${name}_left.png"
        "before $name : $st"
        if ($st -notlike "PAUSED*") { break }
        Key 0x1B 0x01; Start-Sleep 3
    }
    # No Focus (it taps Alt: Arkham City pauses when its window loses focus, run 19b); the game is already in front.
    [FocusV]::keybd_event($vk, $scan, 0x8, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 120
    [FocusV]::keybd_event($vk, $scan, 0xA, [UIntPtr]::Zero)
    Start-Sleep 1
    # Right mouse = aim. The click lands on whatever is under the pointer (another window -> Arkham City loses focus
    # and pauses, run 19b), so the pointer goes over the game window first and comes back afterwards.
    $gp = Get-Process BatmanAC -ErrorAction SilentlyContinue
    $r = New-Object FocusV+RECT
    [void][FocusV]::GetWindowRect($gp.MainWindowHandle, [ref]$r)
    $old = New-Object FocusV+POINT
    [void][FocusV]::GetCursorPos([ref]$old)
    $cx = [int](($r.L + $r.R) / 2); $cy = [int](($r.T + $r.B) / 2)
    [void][FocusV]::SetCursorPos($cx, $cy)
    $pt = New-Object FocusV+POINT; $pt.X = $cx; $pt.Y = $cy
    $under = [FocusV]::GetAncestor([FocusV]::WindowFromPoint($pt), 2)
    if ($under -ne $gp.MainWindowHandle) { "pointer is not over the game window ($under vs $($gp.MainWindowHandle)) - no click"; [void][FocusV]::SetCursorPos($old.X, $old.Y); return }
    [FocusV]::mouse_event(0x8, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep 2
    Shot "aim_$name"
    $st2 = & python $pp "$data\xrsim\capture\vis_aim_${name}_left.png"
    "while aiming $name : $st2"
    Cmd "bc capture" 4
    (Get-Date -Format "HH:mm:ss.fff") + " captured aiming $name"
    Head "12 -6 0"; Start-Sleep 1
    Shot "aim_${name}_head"
    Head "0 0 0"
    [FocusV]::mouse_event(0x10, 0, 0, 0, [UIntPtr]::Zero)
    [void][FocusV]::SetCursorPos($old.X, $old.Y)
    Start-Sleep 2
}
Cmd "bc aimdepth 0" 1
Aim "1" 0x31 0x02 "off"
Cmd "bc aimdepth 1" 1
Cmd "bc aimnopass 1" 1
Aim "1" 0x31 0x02 "nopass"
Cmd "bc aimnopass 0" 1
Aim "1" 0x31 0x02 "full"
Aim "2" 0x32 0x03 "claw"
(Get-Date -Format "HH:mm:ss.fff") + " end"
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Copy-Item $smi "$data\perf_smi.csv" -Force
Get-Process BatmanAC -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
Start-Sleep 1
if (Get-Process BatmanAC -ErrorAction SilentlyContinue) { "STILL RUNNING" } else { "test game closed" }
if (Test-Path "$data\settings.ini.testbackup") { Move-Item "$data\settings.ini.testbackup" "$data\settings.ini" -Force; "settings restored" }

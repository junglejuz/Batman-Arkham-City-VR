# Zoom / detective experiment on the fake headset. Closes the game at the end.
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

if (Get-Process BatmanAC -ErrorAction SilentlyContinue) { "GAME ALREADY RUNNING - abort"; exit 1 }
Get-ChildItem "$data\xrsim\capture\vis_*", "$data\xrsim\capture\bc_*" -ErrorAction SilentlyContinue | Remove-Item -Force
function Passes() {
    Cmd "bc vis" 1
    $l = Select-String -Path "$data\batman_city.log" -Pattern "detective passes (\d+)" | Select-Object -Last 1
    if ($l) { [int]$l.Matches[0].Groups[1].Value } else { 0 }
}
# Detective pass running? (the counter grows ~90/s). If not, the game is paused: Escape resumes.
function EnsureDetective() {
    for ($t = 0; $t -lt 4; $t++) {
        $a = Passes; Start-Sleep 1; $b = Passes
        if ($b -gt $a + 20) { return $true }
        "  detective pass stalled ($a -> $b): Escape"
        Key 0x1B 0x01; Start-Sleep 3
    }
    return $false
}
$pp = Join-Path $PSScriptRoot "is_paused.py"

if (Get-Process BatmanAC -ErrorAction SilentlyContinue) { "GAME ALREADY RUNNING - abort"; exit 1 }
Get-ChildItem "$data\xrsim\capture\vis_*", "$data\xrsim\capture\bc_*" -ErrorAction SilentlyContinue | Remove-Item -Force
Start-Process -FilePath "$g\Batman City VR (test).bat" -WorkingDirectory $g
Start-Sleep -Seconds 40
if (-not (Get-Process BatmanAC -ErrorAction SilentlyContinue)) { "game exited early"; exit 1 }
Key 0x0D 0x1C; Start-Sleep 5
Key 0x0D 0x1C; Start-Sleep 5
Key 0x0D 0x1C; Start-Sleep 60
Shot "a_plain"
for ($t = 0; $t -lt 3; $t++) {
    $st = & python $pp "$data\xrsim\capture\vis_a_plain_left.png"
    "after load: $st"
    if ($st -notlike "PAUSED*") { break }
    Key 0x1B 0x01; Start-Sleep 3
    Shot "a_plain"
}
Key 0x58 0x2D; Start-Sleep 3
Cmd "bc vis strength 0" 1
if (-not (EnsureDetective)) { "detective never ran"; }
Shot "c_det_base"
foreach ($i in 0..39) {
    Cmd "bc vis clear" 1
    Cmd "bc vis set $i 0" 1
    if (-not (EnsureDetective)) { "  [$i] detective not running - skipped"; continue }
    Shot ("d_det_" + $i)
    "  [$i] shot"
}
Cmd "bc vis clear" 1
Cmd "bc vis strength 1" 1
Get-Process BatmanAC -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
Start-Sleep 1
if (Get-Process BatmanAC -ErrorAction SilentlyContinue) { "STILL RUNNING" } else { "test game closed" }

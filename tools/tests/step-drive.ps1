# Step helper for a running Batman City test game (fake headset): optional bc command, optional key, then a shot.
# usage: step-drive.ps1 -Name x [-Vk 0x1B -Scan 0x01] [-Cmd "bc hudline 600"] [-Wait 2]
param([string]$Name = "s", [int]$Vk = 0, [int]$Scan = 0, [string]$Cmd = "", [double]$Wait = 1.5)
$data = "$env:LOCALAPPDATA\Batman_City"
$tools = "D:\Documents\Antigravity\VR_Modding\batman_city\tools"
$env:BVR_XRSIM_DIR = "$data\xrsim"
Add-Type @"
using System; using System.Runtime.InteropServices;
public class FocusS {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
}
"@
$p = Get-Process BatmanAC -ErrorAction SilentlyContinue
if (-not $p) { "GAME NOT RUNNING"; exit 1 }
if ($Cmd) { Set-Content "$data\command.txt" $Cmd -Encoding ascii; Start-Sleep 1 }
if ($Vk) {
    [void][FocusS]::ShowWindow($p.MainWindowHandle, 5)
    [void][FocusS]::SetForegroundWindow($p.MainWindowHandle)
    Start-Sleep -Milliseconds 300
    [FocusS]::keybd_event([byte]$Vk, [byte]$Scan, 0x8, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 120
    [FocusS]::keybd_event([byte]$Vk, [byte]$Scan, 0xA, [UIntPtr]::Zero)
}
Start-Sleep -Milliseconds ([int]($Wait * 1000))
try { & "$tools\xrsim-shot.ps1" -Out "$env:TEMP\bc_$Name" -Quiet -TimeoutSec 6 | Out-Null } catch { "shot failed: $_" }
Start-Sleep 1
Get-ChildItem "$data\xrsim\capture\bc_$Name*" -ErrorAction SilentlyContinue | ForEach-Object {
    $dst = Join-Path $_.DirectoryName ($_.Name -replace "^bc_", "vis_")
    if (Test-Path $dst) { Remove-Item $dst -Force }
    Rename-Item $_.FullName $dst }
"$Name done"

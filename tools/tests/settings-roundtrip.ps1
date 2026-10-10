$ErrorActionPreference = 'Continue'
$g = "E:\Games\Steam\steamapps\common\Batman Arkham City GOTY\Binaries\Win32"
$data = "$env:LOCALAPPDATA\Batman_City"
$env:XR_RUNTIME_JSON = "$data\xrsim\bvr_xrsim32.json"
$env:BVR_XRSIM_DIR = "$data\xrsim"
function Cmd([string]$line, [int]$after = 2) { Set-Content "$data\command.txt" $line -Encoding ascii; Start-Sleep -Seconds $after }
if (Get-Process BatmanAC -ErrorAction SilentlyContinue) { "GAME ALREADY RUNNING - abort"; exit 1 }
Remove-Item "$data\settings.ini" -ErrorAction SilentlyContinue
# run 1: change settings
Start-Process -FilePath "$g\Batman City VR (test).bat" -WorkingDirectory $g
Start-Sleep -Seconds 35
if (-not (Get-Process BatmanAC -ErrorAction SilentlyContinue)) { "game exited early"; exit 1 }
"run 1 log:"; Select-String -Path "$data\batman_city.log" -Pattern "settings" | ForEach-Object { $_.Line }
Cmd "bc fps 45" 1
Cmd "bc fps ssw" 1
Cmd "bc vis zoom 44" 1
Cmd "bc hudhead on" 3
"--- settings.ini after changes:"; Get-Content "$data\settings.ini"
Get-Process BatmanAC -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
Start-Sleep 3
# run 2: must come back
Start-Process -FilePath "$g\Batman City VR (test).bat" -WorkingDirectory $g
Start-Sleep -Seconds 35
"run 2 log:"; Select-String -Path "$data\batman_city.log" -Pattern "settings" | ForEach-Object { $_.Line }
Cmd "bc vis" 1
Cmd "bc capture" 3
Select-String -Path "$data\batman_city.log" -Pattern "zoom below" | Select-Object -Last 1 | ForEach-Object { $_.Line }
$cap = Get-ChildItem "$data\captures" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Get-Content "$($cap.FullName)\status.txt" | Select-String "fps lock|hudHead"
Get-Process BatmanAC -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
Start-Sleep 1
if (Get-Process BatmanAC -ErrorAction SilentlyContinue) { "STILL RUNNING" } else { "test game closed" }
Remove-Item "$data\settings.ini" -ErrorAction SilentlyContinue; "test settings file removed (defaults next launch)"

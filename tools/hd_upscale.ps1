# HD SCREENS (BATMAN_CITY_PLAN.md "HD SCREENS"): upscale the loading-screen pictures the mod saved with Magnific.
# In:  %LOCALAPPDATA%\Batman_City\hd_dump\<key>_<w>x<h>.png   (saved by the mod, screen_hd.cpp)
# Out: <game>\BatmanCityData\hd\<key>.png                       (loaded by the mod in place of the game's picture)
# The API key is read from the user environment variable MAGNIFIC_API_KEY and never printed or written anywhere.
# Magnific (docs.magnific.com): POST https://api.magnific.com/v1/ai/image-upscaler, header x-magnific-api-key,
# body { image: base64, scale_factor, optimized_for, creativity, resemblance, ... } -> task; poll until COMPLETED.
# Transparency: Magnific upscales the colour; the picture's own alpha is enlarged separately (bicubic) and put back.
# The pictures are the game's art (WB): the results stay on this PC, never in the public repository.
#
#   .\hd_upscale.ps1 -DryRun            list what would be sent and the rough cost, send nothing
#   .\hd_upscale.ps1 -Limit 1           upscale one picture (try the settings first)
#   .\hd_upscale.ps1                    upscale every picture that has no upscaled version yet
# MCP route (Magnific MCP server in .mcp.json, JJ's account, OAuth - no key): Claude uploads / upscales / downloads with
# the MCP tools, and this script does the local halves:
#   .\hd_upscale.ps1 -Prepare                       opaque copies to send: %TEMP%\bc_hd_upscale\<key>_send.png
#   .\hd_upscale.ps1 -Finish <key> -From <file>     the downloaded result + the original alpha -> BatmanCityData\hd\<key>.png
param(
    [switch]$DryRun,
    [switch]$Prepare,
    [string]$Finish = '',
    [string]$From = '',
    [int]$Limit = 0,
    [string]$Game = 'E:\Games\Steam\steamapps\common\Batman Arkham City GOTY\Binaries\Win32',
    [string]$OptimizedFor = 'videogame_assets',
    [int]$Creativity = 0,
    [int]$Resemblance = 3,
    [int]$MaxLongSide = 4096
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class HdPix {
    static byte[] Read(Bitmap b, out int stride) {
        var d = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        stride = d.Stride; var px = new byte[stride * b.Height]; Marshal.Copy(d.Scan0, px, 0, px.Length); b.UnlockBits(d); return px;
    }
    static void Write(Bitmap b, byte[] px) {
        var d = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        Marshal.Copy(px, 0, d.Scan0, px.Length); b.UnlockBits(d);
    }
    public static bool HasAlpha(Bitmap b) { int s; var px = Read(b, out s); for (int i = 3; i < px.Length; i += 4) if (px[i] < 250) return true; return false; }
    // colour only, fully opaque (what Magnific gets)
    public static Bitmap Opaque(Bitmap b) {
        var o = new Bitmap(b.Width, b.Height, PixelFormat.Format32bppArgb); int s; var px = Read(b, out s);
        for (int i = 3; i < px.Length; i += 4) px[i] = 255; Write(o, px); return o;
    }
    // the original alpha, enlarged to w x h, put onto the upscaled colour
    public static Bitmap WithAlpha(Bitmap up, Bitmap orig) {
        var a = new Bitmap(orig.Width, orig.Height, PixelFormat.Format32bppArgb); int s; var px = Read(orig, out s);
        for (int i = 0; i < px.Length; i += 4) { byte v = px[i + 3]; px[i] = v; px[i + 1] = v; px[i + 2] = v; px[i + 3] = 255; }
        Write(a, px);
        var big = new Bitmap(up.Width, up.Height, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(big)) { g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality; g.DrawImage(a, 0, 0, up.Width, up.Height); }
        var o = new Bitmap(up.Width, up.Height, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(o)) g.DrawImage(up, 0, 0, up.Width, up.Height);
        int s1, s2; var c = Read(o, out s1); var al = Read(big, out s2);
        for (int i = 0; i < c.Length; i += 4) c[i + 3] = al[i]; Write(o, c); a.Dispose(); big.Dispose(); return o;
    }
}
'@

$dump = Join-Path $env:LOCALAPPDATA 'Batman_City\hd_dump'
$out = Join-Path $Game 'BatmanCityData\hd'
if (-not (Test-Path $dump)) { Write-Host "No pictures saved yet ($dump). Play through a loading screen first."; exit 1 }
New-Item -ItemType Directory -Force $out | Out-Null
$tmp = Join-Path $env:TEMP 'bc_hd_upscale'
New-Item -ItemType Directory -Force $tmp | Out-Null
if ($Finish) {
    $src = Get-ChildItem $dump -Filter "$Finish`_*.png" | Select-Object -First 1
    if (-not $src -or -not (Test-Path $From)) { Write-Host "missing original or result for $Finish"; exit 1 }
    $orig = [System.Drawing.Bitmap]::FromFile($src.FullName); $up = [System.Drawing.Bitmap]::FromFile($From)
    $alpha = [HdPix]::HasAlpha($orig)
    $final = if ($alpha) { [HdPix]::WithAlpha($up, $orig) } else { $up }
    $dest = Join-Path $out "$Finish.png"; $final.Save($dest, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Host ("{0}: saved {1}x{2}{3}" -f $Finish, $final.Width, $final.Height, $(if ($alpha) { ' (transparency put back)' } else { '' }))
    if ($final -ne $up) { $final.Dispose() }; $up.Dispose(); $orig.Dispose(); exit 0
}
if ($Prepare) {
    Get-ChildItem $dump -Filter '*.png' | Where-Object { $_.BaseName -match '^([0-9a-f]{8})_' -and -not (Test-Path (Join-Path $out "$($Matches[1]).png")) } | ForEach-Object {
        $k = $_.BaseName.Substring(0, 8); $b = [System.Drawing.Bitmap]::FromFile($_.FullName)
        $o = [HdPix]::Opaque($b); $f = Join-Path $tmp "$k`_send.png"; $o.Save($f, [System.Drawing.Imaging.ImageFormat]::Png)
        Write-Host ("{0} {1}x{2} alpha={3} -> {4}" -f $k, $b.Width, $b.Height, [HdPix]::HasAlpha($b), $f); $o.Dispose(); $b.Dispose()
    }
    exit 0
}
$key = [Environment]::GetEnvironmentVariable('MAGNIFIC_API_KEY', 'User')
if (-not $key) { $key = $env:MAGNIFIC_API_KEY }
if (-not $DryRun -and -not $key) { Write-Host 'MAGNIFIC_API_KEY is not set (see the instructions).'; exit 1 }

$todo = @(Get-ChildItem $dump -Filter '*.png' | Where-Object { $_.BaseName -match '^([0-9a-f]{8})_(\d+)x(\d+)$' } | ForEach-Object {
    $k = $Matches[1]; $w = [int]$Matches[2]; $h = [int]$Matches[3]
    $long = [Math]::Max($w, $h)
    $f = 2; foreach ($c in 4, 8) { if ($long * $c -le $MaxLongSide) { $f = $c } }
    [pscustomobject]@{ Key = $k; File = $_.FullName; W = $w; H = $h; Factor = $f; Done = Test-Path (Join-Path $out "$k.png") }
} | Where-Object { -not $_.Done })
if ($Limit -gt 0) { $todo = @($todo | Select-Object -First $Limit) }
# rough cost from Magnific's table (about EUR 0.03 per output megapixel, at least EUR 0.10 each)
$cost = 0.0
foreach ($t in $todo) { $mp = $t.W * $t.H * $t.Factor * $t.Factor / 1e6; $cost += [Math]::Max(0.10, 0.03 * $mp) }
Write-Host ("{0} picture(s) to upscale, roughly EUR {1:N2}" -f $todo.Count, $cost)
foreach ($t in $todo) { Write-Host ("  {0}  {1}x{2} -> {3}x{4} ({5}x)" -f $t.Key, $t.W, $t.H, ($t.W * $t.Factor), ($t.H * $t.Factor), $t.Factor) }
if ($DryRun -or $todo.Count -eq 0) { exit 0 }

$hdr = @{ 'x-magnific-api-key' = $key; 'Content-Type' = 'application/json' }
$api = 'https://api.magnific.com/v1/ai/image-upscaler'
foreach ($t in $todo) {
    $orig = [System.Drawing.Bitmap]::FromFile($t.File)
    $alpha = [HdPix]::HasAlpha($orig)
    $send = Join-Path $tmp "$($t.Key)_send.png"
    $op = [HdPix]::Opaque($orig); $op.Save($send, [System.Drawing.Imaging.ImageFormat]::Png); $op.Dispose()
    $body = @{ image = [Convert]::ToBase64String([IO.File]::ReadAllBytes($send)); scale_factor = "$($t.Factor)x";
               optimized_for = $OptimizedFor; creativity = $Creativity; resemblance = $Resemblance } | ConvertTo-Json
    try {
        $r = Invoke-RestMethod -Method Post -Uri $api -Headers $hdr -Body $body
    } catch { Write-Host "  $($t.Key): request refused - $($_.Exception.Message)"; $orig.Dispose(); continue }
    $d = if ($r.data) { $r.data } else { $r }
    $id = $d.task_id
    Write-Host "  $($t.Key): sent ($($t.Factor)x), task $id"
    $url = $null
    for ($i = 0; $i -lt 120 -and -not $url; ++$i) {
        Start-Sleep -Seconds 5
        $p = Invoke-RestMethod -Method Get -Uri "$api/$id" -Headers $hdr
        $pd = if ($p.data) { $p.data } else { $p }
        if ($pd.status -eq 'COMPLETED' -and $pd.generated) { $url = @($pd.generated)[0] }
        elseif ($pd.status -eq 'FAILED') { break }
    }
    if (-not $url) { Write-Host "  $($t.Key): no result"; $orig.Dispose(); continue }
    $got = Join-Path $tmp "$($t.Key)_up.img"
    Invoke-WebRequest -Uri $url -OutFile $got
    $up = [System.Drawing.Bitmap]::FromFile($got)
    $final = if ($alpha) { [HdPix]::WithAlpha($up, $orig) } else { $up }
    $dest = Join-Path $out "$($t.Key).png"
    $final.Save($dest, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Host ("  {0}: saved {1}x{2}{3}" -f $t.Key, $final.Width, $final.Height, $(if ($alpha) { ' (transparency put back)' } else { '' }))
    if ($final -ne $up) { $final.Dispose() }
    $up.Dispose(); $orig.Dispose()
}
Write-Host "Done. The mod loads them the next time each picture is shown."

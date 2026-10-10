# Batman City VR release package (PACKAGING_GUIDE.md): builds dist/BatmanCityVR-ArkhamCity.zip from one source of truth
# per file. The mod DLLs, DLSS helper and HD pictures are taken from the game folder = the build JJ tested; the tested
# VR settings from his live settings.ini. Zip made with zipfile: forward slashes, no top folder (Windows' "Extract All"
# rejects backslash entries). JJ 2026-10-10: the HD pictures (WB art) and NVIDIA's DLSS library ship in the release zip
# only - never committed to the repository.
#   python tools/make_package.py [--game "<...\Binaries\Win32>"]
import argparse, hashlib, os, shutil, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ap = argparse.ArgumentParser()
ap.add_argument('--game', default=r'E:\Games\Steam\steamapps\common\Batman Arkham City GOTY\Binaries\Win32')
ap.add_argument('--settings', default=os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Batman_City', 'settings.ini'))
a = ap.parse_args()

dist = os.path.join(ROOT, 'dist')
stage = os.path.join(dist, 'BatmanCityVR')
zpath = os.path.join(dist, 'BatmanCityVR-ArkhamCity.zip')
if os.path.isdir(stage): shutil.rmtree(stage)
os.makedirs(os.path.join(stage, 'files'))

def put(src, rel):
    dst = os.path.join(stage, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.isdir(src): shutil.copytree(src, dst)
    else: shutil.copy2(src, dst)

inst = os.path.join(ROOT, 'install')
for f in ('Install-BatmanCityVR.bat', 'Install-BatmanCityVR.ps1', 'Uninstall-BatmanCityVR.bat', 'Uninstall-BatmanCityVR.ps1',
          'Batman City VR.bat', 'Batman City VR (SteamVR).bat'):
    put(os.path.join(inst, f), f)
for f in ('xinput1_3.dll', 'batman_city.dll'):
    put(os.path.join(a.game, f), f)
put(os.path.join(a.game, 'BatmanCityDLSS'), os.path.join('files', 'BatmanCityDLSS'))
put(os.path.join(a.game, 'BatmanCityData'), os.path.join('files', 'BatmanCityData'))
put(a.settings, os.path.join('files', 'settings.ini'))
for f in ('README.md', 'LICENSE-bioshock-vr.txt', 'LICENSE-luma-framework.md', 'THIRD_PARTY_NOTICES.md'):
    put(os.path.join(ROOT, f), f)

# checks: batch files CRLF, the proxy is ours, the build in the game folder is the one in build/ (or say so)
bad = []
for dp, _, fs in os.walk(stage):
    for f in fs:
        if f.lower().endswith('.bat'):
            b = open(os.path.join(dp, f), 'rb').read()
            if b.count(b'\n') != b.count(b'\r\n'): bad.append(f + ': not CRLF')
if b'[batman_city-proxy]' not in open(os.path.join(stage, 'xinput1_3.dll'), 'rb').read(): bad.append('xinput1_3.dll is not ours')
built = os.path.join(ROOT, 'build', 'src', 'RelWithDebInfo', 'batman_city.dll')
h = lambda p: hashlib.sha256(open(p, 'rb').read()).hexdigest()
if os.path.exists(built) and h(built) != h(os.path.join(stage, 'batman_city.dll')):
    print('NOTE: build/ has a newer batman_city.dll than the game folder; the package takes the game folder\'s (tested) one')
if bad:
    print('REFUSED:', *bad, sep='\n  '); sys.exit(1)

if os.path.exists(zpath): os.remove(zpath)
with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for dp, _, fs in os.walk(stage):
        for f in sorted(fs):
            full = os.path.join(dp, f)
            arc = os.path.relpath(full, stage).replace(os.sep, '/')
            # pictures and the DLSS library hardly compress: store them
            z.write(full, arc, zipfile.ZIP_STORED if f.lower().endswith(('.png', '.jpg', '.rgba', '.dll', '.exe')) else zipfile.ZIP_DEFLATED)
with zipfile.ZipFile(zpath) as z:
    assert z.testzip() is None
    names = z.namelist()
    assert not any('\\' in n for n in names), 'backslash in an entry name'
print(f'{zpath}: {len(names)} files, {os.path.getsize(zpath) / 1e6:.1f} MB, sha256 {h(zpath)[:16]}')

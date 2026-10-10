"""RETICLEHD (JJ 2026-10-09: "recreate the grappling hook reticle in high resolution; the existing one is low
resolution and poorly compressed").

Input: the grapple reticle's pictures the mod saved from the game (%LOCALAPPDATA%\\Batman_City\\reticle\\piece_*.bmp,
decoded to RGBA8 in-game). Output: <fnv1a32 of the RGBA8 pixels>.rgba files (uint32 width, uint32 height, RGBA8 rows
top-down) that the mod swaps in when it sees a texture with the same pixels.

Hard-edged pieces (rings, arcs, arrow) are enlarged 8x and their edges rebuilt crisp: each channel is normalised to
its own min..max and passed through a smoothstep around the midpoint, so a two-tone edge becomes a clean anti-aliased
edge about 3 output pixels wide. Soft pieces (shadows, glows, gradients) are only enlarged smoothly.
"""
import glob, os, struct, sys
import numpy as np
from PIL import Image

SRC = os.path.expandvars(r"%LOCALAPPDATA%\Batman_City\reticle")
DST = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham City GOTY\Binaries\Win32\BatmanCityData\reticle"
PREVIEW = sys.argv[2] if len(sys.argv) > 2 else None


def load_bmp(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    im = Image.frombytes("RGBA", (w, abs(h)), d[off:off + w * abs(h) * 4], "raw", "BGRA")
    return im.transpose(Image.FLIP_TOP_BOTTOM) if h > 0 else im


def fnv1a(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def crisp(ch):
    lo, hi = float(ch.min()), float(ch.max())
    if hi - lo < 24:  # flat channel: nothing to sharpen
        return ch
    t = (ch - lo) / (hi - lo)
    t = np.clip((t - 0.32) / 0.36, 0, 1)
    t = t * t * (3 - 2 * t)
    return lo + t * (hi - lo)


def ring_fit(a):
    """A ring (annulus) centred in the picture? Returns (bg rgba, fg rgba, r_in, r_out) in source pixels, or None."""
    h, w = a.shape[:2]
    if w != h or w < 12:
        return None
    f = a.astype(np.float32)
    bg = f[0, 0]
    dist = np.sqrt(((f - bg) ** 2).sum(-1))
    if dist.max() < 40:
        return None
    m = dist / dist.max()
    yy, xx = np.mgrid[0:h, 0:w]
    r = np.hypot(xx + 0.5 - w / 2, yy + 0.5 - h / 2)
    fgmask = m > 0.5
    if fgmask.sum() < 8:
        return None
    rin, rout = np.percentile(r[fgmask], 2) , np.percentile(r[fgmask], 98)
    ann = (r >= rin) & (r <= rout)
    iou = (ann & fgmask).sum() / max(1, (ann | fgmask).sum())
    if iou < 0.8 or rin < 1:
        return None
    fg = f[m > 0.8].mean(0)
    # the coverage of the soft source edge sits about half a pixel inside the percentile extremes
    return bg, fg, rin - 0.25, rout + 0.25


def radial_ring(a, k):
    h, w = a.shape[:2]
    f = a.astype(np.float64)
    yy, xx = np.mgrid[0:h, 0:w]
    r = np.hypot(xx + 0.5 - w / 2, yy + 0.5 - h / 2)
    step = 0.25
    nb = int(r.max() / step) + 2
    idx = np.minimum((r / step).astype(int), nb - 1)
    prof = np.zeros((nb, 4))
    cnt = np.zeros(nb)
    # Each pixel spreads over the bins within +-0.5 px of its radius (a smoother, fuller profile).
    for o in np.arange(-2, 3):
        j = np.clip(idx + o, 0, nb - 1)
        np.add.at(prof, j.ravel(), f.reshape(-1, 4))
        np.add.at(cnt, j.ravel(), 1)
    good = cnt > 0
    prof[good] /= cnt[good, None]
    for c in range(4):  # fill empty bins by interpolation
        prof[~good, c] = np.interp(np.flatnonzero(~good), np.flatnonzero(good), prof[good, c])
    n = w * k
    yo, xo = np.mgrid[0:n, 0:n]
    ro = np.hypot((xo + 0.5) / k - w / 2, (yo + 0.5) / k - h / 2) / step
    i0 = np.clip(np.floor(ro).astype(int), 0, nb - 1)
    i1 = np.clip(i0 + 1, 0, nb - 1)
    t = (ro - np.floor(ro))[..., None]
    out = prof[i0] * (1 - t) + prof[i1] * t
    return Image.fromarray(np.clip(out + 0.5, 0, 255).astype(np.uint8), "RGBA")


def draw_ring(size, k, bg, fg, rin, rout):
    n = size * k
    yy, xx = np.mgrid[0:n, 0:n]
    r = np.hypot(xx + 0.5 - n / 2, yy + 0.5 - n / 2) / k  # in source pixels
    aa = 0.6  # edge width in source pixels (~5 output pixels): clean, not razor sharp
    cov = np.clip((r - rin) / aa + 0.5, 0, 1) * np.clip((rout - r) / aa + 0.5, 0, 1)
    out = bg[None, None, :] + cov[..., None] * (fg - bg)[None, None, :]
    return Image.fromarray(np.clip(out + 0.5, 0, 255).astype(np.uint8), "RGBA")


# --- The two arc pieces (64x32 BC1), redrawn from geometry (JJ: "perfect arcs, not weathered-looking lines") --------
# Measured from the decoded pictures (line centres per column, mirrored halves averaged), source pixel units, y down:
#   upper arc: circle centre (32, 51.20) radius 38.66, from x 3.3 to 60.7 (fit rms 0.15 px)
#   lower arc: circle centre (32, 49.40) radius 30.60, between its two ends at x 20.6 / 43.4 (fit rms 0.05 px)
#   connectors: straight from each lower-arc end to the upper arc at x 13.8 / 50.2 (the notches)
#   ticks (with-ticks piece only): radial, r + 2.6 .. r + 6.0 on the upper arc, at x 20, 26, 32, 38, 44, 50, 56
ARC_KEYS = {0x02323DAC: True, 0xBA1AC3BB: False}  # fingerprint -> has ticks
LINE_RGB = np.array([216.0, 220.0, 216.0])
LINE_W = 1.25  # source pixels


def arcs_piece(w, h, k, ticks):
    cU, rU = np.array([32.0, 51.20]), 38.66
    cL, rL = np.array([32.0, 49.40]), 30.60
    on_circle = lambda c, r, x: np.array([x, c[1] - np.sqrt(r * r - (x - c[0]) ** 2)])
    n = (w * k, h * k)
    yy, xx = np.mgrid[0:n[1], 0:n[0]]
    p = np.stack([(xx + 0.5) / k, (yy + 0.5) / k], -1)
    d = np.full(p.shape[:2], 1e9)

    def seg(a, b):
        ab = b - a
        t = np.clip(((p - a) @ ab) / (ab @ ab), 0, 1)
        return np.linalg.norm(p - (a + t[..., None] * ab), axis=-1)

    def arc(c, r, x0, x1):
        e0, e1 = on_circle(c, r, x0), on_circle(c, r, x1)
        v = p - c
        ang = np.arctan2(v[..., 1], v[..., 0])
        a0, a1 = sorted((np.arctan2(*(e0 - c)[::-1]), np.arctan2(*(e1 - c)[::-1])))
        inside = (ang >= a0) & (ang <= a1)
        ring = np.abs(np.linalg.norm(v, axis=-1) - r)
        ends = np.minimum(np.linalg.norm(p - e0, axis=-1), np.linalg.norm(p - e1, axis=-1))
        return np.where(inside, ring, ends)

    d = np.minimum(d, arc(cU, rU, 3.3, 60.7))
    d = np.minimum(d, arc(cL, rL, 20.6, 43.4))
    for xl, xu in ((20.6, 13.8), (43.4, 50.2)):
        d = np.minimum(d, seg(on_circle(cL, rL, xl), on_circle(cU, rU, xu)))
    if ticks:
        for x in (20, 26, 32, 38, 44, 50, 56):
            q = on_circle(cU, rU, x)
            u = (q - cU) / rU
            d = np.minimum(d, seg(cU + u * (rU + 2.6), cU + u * (rU + 6.0)))
    # The faint dark-teal fill of the band between the arcs (measured (20, 28, 27)): inside the upper circle, outside
    # the lower one, and between the two connectors.
    sd = np.minimum(rU - np.linalg.norm(p - cU, axis=-1), np.linalg.norm(p - cL, axis=-1) - rL)
    for xl, xu, s in ((20.6, 13.8, 1.0), (43.4, 50.2, -1.0)):
        a, b = on_circle(cL, rL, xl), on_circle(cU, rU, xu)
        nrm = np.array([b[1] - a[1], -(b[0] - a[0])]) * s
        nrm /= np.linalg.norm(nrm)
        side = (p - a) @ nrm
        sd = np.minimum(sd, side if (np.array([32.0, 16.0]) - a) @ nrm > 0 else -side)
    fill = np.clip(sd * k + 0.5, 0, 1)[..., None] * np.array([20.0, 28.0, 27.0])
    cov = np.clip((LINE_W / 2 - d) * k + 0.5, 0, 1)
    rgb = fill * (1 - cov[..., None]) + cov[..., None] * LINE_RGB
    out = np.concatenate([rgb, np.full(cov.shape + (1,), 255.0)], -1)
    return Image.fromarray(np.clip(out + 0.5, 0, 255).astype(np.uint8), "RGBA")


def enlarge(im, hard):
    key = fnv1a(im.tobytes())
    if key in ARC_KEYS:
        w, h = im.size
        return arcs_piece(w, h, max(1, min(8, 512 // max(w, h))), ARC_KEYS[key]), "geometry"
    w, h = im.size
    k = max(1, min(8, 512 // max(w, h)))
    a = np.asarray(im)
    fit = ring_fit(a) if hard and a[..., 3].min() >= 250 else None  # rings are the solid (no transparency) pieces
    if fit:
        # JJ 2026-10-09: the crisp redraw turned the soft teal glow ring into a hard "green band". Keep the original's
        # softness: average its colour by distance from the centre (radial profile), then draw a perfectly round ring
        # with exactly that profile.
        return radial_ring(a, k), "round"
    big = np.asarray(im.resize((w * k, h * k), Image.BICUBIC), dtype=np.float32)
    if a[..., 3].min() < 250:
        # Has transparency: keep the original soft halo, sharpen only the solid core (alpha = max of the two).
        al = big[..., 3]
        big[..., 3] = np.maximum(al, crisp(al))
        return Image.fromarray(np.clip(big + 0.5, 0, 255).astype(np.uint8), "RGBA"), "core"
    if hard:
        # Soften the compression wobble a little before rebuilding the edges.
        sm = np.asarray(Image.fromarray(np.clip(big, 0, 255).astype(np.uint8), "RGBA").filter(
            __import__("PIL.ImageFilter", fromlist=["GaussianBlur"]).GaussianBlur(k * 0.35)), dtype=np.float32)
        big = np.stack([crisp(sm[..., c]) for c in range(4)], axis=-1)
        return Image.fromarray(np.clip(big + 0.5, 0, 255).astype(np.uint8), "RGBA"), "crisp"
    return Image.fromarray(np.clip(big + 0.5, 0, 255).astype(np.uint8), "RGBA"), "smooth"


os.makedirs(DST, exist_ok=True)
rows = []
for path in sorted(glob.glob(os.path.join(SRC, "piece_*.bmp"))):
    im = load_bmp(path)
    w, h = im.size
    if w * h <= 16 or h == 1 or w == 1:  # solid fills / 1-pixel ramps: shared by other HUD parts, nothing to gain
        print("skip", os.path.basename(path))
        continue
    a = np.asarray(im)
    hard = w >= 16 and h >= 16
    out, how = enlarge(im, hard)
    key = fnv1a(im.tobytes())
    with open(os.path.join(DST, "%08x.rgba" % key), "wb") as f:
        f.write(struct.pack("<II", *out.size))
        f.write(out.tobytes())
    print("%s %dx%d -> %dx%d %s key %08x" % (os.path.basename(path), w, h, out.size[0], out.size[1],
                                             how, key))
    rows.append((im, out))

if PREVIEW and rows:
    cell = 256
    sheet = Image.new("RGB", (cell * 2 + 10, (cell + 10) * len(rows)), (40, 40, 60))
    for i, (im, out) in enumerate(rows):
        for j, src in enumerate((im.resize(out.size, Image.BILINEAR), out)):
            s = src.copy()
            s.thumbnail((cell, cell))
            bg = Image.new("RGBA", s.size, (30, 30, 70, 255))
            bg.alpha_composite(s)
            sheet.paste(bg.convert("RGB"), (j * (cell + 10), i * (cell + 10)))
    sheet.save(PREVIEW)
    print("preview", PREVIEW)

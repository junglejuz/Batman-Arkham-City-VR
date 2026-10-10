# HD LOGO (BATMAN_CITY_PLAN.md "HD SCREENS"): rebuild the opening-logo video bac_logo.usm with a new (larger) MPEG-1
# picture stream, keeping CRI's container, the audio and the chunk timing. Layout read from the original (2026-10-10):
#   CRID (CRIUSF_DIR_STREAM: filesize / minbuf / avbps per stream), @SFV VIDEO_HDRINFO (width, height, mat_*, disp_*,
#   total_frames, ixsize), @SFV VIDEO_SEEKINFO (ofs_byte / ofs_frmid per keyframe), then one @SFV data chunk per frame
#   (payload = 64 zero bytes + that frame's MPEG-1 data, the GOP's sequence header on keyframes), audio chunks between.
#   python usm_rebuild.py <original.usm> <new.m1v> <out.usm>     (new.m1v: same frame count, I/P only, GOP = original)
#   python usm_rebuild.py <original.usm> --selftest              (rebuild with the original frames; must be identical)
import struct, sys

SIZES = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 8, 7: 8, 8: 4, 0xA: 4, 0xB: 8}
FMTS = {0: '>B', 1: '>b', 2: '>H', 3: '>h', 4: '>I', 5: '>i', 6: '>Q', 7: '>q', 8: '>f', 0xA: '>I', 0xB: '>II'}


def utf_fields(b, at):
    """@UTF table at absolute offset `at` in b -> list of rows {name: (value, abs_offset, type)}; constants too."""
    assert b[at:at + 4] == b'@UTF'
    t0 = at + 8
    _, rows_off, str_off, _, _, ncol, rowlen, nrows = struct.unpack('>HHIIIHHI', b[t0:t0 + 24])
    def cstr(o):
        s = t0 + str_off + o
        return b[s:b.index(b'\0', s)].decode()
    p = t0 + 24
    cols = []
    for _ in range(ncol):
        fl = b[p]; nm = cstr(struct.unpack('>I', b[p + 1:p + 5])[0]); p += 5
        st, ty = fl >> 4, fl & 0xF
        cpos = None
        if st == 3:
            cpos = p; p += SIZES[ty]
        cols.append((nm, ty, st, cpos))
    rows = []
    for r in range(nrows):
        q = t0 + rows_off + r * rowlen
        row = {}
        for nm, ty, st, cpos in cols:
            pos = q if st == 5 else cpos
            if st == 5: q += SIZES[ty]
            if pos is None: continue
            v = struct.unpack(FMTS[ty], b[pos:pos + SIZES[ty]])
            row[nm] = (v[0] if ty not in (0xA, 0xB) else v, pos, ty)
        rows.append(row)
    return rows


def put(buf, field, value):
    _, pos, ty = field
    buf[pos:pos + SIZES[ty]] = struct.pack(FMTS[ty], value)


def chunks(b):
    i = 0
    while i + 0x20 <= len(b):
        size = struct.unpack('>I', b[i + 4:i + 8])[0]
        yield i, b[i:i + 4], size, b[i + 9], struct.unpack('>H', b[i + 10:i + 12])[0], b[i + 15] & 3
        i += 8 + size


def split_frames(es):
    """MPEG-1 elementary stream -> one bytes object per picture (sequence / GOP headers go with the next picture)."""
    starts, k = [], 0
    pics = []
    while True:
        k = es.find(b'\x00\x00\x01', k)
        if k < 0 or k + 3 >= len(es): break
        code = es[k + 3]
        if code == 0x00: pics.append(k)
        elif code in (0xB3, 0xB8) and (not starts or starts[-1] < (pics[-1] if pics else -1) or not pics):
            pass
        k += 3
    # frame boundaries: each picture starts at the earliest header after the previous picture
    out, prev_end = [], 0
    bounds = []
    for n, p in enumerate(pics):
        s = p
        h = es.rfind(b'\x00\x00\x01\xb3', prev_end, p)
        if h >= 0: s = h
        bounds.append(s)
        prev_end = p + 4
    bounds.append(len(es))
    if bounds[0] != 0: bounds[0] = 0  # anything before the first picture belongs to it
    return [es[bounds[i]:bounds[i + 1]] for i in range(len(pics))]


def rebuild(orig, frames):
    b = bytes(orig)
    allc = list(chunks(b))
    vdata = [c for c in allc if c[1] == b'@SFV' and c[5] == 0]
    assert len(vdata) == len(frames), f'{len(vdata)} video chunks in the original, {len(frames)} new frames'
    out = bytearray()
    new_off = []  # file offset of each new video chunk
    vi = 0
    vbytes = 0
    vmax = 0
    for (i, sig, size, off, pad, dt) in allc:
        if sig == b'@SFV' and dt == 0:
            hdr = bytearray(b[i:i + 8 + off])
            payload = b'\0' * 64 + frames[vi]
            body = len(hdr) - 8 + len(payload)
            npad = (-(8 + body)) % 32 + 64  # the original pads every frame chunk to 32 bytes plus 64
            hdr[4:8] = struct.pack('>I', body + npad)
            hdr[10:12] = struct.pack('>H', npad)
            new_off.append(len(out))
            chunk = bytes(hdr) + payload + b'\0' * npad
            out += chunk
            vbytes += len(frames[vi])           # CRID video filesize = the frames' MPEG-1 bytes (no prefix / headers)
            vmax = max(vmax, len(frames[vi]))   # ixsize / video minbuf = the largest frame
            vi += 1
        else:
            out += b[i:i + 8 + size]
    # header fields (header chunks are copied byte for byte, so their offsets are unchanged)
    crid = utf_fields(out, allc[0][0] + 8 + allc[0][3])
    hdr_c = next(c for c in allc if c[1] == b'@SFV' and c[5] == 1)
    seek_c = next(c for c in allc if c[1] == b'@SFV' and c[5] == 3)
    vh = utf_fields(out, hdr_c[0] + 8 + hdr_c[3])[0]
    seek = utf_fields(out, seek_c[0] + 8 + seek_c[3])
    return out, crid, vh, seek, new_off, vbytes, vmax


def main():
    orig = open(sys.argv[1], 'rb').read()
    allc = list(chunks(orig))
    vframes = [orig[i + 8 + off + 64:i + 8 + size - pad] for (i, sig, size, off, pad, dt) in allc if sig == b'@SFV' and dt == 0]
    selftest = len(sys.argv) > 2 and sys.argv[2] == '--selftest'
    frames = vframes if selftest else split_frames(open(sys.argv[2], 'rb').read())
    width = height = None
    if not selftest:
        sh = frames[0].find(b'\x00\x00\x01\xb3')
        v = int.from_bytes(frames[0][sh + 4:sh + 7], 'big')
        width, height = v >> 12, v & 0xFFF
        print(f'new stream: {len(frames)} frames, {width}x{height}')
    out, crid, vh, seek, new_off, vbytes, vmax = rebuild(orig, frames)
    # recompute the fields the same way for both runs; the self-test proves the formulas against the original's values
    calc = {
        'usm filesize': len(out),
        'seek ofs_byte': [new_off[r['ofs_frmid'][0]] for r in seek],
    }
    if selftest:
        ok = bytes(out) == orig
        print('byte-identical rebuild:', ok)
        print('usm filesize', crid[0]['filesize'][0], 'calc', calc['usm filesize'])
        print('seek offsets match:', [r['ofs_byte'][0] for r in seek] == calc['seek ofs_byte'])
        print('video stream filesize', crid[1]['filesize'][0], 'sum of video chunks', vbytes, 'largest chunk', vmax,
              'ixsize', vh['ixsize'][0], 'minbuf', crid[1]['minbuf'][0])
        return
    put(out, crid[0]['filesize'], len(out))
    for r, o in zip(seek, calc['seek ofs_byte']): put(out, r['ofs_byte'], o)
    for k in ('width', 'mat_width', 'disp_width'): put(out, vh[k], width)
    for k in ('height', 'mat_height', 'disp_height'): put(out, vh[k], height)
    # relations measured on the original: video avbps = video bytes * 8 * fps / frames; container avbps = video + audio;
    # container minbuf = video minbuf + audio minbuf + 18992 (kept as the same margin)
    frames_n, fps = len(frames), vh['framerate_n'][0] / vh['framerate_d'][0]
    margin = crid[0]['minbuf'][0] - crid[1]['minbuf'][0] - crid[2]['minbuf'][0]
    vavbps = int(vbytes * 8 * fps / frames_n)
    put(out, crid[1]['filesize'], vbytes)
    put(out, vh['ixsize'], vmax)
    put(out, crid[1]['minbuf'], vmax)
    put(out, crid[0]['minbuf'], vmax + crid[2]['minbuf'][0] + margin)
    put(out, crid[1]['avbps'], vavbps)
    put(out, crid[0]['avbps'], vavbps + crid[2]['avbps'][0])
    open(sys.argv[3], 'wb').write(out)
    print(f'wrote {sys.argv[3]}: {len(out)} bytes, largest video chunk {vmax}')


if __name__ == '__main__':
    main()

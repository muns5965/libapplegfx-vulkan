#!/usr/bin/env python3
# libapplegfx-vulkan (MIT) -- offline, READ-ONLY analysis of RGBA16Float (VK_FORMAT_R16G16B16A16_SFLOAT = 97) surface data in a G7 GPU dump directory.
# tools/f1_fp16_surface_analysis.py
#
# Usage:  python3 -I tools/f1_fp16_surface_analysis.py <dump_dir> [--out <dir>] [--png-compare <png> ...]
#   <dump_dir>  a directory holding index.txt + cap_*.bin (LAGFX_ARM64_DUMP_DATA capture, see src/protocol/arm64_capture.h)
# Reads only. Writes (to --out, default: nothing) reconstructed PNGs. Needs numpy; no PIL (own PNG writer/reader via zlib, 8-bit RGB / RGBA, non-interlaced).
#
# What it does (and does NOT do):
#  * lists every cap_img whose vkfmt is 97 (RGBA16Float, 8 bytes per pixel) and checks row pitch == w*8 and blob length vs trunc flag;
#  * decodes the blob as little-endian binary16 with numpy and reports per channel: +-0, subnormal, normal finite, NaN, +-Inf counts, min / max / mean of the finite values,
#    counts of negative and >1.0 values; alpha separately; only the rows that are really present in a truncated blob (len // row) are used;
#  * writes a reconstruction (display-policy of Henge's pvg_half.h mode 0: clamp to [0,1], *255, +0.5, NaN/neg -> 0, alpha forced 255) -- ONLY a visualisation aid;
#  * compares two snapshots bit-exactly (16-bit patterns) and reports the first differing pixel;
#  * with --png-compare: converts each fp16 snapshot with the same policy and compares it, over the rows present, with an 8-bit RGB PNG (e.g. Henge's g7_frame_*.png).
# It never decides "correct": it only states what the bytes are. A snapshot is a copy of the Vulkan image (stage A) taken by the library's capture; it says nothing about guest memory or Android.
import sys, os, re, zlib, struct, hashlib, argparse
import numpy as np


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def parse_index(path):
    """last cap_header section only (ids restart after each header)"""
    blobs, imgs, atts, passes = {}, [], [], {}
    for line in open(path, 'r', errors='replace'):
        if line.startswith('cap_header'):
            blobs, imgs, atts, passes = {}, [], [], {}
            continue
        if not line.startswith('cap_'):
            continue
        tok = line.split()
        kind = tok[0]
        kv = dict(t.split('=', 1) for t in tok[1:] if '=' in t)
        if kind == 'cap_blob':
            blobs[int(kv['id'])] = kv
        elif kind == 'cap_img':
            imgs.append(kv)
        elif kind == 'cap_att':
            atts.append(kv)
        elif kind == 'cap_pass':
            passes[int(kv['id'])] = kv
    return blobs, imgs, atts, passes


def load_fp16(dump, blobs, img):
    """-> (uint16 array [rows, w, 4], rows_present, note) or None"""
    bid = int(img['blob'])
    if bid == 0 or bid not in blobs:
        return None
    w, h, bpp, row = int(img['w']), int(img['h']), int(img['bpp']), int(img['row'])
    note = []
    if bpp != 8 or row != w * 8:
        note.append('PITCH-MISMATCH bpp=%d row=%d w*8=%d' % (bpp, row, w * 8))
    raw = open(os.path.join(dump, blobs[bid]['file']), 'rb').read()
    if len(raw) != int(blobs[bid]['bytes']):
        note.append('FILE-SIZE-MISMATCH %d vs index %s' % (len(raw), blobs[bid]['bytes']))
    rows = len(raw) // row
    part = len(raw) - rows * row
    if int(blobs[bid]['trunc']):
        note.append('TRUNCATED %s of %s bytes -> %d full rows of %d (+%d partial bytes ignored)' % (blobs[bid]['bytes'], blobs[bid]['full'], rows, h, part))
    a = np.frombuffer(raw[:rows * row], dtype='<u2').reshape(rows, w, 4)
    return a, rows, '; '.join(note)


def classify(u16):
    e = (u16 >> 10) & 0x1f
    m = u16 & 0x3ff
    return {'zero': int(((e == 0) & (m == 0)).sum()), 'subnormal': int(((e == 0) & (m != 0)).sum()),
            'normal': int(((e != 0) & (e != 31)).sum()), 'inf': int(((e == 31) & (m == 0)).sum()), 'nan': int(((e == 31) & (m != 0)).sum())}


def channel_report(a):
    f = a.view('<f2').astype(np.float32)
    out = []
    for c, name in enumerate('RGBA'):
        u = a[..., c]
        k = classify(u)
        v = f[..., c]
        fin = np.isfinite(v)
        vv = v[fin]
        out.append('  %s: %s | finite min=%.6g max=%.6g mean=%.6g | neg=%d gt1=%d eq1=%d' % (
            name, ' '.join('%s=%d' % kv for kv in k.items()),
            vv.min() if vv.size else float('nan'), vv.max() if vv.size else float('nan'), vv.mean() if vv.size else float('nan'),
            int((vv < 0).sum()), int((vv > 1).sum()), int((vv == 1).sum())))
    return '\n'.join(out)


def to_rgb8(a):
    """Henge pvg_half.h mode 0: !(v>0)->0, v>=1->255, else (uint8)(v*255+0.5). NaN -> 0. Alpha not used."""
    f = a[..., :3].view('<f2').astype(np.float32)
    with np.errstate(invalid='ignore'):
        r = np.where(~(f > 0), 0, np.where(f >= 1, 255, np.floor(f * 255.0 + 0.5)))
    return r.astype(np.uint8)


def png_write(path, rgb):
    h, w, _ = rgb.shape
    raw = b''.join(b'\x00' + rgb[y].tobytes() for y in range(h))
    def ch(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))


def png_read_rgb(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    p, idat, ihdr = 8, b'', None
    while p < len(d):
        n, = struct.unpack('>I', d[p:p + 4]); t = d[p + 4:p + 8]; body = d[p + 8:p + 8 + n]
        if t == b'IHDR': ihdr = struct.unpack('>IIBBBBB', body)
        elif t == b'IDAT': idat += body
        p += 12 + n
    w, h, depth, ctype, _, _, il = ihdr
    assert depth == 8 and ctype in (2, 6) and il == 0, ihdr
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = np.zeros((h, stride), np.uint8)
    prev = np.zeros(stride, np.uint8)
    q = 0
    for y in range(h):
        ft = raw[q]; line = np.frombuffer(raw[q + 1:q + 1 + stride], np.uint8).copy(); q += 1 + stride
        if ft == 0: pass
        elif ft == 1:
            for i in range(bpp, stride): line[i] = (int(line[i]) + int(line[i - bpp])) & 255
        elif ft == 2: line = (line.astype(np.int32) + prev).astype(np.uint8)
        elif ft == 3:
            for i in range(stride):
                a = int(line[i - bpp]) if i >= bpp else 0
                line[i] = (int(line[i]) + ((a + int(prev[i])) >> 1)) & 255
        elif ft == 4:
            for i in range(stride):
                a = int(line[i - bpp]) if i >= bpp else 0; b = int(prev[i]); c = int(prev[i - bpp]) if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (int(line[i]) + pr) & 255
        out[y] = line; prev = line
    return out.reshape(h, w, bpp)[..., :3]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump')
    ap.add_argument('--out')
    ap.add_argument('--min-w', type=int, default=1920)
    ap.add_argument('--png-compare', nargs='*', default=[])
    args = ap.parse_args()
    idx = os.path.join(args.dump, 'index.txt')
    print('index.txt sha256 %s' % sha256(idx))
    blobs, imgs, atts, passes = parse_index(idx)
    sel = [i for i in imgs if i['vkfmt'] == '97' and int(i['w']) >= args.min_w]
    print('cap_img total %d, vkfmt=97 and w>=%d: %d' % (len(imgs), args.min_w, len(sel)))
    seen = {}
    snaps = []
    for i in sel:
        r = load_fp16(args.dump, blobs, i)
        if r is None:
            print('img id=%s ref=%s use=%s blob=0 (no data)' % (i['id'], i['ref'], i['use'])); continue
        a, rows, note = r
        bid = int(i['blob'])
        key = bid
        tag = 'img id=%s pass=%s ref=%s gen=%s use=%s blob=%d(%s) %sx%s row=%s rows_present=%d' % (i['id'], i['pass'], i['ref'], i['gen'], i['use'], bid, blobs[bid]['hash'], i['w'], i['h'], i['row'], rows)
        if key in seen:
            print(tag + '  == identical blob to img id=%s' % seen[key]); snaps.append((i, a)); continue
        seen[key] = i['id']
        print(tag + ('  [' + note + ']' if note else ''))
        print(channel_report(a))
        snaps.append((i, a))
        if args.out:
            os.makedirs(args.out, exist_ok=True)
            png_write(os.path.join(args.out, 'cap_img%s_ref%s_%s_blob%s.png' % (i['id'], i['ref'], i['use'], blobs[bid]['hash'])), to_rgb8(a))
    # successive snapshots of the same ref: bit-exact compare
    print('--- successive snapshots of the same ref (bit-exact 16-bit patterns, rows present in both) ---')
    last = {}
    for i, a in snaps:
        ref = (i['ref'], i['gen'])
        if ref in last:
            pi, pa = last[ref]
            n = min(a.shape[0], pa.shape[0])
            d = (a[:n] != pa[:n]).any(axis=2)
            cnt = int(d.sum())
            first = tuple(int(x) for x in np.argwhere(d)[0][::-1]) if cnt else None
            print('ref/gen %s: img %s(%s,pass %s) -> img %s(%s,pass %s): differing pixels %d of %d first(x,y)=%s' % (ref, pi['id'], pi['use'], pi['pass'], i['id'], i['use'], i['pass'], cnt, n * a.shape[1], first))
        last[ref] = (i, a)
    for pth in args.png_compare:
        rgb = png_read_rgb(pth)
        print('--- compare with %s sha256 %s (%dx%d) ---' % (pth, sha256(pth), rgb.shape[1], rgb.shape[0]))
        for i, a in snaps:
            if a.shape[1] != rgb.shape[1]: continue
            n = min(a.shape[0], rgb.shape[0])
            c = to_rgb8(a[:n])
            d = (c != rgb[:n]).any(axis=2)
            print('  cap img %s ref %s %s: rows=%d pixels differing %d (%.4f%%), max |dRGB| %d' % (i['id'], i['ref'], i['use'], n, int(d.sum()), 100.0 * d.sum() / d.size, int(np.abs(c.astype(int) - rgb[:n].astype(int)).max())))


if __name__ == '__main__':
    main()

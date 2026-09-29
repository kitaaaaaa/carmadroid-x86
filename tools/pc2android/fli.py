"""Autodesk FLI/FLC animation decoder (8-bit). frames(path) yields (width, height, pixels, palette) per
frame; pixels are palette indices, palette is 256 (r, g, b)."""
import struct


def frames(path):
    d = open(path, 'rb').read()
    size, magic, nframes, w, h = struct.unpack_from('<IHHHH', d, 0)
    if magic not in (0xAF11, 0xAF12):
        raise ValueError('not an FLI/FLC file')
    p = struct.unpack_from('<I', d, 80)[0] if magic == 0xAF12 and struct.unpack_from('<I', d, 80)[0] else 128
    pix = bytearray(w * h)
    pal = [(0, 0, 0)] * 256
    for _ in range(nframes):
        fsize, ftype = struct.unpack_from('<IH', d, p)
        if ftype == 0xF100:  # prefix chunk
            p += fsize
            fsize, ftype = struct.unpack_from('<IH', d, p)
        nchunks = struct.unpack_from('<H', d, p + 6)[0]
        c = p + 16
        for _ in range(nchunks):
            csize, ctype = struct.unpack_from('<IH', d, c)
            b = c + 6
            if ctype in (4, 11):  # colour 256 / colour 64
                npk = struct.unpack_from('<H', d, b)[0]
                b += 2
                idx = 0
                pal = list(pal)
                for _ in range(npk):
                    idx += d[b]
                    n = d[b + 1] or 256
                    b += 2
                    for k in range(n):
                        r, g, bl = d[b], d[b + 1], d[b + 2]
                        if ctype == 11:
                            r, g, bl = r * 255 // 63, g * 255 // 63, bl * 255 // 63
                        if idx + k < 256:
                            pal[idx + k] = (r, g, bl)
                        b += 3
                    idx += n
            elif ctype == 15:  # byte run
                for y in range(h):
                    b += 1
                    x = 0
                    while x < w:
                        cnt = struct.unpack_from('<b', d, b)[0]
                        b += 1
                        if cnt > 0:
                            pix[y * w + x:y * w + x + cnt] = bytes([d[b]]) * cnt
                            b += 1
                            x += cnt
                        else:
                            cnt = -cnt
                            pix[y * w + x:y * w + x + cnt] = d[b:b + cnt]
                            b += cnt
                            x += cnt
            elif ctype == 12:  # FLI delta
                y0, nlines = struct.unpack_from('<HH', d, b)
                b += 4
                for y in range(y0, y0 + nlines):
                    npk = d[b]
                    b += 1
                    x = 0
                    for _ in range(npk):
                        x += d[b]
                        cnt = struct.unpack_from('<b', d, b + 1)[0]
                        b += 2
                        if cnt > 0:
                            pix[y * w + x:y * w + x + cnt] = d[b:b + cnt]
                            b += cnt
                            x += cnt
                        elif cnt < 0:
                            cnt = -cnt
                            pix[y * w + x:y * w + x + cnt] = bytes([d[b]]) * cnt
                            b += 1
                            x += cnt
            elif ctype == 7:  # FLC delta (words)
                nlines = struct.unpack_from('<H', d, b)[0]
                b += 2
                y = 0
                for _ in range(nlines):
                    while True:
                        op = struct.unpack_from('<H', d, b)[0]
                        b += 2
                        if op & 0xC000 == 0xC000:
                            y += 0x10000 - op
                        elif op & 0xC000 == 0x8000:
                            pix[y * w + w - 1] = op & 0xFF
                        else:
                            break
                    x = 0
                    for _ in range(op):
                        x += d[b]
                        cnt = struct.unpack_from('<b', d, b + 1)[0]
                        b += 2
                        if cnt > 0:
                            pix[y * w + x:y * w + x + cnt * 2] = d[b:b + cnt * 2]
                            b += cnt * 2
                            x += cnt * 2
                        elif cnt < 0:
                            cnt = -cnt
                            pix[y * w + x:y * w + x + cnt * 2] = d[b:b + 2] * cnt
                            b += 2
                            x += cnt * 2
                    y += 1
            elif ctype == 13:
                pix = bytearray(w * h)
            elif ctype == 16:
                pix = bytearray(d[b:b + w * h])
            c += csize
        p += fsize
        yield w, h, bytes(pix), pal


def write_png(path, w, h, rgb_rows):
    import zlib
    raw = b''.join(b'\0' + bytes(r) for r in rgb_rows)
    def ch(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                          ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))


if __name__ == '__main__':
    import sys
    fr = list(frames(sys.argv[1]))
    step = max(1, len(fr) // 8)
    picks = fr[::step][:8]
    w, h = picks[0][0], picks[0][1]
    rows = []
    for y in range(h):
        row = bytearray()
        for (_, _, px, pal) in picks:
            for x in range(w):
                row += bytes(pal[px[y * w + x]])
            row += b'\x28\x28\x28' * 4
        rows.append(row)
    write_png(sys.argv[2], (w + 4) * len(picks), h, rows)
    print(len(fr), 'frames', w, 'x', h, 'showing every', step)

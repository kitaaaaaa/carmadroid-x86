"""Carmageddon 1 text files: '@'-prefixed lines are lightly encrypted."""
KEY = bytes([0x6C, 0x1B, 0x99, 0x5F, 0xB9, 0xCD, 0x5F, 0x13, 0xCB, 0x04, 0x20, 0x0E, 0x5E, 0x1C, 0xA1, 0x0E])
COMMENT_KEY = bytes([0x67, 0xA8, 0xD6, 0x26, 0xB6, 0xDD, 0x45, 0x1B, 0x32, 0x7E, 0x22, 0x13, 0x15, 0xC2, 0x94, 0x37])


def decode_line(line):
    if not line.startswith(b'@'):
        return line
    d = bytearray(line[1:])
    key = KEY
    seed = len(d) % 16
    for i in range(len(d)):
        if i >= 2 and d[i - 1] == ord('/') and d[i - 2] == ord('/'):
            key = COMMENT_KEY
        if d[i] == 9:
            d[i] = 0x80
        c = (d[i] - 32) & 0xFF
        if not c & 0x80:
            d[i] = ((c ^ (key[seed] & 127)) + 32) & 0xFF
        seed = (seed + 7) % 16
        if d[i] == 0x80:
            d[i] = 9
    return bytes(d)


def read_text(path):
    """Decoded lines of a C1 text file (comments kept)."""
    out = []
    for raw in open(path, 'rb').read().split(b'\n'):
        raw = raw.rstrip(b'\r')
        out.append(decode_line(raw).decode('latin1'))
    return out


def data_lines(path):
    """Decoded, comment-stripped, non-empty lines."""
    res = []
    for l in read_text(path):
        l = l.split('//', 1)[0].strip()
        if l:
            res.append(l)
    return res


if __name__ == '__main__':
    import sys
    for l in read_text(sys.argv[1])[:int(sys.argv[2]) if len(sys.argv) > 2 else 60]:
        print(l)

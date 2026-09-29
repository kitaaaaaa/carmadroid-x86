"""Readers for Carmageddon 1's BRender files: DAT (models), ACT (actor hierarchies), MAT (materials),
PIX (pixelmaps). All are big-endian chunk streams: u32 id, u32 length, data."""
import struct

FILE_HEADER = 0x12  # every file starts with chunk 0x12 (length 8): u32 type, u32 version


def _chunks(data):
    p = 0
    while p + 8 <= len(data):
        cid, ln = struct.unpack_from('>II', data, p)
        yield cid, data[p + 8:p + 8 + ln]
        p += 8 + ln
        if cid == 0 and ln == 0 and p >= len(data):
            break


def _cstr(b, p):
    e = b.index(b'\0', p)
    return b[p:e].decode('latin1'), e + 1


class Model:
    def __init__(self, name):
        self.name = name
        self.verts = []       # (x, y, z)
        self.uvs = []         # (u, v)
        self.faces = []       # (a, b, c, smoothing, flags)
        self.materials = []   # names
        self.face_mats = []   # 1-based material index per face (0 = none)


def read_dat(path):
    """All models in a DAT file, by upper-case name. Parsed by chunk type (lengths are unreliable)."""
    data = open(path, 'rb').read()
    models = {}
    cur = None
    p = 0
    while p + 8 <= len(data):
        cid, ln = struct.unpack_from('>II', data, p)
        p += 8
        if cid == FILE_HEADER:
            p += ln
        elif cid == 54:  # model: u16 flags, name
            name, p = _cstr(data, p + 2)
            cur = Model(name)
            models[name.upper()] = cur
        elif cid == 23:
            n = struct.unpack_from('>I', data, p)[0]
            cur.verts = [struct.unpack_from('>3f', data, p + 4 + i * 12) for i in range(n)]
            p += 4 + n * 12
        elif cid == 24:
            n = struct.unpack_from('>I', data, p)[0]
            cur.uvs = [struct.unpack_from('>2f', data, p + 4 + i * 8) for i in range(n)]
            p += 4 + n * 8
        elif cid == 53:
            n = struct.unpack_from('>I', data, p)[0]
            cur.faces = [struct.unpack_from('>4HB', data, p + 4 + i * 9) for i in range(n)]
            p += 4 + n * 9
        elif cid == 22:
            n = struct.unpack_from('>I', data, p)[0]
            p += 4
            for _ in range(n):
                name, p = _cstr(data, p)
                cur.materials.append(name)
        elif cid == 26:
            n, per = struct.unpack_from('>II', data, p)
            cur.face_mats = list(struct.unpack_from('>%dH' % n, data, p + 8))
            p += 8 + n * per
        elif cid == 0:
            pass
        else:
            raise ValueError('unknown DAT chunk %d at 0x%x in %s' % (cid, p - 8, path))
    return models


class Actor:
    def __init__(self, name=''):
        self.name = name
        self.model = None
        self.material = None
        self.matrix = [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]  # rows: x axis, y axis, z axis, translation
        self.children = []

    def walk(self, depth=0):
        yield depth, self
        for c in self.children:
            yield from c.walk(depth + 1)


def read_act(path):
    """The root actors in an ACT file. Chunk lengths in C1 ACT files are unreliable (the model chunk's
    doesn't count its whole name), so chunks are parsed by type."""
    data = open(path, 'rb').read()
    stack = []
    roots = []
    p = 0
    while p + 8 <= len(data):
        cid, ln = struct.unpack_from('>II', data, p)
        p += 8
        if cid == FILE_HEADER:
            p += ln
        elif cid == 35:
            name, p = _cstr(data, p + 2)
            stack.append(Actor(name))
        elif cid == 43:
            stack[-1].matrix = list(struct.unpack_from('>12f', data, p))
            p += 48
        elif cid == 36:
            stack[-1].model, p = _cstr(data, p)
        elif cid == 38:
            stack[-1].material, p = _cstr(data, p)
        elif cid == 42:  # add child: pop the top actor into the one below
            child = stack.pop()
            stack[-1].children.append(child)
        elif cid in (37, 41):  # transform / bounds markers, no data
            pass
        elif cid == 50:
            p += 24
        elif cid == 0:
            roots.extend(stack)
            stack = []
        else:
            raise ValueError('unknown ACT chunk %d at 0x%x in %s' % (cid, p - 8, path))
    roots.extend(stack)
    return roots


class Material:
    def __init__(self, name):
        self.name = name
        self.colour = (255, 255, 255, 255)
        self.texture = None
        self.flags = 0
        self.index_base = 0
        self.index_range = 0


def read_mat(path):
    """Materials by upper-case name. Parsed by chunk type (the stored lengths are off by one)."""
    data = open(path, 'rb').read()
    mats = {}
    cur = None
    p = 0
    while p + 8 <= len(data):
        cid, ln = struct.unpack_from('>II', data, p)
        p += 8
        if cid == FILE_HEADER:
            p += ln
        elif cid == 0x4:  # C1 material: colour, 4 lighting floats, u16 flags, 2x3 UV matrix, index base/range, name
            cur = Material('')
            cur.colour = tuple(data[p:p + 4])
            cur.flags = struct.unpack_from('>H', data, p + 20)[0]
            cur.index_base, cur.index_range = data[p + 46], data[p + 47]
            cur.name, p = _cstr(data, p + 48)
            mats[cur.name.upper()] = cur
        elif cid == 0x3c:  # C2-style material
            cur = Material('')
            cur.colour = tuple(data[p:p + 4])
            cur.flags = struct.unpack_from('>I', data, p + 20)[0]
            cur.name, p = _cstr(data, p + 20 + 4 + 24 + 4 + 13)
            mats[cur.name.upper()] = cur
        elif cid == 0x1c:  # colour map (texture)
            cur.texture, p = _cstr(data, p)
        elif cid == 0x1f:  # shade table
            _, p = _cstr(data, p)
        elif cid == 0:
            pass
        else:
            raise ValueError('unknown MAT chunk 0x%x at 0x%x in %s' % (cid, p - 8, path))
    return mats


def read_pix(path):
    """(width, height, row_bytes, pixels) of the first pixelmap; 8-bit palette indices."""
    data = open(path, 'rb').read()
    hdr = None
    for cid, b in _chunks(data):
        if cid in (0x03, 0x3D) and hdr is None:
            typ, row, w, h = struct.unpack_from('>BHHH', b, 0)
            hdr = (typ, row, w, h)
        elif cid == 0x21 and hdr:
            count, esize = struct.unpack_from('>II', b, 0)
            return hdr[2], hdr[3], hdr[1], b[8:8 + count * esize], hdr[0]
    raise ValueError('no pixel data in ' + path)


def read_palette(path):
    w, h, row, px, typ = read_pix(path)
    return [(px[i * 4 + 1], px[i * 4 + 2], px[i * 4 + 3]) for i in range(256)]

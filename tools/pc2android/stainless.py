"""Readers/writers for the Stainless (Beelzebub engine) formats used by Android Carmageddon:
CNT (node hierarchy, v4.0) and MDL (mesh, v6.2)."""
import struct


class Reader:
    def __init__(self, data, pos=0):
        self.d = data
        self.p = pos

    def take(self, n):
        v = self.d[self.p:self.p + n]
        if len(v) != n:
            raise EOFError('read past end at 0x%x' % self.p)
        self.p += n
        return v

    def u8(self): return self.take(1)[0]
    def u16(self): return struct.unpack('<H', self.take(2))[0]
    def i16(self): return struct.unpack('<h', self.take(2))[0]
    def u32(self): return struct.unpack('<I', self.take(4))[0]
    def i32(self): return struct.unpack('<i', self.take(4))[0]
    def f32(self): return struct.unpack('<f', self.take(4))[0]
    def fs(self, n): return list(struct.unpack('<%df' % n, self.take(4 * n)))

    def pstr(self, extra_pad=0):
        """u32 length, chars, padded to 4 bytes (+extra_pad)."""
        n = self.u32()
        s = self.take(n).decode('latin1')
        self.take((-n) % 4 + extra_pad)
        return s

    @property
    def left(self): return len(self.d) - self.p


class Writer:
    def __init__(self):
        self.b = bytearray()

    def raw(self, x): self.b += x
    def u8(self, v): self.b += struct.pack('<B', v)
    def u16(self, v): self.b += struct.pack('<H', v)
    def i16(self, v): self.b += struct.pack('<h', v)
    def u32(self, v): self.b += struct.pack('<I', v & 0xFFFFFFFF)
    def i32(self, v): self.b += struct.pack('<i', v)
    def f32(self, v): self.b += struct.pack('<f', v)
    def fs(self, vs):
        for v in vs: self.f32(v)

    def pstr(self, s, extra_pad=0):
        e = s.encode('latin1')
        self.u32(len(e))
        self.b += e + b'\0' * ((-len(e)) % 4 + extra_pad)


# ---------------------------------------------------------------------------------------------
# CNT: E# 00 04, then a recursive node:
#   name (pstr), flag bytes until 0, u32 0, 12 floats (3x3 rotation rows + translation),
#   4-char type, type data, u32 child count, children, lumps (u32 id ... until 0)
# ---------------------------------------------------------------------------------------------
class Node:
    def __init__(self, name='', kind='NULL', matrix=None, model=None):
        self.name = name
        self.kind = kind
        self.matrix = matrix or [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]
        self.model = model          # MODL/SKIN: model name
        self.extra = b''            # raw type data for types we don't interpret
        self.children = []
        self.lumps = b''            # raw lump data (before the terminating 0)
        self.flags = b'\0'          # flag bytes, 0-terminated

    def walk(self, depth=0):
        yield depth, self
        for c in self.children:
            yield from c.walk(depth + 1)


def read_cnt(data):
    r = Reader(data)
    if r.take(2) != b'E#':
        raise ValueError('not a CNT')
    minor, major = r.u8(), r.u8()
    if (major, minor) != (4, 0):
        raise ValueError('CNT v%d.%d' % (major, minor))
    node = _read_node(r)
    if r.left:
        raise ValueError('%d bytes left' % r.left)
    return node


def _read_node(r):
    n = Node()
    n.name = r.pstr()
    fl = bytearray()
    while True:
        b = r.u8()
        fl.append(b)
        if b == 0:
            break
    n.flags = bytes(fl)
    if r.u32() != 0:
        raise ValueError('expected 0 after flags')
    n.matrix = r.fs(12)
    n.kind = r.take(4).decode('latin1')
    start = r.p
    if n.kind in ('MODL', 'SKIN'):
        n.model = r.pstr()
    elif n.kind == 'NULL':
        pass
    elif n.kind == 'LITg':
        t = r.u32()
        if t == 3:
            r.pstr()
        else:
            raise ValueError('embedded light not supported')
    elif n.kind == 'VFXI':
        r.take(r.u32())
    elif n.kind == 'LITd':
        r.take(16)
    elif n.kind == 'EMIT':
        v = r.u8()
        r.take(25)
        r.take(r.u32())
        r.take(128 if v == 6 else 136)
    elif n.kind == 'EMT2':
        r.take(34)
        r.take(r.u32())
        r.take(612)
    elif n.kind == 'SPLN':
        r.take(88)
    else:
        raise ValueError('unknown node type %r' % n.kind)
    if n.kind not in ('MODL', 'SKIN', 'NULL'):
        n.extra = bytes(r.d[start:r.p])
    for _ in range(r.u32()):
        n.children.append(_read_node(r))
    lump_start = r.p
    while True:
        lump = r.u32()
        if lump == 0:
            break
        raise ValueError('lump %d not supported' % lump)
    return n


def write_cnt(node):
    w = Writer()
    w.raw(b'E#\x00\x04')
    _write_node(w, node)
    return bytes(w.b)


def _write_node(w, n):
    w.pstr(n.name)
    w.raw(n.flags)
    w.u32(0)
    w.fs(n.matrix)
    w.raw(n.kind.encode('latin1'))
    if n.kind in ('MODL', 'SKIN'):
        w.pstr(n.model)
    else:
        w.raw(n.extra)
    w.u32(len(n.children))
    for c in n.children:
        _write_node(w, c)
    w.u32(0)


# ---------------------------------------------------------------------------------------------
# MDL v6.2 (E# 02 06). Only the "PREP" (render) data is interpreted; USER data is kept raw.
# ---------------------------------------------------------------------------------------------
FLAG_USER = 1
FLAG_PREP_SKIN = 32
FLAG_LOD = 512


class Mdl:
    def __init__(self):
        self.checksum = 0
        self.flags = 0
        self.user_faces = 0
        self.user_verts = 0
        self.radius = 0.0
        self.bmin = [0, 0, 0]
        self.bmax = [0, 0, 0]
        self.centre = [0, 0, 0]
        self.materials = []     # names
        self.faces = []         # (material, flags, a, b, c)
        self.verts = []         # (x,y,z, nx,ny,nz, u,v, u2,v2, r,g,b,a)
        self.groups = []        # dicts: centre, radius, min, max, strip_off, strip_count, strip, list_off, list_count, list
        self.tail = b''         # everything after the material groups (skin/LOD/USER data)
        self.unknown = 1
        self.slack = 0


def read_mdl(data):
    r = Reader(data)
    if r.take(2) != b'E#':
        raise ValueError('not an MDL')
    minor, major = r.u8(), r.u8()
    if (major, minor) != (6, 2):
        raise ValueError('MDL v%d.%d' % (major, minor))
    m = Mdl()
    m.checksum = r.u32()
    m.flags = r.u32()
    file_size = r.u32()   # Android: file length - 28 (C:R has the PREP data size here)
    m.slack = len(data) - 28 - file_size  # a few files have trailing bytes not counted
    if not 0 <= m.slack <= 64:
        raise ValueError('file size field %d vs %d' % (file_size, len(data) - 28))
    m.user_faces = r.u32()
    m.user_verts = r.u32()
    m.unknown = r.u32()   # 1 in every Android model (C:R has the file size here)
    m.radius = r.f32()
    m.bmin = r.fs(3)
    m.bmax = r.fs(3)
    m.centre = r.fs(3)
    for _ in range(r.u16()):
        m.materials.append(r.pstr(extra_pad=4))
    prep_start = r.p
    for _ in range(r.u32()):
        m.faces.append(struct.unpack('<HHIII', r.take(16)))
    for _ in range(r.u32()):
        m.verts.append(struct.unpack('<10f4B', r.take(44)))
    ngroups = r.u16()
    for _ in range(ngroups):
        g = {}
        g['centre'] = r.fs(3)
        g['radius'] = r.f32()
        g['min'] = r.fs(3)
        g['max'] = r.fs(3)
        g['strip_off'] = r.u32()
        g['strip_count'] = r.u32()
        g['strip'] = [r.u32() for _ in range(r.u32())]
        g['list_off'] = r.u32()
        g['list_count'] = r.u32()
        g['list'] = [r.u32() for _ in range(r.u32())]
        m.groups.append(g)
    m.tail = bytes(r.d[r.p:])
    return m


def write_mdl(m):
    w = Writer()
    w.raw(b'E#\x02\x06')
    w.u32(m.checksum)
    w.u32(m.flags)
    prep = Writer()
    prep.u32(len(m.faces))
    for f in m.faces:
        prep.raw(struct.pack('<HHIII', *f))
    prep.u32(len(m.verts))
    for v in m.verts:
        prep.raw(struct.pack('<10f4B', *v))
    prep.u16(len(m.groups))
    for g in m.groups:
        prep.fs(g['centre'])
        prep.f32(g['radius'])
        prep.fs(g['min'])
        prep.fs(g['max'])
        prep.u32(g['strip_off'])
        prep.u32(g['strip_count'])
        prep.u32(len(g['strip']))
        for i in g['strip']:
            prep.u32(i)
        prep.u32(g['list_off'])
        prep.u32(g['list_count'])
        prep.u32(len(g['list']))
        for i in g['list']:
            prep.u32(i)
    size_at = len(w.b)
    w.u32(0)              # file size, filled in below
    w.u32(m.user_faces)
    w.u32(m.user_verts)
    w.u32(m.unknown)
    w.f32(m.radius)
    w.fs(m.bmin)
    w.fs(m.bmax)
    w.fs(m.centre)
    w.u16(len(m.materials))
    for name in m.materials:
        w.pstr(name, extra_pad=4)
    w.raw(prep.b)
    w.raw(m.tail)
    struct.pack_into('<I', w.b, size_at, len(w.b) - 28 - m.slack)
    return bytes(w.b)

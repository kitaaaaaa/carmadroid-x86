"""Minimal Lua 5.1 bytecode (.LOL) reader: dumps each function's constants with file offsets, so numeric
constants can be patched in place.  usage: lol.py file.LOL"""
import struct, sys


class R:
    def __init__(self, d):
        self.d, self.p = d, 0

    def take(self, n):
        v = self.d[self.p:self.p + n]
        self.p += n
        return v

    def u8(self): return self.take(1)[0]
    def i32(self): return struct.unpack('<i', self.take(4))[0]

    def string(self):
        n = self.i32()  # size_t = 4 on ARM
        s = self.take(n)
        return s[:-1].decode('latin1') if n else None


def read_function(r, out, depth=0):
    src = r.string()
    r.i32(); r.i32()
    nups, nparams, vararg, maxstack = r.take(4)
    ncode = r.i32()
    code_at = r.p
    r.take(4 * ncode)
    consts = []
    for _ in range(r.i32()):
        t = r.u8()
        if t == 0:
            consts.append(('nil', None, r.p))
        elif t == 1:
            consts.append(('bool', r.u8(), r.p))
        elif t == 3:
            at = r.p
            consts.append(('num', struct.unpack('<d', r.take(8))[0], at))
        elif t == 4:
            consts.append(('str', r.string(), r.p))
        else:
            raise ValueError('const type %d' % t)
    out.append((depth, consts, code_at, ncode))
    for _ in range(r.i32()):
        read_function(r, out, depth + 1)
    for _ in range(r.i32()):   # line info
        r.i32()
    for _ in range(r.i32()):   # locals
        r.string(); r.i32(); r.i32()
    for _ in range(r.i32()):   # upvalue names
        r.string()


def load(data):
    if data[:4] != b'\x1bLua' or data[4] != 0x51:
        raise ValueError('not Lua 5.1 bytecode')
    r = R(data)
    r.take(12)
    funcs = []
    read_function(r, funcs)
    return funcs


if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    for depth, consts, code_at, ncode in load(d):
        print('function depth %d, %d instructions' % (depth, ncode))
        print('   ' + ', '.join('%s@%x' % (repr(v), at) if t == 'num' else repr(v) for t, v, at in consts))

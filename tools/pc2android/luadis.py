"""Lua 5.1 bytecode disassembler (enough to read the UI layout tables)."""
import struct, sys
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
import lol

OPS = ['MOVE', 'LOADK', 'LOADBOOL', 'LOADNIL', 'GETUPVAL', 'GETGLOBAL', 'GETTABLE', 'SETGLOBAL', 'SETUPVAL',
       'SETTABLE', 'NEWTABLE', 'SELF', 'ADD', 'SUB', 'MUL', 'DIV', 'MOD', 'POW', 'UNM', 'NOT', 'LEN', 'CONCAT',
       'JMP', 'EQ', 'LT', 'LE', 'TEST', 'TESTSET', 'CALL', 'TAILCALL', 'RETURN', 'FORLOOP', 'FORPREP',
       'TFORLOOP', 'SETLIST', 'CLOSE', 'CLOSURE', 'VARARG']


def dis(path):
    d = open(path, 'rb').read()
    funcs = lol.load(d)
    depth, consts, code_at, n = funcs[0]
    K = [v for t, v, at in consts]

    def rk(x):
        return repr(K[x - 256]) if x >= 256 else 'r%d' % x

    for i in range(n):
        ins = struct.unpack_from('<I', d, code_at + 4 * i)[0]
        op, a, c, b = ins & 0x3f, (ins >> 6) & 0xff, (ins >> 14) & 0x1ff, (ins >> 23) & 0x1ff
        bx = ins >> 14
        name = OPS[op]
        if name == 'LOADK':
            s = 'r%d = %r' % (a, K[bx])
        elif name == 'SETTABLE':
            s = 'r%d[%s] = %s' % (a, rk(b), rk(c))
        elif name == 'NEWTABLE':
            s = 'r%d = {}' % a
        elif name == 'SETLIST':
            s = 'r%d[...] = r%d..r%d' % (a, a + 1, a + b)
        elif name in ('GETGLOBAL', 'SETGLOBAL'):
            s = '%s r%d %r' % (name, a, K[bx])
        elif name == 'UNM':
            s = 'r%d = -r%d' % (a, b)
        else:
            s = '%s %d %d %d' % (name, a, b, c)
        print('%3d %s' % (i, s))


if __name__ == '__main__':
    dis(sys.argv[1])

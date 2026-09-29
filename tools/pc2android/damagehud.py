"""Damage HUD silhouettes for converted cars.

The HUD's damage display (UI/LAYOUT/<lang>/<type>/HUD_DAMAGE/<CAR>_LAYOUT.LOL) draws the image
'damage\\<Car>' (UI/ASSETS/PPI_HIGH/DAMAGE/<CAR>.IMG, 128x256, front at the top) centred, and part
indicators on top at fixed offsets: wheels ('_10x20' and similar), '_susp', '_steer', '_strut', '_engine'.
A new car's layout is copied from a template car, so its silhouette is drawn with the wheels exactly where
that layout puts the wheel indicators, and the layout's image name is changed to the new car's."""
import math, os, re, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import lol, uiimg
import stainless as st

W, H = 128, 256


def layout_parts(path):
    """[(part name, x, y)] from a compiled damage layout."""
    d = open(path, 'rb').read()
    depth, consts, code_at, n = lol.load(d)[0]
    K = [v for t, v, at in consts]
    parts, name, cur = [], None, {}
    for i in range(n):
        ins = struct.unpack_from('<I', d, code_at + 4 * i)[0]
        op, a, c, b = ins & 0x3f, (ins >> 6) & 0xff, (ins >> 14) & 0x1ff, (ins >> 23) & 0x1ff
        if op == 5:  # GETGLOBAL
            name, cur = K[ins >> 14], {}
        elif op == 9 and b >= 256 and c >= 256:  # SETTABLE r[K] = K
            cur[K[b - 256]] = K[c - 256]
        elif op == 28 and name and 'x' in cur and 'y' in cur:  # CALL: end of an item
            parts.append((name, cur['x'], cur['y']))
            name = None
    return parts


def wheel_spots(parts):
    """(front y, rear y, front half-track, rear half-track) of the wheel indicators, in HUD pixels
    relative to the image centre."""
    wheels = [(x, y) for n, x, y in parts if re.fullmatch(r'_\d+x\d+', n or '')]
    if len(wheels) < 4:
        return -60.0, 60.0, 34.0, 36.0
    front = [w for w in wheels if w[1] < 0]
    rear = [w for w in wheels if w[1] >= 0]
    fy = sum(y for x, y in front) / len(front)
    ry = sum(y for x, y in rear) / len(rear)
    return fy, ry, sum(abs(x) for x, y in front) / len(front), sum(abs(x) for x, y in rear) / len(rear)


def set_layout_image(path, image_name):
    """Replace the layout's 'damage\\...' image name (a length-prefixed string constant)."""
    d = open(path, 'rb').read()
    m = re.search(rb'(....)(damage\\\\[^\0]*)\0', d, re.S)
    if not m:
        return False
    new = b'damage\\\\' + image_name.encode('latin1')
    d = d[:m.start()] + struct.pack('<I', len(new) + 1) + new + b'\0' + d[m.end():]
    open(path, 'wb').write(d)
    return True


def car_geometry(vehicle_folder):
    """(triangles, wheel positions {name: (x, z)}, bounding box (x0, x1, z0, z1))."""
    tris = uiimg.load_car(vehicle_folder)
    cnt = st.read_cnt(open(os.path.join(vehicle_folder, 'CARBODY.CNT'), 'rb').read())
    wheels = {n.name.lower(): (n.matrix[9], n.matrix[11]) for _, n in cnt.walk() if n.name.lower().startswith('whl')}
    xs = [p[0] for ps, _, _ in tris for p in ps]
    zs = [p[2] for ps, _, _ in tris for p in ps]
    return tris, wheels, (min(xs), max(xs), min(zs), max(zs))


def fit(bbox):
    """Scales (pixels per metre, across and along) and centre that stretch the car to fill the 128x256
    frame: the HUD picture is not meant to keep the car's proportions."""
    x0, x1, z0, z1 = bbox
    return (0.92 * W / (x1 - x0), 0.95 * H / (z1 - z0)), (x0 + x1) / 2, (z0 + z1) / 2


def to_hud(x, z, s, cx, cz):
    """Car position (front is +z) -> HUD offset from the image centre (front at the top: -y)."""
    return (x - cx) * s[0], -(z - cz) * s[1]


def render_silhouette(vehicle_folder):
    """Grey top view of the car, front up, fitted to the frame."""
    tris, wheels, bbox = car_geometry(vehicle_folder)
    s, cx, cz = fit(bbox)
    SS = 2
    img = bytearray(W * SS * H * SS * 4)
    zbuf = [-1e30] * (W * SS * H * SS)

    def proj(p):
        hx, hy = to_hud(p[0], p[2], s, cx, cz)
        return (W / 2 + hx) * SS, (H / 2 + hy) * SS, p[1]

    light = (0.3, 0.9, 0.35)
    for ps, uvs, tex in tris:
        a, b, c = [proj(p) for p in ps]
        ux, uy, uz = [ps[1][k] - ps[0][k] for k in range(3)]
        vx, vy, vz = [ps[2][k] - ps[0][k] for k in range(3)]
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        nl = math.sqrt(nx * nx + ny * ny + nz * nz) or 1
        if ny < 0:
            nx, ny, nz = -nx, -ny, -nz
        shade = 0.45 + 0.6 * max(0.0, (nx * light[0] + ny * light[1] + nz * light[2]) / nl)
        area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])
        if abs(area) < 1e-9:
            continue
        x0, x1 = max(0, int(min(a[0], b[0], c[0]))), min(W * SS - 1, int(max(a[0], b[0], c[0])) + 1)
        y0, y1 = max(0, int(min(a[1], b[1], c[1]))), min(H * SS - 1, int(max(a[1], b[1], c[1])) + 1)
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                px_, py = x + 0.5, y + 0.5
                w0 = ((b[0] - px_) * (c[1] - py) - (c[0] - px_) * (b[1] - py)) / area
                w1 = ((c[0] - px_) * (a[1] - py) - (a[0] - px_) * (c[1] - py)) / area
                w2 = 1 - w0 - w1
                if w0 < 0 or w1 < 0 or w2 < 0:
                    continue
                height = w0 * a[2] + w1 * b[2] + w2 * c[2]
                i = y * W * SS + x
                if height <= zbuf[i]:
                    continue
                lum = 150
                if tex:
                    tw, th, tpx = tex
                    u = w0 * uvs[0][0] + w1 * uvs[1][0] + w2 * uvs[2][0]
                    v = w0 * uvs[0][1] + w1 * uvs[1][1] + w2 * uvs[2][1]
                    ti = ((int(v * th) % th) * tw + int(u * tw) % tw) * 4
                    if tpx[ti + 3] < 128:
                        continue
                    lum = 0.3 * tpx[ti] + 0.59 * tpx[ti + 1] + 0.11 * tpx[ti + 2]
                zbuf[i] = height
                g = min(255, int((90 + lum * 0.45) * shade))
                o = i * 4
                img[o:o + 4] = bytes((g, g, g, 255))
    return uiimg.resize(W * SS, H * SS, bytes(img), W, H)


def new_positions(parts, vehicle_folder):
    """New (x, y) per layout item: wheels and suspension at the car's wheels, the others (steering,
    strut, engine) moved proportionally from the template's axles to the car's."""
    tris, wheels, bbox = car_geometry(vehicle_folder)
    s, cx, cz = fit(bbox)
    fy, ry, ftrack, rtrack = wheel_spots(parts)
    hud = {k: to_hud(x, z, s, cx, cz) for k, (x, z) in wheels.items()}
    if len(hud) < 4:
        return None
    nfy = (hud['whlfl'][1] + hud['whlfr'][1]) / 2
    nry = (hud['whlrl'][1] + hud['whlrr'][1]) / 2
    nft = (abs(hud['whlfl'][0]) + abs(hud['whlfr'][0])) / 2
    nrt = (abs(hud['whlrl'][0]) + abs(hud['whlrr'][0])) / 2

    def map_y(y):
        return nfy + (y - fy) * (nry - nfy) / (ry - fy) if ry != fy else y

    def map_x(x, y):
        t = min(1.0, max(0.0, (y - fy) / (ry - fy))) if ry != fy else 0.5
        old_track = ftrack + (rtrack - ftrack) * t
        new_track = nft + (nrt - nft) * t
        return x * new_track / old_track if old_track else x

    out = []
    for name, x, y in parts:
        if name == 'anchor' or name is None:
            out.append((x, y))  # the car image itself stays centred
        else:
            out.append((round(map_x(x, y), 1), round(map_y(y), 1)))
    return out


def rewrite_layout(path, image_name, positions):
    """Rewrites a compiled layout: the image name, and each item's x/y (as new constants, since values
    are shared between items)."""
    d = bytearray(open(path, 'rb').read())
    depth, consts, code_at, n = lol.load(bytes(d))[0]
    K = [(t, v) for t, v, at in consts]
    # locate the constant section: right after the code
    cstart = code_at + 4 * n
    r = lol.R(bytes(d))
    r.p = cstart
    for _ in range(r.i32()):
        t = r.u8()
        if t == 1:
            r.u8()
        elif t == 3:
            r.take(8)
        elif t == 4:
            r.string()
    cend = r.p

    def kindex(t, v):
        if (t, v) in K:
            return K.index((t, v))
        K.append((t, v))
        return len(K) - 1

    # image name
    for i, (t, v) in enumerate(K):
        if t == 'str' and v.lower().startswith('damage\\'):
            K[i] = ('str', 'damage\\' + image_name)
    # item positions
    item = -1
    naming = had_xy = False
    for i in range(n):
        at = code_at + 4 * i
        ins = struct.unpack_from('<I', d, at)[0]
        op, a, c, b = ins & 0x3f, (ins >> 6) & 0xff, (ins >> 14) & 0x1ff, (ins >> 23) & 0x1ff
        if op == 5:
            naming = True
            had_xy = False
        elif op == 9 and b >= 256 and c >= 256 and naming and K[b - 256][1] in ('x', 'y'):
            had_xy = True
            if item + 1 < len(positions):
                val = positions[item + 1][0 if K[b - 256][1] == 'x' else 1]
                k = kindex('num', float(val))
                if k > 255:
                    raise ValueError('too many constants')
                ins = (ins & ~(0x1ff << 14)) | ((k + 256) << 14)
                struct.pack_into('<I', d, at, ins)
        elif op == 28 and naming:
            if had_xy:
                item += 1
            naming = False
    sec = bytearray(struct.pack('<i', len(K)))
    for t, v in K:
        if t == 'nil':
            sec += b'\0'
        elif t == 'bool':
            sec += bytes((1, v))
        elif t == 'num':
            sec += b'\3' + struct.pack('<d', v)
        else:
            e = v.encode('latin1') + b'\0'
            sec += b'\4' + struct.pack('<i', len(e)) + e
    out = bytes(d[:cstart]) + bytes(sec) + bytes(d[cend:])
    lol.load(out)  # sanity: still parses
    open(path, 'wb').write(out)


def make_damage_hud(game_dir, name, vehicle_folder):
    """Silhouette image + layout image name for an added car. Returns the number of layouts updated."""
    ui = os.path.join(game_dir, 'DATA', 'CONTENT', 'UI')
    layouts = []
    for dp, dn, fn in os.walk(ui):
        for f in fn:
            if f.upper() == name.upper() + '_LAYOUT.LOL' and 'HUD_DAMAGE' in dp.upper():
                layouts.append(os.path.join(dp, f))
    if not layouts:
        return 0
    rgba = render_silhouette(vehicle_folder)
    for dp, dn, fn in os.walk(os.path.join(ui, 'ASSETS')):
        if os.path.basename(dp).upper() == 'DAMAGE':
            uiimg.write_img(os.path.join(dp, name.upper() + '.IMG'), W, H, rgba)
    for p in layouts:
        parts = layout_parts(p)
        positions = new_positions(parts, vehicle_folder) or [(x, y) for _, x, y in parts]
        rewrite_layout(p, name, positions)
    return len(layouts)


if __name__ == '__main__':
    parts = layout_parts(sys.argv[1])
    for p in parts:
        print(p)
    print('wheel spots', wheel_spots(parts))

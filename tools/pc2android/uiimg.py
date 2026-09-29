"""Menu pictures for converted cars: IMG encoding (the game's 4-plane RLE format), a small software
renderer for car pictures, and driver portraits from the PC game's mugshot animations."""
import math, os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import stainless as st


# ---------------------------------------------------------------------------------------------
# IMG v1.0, format 6 (planes A, R, G, B), RLE: a count byte with the high bit set is a literal run
# of (count & 0x7f) bytes, otherwise the next byte is repeated count times.
# ---------------------------------------------------------------------------------------------
def rle(plane):
    out = bytearray()
    i, n = 0, len(plane)
    while i < n:
        run = 1
        while i + run < n and run < 127 and plane[i + run] == plane[i]:
            run += 1
        if run >= 3:
            out += bytes((run, plane[i]))
            i += run
            continue
        j = i
        while j < n and j - i < 127 and not (j + 2 < n and plane[j] == plane[j + 1] == plane[j + 2]):
            j += 1
        out += bytes((0x80 | (j - i),)) + plane[i:j]
        i = j
    return bytes(out)


def write_img(path, w, h, rgba, basic=0x6b):
    """basic flags 0x6b as the game's menu pictures: no mipmaps, no downsampling, no 16-bit, RLE."""
    planes = [bytes(rgba[3::4]), bytes(rgba[0::4]), bytes(rgba[1::4]), bytes(rgba[2::4])]
    data = [rle(p) for p in planes]
    body = struct.pack('<4I', *[len(x) for x in data]) + b''.join(data)
    hdr = b'IMAGEMAP' + bytes((0, 1, basic, 0)) + struct.pack('<IIHH', 6, len(body), w, h)
    open(path, 'wb').write(hdr + body)


def read_img(path):
    """(w, h, rgba) for IMGs this tool writes: format 0 uncompressed A,R,G,B."""
    d = open(path, 'rb').read()
    fmt, size, w, h = struct.unpack_from('<IIHH', d, 12)
    px = d[24:24 + w * h * 4]
    rgba = bytearray(len(px))
    rgba[0::4], rgba[1::4], rgba[2::4], rgba[3::4] = px[1::4], px[2::4], px[3::4], px[0::4]
    return w, h, bytes(rgba)


def resize(w, h, rgba, nw, nh):
    """Area-averaged resize with premultiplied alpha."""
    out = bytearray(nw * nh * 4)
    sx, sy = w / nw, h / nh
    for y in range(nh):
        y0, y1 = int(y * sy), max(int(y * sy) + 1, int((y + 1) * sy))
        for x in range(nw):
            x0, x1 = int(x * sx), max(int(x * sx) + 1, int((x + 1) * sx))
            r = g = b = a = 0
            cnt = 0
            for yy in range(y0, min(y1, h)):
                base = yy * w
                for xx in range(x0, min(x1, w)):
                    i = (base + xx) * 4
                    al = rgba[i + 3]
                    r += rgba[i] * al; g += rgba[i + 1] * al; b += rgba[i + 2] * al; a += al
                    cnt += 1
            o = (y * nw + x) * 4
            if a:
                out[o:o + 4] = bytes((r // a, g // a, b // a, a // cnt))
    return bytes(out)


# ---------------------------------------------------------------------------------------------
# Car picture renderer
# ---------------------------------------------------------------------------------------------
def load_car(folder):
    """Triangles (3 positions, 3 uvs, texture) from a converted vehicle folder (CNT + MDL + MTL + IMG)."""
    textures = {}

    def texture_for(material):
        if material not in textures:
            tex = None
            p = os.path.join(folder, material.upper() + '.MTL')
            if os.path.exists(p):
                r = st.Reader(open(p, 'rb').read())
                r.take(2)
                if r.u32() >= 1:
                    name = r.pstr()
                    ip = os.path.join(folder, name.upper() + '.IMG')
                    if os.path.exists(ip):
                        tex = read_img(ip)
            textures[material] = tex
        return textures[material]

    tris = []
    cnt = st.read_cnt(open(os.path.join(folder, 'CARBODY.CNT'), 'rb').read())
    for depth, node in cnt.walk():
        if not node.model:
            continue
        mp = os.path.join(folder, node.model.upper() + '.MDL')
        if not os.path.exists(mp):
            continue
        m = st.read_mdl(open(mp, 'rb').read())
        t = node.matrix[9:12]
        for mat, _, a, b, c in m.faces:
            vs = [m.verts[i] for i in (a, b, c)]
            tris.append(([(v[0] + t[0], v[1] + t[1], v[2] + t[2]) for v in vs], [(v[6], v[7]) for v in vs],
                         texture_for(m.materials[mat])))
    return tris


def render_car(folder, width, height, yaw_deg=-35.0, pitch_deg=-28.0, fill=0.66, ss=2):
    """RGBA picture of the car in a 3/4 view on a transparent background, with a soft shadow."""
    tris = load_car(folder)
    W, H = width * ss, height * ss
    # Android vehicles are left-handed (front +z); flip z to view them in a right-handed frame.
    yaw, pitch = math.radians(yaw_deg), math.radians(pitch_deg)
    cy, sy_, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)

    def view(p):
        x, y, z = p[0], p[1], -p[2]
        x, z = x * cy - z * sy_, x * sy_ + z * cy          # yaw about y
        y, z = y * cp - z * sp, y * sp + z * cp            # pitch about x
        return x, y, z

    vt = [([view(p) for p in ps], uvs, tex) for ps, uvs, tex in tris]
    xs = [p[0] for ps, _, _ in vt for p in ps]
    ys = [p[1] for ps, _, _ in vt for p in ps]
    # ground footprint for the shadow (y = lowest point)
    ground = min(p[1] for ps, _, _ in tris for p in ps)
    span = max(max(xs) - min(xs), (max(ys) - min(ys)) * W / H)
    scale = W * fill / span
    cx, cyy = (max(xs) + min(xs)) / 2, (max(ys) + min(ys)) / 2

    def screen(p):
        return (W / 2 + (p[0] - cx) * scale, H * 0.55 - (p[1] - cyy) * scale, p[2])

    img = bytearray(W * H * 4)
    zbuf = [1e30] * (W * H)
    # soft shadow: the car's ground-plane bounding box, projected, blurred ellipse
    corners = []
    for ps, _, _ in tris:
        for p in ps:
            corners.append(screen(view((p[0], ground, p[2]))))
    sx0, sx1 = min(c[0] for c in corners), max(c[0] for c in corners)
    sy0, sy1 = min(c[1] for c in corners), max(c[1] for c in corners)
    ecx, ecy, erx, ery = (sx0 + sx1) / 2, (sy0 + sy1) / 2, (sx1 - sx0) / 2 * 1.05, (sy1 - sy0) / 2 * 1.1
    for y in range(max(0, int(ecy - ery)), min(H, int(ecy + ery) + 1)):
        for x in range(max(0, int(ecx - erx)), min(W, int(ecx + erx) + 1)):
            d = ((x - ecx) / erx) ** 2 + ((y - ecy) / ery) ** 2
            if d < 1:
                img[(y * W + x) * 4 + 3] = int(110 * (1 - d) ** 1.5)
    light = (-0.45, 0.8, 0.4)
    ln = math.sqrt(sum(c * c for c in light))
    light = tuple(c / ln for c in light)
    for ps, uvs, tex in vt:
        a, b, c = [screen(p) for p in ps]
        # face normal in view space for lighting (winding independent: use abs)
        ux, uy, uz = ps[1][0] - ps[0][0], ps[1][1] - ps[0][1], ps[1][2] - ps[0][2]
        vx, vy, vz = ps[2][0] - ps[0][0], ps[2][1] - ps[0][1], ps[2][2] - ps[0][2]
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        nl = math.sqrt(nx * nx + ny * ny + nz * nz) or 1
        if nz > 0:
            nx, ny, nz = -nx, -ny, -nz
        shade = 0.72 + 0.55 * max(0.0, (nx * light[0] + ny * light[1] + nz * light[2]) / nl)
        area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])
        if abs(area) < 1e-9:
            continue
        x0, x1 = max(0, int(min(a[0], b[0], c[0]))), min(W - 1, int(max(a[0], b[0], c[0])) + 1)
        y0, y1 = max(0, int(min(a[1], b[1], c[1]))), min(H - 1, int(max(a[1], b[1], c[1])) + 1)
        tw = th = 0
        if tex:
            tw, th, tpx = tex
        for y in range(y0, y1 + 1):
            py = y + 0.5
            for x in range(x0, x1 + 1):
                px_ = x + 0.5
                w0 = ((b[0] - px_) * (c[1] - py) - (c[0] - px_) * (b[1] - py)) / area
                w1 = ((c[0] - px_) * (a[1] - py) - (a[0] - px_) * (c[1] - py)) / area
                w2 = 1 - w0 - w1
                if w0 < 0 or w1 < 0 or w2 < 0:
                    continue
                z = w0 * a[2] + w1 * b[2] + w2 * c[2]
                i = y * W + x
                if z >= zbuf[i]:
                    continue
                if tex:
                    u = w0 * uvs[0][0] + w1 * uvs[1][0] + w2 * uvs[2][0]
                    v = w0 * uvs[0][1] + w1 * uvs[1][1] + w2 * uvs[2][1]
                    ti = ((int(v * th) % th) * tw + int(u * tw) % tw) * 4
                    if tpx[ti + 3] < 128:
                        continue
                    r, g, bl = tpx[ti], tpx[ti + 1], tpx[ti + 2]
                else:
                    r = g = bl = 160
                zbuf[i] = z
                o = i * 4
                img[o] = min(255, int(r * shade)); img[o + 1] = min(255, int(g * shade))
                img[o + 2] = min(255, int(bl * shade)); img[o + 3] = 255
    return resize(W, H, bytes(img), width, height)


# ---------------------------------------------------------------------------------------------
def portrait(fli_path, size):
    """Square driver portrait from the first frame of a PC mugshot animation (the name label and the
    frame border are cropped off)."""
    import fli
    w, h, px, pal = next(fli.frames(fli_path))
    side = min(w - 8, h - 30)
    x0, y0 = (w - side) // 2, max(4, (h - 22 - side) // 2 + 2)
    rgba = bytearray()
    for y in range(y0, y0 + side):
        for x in range(x0, x0 + side):
            rgba += bytes(pal[px[y * w + x]]) + b'\xff'
    return resize(side, side, bytes(rgba), size, size)


def save_png(path, w, h, rgba, bg=(90, 90, 90)):
    import fli
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r, g, b, a = rgba[(y * w + x) * 4:(y * w + x) * 4 + 4]
            row += bytes(int(c * a / 255 + k * (1 - a / 255)) for c, k in zip((r, g, b), bg))
        rows.append(row)
    fli.write_png(path, w, h, rows)


# Picture files per car, under DATA/CONTENT/UI/ASSETS: (folder, width, height)
CAR_PICTURES = [('PPI_HIGH/THUMBS', 240, 240), ('PPI_LOW/THUMBS', 160, 160),
                ('H720/GRID', 300, 150), ('H600/GRID', 250, 125), ('H480/GRID', 200, 100)]
DRIVER_PICTURES = [('PPI_HIGH/DRIVERS', 138), ('PPI_LOW/DRIVERS', 92)]


def make_pictures(game_dir, name, vehicle_folder, mug_fli=None):
    """Writes the car's menu pictures (and driver portraits, given the PC mugshot animation)."""
    assets = os.path.join(game_dir, 'DATA', 'CONTENT', 'UI', 'ASSETS')
    big = render_car(vehicle_folder, 480, 480, ss=1)
    for sub, w, h in CAR_PICTURES:
        folder = os.path.join(assets, *sub.split('/'))
        if not os.path.isdir(folder):
            continue
        if w == h:
            rgba = resize(480, 480, big, w, h)
        else:  # wide grid pictures: the car centred on a 2:1 canvas
            rgba = render_car(vehicle_folder, w, h, fill=0.5)
        write_img(os.path.join(folder, name.upper() + '.IMG'), w, h, rgba)
    if mug_fli and os.path.exists(mug_fli):
        for sub, size in DRIVER_PICTURES:
            folder = os.path.join(assets, *sub.split('/'))
            if os.path.isdir(folder):
                write_img(os.path.join(folder, name.upper() + '.IMG'), size, size, portrait(mug_fli, size))


if __name__ == '__main__':
    folder, out = sys.argv[1], sys.argv[2]
    rgba = render_car(folder, 240, 240)
    save_png(out, 240, 240, rgba)

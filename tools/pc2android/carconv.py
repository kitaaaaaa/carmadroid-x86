"""Convert a Carmageddon 1 (PC) car into an Android Carmageddon vehicle folder.

usage: carconv.py <PC DATA dir> <CAR.TXT name> <output vehicle dir> [--template <android vehicle dir>]
"""
import math, os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import brender as br
import c1text
import stainless as st

SCALE = 6.9  # C1 world units -> metres (dethrace WORLD_SCALE)
WHEELS = {'FLWHEEL.ACT': 'whlFL', 'FRWHEEL.ACT': 'whlFR', 'RLWHEEL.ACT': 'whlRL', 'RRWHEEL.ACT': 'whlRR'}


# ---------------------------------------------------------------------------------------------
# math: BRender matrices are row-vector 4x3 (rows: x axis, y axis, z axis, translation)
# ---------------------------------------------------------------------------------------------
def mat_mul(a, b):
    """a then b (row vectors: v * a * b)."""
    r = [0.0] * 12
    for i in range(4):
        for j in range(3):
            r[i * 3 + j] = sum(a[i * 3 + k] * b[k * 3 + j] for k in range(3)) + (b[9 + j] if i == 3 else 0)
    return r


def xform(m, v):
    x, y, z = v
    return (x * m[0] + y * m[3] + z * m[6] + m[9], x * m[1] + y * m[4] + z * m[7] + m[10],
            x * m[2] + y * m[5] + z * m[8] + m[11])


def det3(m):
    return (m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
            m[2] * (m[3] * m[7] - m[4] * m[6]))


def to_android(p):
    """C1 position -> Android: scaled, z flipped (a mirror, so triangle winding flips too)."""
    return (p[0] * SCALE, p[1] * SCALE, -p[2] * SCALE)


def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(a):
    l = math.sqrt(a[0] ** 2 + a[1] ** 2 + a[2] ** 2)
    return (a[0] / l, a[1] / l, a[2] / l) if l > 1e-12 else (0.0, 1.0, 0.0)


# ---------------------------------------------------------------------------------------------
# Mesh building
# ---------------------------------------------------------------------------------------------
class Mesh:
    """Triangles with per-corner data, grouped by material name."""
    def __init__(self, two_sided=None, inset=None):
        self.tris = []  # (material, [(pos, uv) * 3], back face?)
        self.two_sided = two_sided if two_sided is not None else set()  # Android material names
        self.inset = inset if inset is not None else {}  # material -> (texture w, h): half-texel UV inset

    def add_model(self, model, matrix, mat_names, default_mat):
        flip = det3(matrix) < 0
        for fi, f in enumerate(model.faces):
            mi = model.face_mats[fi] if fi < len(model.face_mats) else 0
            mat = mat_names(model.materials[mi - 1]) if mi > 0 and mi - 1 < len(model.materials) else default_mat
            corners = []
            for vi in f[:3]:
                p = to_android(xform(matrix, model.verts[vi]))
                uv = model.uvs[vi] if vi < len(model.uvs) else (0.0, 0.0)
                if mat in self.inset:  # sample texel centres like the PC renderer (no wrap-around at edges)
                    tw, th = self.inset[mat]
                    uv = (uv[0] * (tw - 1) / tw + 0.5 / tw, uv[1] * (th - 1) / th + 0.5 / th)
                corners.append((p, uv))
            # z flip mirrors: reverse winding (and again if the actor matrix mirrors)
            if not flip:
                corners = [corners[0], corners[2], corners[1]]
            self.tris.append((mat, corners, False))
            if mat in self.two_sided:  # PC two-sided material: also draw the inside
                self.tris.append((mat, [corners[0], corners[2], corners[1]], True))

    def bounds(self):
        ps = [c[0] for _, cs, _ in self.tris for c in cs]
        lo = [min(p[i] for p in ps) for i in range(3)]
        hi = [max(p[i] for p in ps) for i in range(3)]
        return lo, hi


def build_mdl(mesh):
    """An MDL (v6.2, with USER data) from a Mesh."""
    mats = []
    for m, _, _ in mesh.tris:
        if m not in mats:
            mats.append(m)
    # smooth normals per position (C1 models share vertices between faces)
    face_n = []
    acc = {}
    for m, cs, back in mesh.tris:
        n = norm(cross(sub(cs[1][0], cs[0][0]), sub(cs[2][0], cs[0][0])))
        face_n.append(n)
        for p, _ in cs:
            k = tuple(round(x, 5) for x in p) + (back,)
            a = acc.setdefault(k, [0.0, 0.0, 0.0])
            a[0] += n[0]; a[1] += n[1]; a[2] += n[2]
    out = st.Mdl()
    out.materials = mats
    out.flags = st.FLAG_USER
    verts, faces, groups = [], [], []
    user_faces = []
    for mi, mname in enumerate(mats):
        start = len(verts)
        index = {}
        tri_list = []
        for ti, (m, cs, back) in enumerate(mesh.tris):
            if m != mname:
                continue
            ids = []
            for p, uv in cs:
                k = tuple(round(x, 5) for x in p) + (back,)
                n = norm(acc[k])
                key = (k, round(uv[0], 5), round(uv[1], 5))
                if key not in index:
                    index[key] = len(verts)
                    verts.append((p[0], p[1], p[2], n[0], n[1], n[2], uv[0], uv[1], 0.0, 0.0, 128, 128, 128, 255))
                ids.append(index[key])
            faces.append((mi, 0, ids[0], ids[1], ids[2]))
            tri_list += [i - start for i in ids]
            user_faces.append((ti, ids, face_n[ti]))
        gv = verts[start:]
        lo = [min(v[i] for v in gv) for i in range(3)]
        hi = [max(v[i] for v in gv) for i in range(3)]
        centre = [(lo[i] + hi[i]) / 2 for i in range(3)]
        radius = max(math.dist(centre, v[:3]) for v in gv)
        groups.append({'centre': centre, 'radius': radius, 'min': lo, 'max': hi,
                       'strip_off': start, 'strip_count': 0, 'strip': [],
                       'list_off': start, 'list_count': len(gv), 'list': tri_list})
    out.verts, out.faces, out.groups = verts, faces, groups
    lo = [min(v[i] for v in verts) for i in range(3)]
    hi = [max(v[i] for v in verts) for i in range(3)]
    out.bmin, out.bmax = lo, hi
    out.centre = [(lo[i] + hi[i]) / 2 for i in range(3)]
    out.radius = max(math.dist(out.centre, v[:3]) for v in verts)
    out.user_verts, out.user_faces = len(verts), len(faces)
    # USER data: flags, verts (x,y,z,1), faces (137 bytes), PREP->USER face and vertex lookups
    w = st.Writer()
    w.u32(0)
    for v in verts:
        w.fs(v[:3]); w.u32(1)
    for fi, (mi, _, a, b, c) in enumerate(faces):
        va, vb, vc = verts[a], verts[b], verts[c]
        n = norm(cross(sub(vb[:3], va[:3]), sub(vc[:3], va[:3])))
        w.f32(-(n[0] * va[0] + n[1] * va[1] + n[2] * va[2])); w.fs(n)
        for v in (va, vb, vc):
            w.fs(v[3:6])
        w.u32(mi); w.u32(0); w.u32(a); w.u32(b); w.u32(c)
        for v in (va, vb, vc):
            w.raw(bytes(v[10:14]))
        for v in (va, vb, vc):
            w.fs(v[6:8]); w.fs(v[8:10])
        w.u8(0); w.u32(0)
    for i in range(len(faces)):
        w.u32(i)
    w.u32(len(verts))
    for i in range(len(verts)):
        w.u32(i)
    out.tail = bytes(w.b)
    return out


# ---------------------------------------------------------------------------------------------
# Textures and materials
# ---------------------------------------------------------------------------------------------
def bleed(w, h, rgba):
    """Gives transparent pixels the colour of a neighbouring opaque one (repeatedly), so texture
    filtering at the edge of a see-through area doesn't blend in black."""
    px = bytearray(rgba)
    solid = [px[i * 4 + 3] >= 128 for i in range(w * h)]
    if all(solid) or not any(solid):
        return bytes(px)
    for _ in range(8):
        grown = []
        for y in range(h):
            for x in range(w):
                i = y * w + x
                if solid[i]:
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    j = ((y + dy) % h) * w + (x + dx) % w
                    if solid[j]:
                        px[i * 4:i * 4 + 3] = px[j * 4:j * 4 + 3]
                        grown.append(i)
                        break
        if not grown:
            break
        for i in grown:
            solid[i] = True
    return bytes(px)


def write_img(path, w, h, rgba):
    """Solid textures: IMG v1.0, one uncompressed A,R,G,B plane (drawn as normal opaque surfaces).
    Textures with transparent pixels (two-sided PC materials only): 4 RLE planes with the one-bit-alpha
    flag (0x04), as the game's own see-through textures."""
    if any(a < 128 for a in rgba[3::4]):
        import uiimg
        uiimg.write_img(path, w, h, rgba, basic=0x06)
        return
    data = bytearray(len(rgba))
    data[0::4], data[1::4], data[2::4], data[3::4] = rgba[3::4], rgba[0::4], rgba[1::4], rgba[2::4]
    hdr = b'IMAGEMAP' + bytes((0, 1, 0, 0)) + struct.pack('<IIHH', 0, len(data), w, h)
    open(path, 'wb').write(hdr + bytes(data))


class PixelmapSource:
    """All named pixelmaps in the car's PIX files."""
    def __init__(self, data_dirs, pix_files, palette):
        self.maps = {}
        self.palette = palette
        for f in pix_files:
            for d in data_dirs:
                p = os.path.join(d, 'PIXELMAP', f)
                if os.path.exists(p):
                    for name, m in read_all_pix(p).items():
                        self.maps.setdefault(name, m)
                    break

    def rgba(self, name):
        m = self.maps.get(name.upper())
        if not m:
            return None
        w, h, row, px, own_palette = m
        palette = own_palette or self.palette
        out = bytearray()
        for y in range(h):
            for x in range(w):
                i = px[y * row + x]
                r, g, b = palette[i] if i < len(palette) else (0, 0, 0)
                out += bytes((r, g, b, 0 if i == 0 else 255))
        return w, h, out


def read_all_pix(path):
    """name -> (w, h, row bytes, 8-bit pixels, own palette or None) for every pixelmap in a PIX file.
    A pixelmap may carry its own palette as a nested 1x256 pixelmap (header 0x03 type 7, its data 0x21,
    then 0x22), before its own pixel data (0x21)."""
    data = open(path, 'rb').read()
    res = {}
    p = 0
    cur = None       # [name, w, h, row, type, palette]
    child = None     # nested palette header
    while p + 8 <= len(data):
        cid, ln = struct.unpack_from('>II', data, p)
        b = data[p + 8:p + 8 + ln]
        p += 8 + ln
        if cid in (0x03, 0x3D):
            typ, row, w, h = struct.unpack_from('>BHHH', b, 0)
            name = b[11:].split(b'\0')[0].decode('latin1')
            if cur is None:
                cur = [name.upper(), w, h, row, typ, None]
            else:
                child = (typ, w, h)
        elif cid == 0x21 and cur:
            count, esize = struct.unpack_from('>II', b, 0)
            px = b[8:8 + count * esize]
            if child is not None:
                if child[0] == 7 and esize == 4:  # palette: x, r, g, b
                    cur[5] = [(px[i * 4 + 1], px[i * 4 + 2], px[i * 4 + 3]) for i in range(min(256, count))]
            else:
                name, w, h, row, typ, pal = cur
                if typ == 3:
                    res[name] = (w, h, row, px, pal)
                cur = None
        elif cid == 0x22:
            child = None
        elif cid == 0:
            cur, child = None, None
    return res


def mtl_bytes(template, texture):
    """A one-texture material from a one-texture template MTL, with the texture name replaced."""
    r = st.Reader(template)
    head = r.take(2)
    if r.u32() != 1:
        raise ValueError('template must have one texture')
    r.pstr()
    rest = template[r.p:]
    w = st.Writer()
    w.raw(head)
    w.u32(1)
    w.pstr(texture)
    w.raw(rest)
    return bytes(w.b)


def parse_mtl(data):
    """MTL: 2 header bytes, u32 texture count, per texture: name (pstr) + 30 bytes of stage settings (the first
    u32 is the stage's use: 0 texture, 1 environment map, 8 shine mask), then 63 bytes."""
    r = st.Reader(data)
    head = r.take(2)
    stages = []
    for _ in range(r.u32()):
        name = r.pstr()
        stages.append((name, r.take(30)))
    return head, stages, data[r.p:]


def mtl_with_reflection(template, car_template, texture):
    """A one-texture material with the game's car reflection: stage 1 "env" (the track's environment map) and
    stage 2 "<texture>_s" (the shine mask), those stages' settings from a stock car's body material, and the
    car's lighting settings (with them the game builds the meshes with the normals the reflection uses)."""
    head, one, _ = parse_mtl(template)
    _, car, trailer = parse_mtl(car_template)
    if len(one) != 1 or len(car) != 3:
        raise ValueError('unexpected material templates')
    w = st.Writer()
    w.raw(head)
    w.u32(3)
    for name, settings in ((texture, one[0][1]), ('env', car[1][1]), (texture + '_s', car[2][1])):
        w.pstr(name)
        w.raw(settings)
    w.raw(trailer)
    return bytes(w.b)


SHINE = 0.35  # shine mask brightness relative to the texture (the game's own masks average ~13-45/255)


def shine_mask(w, h, rgba):
    """The game's shine masks look like their cars' skins darkened (brighter on metal) and are smooth: the
    texture at SHINE, averaged down to a quarter size (the PC textures' dithering would sparkle)."""
    import uiimg
    mw, mh = max(1, w // 4), max(1, h // 4)
    m = bytearray(uiimg.resize(w, h, rgba, mw, mh))
    for i in range(0, len(m), 4):
        m[i] = int(m[i] * SHINE); m[i + 1] = int(m[i + 1] * SHINE); m[i + 2] = int(m[i + 2] * SHINE)
    return mw, mh, bytes(m)


def stretch_shape(txt, a, b, lo, hi, template_dir, cover=None):
    """The template's shape block (txt[a:b]) with its points stretched from the template's body to the new
    one: across and along the car, and upwards from the shape's own lowest point (so it keeps the template's
    clearance above the wheel mounts)."""
    tb = st.read_mdl(open(os.path.join(template_dir, 'CARBODY.MDL'), 'rb').read())
    lines = txt[a:b].split('\n')
    if lines and lines[-1] == '':
        lines.pop()

    def point(l):
        f = l.split(',')
        if len(f) != 3:
            return None
        try:
            return [float(x) for x in f]
        except ValueError:
            return None

    pts = [p for p in (point(l) for l in lines) if p]
    if not pts:
        raise ValueError('template CAR.TXT has no shape points')
    floor_y = min(p[1] for p in pts)
    sx = (hi[0] - lo[0]) / (tb.bmax[0] - tb.bmin[0])
    sz = (hi[2] - lo[2]) / (tb.bmax[2] - tb.bmin[2])
    sy = (hi[1] - floor_y) / max(0.1, tb.bmax[1] - floor_y)
    new = [[p[0] * sx, floor_y + (p[1] - floor_y) * sy, lo[2] + (p[2] - tb.bmin[2]) * sz] for p in pts]
    solid, in_solid = [], False  # the point belongs to a Rounded* form (not a wireframe)
    for l in lines:
        if l.startswith('Rounded'):
            in_solid = True
        elif l.startswith('wireframe'):
            in_solid = False
        if point(l):
            solid.append(in_solid)
    if cover:  # (tyre outer x, tyre z min, tyre z max): the solid forms out to the tyres' outer edges and ends
        tx, tz0, tz1 = cover
        sp = [p for p, s in zip(new, solid) if s]
        x = max(abs(p[0]) for p in sp)
        z0, z1 = min(p[2] for p in sp), max(p[2] for p in sp)
        fx = max(1.0, tx / x) if x > 0 else 1.0
        nz0, nz1 = min(z0, tz0), max(z1, tz1)
        for p in sp:
            p[0] *= fx
            if z1 > z0:
                p[2] = nz0 + (p[2] - z0) * (nz1 - nz0) / (z1 - z0)
    out = []
    it = iter(new)
    for l in lines:
        out.append('%f,%f,%f' % tuple(next(it)) if point(l) else l)
    return '\n'.join(out) + '\n'


# ---------------------------------------------------------------------------------------------
def convert(data_dirs, car_txt, out_dir, template_dir, mtl_template, car_mtl_template, physics_dir=None,
            cover_wheels=False):
    """template_dir: an Android vehicle whose CAR.TXT is used; physics_dir: one whose CAR.TXT [DYNAMICS]
    (handling, collision shape) replaces the template's (default: the template's own)."""
    physics_dir = physics_dir or template_dir
    lines = c1text.data_lines(find(data_dirs, 'CARS', car_txt))
    car = lines[0].split()[0].upper().replace('.TXT', '')
    # file lists: find "Number of pixelmap files" etc. by structure: the first list after the grid images
    i = next(k for k, l in enumerate(lines)
             if l.count(',') == 2 and all(x.strip().upper().endswith('.PIX') for x in l.split(','))) + 1  # grid images
    def take_list():
        nonlocal i
        n = int(lines[i]); i += 1
        items = lines[i:i + n]; i += n
        return items
    pix = take_list(); take_list(); take_list()        # three detail levels
    take_list()                                        # shade tables
    mats = take_list(); take_list(); take_list()
    dats = take_list()
    nact = int(lines[i]); i += 1
    acts = [lines[i + k].split(',') for k in range(nact)]
    actor_file = next(a[1].strip() for a in acts if a[0].strip() == '0')
    print(car, 'actor', actor_file, 'models', dats, 'materials', mats, 'pixelmaps', pix)

    models = {}
    for d in dats:
        models.update(br.read_dat(find(data_dirs, 'MODELS', d)))
    materials = {}
    for m in mats:
        materials.update(br.read_mat(find(data_dirs, 'MATERIAL', m)))
    palette = br.read_palette(find(data_dirs, os.path.join('REG', 'PALETTES'), 'DRRENDER.PAL'))
    pixmaps = PixelmapSource(data_dirs, pix, palette)
    root = br.read_act(find(data_dirs, 'ACTORS', actor_file))[0]

    os.makedirs(out_dir, exist_ok=True)
    prefix = car.lower()[:6]
    used_mats = {}

    def mat_name(pc_name):
        key = pc_name.upper()
        if key not in used_mats:
            used_mats[key] = '%s_%s' % (prefix, key.replace('.MAT', '').lower())
        return used_mats[key]

    two_sided = {'%s_%s' % (prefix, k.upper().replace('.MAT', '').lower()) for k, m in materials.items() if m.flags & 0x1000}
    inset = {}
    for k, m in materials.items():
        if m.texture:
            t = pixmaps.rgba(m.texture)
            if t and any(a < 128 for a in t[2][3::4]):
                inset['%s_%s' % (prefix, k.upper().replace('.MAT', '').lower())] = (t[0], t[1])
    body = Mesh(two_sided, inset)
    wheels = {}  # android node name -> (world matrix, Mesh)

    def visit(actor, parent_m, inherited_mat):
        m = mat_mul(actor.matrix, parent_m)
        mat = actor.material or inherited_mat
        wheel = WHEELS.get(actor.name.upper())
        if actor.model and actor.model.upper() in models:
            model = models[actor.model.upper()]
            default = mat_name(mat) if mat else mat_name('DEFAULT')
            if wheel:
                mesh = Mesh(two_sided, inset)
                rot = m[:9] + [0.0, 0.0, 0.0]
                mesh.add_model(model, rot, mat_name, default)
                wheels[wheel] = (m, mesh)
            else:
                body.add_model(model, m, mat_name, default)
        for c in actor.children:
            visit(c, m, mat)

    visit(root, [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0], None)
    print('body', len(body.tris), 'triangles; wheels', {k: len(v[1].tris) for k, v in wheels.items()})

    open(os.path.join(out_dir, 'CARBODY.MDL'), 'wb').write(st.write_mdl(build_mdl(body)))
    cnt = st.Node('carbody', 'MODL', model='carbody')
    for name in ('whlFL', 'whlFR', 'whlRL', 'whlRR'):
        if name not in wheels:
            continue
        m, mesh = wheels[name]
        open(os.path.join(out_dir, name.upper() + '.MDL'), 'wb').write(st.write_mdl(build_mdl(mesh)))
        pos = to_android((m[9], m[10], m[11]))
        cnt.children.append(st.Node(name, 'MODL', [1, 0, 0, 0, 1, 0, 0, 0, 1, pos[0], pos[1], pos[2]], model=name))
    open(os.path.join(out_dir, 'CARBODY.CNT'), 'wb').write(st.write_cnt(cnt))

    # materials and textures
    tmpl = open(mtl_template, 'rb').read()
    car_tmpl = open(car_mtl_template, 'rb').read()
    for pc_name, name in used_mats.items():
        m = materials.get(pc_name) or materials.get(pc_name.replace('.MAT', '').upper())
        tex = None
        if m and m.texture:
            tex = pixmaps.rgba(m.texture)
            if not tex:
                print('  missing pixelmap', m.texture, 'for', pc_name)
        if not tex:  # untextured: a small solid texture from the material colour
            c = m.colour if m else (160, 160, 160, 255)
            if c[:3] == (0, 0, 0) and m:
                c = palette[min(255, (m_index_base(m) or 0))] + (255,)
            tex = (8, 8, bytes(c[:3]) * 0 + bytes((c[0], c[1], c[2], 255)) * 64)
        w, h, rgba = tex
        rgba = bleed(w, h, rgba)
        write_img(os.path.join(out_dir, name.upper() + '.IMG'), w, h, rgba)
        if any(a < 128 for a in rgba[3::4]):  # see-through: no reflection
            open(os.path.join(out_dir, name.upper() + '.MTL'), 'wb').write(mtl_bytes(tmpl, name))
        else:  # reflection of the track's environment map, as on the game's own cars
            write_img(os.path.join(out_dir, name.upper() + '_S.IMG'), *shine_mask(w, h, rgba))
            open(os.path.join(out_dir, name.upper() + '.MTL'), 'wb').write(mtl_with_reflection(tmpl, car_tmpl, name))
    print('materials', len(used_mats))

    # CAR.TXT from the template car (its handling and collision shape), the shape stretched to the new body
    lo, hi = body.bounds()
    txt = open(os.path.join(template_dir, 'CAR.TXT'), encoding='latin1').read()
    if physics_dir != template_dir:  # [DYNAMICS] (up to the next section) from the physics car
        phys = open(os.path.join(physics_dir, 'CAR.TXT'), encoding='latin1').read()

        def dynamics(t):
            f = t.index('[DYNAMICS]')
            e = t.find('\n[', f)
            return f, (len(t) if e < 0 else e + 1)
        f0, t0 = dynamics(txt)
        f1, t1 = dynamics(phys)
        txt = txt[:f0] + phys[f1:t1] + txt[t0:]
    # The checksum after the shape is set to -1 (as in cars whose shapes were edited by hand), since it changed.
    a = txt.index('<Shape>')
    b = txt.index('<ShapeCheckSum>')
    e = txt.index('\n', txt.index('\n', b) + 1)
    cover = None
    if cover_wheels and wheels:  # the shape takes in the tyres (huge wheels would run over peds untouched)
        spots = []
        for m, mesh in wheels.values():
            pos = to_android((m[9], m[10], m[11]))
            wlo, whi = mesh.bounds()
            spots.append((abs(pos[0]) + (whi[0] - wlo[0]) / 2, pos[2] + wlo[2], pos[2] + whi[2]))
        cover = (max(s[0] for s in spots), min(s[1] for s in spots), max(s[2] for s in spots))
    txt = txt[:a] + stretch_shape(txt, a, b, lo, hi, physics_dir, cover) + '<ShapeCheckSum>\n-1' + txt[e:]
    open(os.path.join(out_dir, 'CAR.TXT'), 'w', encoding='latin1', newline='\n').write(txt)
    print('bounds', [round(x, 2) for x in lo], [round(x, 2) for x in hi])


def m_index_base(m):
    return None


def find(data_dirs, sub, name):
    for d in data_dirs:
        p = os.path.join(d, sub, name)
        if os.path.exists(p):
            return p
    raise FileNotFoundError(os.path.join(sub, name))


if __name__ == '__main__':
    pc_dirs = sys.argv[1].split(';')
    convert(pc_dirs, sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6],
            sys.argv[7] if len(sys.argv) > 7 else None)

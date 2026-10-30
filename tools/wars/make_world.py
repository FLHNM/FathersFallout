"""Wasteland Wars: builds the A-Life world graph from the player's own Fallout4.esm.

  python -I make_world.py <Data folder> <out.json>

Every exterior map marker of the Commonwealth becomes a node (position, name, marker type). For each node the
NPCs the game itself placed nearby are counted by faction (from their base record's editor ID: raider, gunner,
super mutant, ...), which is where the server's squads live. No game files are copied: only positions, names and
counts go into the JSON. Each node also gets up to 12 spawn spots: where the game placed its NPCs (on the ground, on
navmesh; a map marker can be on a roof), with those references, which the Wars client also removes in Wars worlds.

Camps: from the landscape's height map (LAND) and the size of everything the game placed (object bounds), each place
with spawn spots gets a camp: a middle on open, flat, dry ground near where its NPCs are (never on a floor inside a
house), and spots for the camp's things, each with the ground's exact height there: along the walls of the buildings
around it (backs to the wall, facing out), seats around the middle (the fire), and its edge (facing out).
The game's own furniture comes first: a camp's middle is where the most beds, seats, workbenches and cooking spots
the game placed are within the NPCs' sandbox reach, and the camp only builds what the place lacks.
A wall is where the game's navmesh ends (an edge with no triangle beyond it) with something solid right behind it:
an object's bounds alone gave walls in thin air (under a canopy, its box reaches the ground).
"""
import json
import math
import os
import struct
import sys
import zlib

DATA_DIR, OUT = sys.argv[1], sys.argv[2]
ESM = os.path.join(DATA_DIR, 'Fallout4.esm')
COMMONWEALTH = 0x3C
MAPMARKER = 0x10
WORKBENCH = 0x000C1AEB   # WorkshopWorkbench: a settlement's own workbench (captured places get it)
LAIR_RADIUS = 6000.0   # placed NPCs this close to a marker belong to it

# Editor-ID fragments -> A-Life faction. First match wins.
FACTIONS = [
    ('raider', ('Raider',)),
    ('gunner', ('Gunner',)),
    ('mutant', ('SuperMutant', 'Supermutant', 'Behemoth', 'MutantHound')),
    ('feral', ('FeralGhoul', 'Feral')),
    ('mirelurk', ('Mirelurk',)),
    ('deathclaw', ('Deathclaw',)),
    ('creature', ('Molerat', 'RadRoach', 'Radroach', 'Bloatfly', 'Stingwing', 'Bloodbug', 'Yaoguai', 'RadScorpion',
                  'Radscorpion', 'MirelurkHunter', 'Mongrel', 'Dog', 'Brahmin', 'Radstag')),
    ('synth', ('Synth',)),
    ('robot', ('Assaultron', 'Protectron', 'Sentry', 'Robobrain', 'Eyebot', 'Turret')),
    ('brotherhood', ('BoS', 'Brotherhood')),
    ('minutemen', ('Minutemen',)),
    ('railroad', ('Railroad',)),
]

# Base records whose placed references take up ground (their bounds = their footprint).
SOLID = (b'STAT', b'SCOL', b'FURN', b'CONT', b'ACTI', b'MSTT', b'DOOR', b'TERM', b'FLOR', b'TREE', b'PKIN', b'MISC')
CELL_SIZE = 4096.0

data = open(ESM, 'rb').read()


def load_strings():
    names = {}
    path = os.path.join(DATA_DIR, 'Strings', 'Fallout4_en.STRINGS')
    if os.path.exists(path):
        sb = open(path, 'rb').read()
    else:
        sb = ba2_file(os.path.join(DATA_DIR, 'Fallout4 - Interface.ba2'), 'strings/fallout4_en.strings')
        if sb is None:
            return names
    cnt = struct.unpack_from('<I', sb, 0)[0]
    base = 8 + cnt * 8
    for k in range(cnt):
        sid, off = struct.unpack_from('<II', sb, 8 + k * 8)
        end = sb.index(bytes(1), base + off)
        names[sid] = sb[base + off:end].decode('cp1252', 'replace')
    return names


def ba2_file(archive, want):
    """One file out of a general (GNRL) BA2 archive, by its lower-case path; None if missing."""
    if not os.path.exists(archive):
        return None
    d = open(archive, 'rb').read()
    _, _, typ, cnt, name_table = struct.unpack_from('<4sI4sIQ', d, 0)
    if typ != b'GNRL':
        return None
    i = name_table
    for k in range(cnt):
        n = struct.unpack_from('<H', d, i)[0]
        name = d[i + 2:i + 2 + n].decode('latin-1').lower().replace(chr(92), '/')
        i += 2 + n
        if name != want:
            continue
        _, _, _, _, off, packed, unpacked, _ = struct.unpack_from('<I4sIIQIII', d, 24 + k * 36)
        raw = d[off:off + (packed or unpacked)]
        return zlib.decompress(raw) if packed else raw
    return None


def subs(buf):
    i, big = 0, None
    while i + 6 <= len(buf):
        t, n = buf[i:i + 4], struct.unpack_from('<H', buf, i + 4)[0]
        if t == b'XXXX':
            big = struct.unpack_from('<I', buf, i + 6)[0]
            i += 6 + n
            continue
        if big is not None:
            n, big = big, None
        yield t, buf[i + 6:i + 6 + n]
        i += 6 + n


edids = {}      # NPC_/LVLN form id -> editor id
markers = []    # (fid, x, y, z, type, full)
placed = []     # (ref fid, base fid, x, y, z)
bounds = {}     # solid base fid -> its bounds box (x1, y1, z1, x2, y2, z2) and whether it's a tree (its trunk)
refs = []       # (base fid, x, y, z, z rotation, scale) of every placed reference in the Commonwealth
cells = {}      # exterior cell fid -> (grid x, grid y, water height or None)
heights = {}    # (grid x, grid y) -> 33x33 ground heights
furn_kind = {}  # furniture base fid -> 'bed' | 'seat' | 'bench' | 'cook'
furniture = []  # (kind, x, y, z) of the game's placed furniture in the Commonwealth
workbenches = []   # (ref fid, x, y, z) of the settlements' workbenches
borders = {}    # bucket -> [(ax, ay, bx, by, z, inward x, inward y)]: navmesh edges with nothing beyond (walkable side in)
water_default = [0.0]


def furniture_kind(edid):
    """What NPCs do with a piece of the game's furniture, from its editor ID (None: not for a camp)."""
    if 'workshop' in edid and 'workbench' in edid and 'weapon' not in edid and 'armor' not in edid:
        return None   # the settlement's own workbench: players build with it
    if 'cooking' in edid or 'cookpot' in edid or 'stove' in edid:
        return 'cook'
    if 'workbench' in edid or 'chemistry' in edid or 'chemlab' in edid:
        return 'bench'
    if 'bed' in edid or 'sleepingbag' in edid or 'mattress' in edid or 'cot' in edid:
        return 'bed'
    if any(k in edid for k in ('chair', 'stool', 'bench', 'couch', 'sofa', 'sit', 'seat', 'lean')):
        return 'seat'
    return None


def record(t, flags, fid, body, world, cell):
    if flags & 0x00040000:
        body = zlib.decompress(body[4:])
    if t in SOLID:
        edid, box = '', None
        for st, sd in subs(body):
            if st == b'EDID':
                edid = sd.rstrip(bytes(1)).decode('cp1252', 'replace').lower()
            elif st == b'OBND' and len(sd) >= 12:
                box = struct.unpack_from('<6h', sd)
                break
        if t == b'FURN':
            kind = furniture_kind(edid)
            if kind:
                furn_kind[fid] = kind
        if box is None or any(k in edid for k in ('shrub', 'bush', 'grass', 'weed', 'fern', 'vine', 'ivy', 'decal')):
            return
        x1, y1, z1, x2, y2, z2 = box
        tree = t == b'TREE' or 'tree' in edid
        if tree:
            box = (-40, -40, z1, 40, 40, z2)   # the trunk: the canopy is overhead
        elif 0.5 * math.hypot(x2 - x1, y2 - y1) < 50:
            return            # clutter: cans, debris
        elif z2 - z1 < 60 and not (z2 - z1 < 30 and min(x2 - x1, y2 - y1) >= 128):
            return            # low rocks and junk (flat and wide = a road or a floor: ground, below)
        bounds[fid] = tuple(box) + (tree,)
        return
    if t == b'WRLD':
        if fid == COMMONWEALTH:
            for st, sd in subs(body):
                if st == b'DNAM' and len(sd) >= 8:
                    water_default[0] = struct.unpack_from('<f', sd, 4)[0]
        return
    if t == b'CELL':
        if world != COMMONWEALTH:
            return
        grid, water = None, None
        for st, sd in subs(body):
            if st == b'XCLC' and len(sd) >= 8:
                grid = struct.unpack_from('<ii', sd)
            elif st == b'XCLW' and len(sd) >= 4:
                water = struct.unpack_from('<f', sd)[0]
        if grid is not None:
            cells[fid] = (grid[0], grid[1], water)
        return
    if t == b'NAVM':
        if cell not in cells:
            return
        for st, sd in subs(body):
            if st != b'NVNM' or len(sd) < 24 or struct.unpack_from('<I', sd)[0] != 15:
                continue
            nv = struct.unpack_from('<I', sd, 16)[0]
            vs = [struct.unpack_from('<3f', sd, 20 + k * 12) for k in range(nv)]
            o = 20 + nv * 12
            nt = struct.unpack_from('<I', sd, o)[0]
            o += 4
            if o + nt * 21 > len(sd):
                return
            for k in range(nt):
                tri = struct.unpack_from('<3h3h', sd, o + k * 21)
                if any(v < 0 or v >= nv for v in tri[:3]):
                    continue
                for e in range(3):
                    if tri[3 + e] != -1:
                        continue
                    a, b, c = vs[tri[e]], vs[tri[(e + 1) % 3]], vs[tri[(e + 2) % 3]]
                    ex, ey = b[0] - a[0], b[1] - a[1]
                    ln = math.hypot(ex, ey)
                    if ln < 40:
                        continue
                    nx, ny = -ey / ln, ex / ln   # toward the triangle's own (walkable) side
                    if (c[0] - a[0]) * nx + (c[1] - a[1]) * ny < 0:
                        nx, ny = -nx, -ny
                    mx, my = 0.5 * (a[0] + b[0]), 0.5 * (a[1] + b[1])
                    borders.setdefault((math.floor(mx / 512.0), math.floor(my / 512.0)), []).append(
                        (a[0], a[1], b[0], b[1], 0.5 * (a[2] + b[2]), nx, ny))
        return
    if t == b'LAND':
        if cell not in cells:
            return
        for st, sd in subs(body):
            if st == b'VHGT' and len(sd) >= 4 + 33 * 33:
                off = struct.unpack_from('<f', sd)[0]
                g = struct.unpack_from('<1089b', sd, 4)
                h, row = [], 0.0
                for r in range(33):
                    row += g[r * 33]
                    col = row
                    line = [col]
                    for c in range(1, 33):
                        col += g[r * 33 + c]
                        line.append(col)
                    h.append([(off + v) * 8.0 for v in line])
                heights[cells[cell][:2]] = h
        return
    if t in (b'NPC_', b'LVLN'):
        for st, sd in subs(body):
            if st == b'EDID':
                edids[fid] = sd.rstrip(bytes(1)).decode('cp1252', 'replace')
                break
        return
    if world != COMMONWEALTH:
        return
    base = pos = mtype = full = None
    scale, rz = 1.0, 0.0
    for st, sd in subs(body):
        if st == b'XSCL':
            scale = struct.unpack_from('<f', sd)[0]
        elif st == b'NAME':
            base = struct.unpack_from('<I', sd)[0]
        elif st == b'DATA':
            pos = struct.unpack_from('<3f', sd)
            if len(sd) >= 24:
                rz = struct.unpack_from('<f', sd, 20)[0]
        elif st == b'TNAM':
            mtype = sd[0]
        elif st == b'FULL':
            full = struct.unpack_from('<I', sd)[0]
    if base is None or pos is None:
        return
    if t == b'REFR':
        refs.append((base, pos[0], pos[1], pos[2], rz, scale))
        if base in furn_kind:
            furniture.append((furn_kind[base], pos[0], pos[1], pos[2]))
        if base == WORKBENCH:
            workbenches.append((fid, pos[0], pos[1], pos[2]))
    if t == b'REFR' and base == MAPMARKER:
        markers.append((fid, pos[0], pos[1], pos[2], mtype, full))
    elif t == b'ACHR':
        placed.append((fid, base, pos[0], pos[1], pos[2]))


def walk(start, end, world, cell=None):
    i = start
    while i < end:
        t = data[i:i + 4]
        size = struct.unpack_from('<I', data, i + 4)[0]
        if t == b'GRUP':
            label = struct.unpack_from('<I', data, i + 8)[0]
            gtype = struct.unpack_from('<I', data, i + 12)[0]
            if gtype == 0 and data[i + 8:i + 12] not in (b'WRLD', b'NPC_', b'LVLN') + SOLID:
                i += size
                continue
            w = label if gtype == 1 else world
            c = label if gtype == 6 else cell
            if gtype == 0 or w == COMMONWEALTH or gtype == 1:
                walk(i + 24, i + size, w, c)
            i += size
            continue
        flags, fid = struct.unpack_from('<II', data, i + 8)
        if t in (b'REFR', b'ACHR', b'NPC_', b'LVLN', b'CELL', b'LAND', b'NAVM', b'WRLD') + SOLID:
            record(t, flags, fid, data[i + 24:i + 24 + size], world, cell)
        i += 24 + size


def faction_of(edid):
    for name, frags in FACTIONS:
        if any(f in edid for f in frags):
            return name
    return None


walk(0, len(data), None)
names = load_strings()
nodes = []
for fid, x, y, z, mtype, full in markers:
    nodes.append({'ref': fid, 'name': names.get(full, ''), 'x': round(x), 'y': round(y), 'z': round(z),
                  'type': mtype, 'residents': {}, 'spawns': []})
SPAWN_RADIUS = 3000.0   # spawn spots: where the game put a place's NPCs, this close to its marker
SPAWN_MAX = 12
for ref, base, x, y, z in placed:
    fac = faction_of(edids.get(base, ''))
    if not fac:
        continue
    best, bd = None, LAIR_RADIUS
    for n in nodes:
        d = math.hypot(n['x'] - x, n['y'] - y)
        if d < bd:
            best, bd = n, d
    if best is None:
        continue
    best['residents'][fac] = best['residents'].get(fac, 0) + 1
    # A spot on the ground the game itself used (map markers can sit on roofs - Sanctuary's does).
    if bd < SPAWN_RADIUS and len(best['spawns']) < SPAWN_MAX and             all(math.hypot(sx - x, sy - y) > 250 for _, sx, sy, _ in best['spawns']):
        best['spawns'].append([ref, round(x), round(y), round(z)])


# --- Camps: open, flat, dry ground ---

def ground(x, y):
    """The landscape's height at x, y (bilinear between its vertices, 128 units apart); None outside the map."""
    gx, gy = math.floor(x / CELL_SIZE), math.floor(y / CELL_SIZE)
    h = heights.get((gx, gy))
    if h is None:
        return None
    fx, fy = (x - gx * CELL_SIZE) / 128.0, (y - gy * CELL_SIZE) / 128.0
    c, r = min(int(fx), 31), min(int(fy), 31)
    tx, ty = fx - c, fy - r
    a = h[r][c] * (1 - tx) + h[r][c + 1] * tx
    b = h[r + 1][c] * (1 - tx) + h[r + 1][c + 1] * tx
    return a * (1 - ty) + b * ty


water_at = {}
for _, (gx, gy, w) in cells.items():
    water_at[(gx, gy)] = w if w is not None and -1e6 < w < 1e6 else water_default[0]

BUCKET = 512.0
solid = {}   # bucket -> [(box centre x, y, cos, sin, half x, half y, radius)]: what stands on the ground
flats = {}   # bucket -> [(the same, top height)]: roads, pavements, floors - the ground where they lie
for base, x, y, z, rz, scale in refs:
    b = bounds.get(base)
    if b is None:
        continue
    x1, y1, z1, x2, y2, z2 = (v * scale for v in b[:6])
    hx, hy = 0.5 * (x2 - x1), 0.5 * (y2 - y1)
    # The game turns references clockwise (heading): local (u, v) -> world (u cos + v sin, -u sin + v cos).
    c, sn = math.cos(rz), math.sin(rz)
    ou, ov = 0.5 * (x1 + x2), 0.5 * (y1 + y2)
    cx, cy = x + ou * c + ov * sn, y - ou * sn + ov * c
    # A wall: something tall and long enough to put a bench or a bed against (a building, a wall, a container).
    wall = not b[6] and z2 - z1 >= 150 and max(hx, hy) >= 100
    box = (cx, cy, c, sn, hx, hy, math.hypot(hx, hy), wall)
    key = (math.floor(cx / BUCKET), math.floor(cy / BUCKET))
    g = ground(x, y)
    if z2 - z1 < 30:   # flat: walked on, not in the way (a bridge deck far above stays out)
        if g is not None and -60 < z + z2 - g < 250 and box[6] < 3000:
            flats.setdefault(key, []).append(box[:7] + (z + z2,))
        continue
    if box[6] > 1400:   # scenery (cliffs, rock ranges): the slope test's job
        continue
    if g is not None and z + z1 > g + 220:   # high up (a sign, a bridge deck): not in the way on the ground
        continue
    solid.setdefault(key, []).append(box)
BIGGEST = max((e[6] for v in solid.values() for e in v), default=0.0)
BIGGEST_FLAT = max((e[6] for v in flats.values() for e in v), default=0.0)


def box_distance(x, y, e):
    """Distance from x, y to a turned box (negative inside)."""
    cx, cy, c, sn, hx, hy = e[:6]
    dx, dy = x - cx, y - cy
    u, v = abs(dx * c - dy * sn), abs(dx * sn + dy * c)   # world -> the box's own axes
    ex, ey = max(u - hx, 0.0), max(v - hy, 0.0)
    return math.hypot(ex, ey) if ex > 0 or ey > 0 else -min(hx - u, hy - v)


def surface(x, y):
    """What you stand on at x, y: the landscape, or a road / pavement / floor lying on it."""
    g = ground(x, y)
    if g is None:
        return None
    top = g
    k = int(math.ceil(BIGGEST_FLAT / BUCKET))
    bx, by = math.floor(x / BUCKET), math.floor(y / BUCKET)
    for i in range(bx - k, bx + k + 1):
        for j in range(by - k, by + k + 1):
            for e in flats.get((i, j), ()):
                if e[7] > top and math.hypot(x - e[0], y - e[1]) <= e[6] and box_distance(x, y, e) <= 0:
                    top = e[7]
    return top


def clearance(x, y, upto):
    """How far from x, y to the nearest thing on the ground (its turned bounds box), up to `upto`."""
    best = upto
    k = int(math.ceil((upto + min(BIGGEST, 2000.0)) / BUCKET))
    bx, by = math.floor(x / BUCKET), math.floor(y / BUCKET)
    for i in range(bx - k, bx + k + 1):
        for j in range(by - k, by + k + 1):
            for e in solid.get((i, j), ()):
                if math.hypot(x - e[0], y - e[1]) - e[6] >= best:
                    continue
                d = box_distance(x, y, e)
                if d < best:
                    best = d
    return best


def open_ground(x, y, radius, flat):
    """Height there if x, y is dry ground with nothing within `radius` and no more than `flat` up/down, else None."""
    g = ground(x, y)
    if g is None:
        return None
    w = water_at.get((math.floor(x / CELL_SIZE), math.floor(y / CELL_SIZE)), water_default[0])
    if g < w + 40:
        return None
    for a in range(8):
        px, py = x + math.cos(a * 0.785) * radius, y + math.sin(a * 0.785) * radius
        pg = ground(px, py)
        if pg is None or abs(pg - g) > flat or pg < w + 20:
            return None
    if clearance(x, y, radius) < radius:
        return None
    return surface(x, y)


def solid_behind(x, y):
    """A wall-like thing (tall, long; not a tree) within 45 units of x, y."""
    k = int(math.ceil((45 + min(BIGGEST, 2000.0)) / BUCKET))
    bx, by = math.floor(x / BUCKET), math.floor(y / BUCKET)
    for i in range(bx - k, bx + k + 1):
        for j in range(by - k, by + k + 1):
            for e in solid.get((i, j), ()):
                if e[7] and math.hypot(x - e[0], y - e[1]) - e[6] < 45 and box_distance(x, y, e) < 45:
                    return True
    return False


def blocked_flat(x, y, z):
    """Beyond a navmesh edge at x, y: the ground there is no lower and not water, so what stops you is a thing
    (a building the bounds don't show: combined models, the game's large ones)."""
    g = ground(x, y)
    if g is None or g < z - 30:
        return False
    w = water_at.get((math.floor(x / CELL_SIZE), math.floor(y / CELL_SIZE)), water_default[0])
    return g > w + 20


def wall_spots(mx, my, mz, reach):
    """Spots along the walls near the middle: 45 units in from where the navmesh ends against something solid, with
    room in front, flat, near the middle's height. Each: x, y, z, heading (the game's: 0 = north, clockwise) facing
    away from the wall."""
    out = []
    k = int(math.ceil((reach + 600) / 512.0))
    bx, by = math.floor(mx / 512.0), math.floor(my / 512.0)
    for i in range(bx - k, bx + k + 1):
        for j in range(by - k, by + k + 1):
            for ax, ay, bx2, by2, ez, nx, ny in borders.get((i, j), ()):
                ln = math.hypot(bx2 - ax, by2 - ay)
                n = max(1, int(ln // 130))
                for q in range(n):
                    t = (q + 0.5) / n
                    ex, ey = ax + (bx2 - ax) * t, ay + (by2 - ay) * t
                    x, y = ex + nx * 45, ey + ny * 45
                    if math.hypot(x - mx, y - my) > reach or abs(ez - mz) > 120:
                        continue
                    if not solid_behind(ex - nx * 20, ey - ny * 20) and not blocked_flat(ex - nx * 60, ey - ny * 60, ez):
                        continue   # the navmesh ends at a drop, water or a cliff: no wall
                    g = open_ground(x, y, 30, 30)
                    if g is None or clearance(x + nx * 90, y + ny * 90, 40) < 40:
                        continue
                    out.append([round(x), round(y), round(g, 1), round(math.atan2(nx, ny), 3)])
    return out


def spaced(spots, mx, my, gap, most):
    """The nearest to the middle first, none closer than `gap` to another."""
    keep = []
    for sp in sorted(spots, key=lambda q: math.hypot(q[0] - mx, q[1] - my)):
        if all(math.hypot(sp[0] - o[0], sp[1] - o[1]) >= gap for o in keep):
            keep.append(sp)
            if len(keep) >= most:
                break
    return keep


def circle(mx, my, r, n, radius, flat, outward, turn):
    """Spots on a circle around the middle on open ground, facing it (or away from it)."""
    out = []
    for a in range(n):
        ang = turn + a * 6.2832 / n
        x, y = mx + math.cos(ang) * r, my + math.sin(ang) * r
        g = open_ground(x, y, radius, flat)
        if g is None:
            continue
        h = math.atan2(x - mx, y - my) if outward else math.atan2(mx - x, my - y)
        out.append([round(x), round(y), round(g, 1), round(h, 3)])
    return out


# A place's settlement workbench: the nearest within 4000 of its marker that no nearer place has.
for fid, x, y, z in workbenches:
    best = min(nodes, key=lambda n: math.hypot(n['x'] - x, n['y'] - y))
    if math.hypot(best['x'] - x, best['y'] - y) < 4000 and 'workshop' not in best:
        best['workshop'] = [fid, round(x), round(y), round(z)]

CAMP_CLEAR = 300.0   # nothing standing this close to the middle
RING = 16
camps = 0
for n in nodes:
    sp = n['spawns']
    # Spawn spots on a floor or roof (well above the landscape) are fine for NPCs, not for a camp: 5th value 1 = ground.
    for s in sp:
        g = surface(s[1], s[2])
        s.append(1 if g is not None and abs(s[3] - g) < 48 else 0)
    if not sp:
        continue
    # The game's own gathering place: the spot with most NPC spots around it.
    cx, cy = max(((s[1], s[2]) for s in sp),
                 key=lambda p: sum(1 for o in sp if math.hypot(o[1] - p[0], o[2] - p[1]) < 600))
    # Its furniture: the piece with the most others within the sandbox's reach (450 of its 512) is the camp's hub,
    # if it's near where the NPCs are. Their sandbox then uses all of it; the camp builds only what's missing.
    near = [f for f in furniture if math.hypot(f[1] - cx, f[2] - cy) < 2000]
    hub = None
    if near:
        best = max(near, key=lambda f: sum(1 for o in near if math.hypot(o[1] - f[1], o[2] - f[2]) < 450
                                          and abs(o[3] - f[3]) < 200))
        around = [o for o in near if math.hypot(o[1] - best[1], o[2] - best[2]) < 450 and abs(o[3] - best[3]) < 200]
        if len(around) >= 3:
            hub = {'x': round(best[1]), 'y': round(best[2]), 'z': round(best[3], 1),
                   'beds': sum(1 for o in around if o[0] == 'bed'), 'seats': sum(1 for o in around if o[0] == 'seat'),
                   'benches': sum(1 for o in around if o[0] == 'bench'), 'cook': sum(1 for o in around if o[0] == 'cook')}
            n['hub'] = hub
            cx, cy = best[1], best[2]
    # Nearest open ground to it, roomier preferred at the same distance (a street between houses is fine).
    best = None
    for ring in range(0, 15):
        cand = [(cx, cy)] if ring == 0 else [
            (cx + math.cos(a / (6 * ring) * 6.2832) * ring * 160, cy + math.sin(a / (6 * ring) * 6.2832) * ring * 160)
            for a in range(6 * ring)]
        for clear, flat in ((CAMP_CLEAR, 90), (200, 110), (140, 130)):
            for x, y in cand:
                g = open_ground(x, y, clear, flat)
                if g is not None:
                    best = (x, y, g)
                    break
            if best:
                break
        if best:
            break
    if not best:
        continue
    mx, my, mz = best
    items = []
    for i in range(RING * 3):
        if len(items) >= RING:
            break
        turn = i * 2.39996 + (n['ref'] % 7) * 0.9   # golden angle: spread out
        r = 150 + 45 * (i % 6)
        x, y = mx + math.cos(turn) * r, my + math.sin(turn) * r
        g = open_ground(x, y, 45, 40)
        if g is None or any(math.hypot(x - a[0], y - a[1]) < 110 for a in items):
            continue
        items.append([round(x), round(y), round(g, 1)])
    turn = (n['ref'] % 7) * 0.9
    n['camp'] = {'x': round(mx), 'y': round(my), 'z': round(mz, 1), 'items': items,
                 'walls': spaced(wall_spots(mx, my, mz, 900), mx, my, 140, 16),
                 'seats': circle(mx, my, 125, 6, 30, 30, False, turn),
                 'edge': circle(mx, my, 430, 10, 50, 50, True, turn)}
    camps += 1

json.dump({'world': 'Commonwealth', 'worldspace': COMMONWEALTH, 'nodes': nodes}, open(OUT, 'w'), indent=1)
lairs = sum(1 for n in nodes if n['residents'])
print('%d nodes, %d with residents, %d placed NPCs, %d NPC/leveled records' % (len(nodes), lairs, len(placed), len(edids)))
print('%d places with a settlement workbench' % sum(1 for n in nodes if 'workshop' in n))
print('%d places use the game\'s own furniture (%d pieces in the Commonwealth)' %
      (sum(1 for n in nodes if 'hub' in n), len(furniture)))
print('%d landscape cells, %d navmesh border edges, %d placed objects with a footprint, %d camps on open ground '
      '(%d with 4+ wall spots)' % (len(heights), sum(len(v) for v in borders.values()), sum(len(v) for v in solid.values()), camps,
       sum(1 for n in nodes if len(n.get('camp', {}).get('walls', ())) >= 4)))

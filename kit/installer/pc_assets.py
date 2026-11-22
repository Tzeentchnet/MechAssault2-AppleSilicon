"""pc_assets/common.mgf: the PC menu overlay, built from the disc's own common.mgf.

The game loads pc_assets/common.mgf over game_files/common.mgf. It is the disc archive with these
files replaced or added, made the same way as the release's:

  1. build_pc_assets.py: the PC settings pages (Display, Volume, Controls) in the game's own blue
     panels and fonts, keyboard/mouse legends in place of the controller button icons; with
     build_pc_assets_bots.py's Bots rows in the Multiplayer New Game form
  2. the Display page's 13th row (Pre-load shaders on loading screens), rows 23 units apart from y=90
  3. the keyboard/mouse quick-time-event icons, interface/PC/QTE/*.tif (qte_icons.py)
  4. the hub's Game Demos button as Custom Scenarios (scenario_assets.py)

Every replaced or added file is then checked against data/pc-overlay.json (the release's SHA-256
of each), and cache.version names the result so the game drops caches of an older overlay.

  python pc_assets.py <install folder>                 (reads game_files/, writes pc_assets/)
  python pc_assets.py make <disc common.mgf> <release overlay>     (release tooling: the manifest)
"""
import copy, hashlib, json, struct, sys, zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from inspect_mgf import Mgf                       # noqa: E402
import build_pc_assets as base                    # noqa: E402
import build_pc_assets_bots                       # noqa: E402,F401  (adds the Bots rows to base.build)
import qte_icons                                  # noqa: E402
import scenario_assets                            # noqa: E402

DISPLAY = 'interface/menus/calibration/calibration.ggf'
TOP, STEP = 90, 23            # pc_native_settings.cpp row_top()/row_step()

def sha(data):
    return hashlib.sha256(data).hexdigest()

def crc(data):
    return zlib.crc32(data) ^ 0xffffffff        # MGF keeps the un-finalized CRC accumulator

def display_rows(objects):
    """Twelve Display rows -> thirteen (action 9112, the next text id), re-laid; slider follows row 7."""
    get, put = base.get, base.put
    rows = sorted((o for o in objects if 9100 <= get(o, base.ACTION) < 9200), key=lambda o: get(o, base.ACTION))
    if len(rows) != 12: raise ValueError('unexpected Display page (%d rows)' % len(rows))
    last = rows[-1]; new = copy.deepcopy(last)
    new_id = max(get(o, base.ID) for o in objects) + 1
    put(new, base.ID, new_id); put(new, base.ACTION, 9112); put(new, base.TEXT, get(last, base.TEXT) + 1)
    put(new, base.NEXT, get(last, base.NEXT)); put(last, base.NEXT, new_id)
    objects.insert(objects.index(last) + 1, new)
    for i, row in enumerate(rows + [new]):
        base.rectangle(row, 76, TOP + i * STEP, 490, STEP)
    for o in objects:
        if get(o, base.ACTION) in (9301, 9302):
            x, w, h = (struct.unpack('<i', o[1][k][1])[0] for k in (base.X, base.W, base.H))
            base.rectangle(o, x, TOP + 7 * STEP + 5, w, h)
    return objects

def replace(target, name, payload):
    """Append a new payload for an existing overlay file and point its directory record at it."""
    m = Mgf(target); key = next(n for n in m.files if n.lower() == name)
    data = bytearray(target.read_bytes())
    table = struct.unpack_from('<I', data, 20)[0]
    data += b'\0' * ((-len(data)) % 16); offset = len(data); data += payload
    record = list(m.records[m.files[key]])
    record[1] = crc(payload); record[3] = record[4] = len(payload); record[6] = offset
    struct.pack_into('<8I', data, table + m.files[key] * 32, *record)
    target.write_bytes(data)
    if Mgf(target).file(key) != payload: raise ValueError('replacement of %s did not read back' % name)

def _signed(x):
    return x if x < 0x80000000 else x - 0x100000000

def _path_hash(name):
    return crc(('\\' + name.replace('/', '\\').strip('\\').lower()).encode('ascii'))

def add_qte(target, assets):
    """Add interface/PC/QTE/<icon>.tif: two directory nodes, 24 file nodes and records (the same
    layout as the release; the original records and payloads are kept as they are)."""
    source = Mgf(target); before = source.read(0, source.size)
    header = list(struct.unpack_from('<11I', before))
    records = [list(r) for r in source.records]; rows = [list(r) for r in source.rows]
    strings = bytearray(before[header[10]:header[10] + header[9]])
    dirs = {source.name(i).replace('\\', '/').strip('/').lower(): i for i, r in enumerate(rows) if r[5] < 0}
    if 'interface/pc' in dirs: raise ValueError('the overlay already has interface/PC')
    interface = dirs['interface']; result = bytearray(before)
    old_child = rows[interface][2]; pc = len(rows); qte = pc + 1
    def add_dir(name, parent, child, sibling, fileindex, full):
        offset = len(strings); strings.extend(name.encode('ascii') + b'\0')
        rows.append([_signed(_path_hash(full)) if fileindex < 0 else 0, parent, child, sibling, offset, fileindex])
    add_dir('PC', interface, qte, old_child, -1, 'interface/PC')
    add_dir('QTE', pc, qte + 1, -1, -1, 'interface/PC/QTE')
    rows[interface][2] = pc
    template = source.records[source.files[next(n for n in source.files if n.lower().endswith('/button_a_idle.tif'))]]
    for index, (name, payload) in enumerate(sorted(assets.items())):
        result += b'\0' * ((-len(result)) % 16); offset = len(result); result += payload
        di, fi = len(rows), len(records)
        add_dir(name.rsplit('/', 1)[1], qte, -1, di + 1 if index + 1 < len(assets) else -1, fi, name)
        record = list(template); record[0] = di; record[1] = crc(payload); record[2] = _path_hash(name)
        record[3] = record[4] = len(payload); record[6] = offset; records.append(record)
    if len({r[2] for r in records}) != len(records): raise ValueError('path hash collision')
    result += b'\0' * ((-len(result)) % 16); header[5] = len(result); result += b''.join(struct.pack('<8I', *r) for r in records)
    result += b'\0' * ((-len(result)) % 16); header[8] = len(result); result += b''.join(struct.pack('<6i', *r) for r in rows)
    result += b'\0' * ((-len(result)) % 16); header[10] = len(result); result += strings
    header[3] = len(records); header[4] = len(records) * 32; header[6] = len(rows); header[7] = len(rows) * 24; header[9] = len(strings)
    struct.pack_into('<11I', result, 0, *header)
    target.write_bytes(result)
    check = Mgf(target)
    for name, payload in assets.items():
        if check.file(name) != payload: raise ValueError('%s did not read back' % name)

def overlay_changes(disc, overlay):
    """{path: sha256} of every overlay file that is new or differs from the disc archive."""
    nd = {n.lower(): n for n in disc.files}
    out = {}
    for n in overlay.files:
        data = overlay.file(n)
        if n.lower() not in nd or disc.file(nd[n.lower()]) != data:
            out[n] = sha(data)
    return out

def verify_overlay(disc, target):
    expected = json.loads((HERE / 'data' / 'pc-overlay.json').read_text(encoding='utf-8'))
    overlay = Mgf(target)
    got = overlay_changes(disc, overlay)
    if len(overlay.files) != expected['files'] or got != expected['changed']:
        wrong = sorted(set(got.items()) ^ set(expected['changed'].items()))
        raise SystemExit('pc_assets/common.mgf does not match the release (%d files differ, e.g. %s)' %
                         (len(wrong), ', '.join(n for n, _ in wrong[:3])))
    return got

def verify_existing(install):
    install = Path(install)
    target = install / 'pc_assets' / 'common.mgf'
    verify_overlay(Mgf(install / 'game_files' / 'common.mgf'), target)
    if (target.parent / 'cache.version').read_bytes() != base.cache_version(target.read_bytes()):
        raise ValueError('PC asset cache version does not match the overlay')

def build(install, log=print):
    install = Path(install)
    disc_path = install / 'game_files' / 'common.mgf'
    base.PROJECT = install                        # reads game_files/common.mgf, writes pc_assets/
    base.build()
    target = install / 'pc_assets' / 'common.mgf'
    replace(target, DISPLAY, bytes(base.encode(display_rows(base.parse(Mgf(target).file(
        next(n for n in Mgf(target).files if n.lower() == DISPLAY)))))))
    disc = Mgf(disc_path)
    add_qte(target, qte_icons.icons(disc))
    hub = next(n for n in Mgf(target).files if n.lower() == scenario_assets.NAME)
    replace(target, scenario_assets.NAME, scenario_assets.transform(Mgf(target).file(hub), base))
    got = verify_overlay(disc, target)
    (target.parent / 'cache.version').write_bytes(base.cache_version(target.read_bytes()))
    log('PC menu overlay built: %d files changed or added, all match the release.' % len(got))

def make(disc_common, release_overlay):
    got = overlay_changes(Mgf(disc_common), Mgf(release_overlay))
    (HERE / 'data').mkdir(exist_ok=True)
    (HERE / 'data' / 'pc-overlay.json').write_text(json.dumps(
        {'files': len(Mgf(release_overlay).files), 'changed': got}, indent=1, sort_keys=True) + '\n', encoding='utf-8')
    print('recorded', len(got), 'changed/added overlay files')

if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'make': make(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 2: build(sys.argv[1])
    else: sys.exit(__doc__)

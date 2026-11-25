#!/usr/bin/env python3
"""Self-test for vcdiff.apply() and xdelta_mod.py import / build-code helpers on synthetic data (no game files).
Run: python3 -I test_xdelta_mod.py"""
import json
import lzma
import os
import shutil
import struct
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import package_extract as pe  # noqa: E402
import vcdiff  # noqa: E402
import xdelta_mod as X  # noqa: E402
import xdelta_code as XC  # noqa: E402

SECTOR = 2048


def varint(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))


# --------------------------------------------------------------------------------------------- a small VCDIFF writer

def window(ops, src_len, src_pos, tlen, checksum=None, secondary=None):
    """ops: ('add', bytes) | ('run', byte, n) | ('copy', abs addr, n) (mode 0). secondary: a list of three LZMA
    compressors -> sections compressed the way xdelta3 does it (varint size + xz data)."""
    data, inst, addr = b'', b'', b''
    for op in ops:
        if op[0] == 'add':
            inst += b'\x01' + varint(len(op[1]))
            data += op[1]
        elif op[0] == 'run':
            inst += b'\x00' + varint(op[2])
            data += bytes([op[1]])
        else:
            inst += bytes([19]) + varint(op[2])
            addr += varint(op[1])
    dind = 0
    if secondary:
        secs = []
        for k, b in enumerate((data, inst, addr)):
            c = secondary[k].compress(b) + secondary[k].flush()
            secs.append(varint(len(b)) + c)
            dind |= 1 << k
        data, inst, addr = secs
    body = varint(tlen) + bytes([dind]) + varint(len(data)) + varint(len(inst)) + varint(len(addr))
    if checksum is not None:
        body += struct.pack('>I', checksum)
    body += data + inst + addr
    wind = (1 if src_len else 0) | (4 if checksum is not None else 0)
    head = bytes([wind]) + (varint(src_len) + varint(src_pos) if src_len else b'')
    return head + varint(len(body)) + body


def patch_file(windows, app=b'', secondary=False):
    hind = (1 if secondary else 0) | (4 if app else 0)
    return b'\xd6\xc3\xc4\x00' + bytes([hind]) + (b'\x02' if secondary else b'') + (varint(len(app)) + app if app else b'') + b''.join(windows)


def encode(src, dst, win=1 << 16, checksum=True):
    """Naive encoder: per window, blocks equal to the source at the same place are COPYs, the rest ADDs."""
    out = []
    for w in range(0, len(dst), win):
        t = dst[w:w + win]
        ops, i = [], 0
        while i < len(t):
            n = min(SECTOR, len(t) - i)
            if src[w + i:w + i + n] == t[i:i + n] and len(src) >= w + i + n:
                ops.append(('copy', w + i, n))
            else:
                ops.append(('add', t[i:i + n]))
            i += n
        out.append(window(ops, len(src), 0, len(t), zlib.adler32(t) if checksum else None))
    return out


# --------------------------------------------------------------------------------------------- a small ISO 9660 writer

def iso(files):
    """files {path: bytes} -> a minimal ISO 9660 image (PVD + directories + files, no path tables)."""
    tree = {}
    for p in files:
        parts = p.split('/')
        d = tree
        for part in parts[:-1]:
            d = d.setdefault(part, {})
        d[parts[-1]] = p
    dirs = []                                  # (node, path) in creation order

    def collect(node, path):
        dirs.append((node, path))
        for k in sorted(node):
            if isinstance(node[k], dict):
                collect(node[k], path + k + '/')
    collect(tree, '')
    ext, sec = {}, 18
    for node, path in dirs:
        ext[path] = sec
        sec += 1                                # one sector per directory is enough here
    fext = {}
    for p in sorted(files):
        fext[p] = sec
        sec += (len(files[p]) + SECTOR - 1) // SECTOR or 1
    img = bytearray(sec * SECTOR)

    def rec(name, e, size, is_dir):
        n = name if isinstance(name, bytes) else name.encode()
        ln = 33 + len(n) + (1 - len(n) % 2)
        r = bytearray(ln)
        r[0] = ln
        struct.pack_into('<I', r, 2, e)
        struct.pack_into('>I', r, 6, e)
        struct.pack_into('<I', r, 10, size)
        struct.pack_into('>I', r, 14, size)
        r[25] = 2 if is_dir else 0
        r[32] = len(n)
        r[33:33 + len(n)] = n
        return bytes(r)

    for node, path in dirs:
        parent = path.rsplit('/', 2)[0] + '/' if path.count('/') > 1 else ''
        b = rec(b'\0', ext[path], SECTOR, True) + rec(b'\1', ext[parent if path else ''], SECTOR, True)
        for k in sorted(node):
            if isinstance(node[k], dict):
                b += rec(k, ext[path + k + '/'], SECTOR, True)
            else:
                b += rec(k + ';1', fext[node[k]], len(files[node[k]]), False)
        img[ext[path] * SECTOR:ext[path] * SECTOR + len(b)] = b
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b'CD001'
    pvd[6] = 1
    struct.pack_into('<H', pvd, 128, SECTOR)
    pvd[156:156 + 34] = rec(b'\0', ext[''], SECTOR, True)
    img[16 * SECTOR:17 * SECTOR] = pvd
    img[17 * SECTOR:17 * SECTOR + 6] = b'\xffCD001'
    for p, data in files.items():
        img[fext[p] * SECTOR:fext[p] * SECTOR + len(data)] = data
    return bytes(img)


def sfo(disc_id):
    key, val = b'DISC_ID\0', disc_id.encode() + b'\0'
    kt = 20 + 16
    dt = kt + len(key) + (-len(key)) % 4
    out = bytearray(dt + 16)
    out[:4] = b'\0PSF'
    struct.pack_into('<IIII', out, 4, 0x101, kt, dt, 1)
    struct.pack_into('<HHIII', out, 20, 0, 0x0204, len(val), 16, 0)
    out[kt:kt + len(key)] = key
    out[dt:dt + len(val)] = val
    return bytes(out)


def package(entries):
    """entries [(asset path, bytes)] -> (PACKAGE_INFO.BIN, PACKAGE.BIN)."""
    rows, body = [], b''
    for path, data in entries:
        rows.append((pe.package_crc(path), len(body) // SECTOR, (1 << 28) | len(data)))
        body += data + b'\0' * ((-len(data)) % SECTOR)
    rows.sort()
    info = struct.pack('<I4sII', 0x20090218, b'pack', len(rows), 0) + b''.join(struct.pack('<III', *r) for r in rows)
    return info, body


# --------------------------------------------------------------------------------------------- tests

def test_apply():
    src = bytes(range(256)) * 40                         # 10240 bytes
    tgt = src[:1000] + b'NEW' + bytes([7]) * 50 + src[2000:2100]
    tgt += tgt[1000:1003]                               # a COPY out of the target window itself
    ops = [('copy', 0, 1000), ('add', b'NEW'), ('run', 7, 50), ('copy', 2000, 100), ('copy', len(src) + 1000, 3)]
    p = patch_file([window(ops, len(src), 0, len(tgt), zlib.adler32(tgt))])
    unknown = [(1500, 2050)]                            # bytes 2000..2050 of the source are not known
    ws = list(vcdiff.apply(p, lambda pos, n: (src[pos:pos + n], [(max(a, pos), min(b, pos + n)) for a, b in unknown if a < pos + n and b > pos])))
    assert len(ws) == 1 and ws[0].data == tgt and ws[0].checksum_ok is True, ws
    assert ws[0].unknown == [(1053, 1103)], ws[0].unknown     # target 1053.. = source 2000..2050
    bad = patch_file([window(ops, len(src), 0, len(tgt), zlib.adler32(tgt) ^ 1)])
    assert next(vcdiff.apply(bad, lambda pos, n: (src[pos:pos + n], []))).checksum_ok is False
    # LZMA secondary compression: one xz stream per section type that runs on across windows
    comp = [lzma.LZMACompressor(format=lzma.FORMAT_XZ, check=lzma.CHECK_NONE) for _ in range(3)]
    w1 = window([('add', b'hello' * 100)], 0, 0, 500, zlib.adler32(b'hello' * 100), secondary=comp)
    p2 = patch_file([w1], secondary=True)
    ws = list(vcdiff.apply(p2, lambda pos, n: (b'', [])))
    assert ws[0].data == b'hello' * 100 and ws[0].checksum_ok, ws[0]
    # the header and the application header
    app, start, sec = vcdiff.header(patch_file([w1], app=b'out.iso//in.iso/', secondary=True))
    assert app == b'out.iso//in.iso/' and sec == 2


def make_disc(pkg_entries, extra=None, disc_id='ULUS10566'):
    info, body = package(pkg_entries)
    files = {'UMD_DATA.BIN': b'ULUS-10566|0000000000000001|0001|G',
             'PSP_GAME/PARAM.SFO': sfo(disc_id),
             'PSP_GAME/SYSDIR/EBOOT.BIN': b'~PSP' + bytes(4092),
             'PSP_GAME/USRDIR/DATA/PACKAGE_INFO.BIN': info,
             'PSP_GAME/USRDIR/DATA/PACKAGE.BIN': body,
             'PSP_GAME/USRDIR/DATA/OVL_MENU_APP.ELF': b'\x7fELF' + bytes(3000),
             'PSP_GAME/USRDIR/DATA/MOVIE/END.PMF': b'PSMF' + bytes(9000),
             'PSP_GAME/USRDIR/DATA/BGM/A.AT3': b'RIFF' + bytes(500)}
    files.update(extra or {})
    return files


def test_import_and_layout():
    tmp = tempfile.mkdtemp(prefix='xdelta_test_')
    old_layouts, old_table = X.LAYOUTS, X.LAYOUT_TABLE
    try:
        e1, e2, e3 = b'one' * 300, b'two' * 900, b'three' * 100
        base_files = make_disc([('obj/a.gmo', e1), ('obj/b.objx', e2), ('menu/c.bin', e3)])
        base = iso(base_files)
        base_path = os.path.join(tmp, 'base.iso')
        open(base_path, 'wb').write(base)
        # the layout table of this "retail" disc
        X.LAYOUTS = os.path.join(tmp, 'layouts.json')
        X.LAYOUT_TABLE = os.path.join(tmp, 'disc_layouts.inc')   # never the runtime's table (it shipped a test disc once, issue #124)
        assert X.main(['layout', base_path]) == 0
        lay = json.load(open(X.LAYOUTS))['discs'][0]
        assert lay['id'] == 'ULUS10566' and 'PSP_GAME/USRDIR/DATA/PACKAGE.BIN' in lay['files'], lay['id']
        # an install folder made from it
        work = os.path.join(tmp, 'work')
        for p, data in base_files.items():
            if p.startswith('PSP_GAME/') and not p.endswith('EBOOT.BIN'):
                d = os.path.join(work, 'disc0', *p.split('/'))
                os.makedirs(os.path.dirname(d), exist_ok=True)
                open(d, 'wb').write(data)
        open(os.path.join(work, 'EBOOT.enc.bin'), 'wb').write(base_files['PSP_GAME/SYSDIR/EBOOT.BIN'])
        # the patched disc: entry b changed (same size), c grown, data appended after the package, BGM and the menu
        # overlay changed, the movie made smaller
        tinfo, tbody = package([('obj/a.gmo', e1), ('obj/b.objx', b'TWO' * 900), ('menu/c.bin', b'THREE' * 700)])
        # keep the retail sector layout for a and b (the patch tool would), put c where it was but bigger
        tbody = tbody + b'TAIL' * 1024
        tfiles = dict(base_files)
        tfiles['PSP_GAME/USRDIR/DATA/PACKAGE_INFO.BIN'] = tinfo
        tfiles['PSP_GAME/USRDIR/DATA/PACKAGE.BIN'] = tbody
        tfiles['PSP_GAME/USRDIR/DATA/BGM/A.AT3'] = b'RIFF' + b'x' * 500
        tfiles['PSP_GAME/USRDIR/DATA/OVL_MENU_APP.ELF'] = b'\x7fELF' + b'\x01' * 3000
        tfiles['PSP_GAME/USRDIR/DATA/MOVIE/END.PMF'] = b'PSMF' + bytes(100)
        target = iso(tfiles)
        patch = patch_file(encode(base, target), app=b'C:\\mods\\Test.iso//C:\\base\\USA.iso/', secondary=False)
        pp = os.path.join(tmp, 'Test Mod.xdelta')
        open(pp, 'wb').write(patch)
        assert X.main(['import', pp, '--work', work]) == 0
        mod = os.path.join(work, 'mods', 'test-mod')
        meta = json.load(open(os.path.join(mod, 'mod.json')))
        assert meta['kind'] == 'xdelta' and meta['enabled'] is True and meta['code'] == ['OVL_MENU_APP.ELF'], meta
        assert open(os.path.join(mod, 'install', 'by_crc', '%08x.bin' % pe.package_crc('obj/b.objx')), 'rb').read() == b'TWO' * 900 \
            or open(os.path.join(mod, 'install', 'obj', 'b.objx'), 'rb').read() == b'TWO' * 900
        names = set()
        for dp, _, fns in os.walk(os.path.join(mod, 'install')):
            names |= set(fns)
        assert len(names) == 2, names                      # b and c; a is unchanged
        assert open(os.path.join(mod, 'package_tail.bin'), 'rb').read() == tbody[len(base_files['PSP_GAME/USRDIR/DATA/PACKAGE.BIN']):]
        assert open(os.path.join(mod, 'usrdir', 'DATA', 'BGM', 'A.AT3'), 'rb').read() == b'RIFF' + b'x' * 500
        assert not os.path.exists(os.path.join(mod, 'usrdir', 'DATA', 'MOVIE', 'END.PMF'))   # smaller movie: kept
        assert open(os.path.join(mod, 'code', 'OVL_MENU_APP.ELF'), 'rb').read() == tfiles['PSP_GAME/USRDIR/DATA/OVL_MENU_APP.ELF']
        assert meta['code_stamp'] == X.code_stamp(mod)
        # the same patch again: nothing to do; a missing base file is refused, nothing installed
        assert X.main(['import', pp, '--work', work]) == 0
        enc = os.path.join(work, 'EBOOT.enc.bin')
        os.rename(enc, enc + '.away')                    # the patch copies the (unchanged) EBOOT from the base
        try:
            X.main(['import', pp, '--work', work, '--name', 'other'])
            raise AssertionError('should refuse')
        except SystemExit as e:
            assert e.code == 1
        assert not os.path.exists(os.path.join(work, 'mods', 'other'))
        os.rename(enc + '.away', enc)
        # an update of the same mod (version in the file name) replaces it and keeps the on/off choice; a record of
        # run-time code survives where the new version carries the same bytes
        m = json.load(open(os.path.join(mod, 'mod.json'))); m['enabled'] = False
        json.dump(m, open(os.path.join(mod, 'mod.json'), 'w'))
        open(os.path.join(mod, 'code', 'dyn_00000000_0.bin'), 'wb').write(b'x' * 16)
        open(os.path.join(mod, 'code', 'dynamic.txt'), 'w').write('blob 0 16 134217728 dyn_00000000_0.bin\nblob 16 16 134217744 gone.bin\n')
        tfiles2 = dict(tfiles)
        tfiles2['PSP_GAME/USRDIR/DATA/BGM/A.AT3'] = b'RIFF' + b'y' * 500
        p2 = os.path.join(tmp, 'Test Mod v1.1.xdelta')
        open(p2, 'wb').write(patch_file(encode(base, iso(tfiles2))))
        assert X.main(['import', p2, '--work', work]) == 0
        meta2 = json.load(open(os.path.join(mod, 'mod.json')))
        assert meta2['version'] == '1.1' and meta2['family'] == 'test-mod' and meta2['enabled'] is False, meta2
        assert open(os.path.join(mod, 'usrdir', 'DATA', 'BGM', 'A.AT3'), 'rb').read() == b'RIFF' + b'y' * 500
        assert open(os.path.join(mod, 'code', 'dynamic.txt')).read() == 'blob 0 16 134217728 dyn_00000000_0.bin\n'
        assert [d for d in os.listdir(os.path.join(work, 'mods')) if d.startswith('test')] == ['test-mod']
        # a patch for another base (a block fails although all it copies was known) is refused
        ws = encode(base, iso(tfiles2), win=8192)
        bad = bytearray(ws[3])
        k = bad.index(bytes(struct.pack('>I', zlib.adler32(iso(tfiles2)[3 * 8192:4 * 8192]))))
        bad[k] ^= 1
        ws[3] = bytes(bad)
        p3 = os.path.join(tmp, 'Wrong.xdelta')
        open(p3, 'wb').write(patch_file(ws))
        try:
            X.main(['import', p3, '--work', work])
            raise AssertionError('should refuse')
        except SystemExit as e:
            assert e.code == 1
        assert not os.path.exists(os.path.join(work, 'mods', 'wrong'))
        # the same files from the disc image itself
        X.main(['import', pp, '--work', work, '--iso', base_path, '--name', 'from iso'])
        assert os.path.isfile(os.path.join(work, 'mods', 'from-iso', 'package_tail.bin'))
    finally:
        X.LAYOUTS = old_layouts
        X.LAYOUT_TABLE = old_table
        shutil.rmtree(tmp, ignore_errors=True)


def test_code_helpers():
    # jumps out of changed words: a call into the middle of a function is a force entry, a jump a mid-entry
    funcs = [{'address': '0x08800000', 'size': 64}, {'address': '0x08800040', 'size': 64}]
    old = bytes(128)
    new = bytearray(old)
    struct.pack_into('<I', new, 0, (3 << 26) | ((0x08800048 >> 2) & 0x3FFFFFF))      # jal 0x08800048 (inside #2)
    struct.pack_into('<I', new, 8, (2 << 26) | ((0x08800050 >> 2) & 0x3FFFFFF))      # j 0x08800050 (inside #2)
    struct.pack_into('<I', new, 16, (3 << 26) | ((0x08800040 >> 2) & 0x3FFFFFF))     # jal 0x08800040 (a start)
    import base64
    seg = lambda b: {'p_vaddr': '0x08800000', 'data_b64': base64.b64encode(bytes(b)).decode()}
    force, mid = XC.entries_for({'functions': funcs}, [seg(old)], [seg(new)])
    assert force == [0x08800048] and mid == [(0x08800050, 0x08800040)], (force, mid)
    t = XC.toml_with(None, [0x08800048], [(0x08800050, 0x08800040)])
    assert 'force_entries = ["0x08800048"]' in t and 'entry  = "0x08800050"' in t, t
    # the generated table without mods still defines dissidia_xmod_sets()
    d = tempfile.mkdtemp()
    try:
        XC.write_set(d, [], {}, [], [])
        s = open(os.path.join(d, 'dispatch.cpp')).read()
        assert 'dissidia_xmod_sets(int* count)' in s and '*count = 0;' in s
        XC.write_set(d, [{'folder': 'm', 'stamp': 'abc', 'module': 2, 'fns': [(0x089E0000, 'xm0_m_menu_FUN_089e0000')], 'blob': (0, 0, 0)}],
                     {'batch_xm0_m_menu.cpp': 'RECOMP_FUNC void xm0_m_menu_FUN_089e0000(uint8_t* rdram, recomp_context* ctx) {}\n'}, [], [])
        s = open(os.path.join(d, 'dispatch.cpp')).read()
        assert '{0x089E0000U, xm0_m_menu_FUN_089e0000}' in s and '{"m", "abc", 2, s_fns0, 1, 0ull, 0u, 0x00000000U}' in s, s
    finally:
        shutil.rmtree(d)


def test_run_time_code_helpers():
    import base64
    d = tempfile.mkdtemp()
    try:
        # the runtime's record of code a mod loads while the game runs
        open(os.path.join(d, 'dynamic.txt'), 'w').write(
            'blob 4096 32768 187891712 dyn_0B330000_1000.bin\n'
            'write 0 142790088 248301704 4096 32768\n'
            'write 2 144682248 248303534 4096 32768\n'
            'write 0 1 2 999 999\n')                      # a block that is not recorded: ignored
        # entries of loaded code: `bal` to pc+8 reads the PC (no entry); a pointer-like word into a delay slot is none
        A = 0x0B330000
        words = [0x04110001, 0, 0x0020F821,            # bal +8; nop; move ra, at
                 0x1160000A, 0x27BDFFF0,               # beqz t3, ...; addiu sp, sp, -16 (delay slot)
                 0x03E00008, 0,                        # jr ra; nop
                 A + 16, A + 28]                       # pointers: into the delay slot, and after jr ra; nop
        blob = struct.pack('<%dI' % len(words), *words)
        assert XC.blob_entries(blob, A) == [A, A + 28], [hex(x) for x in XC.blob_entries(blob, A)]
        dyn = XC.read_dynamic(d)
        assert list(dyn) == [(4096, 32768)] and dyn[(4096, 32768)]['addr'] == 0x0B330000, dyn
        assert sorted(dyn[(4096, 32768)]['writes']) == [(0, 142790088, 248301704), (2, 144682248, 248303534)]
    finally:
        shutil.rmtree(d)
    # entry points of a block: its start, bal / jal targets and pointer words into it; jumps into the game are not
    addr = 0x0B330000
    code = bytearray(64)
    struct.pack_into('<I', code, 4, (1 << 26) | (0x11 << 16) | 3)                          # bal -> 0x0B330014 (pc + 4 + 3 words)
    struct.pack_into('<I', code, 12, (3 << 26) | ((0x0B330030 >> 2) & 0x3FFFFFF))          # jal 0x0B330030
    struct.pack_into('<I', code, 20, (2 << 26) | ((0x0887A814 >> 2) & 0x3FFFFFF))          # j into the game
    struct.pack_into('<I', code, 40, 0x0B330020)                                           # a pointer
    assert XC.blob_entries(bytes(code), addr) == [0x0B330000, 0x0B330014, 0x0B330020, 0x0B330030], XC.blob_entries(bytes(code), addr)
    # a rewritten instruction gets a check in front of it that follows what memory holds then
    seg = [{'p_vaddr': '0x08800000', 'data_b64': base64.b64encode(struct.pack('<4I', 0x27BDFFF0, 0x0E271616, 0, 0x03E00008)).decode()}]
    body = ('RECOMP_FUNC void FUN_08800000(uint8_t* rdram, recomp_context* ctx) { /* 0x08800000 */\nL_08800000:\n    x;\n'
            'L_08800004:\n    y;\nL_08800008:\n    RECOMP_LOOKUP(0x089C5858)(rdram, ctx);\nL_0880000C:\n    return;\n}\n')
    keep = {}
    XC.guard_sites(keep, {0x08800000: ('FUN_08800000', body)}, [0x08800004], seg, 'test')
    out = keep[0x08800000][1]
    assert 'L_08800004:\n    if ((uint32_t)MEM_W(rdram, 0x08800004U) != 0x0E271616U) { switch (dissidia_xmod_site(rdram, ctx, 0x08800004U)) { case 1: goto L_0880000C; case 2: return;' in out, out



def test_shipped_layout_table():
    # runtime/disc_layouts.inc (the in-game importer's base discs) is the retail table: generated from disc_layouts.json, the
    # full 1.75 GB discs. A test disc there made every real patch fail to install (issue #124: "203 block(s) do not match").
    text = open(X.LAYOUT_TABLE).read()
    assert text == X.layout_table_text(X.load_layouts()), 'runtime/disc_layouts.inc is stale: run xdelta_mod.py layout-table'
    assert '{"ULUS10566", 1754529792ull' in text and '{"ULES01505", 1754529792ull' in text, 'not the retail discs'


if __name__ == '__main__':
    for name, fn in sorted(globals().items()):
        if name.startswith('test_') and callable(fn):
            fn()
            print('ok', name)

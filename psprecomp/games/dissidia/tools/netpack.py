#!/usr/bin/env python3
"""Online play with players who don't have a retail-moves mod (Bartz's Dissidia 1 moves): verified packs.

A player who uses such a mod sends the other players what their game needs to show it (the changed moveset, models and
voice bank), as deltas against the retail 012 files every player already has. Nothing a peer sends is trusted:

  * games/dissidia/net_packs.json lists, per known pack, every file it installs with its size and SHA-256, the SHA-256 of
    the retail file the delta applies to, and the abilities it adds (id, the record it copies, command, kind, CP / AP costs: a record made from the list must be byte-for-byte what the mod's own build writes, or two players' fights differ). The game
    compiles the list in (runtime/net_packs.inc, generated from the JSON by `netpack.py inc`).
  * A receiver applies a delta to ITS OWN retail file and keeps the result only when its size and SHA-256 are the listed
    ones; the file is stored under that hash, never under a name a peer chose. The ability records are made locally
    from the list. So a peer can make a receiver install exactly the listed files and nothing else.
  * A peer that uses a pack the receiver does not know (another version, an edited mod) cannot start a match with it.

    netpack.py record <mods dir> <folder> [--name NAME] [--version N] [--disc DIR]   # add / update a pack in net_packs.json
    netpack.py inc                                                                   # net_packs.json -> runtime/net_packs.inc
    netpack.py deltas <mods dir> [--disc DIR]        # <mods>/.net/<sha256>.dlt for each listed pack whose files are installed
    netpack.py check <delta> <retail file> <sha256>  # decode a delta and compare

Delta format (both ends: this file and runtime/net_packs.cpp): "NPD1", u32 target size, u32 source size, then a zlib
stream of ops: 0x00 COPY {u32 source offset, u32 length}, 0x01 ADD {u32 length, bytes}. Little endian.
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import package_extract as pe  # noqa: E402
import roster_mainbin as rm  # noqa: E402

GAME = os.path.abspath(os.path.join(HERE, '..'))
PACKS_JSON = os.path.join(GAME, 'net_packs.json')
PACKS_INC = os.path.join(GAME, 'runtime', 'net_packs.inc')
RETAIL_STATE = 'readme_charmod_retail.json'
ABILITIES = 'extra_abilities.json'
MAGIC, BLOCK = b'NPD1', 32
MAX_NAME = 23


def sha(b):
    return hashlib.sha256(b).hexdigest()


def read_retail(disc, gp):
    if gp.startswith('by_crc/'):
        return disc.read_crc(int(gp[7:15], 16))
    return disc.read(gp)


# ---------------------------------------------------------------- delta
def encode(src, dst):
    """Greedy block matcher: COPY runs found through 32-byte blocks of the source (every offset aligned to 4, so data
    moved by any multiple of 4 is found), ADD for the rest."""
    index = {}
    for o in range(0, len(src) - BLOCK + 1, 4):
        index.setdefault(src[o:o + BLOCK], o)
    ops, lit, i = bytearray(), bytearray(), 0

    def flush():
        if lit:
            ops.extend(struct.pack('<BI', 1, len(lit)))
            ops.extend(lit)
            lit.clear()
    while i < len(dst):
        o = index.get(dst[i:i + BLOCK]) if i + BLOCK <= len(dst) else None
        if o is None:
            lit.append(dst[i])
            i += 1
            continue
        n = BLOCK
        while i + n < len(dst) and o + n < len(src) and dst[i + n] == src[o + n]:
            n += 1
        flush()
        ops.extend(struct.pack('<BII', 0, o, n))
        i += n
    flush()
    return MAGIC + struct.pack('<II', len(dst), len(src)) + zlib.compress(bytes(ops), 9)


def decode(delta, src, max_out=64 << 20):
    """Mirror of the runtime decoder, with the same checks (any violation -> ValueError)."""
    if delta[:4] != MAGIC or len(delta) < 12:
        raise ValueError('not a delta')
    tsize, ssize = struct.unpack_from('<II', delta, 4)
    if tsize > max_out or ssize != len(src):
        raise ValueError('sizes do not fit')
    d = zlib.decompressobj()
    ops = d.decompress(delta[12:], tsize * 2 + 4096)
    out, p = bytearray(), 0
    while p < len(ops):
        op = ops[p]
        if op == 0 and p + 9 <= len(ops):
            o, n = struct.unpack_from('<II', ops, p + 1)
            if o + n > len(src) or len(out) + n > tsize:
                raise ValueError('copy out of range')
            out += src[o:o + n]
            p += 9
        elif op == 1 and p + 5 <= len(ops):
            n = struct.unpack_from('<I', ops, p + 1)[0]
            if p + 5 + n > len(ops) or len(out) + n > tsize:
                raise ValueError('add out of range')
            out += ops[p + 5:p + 5 + n]
            p += 5 + n
        else:
            raise ValueError('bad op')
    if len(out) != tsize:
        raise ValueError('short output')
    return bytes(out)


# ---------------------------------------------------------------- the list
def load():
    return json.load(open(PACKS_JSON, encoding="utf-8")) if os.path.exists(PACKS_JSON) else {'_info': __doc__.split('\n\n')[1], 'packs': []}


def save(j):
    with open(PACKS_JSON, 'w', encoding="utf-8") as f:
        json.dump(j, f, indent=1)
        f.write('\n')


MAIN_BIN_CRC = 'by_crc/93307c1c.bin'   # the mod build's main.bin (MPK): system.bin 'ablt' + 'chrl' rows


def chrl_row(mods, char):
    """The character's 'chrl' row (abilities learned per level: 8-byte entries) of the mod's built main.bin, as hex; '' when the mod has none.
    A native install has it through main.bin, a received pack must set it too (the CPU records learn by level: a Bartz CPU fighter had other
    abilities on a machine that got the pack online, and the fights desynced)."""
    path = os.path.join(mods, MAIN_BIN_CRC)
    if not os.path.exists(path):
        return ''
    _, ents = rm.mpk_unpack(open(path, 'rb').read())
    system = next(e[2] for e in ents if e[0] == 'system.bin')
    _, arc = rm.arc_unpack(system)
    _, rows = rm.parse_level_table(next(e[2] for e in arc if e[0] == 'chrl'), 'chrl')
    return b''.join(rows[char][1]).hex() if char in rows else ''


def cmd_record(a):
    mods, disc = a.mods, pe.Package(a.disc)
    owner = json.load(open(os.path.join(mods, RETAIL_STATE), encoding="utf-8"))
    files = sorted(gp for gp, n in owner.items() if n == a.folder)
    if not files:
        raise SystemExit('%s installs no retail files in %s (build it first)' % (a.folder, mods))
    abil = json.load(open(os.path.join(mods, ABILITIES), encoding="utf-8"))
    mod = json.load(open(os.path.join(HERE, '..', '..', '..', '..', 'mods', a.folder, 'mod.json'), encoding="utf-8"))
    code = mod['character']['code']
    lines = [l.split() for l in open(os.path.join(mods, 'retail_moves.txt'), encoding="utf-8").read().splitlines()]
    char = next(int(l[l.index(a.folder) + 1]) for l in lines if a.folder in l)   # "[#off#] <folder> <charId> <ids> nm=... [files=...]"
    name = a.name or a.folder
    if len(name) > MAX_NAME:
        raise SystemExit('name %r: at most %d characters' % (name, MAX_NAME))
    pack = {'name': name, 'version': a.version, 'char': char, 'files': [], 'abilities': []}
    for gp in files:
        new = open(os.path.join(mods, gp), 'rb').read()
        base = read_retail(disc, gp)
        pack['files'].append({'path': gp, 'size': len(new), 'sha256': sha(new), 'base_size': len(base), 'base_sha256': sha(base)})
    for k, e in sorted(abil[code].items()):
        if 'copy' in e and not e.get('disabled'):
            pack['abilities'].append({'id': '0x%04X' % ((char << 8) | int(k, 16)), 'copy': '0x%04X' % ((char << 8) | int(str(e['copy']), 16)),
                                      'cmd': str(e['cmd']), 'type': {'brave': 0, 'hp': 1}[e['type']],
                                      'phase': {'ground': 0, 'midair': 1, 'follow-up': 0xFF}[e['phase']],
                                      # the costs the mod's build writes (roster_mainbin.py: cp +0x0E, cp_mastered +0x10 default cp // 2, ap +0x12); -1 = the copy's own
                                      'cp': int(e['cp']) if e.get('cp') is not None else -1,
                                      'cp_mastered': (int(e['cp_mastered']) if e.get('cp_mastered') is not None else int(e['cp']) // 2) if e.get('cp') is not None else -1,
                                      'ap': int(e['ap']) if e.get('ap') is not None else -1})
    pack['chrl'] = chrl_row(mods, char)
    j = load()
    j['packs'] = [p for p in j['packs'] if p['name'] != name] + [pack]
    save(j)
    cmd_inc(a)
    print('%s v%d: %d files, %d abilities' % (name, a.version, len(pack['files']), len(pack['abilities'])))


def cmd_inc(_a=None):
    j = load()
    out = ['// Generated by tools/netpack.py inc from games/dissidia/net_packs.json. Do not edit.',
           '// The packs a player may receive from another player online (net_packs.cpp): exact sizes and SHA-256.']
    for p in j['packs']:
        out.append('{"%s", %d, %d, {' % (p['name'], p['version'], p['char']))
        for f in p['files']:
            out.append('    {"%s", %d, "%s", %d, "%s"},' % (f['path'], f['size'], f['sha256'], f['base_size'], f['base_sha256']))
        out.append('  }, {')
        for x in p['abilities']:
            out.append('    {%s, %s, %s, %d, %d, %d, %d, %d},' % (x['id'], x['copy'], x['cmd'], x['type'], x['phase'], x.get('cp', -1), x.get('cp_mastered', -1), x.get('ap', -1)))
        out.append('  }, "%s"},' % p.get('chrl', ''))
    with open(PACKS_INC, 'w', encoding="utf-8") as f:
        f.write('\n'.join(out) + '\n')


def cmd_deltas(a):
    """Deltas for the listed packs whose files this mods dir has installed byte for byte."""
    disc, out_dir = pe.Package(a.disc), os.path.join(a.mods, '.net')
    os.makedirs(out_dir, exist_ok=True)
    keep = set()
    for p in load()['packs']:
        ok = True
        for f in p['files']:
            path = os.path.join(a.mods, f['path'])
            if not os.path.exists(path) or os.path.getsize(path) != f['size'] or sha(open(path, 'rb').read()) != f['sha256']:
                ok = False
                break
        if not ok:
            continue
        for f in p['files']:
            dst = os.path.join(out_dir, f['sha256'] + '.dlt')
            keep.add(os.path.basename(dst))
            base = read_retail(disc, f['path'])
            if os.path.exists(dst):            # kept only while it still decodes to the listed file (a damaged one is
                try:                           # rebuilt: receivers would throw it away and refuse the match)
                    if sha(decode(open(dst, 'rb').read(), base)) == f['sha256']:
                        continue
                except ValueError:
                    pass
                print('  %s: %s did not decode to the listed file: rebuilt' % (p['name'], os.path.basename(dst)))
            if sha(base) != f['base_sha256']:
                print('  %s: the retail %s differs from the listed one: not shareable' % (p['name'], f['path']))
                ok = False
                break
            new = open(os.path.join(a.mods, f['path']), 'rb').read()
            d = encode(base, new)
            assert decode(d, base) == new
            with open(dst, 'wb') as fh:
                fh.write(d)
        print('  %s v%d: %s' % (p['name'], p['version'], 'shareable online' if ok else 'NOT shareable'))
    for n in os.listdir(out_dir):
        if n.endswith('.dlt') and n not in keep:
            os.remove(os.path.join(out_dir, n))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('record')
    r.add_argument('mods')
    r.add_argument('folder')
    r.add_argument('--name')
    r.add_argument('--version', type=int, default=1)
    r.add_argument('--disc', default=os.path.expanduser('~/dissidia/work/disc0'))
    sub.add_parser('inc')
    d = sub.add_parser('deltas')
    d.add_argument('mods')
    d.add_argument('--disc', default=os.path.expanduser('~/dissidia/work/disc0'))
    c = sub.add_parser('check')
    c.add_argument('delta')
    c.add_argument('base')
    c.add_argument('sha')
    a = ap.parse_args(argv)
    if a.cmd == 'record':
        cmd_record(a)
    elif a.cmd == 'inc':
        cmd_inc()
    elif a.cmd == 'deltas':
        cmd_deltas(a)
    else:
        out = decode(open(a.delta, 'rb').read(), open(a.base, 'rb').read())
        print('ok' if sha(out) == a.sha else 'MISMATCH')


if __name__ == '__main__':
    main(sys.argv[1:])

#!/usr/bin/env python3
"""Bartz's Dissidia (2008) moves for Dissidia 012: build mods/bartz-dff from your two discs.

    d1_moves.py --d1 "Dissidia Final Fantasy (USA).iso" --iso012 "Dissidia 012 (USA).iso" [--out mods/bartz-dff]   # dev: the mod folder
    d1_moves.py patches --d1 D1.iso --mods work/mods --disc work/disc0    # dev: games/dissidia/d1_moves/ for a release
    d1_moves.py install --d1 D1.iso --work <installed game>              # installer: rebuild the files from the patches

012 dropped eight of the moves Bartz had in the first Dissidia: Ascendant Lance, Climbarrel (ground Bravery), Storm Shot,
Slidehazzard (air Bravery), Soul Eater, Flood (ground HP), Wind Shear and Paladin Force (air HP). This puts them back as
new abilities next to his 012 kit (he keeps everything he has). The result is a "kind": "retail" mod folder, built into
the game by run.sh / charmod.py like any other; Settings > Mods switches the moves on and off (retail_moves.txt).

Steps:
  1. both packages are read straight from the ISOs (PACKAGE.BIN by path CRC);
  2. D1's p_fiv100.objx is rewritten in the 012 layout (d1_objx.py: actions, commands, hits, projectiles, effect
     placements, event ops) and unpacked next to D1's model as a mod folder "d1bartz" (based_on fiv100);
  3. retail Bartz is unpacked as the folder "bartz";
  4. the Move Studio copies the eight commands from d1bartz into bartz in a headless Chrome (d1_moves_studio.js: the
     Studio's own copy-a-move, so animations go into all four costumes, hits, hitboxes, projectiles, effects, sounds,
     voices and face textures come along);
  5. names, D1's help texts, CP / AP and learn levels are set, and the battle voice bank gets D1's lines for the slots 012
     left empty (the banks are slot-for-slot the same; only the English bank: D1 has no Japanese voices).

Needs Python 3, Node.js with playwright-core (npm i playwright-core; NODE_PATH or a node_modules next to this file) and
Chrome / Chromium (CHROME=<binary>, default /usr/bin/google-chrome). Only tested with the USA discs of both games.
The folder holds data from both discs: keep it to yourself (mods/ is not in git).
"""
import argparse
import glob
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

# On Android the bundled Python runs through the app's loader (setup/proc.cpp): child pythons need the same prefix
PYEXE = ([os.environ['PSPRECOMP_PY_LOADER']] if os.environ.get('PSPRECOMP_PY_LOADER') else []) + [sys.executable]
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import discdir  # noqa: E402  (where disc0 is)
import charmod_gmo as G  # noqa: E402
import charmod_objx as co  # noqa: E402
import d1_objx  # noqa: E402
import package_extract as pe  # noqa: E402
import roster_mainbin as rm  # noqa: E402
import voice_bank as vb  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..'))
D1_VOICE_BANK = 0xDD2E4D13      # D1 has no package path list for it: Bartz's battle bank (SSCF bank id 18100), by CRC
VOICE_012 = 'voice/en/battle/voice_p_fiv100_00.scd'
VOICE_BASE = 1810000            # voice event ids: bank id * 100 + slot

# D1 command -> (name, type, phase, fallback help (D1's own is read from the disc), CP, CP mastered, AP, learn level). CP / AP from D1's own ability records;
# learn levels: D1's for Climbarrel 3, Flood 19, Slidehazzard 28, Paladin Force 38, chosen for the rest.
MOVES = [
    (0x198, 'Ascendant Lance', 'brave', 'ground', "[Ground] A lance lunge. Press\nagain for a flurry that ends in a\nblow. (Dissidia 1)", 35, 15, 120, 6),
    (0x19A, 'Climbarrel', 'brave', 'ground', "[Ground] Rising strikes into a\ncombo that blows the foe away.\n(Dissidia 1)", 35, 20, 120, 3),
    (0x19E, 'Storm Shot', 'brave', 'midair', "[Midair] A gust of strikes. Press\nagain to blow the foe away.\n(Dissidia 1)", 35, 15, 180, 12),
    (0x19F, 'Slidehazzard', 'brave', 'midair', "[Midair] A four-part combo that\ncarries the foe down to the ground.\n(Dissidia 1)", 35, 15, 180, 28),
    (0x19C, 'Soul Eater', 'hp', 'ground', "[Ground] HP attack. A strike, then\ndark slashes and the HP blow.\n(Dissidia 1)", 45, 25, 180, 24),
    (0x19D, 'Flood', 'hp', 'ground', "[Ground] HP attack. A torrent of\nwater sweeps the foe away.\n(Dissidia 1)", 45, 25, 180, 19),
    (0x1A1, 'Wind Shear', 'hp', 'midair', "[Midair] HP attack. A whirlwind\nholds the foe, then blasts it.\n(Dissidia 1)", 45, 25, 300, 46),
    (0x1A2, 'Paladin Force', 'hp', 'midair', "[Midair] HP attack. Holy blades\ncrash down on the foe.\n(Dissidia 1)", 45, 25, 180, 38),
]


D1_HELP_PACK, D1_HELP_ENTRY = 0xAE729F6B, 2   # D1's ability help texts (a mess pak 012 has no path for), by the ablt +4 id


def d1_help(d1):
    """{D1 command: 012 help text} from D1's own help: its description line and its orange effect line, with the
    record's 012 equip bonus ({bonus}) between them. D1 keeps ability records as {id, name, help, after, ?, command}."""
    import mess_text as mt
    import menu_pack as mp
    texts = mt.records(mp.entry(mp.parse(d1.read_crc(D1_HELP_PACK)), D1_HELP_ENTRY))
    _, ents = rm.mpk_unpack(d1.read_crc(rm.MAIN_CRC))
    _, aents = rm.arc_unpack(next(e for e in ents if e[0] == 'system.bin')[2])
    ab = next(e for e in aents if e[0] == 'ablt')[2]
    out = {}
    for i in range(struct.unpack_from('<I', ab, 8)[0]):
        _id, _nm, hid, _after, _x, cmd = struct.unpack_from('<6H', ab, 16 + 40 * i)
        if hid < len(texts):
            lines = texts[hid].split('\n')
            desc = [ln for ln in lines if '\x10' not in ln]          # D1's bonus and effect lines are coloured
            eff = [ln for ln in lines if ln.startswith(rm.EFFECT_PREFIX)]
            out[cmd] = '\n'.join(mark_d1(desc[:2]) + ['{bonus}'] + eff[:1])
    return out


D1_MARKS = (' (D1)',)      # one form for all eight (longer ones fit only some lines)


def mark_d1(desc):
    """The description lines with a note that the move comes from the first Dissidia, in the longest form that fits the
    help panel's width (the panel's 3 lines are already taken by description, bonus and effect)."""
    desc = list(desc) or ['']
    for m in D1_MARKS:
        if len(rm._shown(desc[-1] + m)) <= rm.HELP_WIDTH:
            desc[-1] += m
            return desc
    desc[-1] = desc[-1][:rm.HELP_WIDTH - len(D1_MARKS[-1])].rstrip() + D1_MARKS[-1]
    return desc


D1_DISC_ID = 'ULUS10437'   # Dissidia Final Fantasy (2008), USA: the release the shipped patches are made from


class IsoPackage(pe.Package):
    """PACKAGE.BIN / PACKAGE_INFO.BIN read in place from an ISO 9660 image."""

    def __init__(self, iso):
        # "fd:N": a descriptor the Android setup app's file picker handed over (as setup_from_iso.py takes it)
        self.iso = os.fdopen(os.dup(int(iso[3:].split(':')[0])), 'rb') if iso.startswith('fd:') else open(iso, 'rb')
        info_lba, info_size = self._find('PSP_GAME/USRDIR/DATA/PACKAGE_INFO.BIN')
        self.pkg_lba, _ = self._find('PSP_GAME/USRDIR/DATA/PACKAGE.BIN')
        self.iso.seek(info_lba * 2048)
        idx = self.iso.read(info_size)
        if idx[4:8] != b'pack':
            raise SystemExit('%s: PACKAGE_INFO.BIN is not a Dissidia package index' % iso)
        n = struct.unpack_from('<I', idx, 8)[0]
        self.entries = {}
        for i in range(n):
            crc, sector, sf = struct.unpack_from('<III', idx, 16 + 12 * i)
            self.entries[crc] = (sector, sf & 0x0FFFFFFF, sf >> 28)

    def _find(self, path):
        self.iso.seek(16 * 2048)
        pvd = self.iso.read(2048)
        if pvd[1:6] != b'CD001':
            raise SystemExit('not an ISO 9660 image')
        rec = pvd[156:190]
        for part in path.split('/'):
            lba, size = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
            self.iso.seek(lba * 2048)
            d = self.iso.read(size)
            i, found = 0, None
            while i < len(d):
                ln = d[i]
                if not ln:
                    i = (i // 2048 + 1) * 2048
                    continue
                r = d[i:i + ln]
                if r[33:33 + r[32]].decode('latin1').split(';')[0].upper() == part.upper():
                    found = r
                    break
                i += ln
            if found is None:
                raise SystemExit('%s not found in the ISO' % path)
            rec = found
        return struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]

    def disc_id(self):
        """(DISC_ID, TITLE) from PSP_GAME/PARAM.SFO, e.g. ('ULUS10437', 'DISSIDIA FINAL FANTASY')."""
        lba, size = self._find('PSP_GAME/PARAM.SFO')
        self.iso.seek(lba * 2048)
        s = self.iso.read(size)
        if s[:4] != b'\0PSF':
            return '', ''
        keys, data, n = struct.unpack_from('<III', s, 8)
        out = {}
        for i in range(n):
            ko, fmt, ln, _mx, do = struct.unpack_from('<HHIII', s, 20 + 16 * i)
            if fmt != 0x404:
                k = s[keys + ko:s.index(b'\0', keys + ko)].decode('latin1')
                out[k] = s[data + do:data + do + ln].split(b'\0')[0].decode('utf8', 'replace')
        return out.get('DISC_ID', ''), out.get('TITLE', '')

    def read_crc(self, crc):
        sector, size, _ = self.entries[crc]
        self.iso.seek((self.pkg_lba + sector) * 2048)
        return self.iso.read(size)


def prepare(src, d1, d12):
    """<src>/d1bartz (D1, 012 layout) and <src>/bartz (retail Bartz) for the Studio."""
    os.makedirs(src + '/_template')
    with open(src + '/_template/mod.json', 'w', encoding="utf-8") as f:
        f.write('{}')
    gmo = d1.read('obj/p_fiv100_1p.gmo')
    os.makedirs(src + '/d1bartz/models')
    with open(src + '/d1bartz/models/p_d1b_1p.gmo', 'wb') as f:
        f.write(gmo)
    co.unpack(d1_objx.convert(d1.read('obj/p_fiv100.objx')), src + '/d1bartz/moveset/p_d1b', bones=G.Model(gmo).bone_names())
    names = {'%02X' % (0x60 + i): {'cmd': '0x%X' % c, 'name': n, 'type': t, 'phase': ph}
             for i, (c, n, t, ph, *_r) in enumerate(MOVES)}
    with open(src + '/d1bartz/mod.json', 'w', encoding="utf-8") as f:      # based_on = this character: voices and face textures come along
        json.dump({'character': {'name': 'Bartz (DFF)', 'code': 'd1b', 'based_on': 'fiv100'}, 'move_names': names, 'files': {}}, f, indent=1)
    os.makedirs(src + '/bartz/models')
    files = {}
    for ms, rig in (('p_fiv100', 'p_fiv100_1p'), ('p_fiv100_a', 'p_fiv100_1p_a')):
        g = d12.read('obj/%s.gmo' % rig)
        with open('%s/bartz/models/%s.gmo' % (src, rig), 'wb') as f:
            f.write(g)
        co.unpack(d12.read('obj/%s.objx' % ms), '%s/bartz/moveset/%s' % (src, ms), bones=G.Model(g).bone_names())
        files['obj/%s.objx' % ms] = 'moveset/' + ms
        files['obj/%s.gmo' % rig] = 'models/%s.gmo' % rig
    with open(src + '/bartz/mod.json', 'w', encoding="utf-8") as f:      # move_names: the retail abilities, filled in by the page from the ISO
        json.dump({'kind': 'retail', 'character': {'name': 'Bartz', 'code': 'fiv100'}, 'move_names': {}, 'files': files}, f, indent=1)


def finish(mod, d1, d12):
    """Names, help, costs, learn levels; D1's voice lines in the slots 012's bank left empty."""
    with open(mod + '/mod.json', encoding="utf-8") as f:
        m = json.load(f)
    by_name = {mv[1]: mv for mv in MOVES}
    helps = d1_help(d1)
    for e in m['move_names'].values():
        if 'copy' not in e:
            continue
        cmd, name, _t, _p, help_, cp, cpm, ap, lv = by_name[e['name']]
        help_ = helps.get(cmd, help_)            # D1's own text; the one in MOVES if the disc has none
        rm.expand_help(help_, '')
        e.update(help=help_, cp=cp, cp_mastered=cpm, ap=ap, learn=lv)
        e.pop('status', None)
        e['_note'] = 'Dissidia (2008) Bartz move, command 0x%X there (tools/d1_moves.py)' % cmd
    m['display'] = 'Bartz: Dissidia (2008) moves'
    ms = mod + '/moveset/p_fiv100'
    rj = lambda p: json.load(open(p, encoding="utf-8"))  # noqa: E731
    acts = {a['action']: a['script'] for a in rj(ms + '/actions.json')['actions']}
    rows = rj(ms + '/motion_map.json')['rows']
    new_cmds = {int(e['cmd'], 16) for e in m['move_names'].values() if 'copy' in e}
    motions = set()
    for c in rj(ms + '/moves.json')['commands']:
        if int(c['id'], 16) in new_cmds:
            for st in c['steps']:
                motions |= {r['motion'] for r in rows if r['script'] == acts.get(st['action'])}
    used = set()
    for p in glob.glob(ms + '/timelines/*.json'):
        t = rj(p)
        if not p.endswith('index.json') and t['motion'] in motions:
            used |= {e['voice'] - VOICE_BASE for e in t['ranged'] + t['keyed'] if e.get('op') == 'voice'}
    b12, b1 = vb.Bank.parse(d12.read(VOICE_012)), vb.Bank.parse(d1.read_crc(D1_VOICE_BANK))
    filled = []
    for i in sorted(used):
        if 0 <= i < min(len(b12.entries), len(b1.entries)) and b12.entries[i]['kind'] == 'empty' and b1.entries[i]['kind'] == 'data':
            b12.entries[i] = {'raw': bytearray(b1.entries[i]['raw']), 'kind': 'data', 'owner': None, 'blob': b1.entries[i]['blob']}
            filled.append(i)
    if filled:
        p = mod + '/files/' + VOICE_012
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(b12.to_bytes())
        m['files'][VOICE_012] = 'files/' + VOICE_012
    with open(mod + '/mod.json', 'w', encoding="utf-8") as f:
        json.dump(m, f, indent=1, ensure_ascii=False)
        f.write('\n')
    print('voice lines from Dissidia (2008) in slots %s' % ', '.join(map(str, filled)))


# ---------------------------------------------------------------- patches (what a release ships) and install (the wizard)
PATCH_DIR = os.path.join(HERE, '..', 'd1_moves')            # games/dissidia/d1_moves: manifest.json + one .npd per file
DONORS = ('for100', 'six100', 'thr100', 'ten100', 'sev100', 'one100', 'two100')   # the moves' owners in 012 (effects)


def sources(spec, d1, d12, cache):
    """The bytes a patch applies to: 012:<path> (the player's own game), d1:<path> / d1crc:<hex> (their Dissidia 1
    disc), d1conv:<path> (a D1 moveset in the 012 layout, d1_objx.convert)."""
    out = []
    for s_ in spec:
        if s_ not in cache:
            kind, arg = s_.split(':', 1)
            if kind == '012':
                cache[s_] = d12.read(arg)
            elif kind == 'd1':
                cache[s_] = d1.read(arg)
            elif kind == 'd1crc':
                cache[s_] = d1.read_crc(int(arg, 16))
            elif kind == 'd1conv':
                cache[s_] = d1_objx.convert(d1.read(arg), log=lambda *x: None)
            else:
                raise SystemExit('unknown patch source %r' % s_)
        out.append(cache[s_])
    return b''.join(out)


def file_sources(gp):
    if gp.endswith('.objx'):
        return ['012:' + gp, 'd1conv:obj/p_fiv100.objx'] + ['012:obj/p_%s.objx' % c for c in DONORS]
    if gp.endswith('.gmo'):
        return ['012:' + gp, 'd1:obj/p_fiv100_1p.gmo']
    if gp.endswith('.scd'):
        return ['012:' + gp, 'd1crc:%08x' % D1_VOICE_BANK]
    raise SystemExit('no patch sources for %s' % gp)


def cmd_patches(a):
    """Dev: patches + manifest from a mods dir with bartz-dff built in (charmod.py build), for the release."""
    import netpack as npk
    npk.BLOCK = 8
    d1, d12 = IsoPackage(a.d1), pe.Package(a.disc)
    m = json.load(open(os.path.join(REPO, 'mods', 'bartz-dff', 'mod.json'), encoding="utf-8"))
    owner = json.load(open(os.path.join(a.mods, 'readme_charmod_retail.json'), encoding="utf-8"))
    os.makedirs(PATCH_DIR, exist_ok=True)
    for f in os.listdir(PATCH_DIR):
        if f.endswith('.npd'):
            os.remove(os.path.join(PATCH_DIR, f))
    man = {'_info': 'Bartz\'s Dissidia 1 moves: patches that rebuild the mod\'s files from the player\'s own Dissidia 012 and '
                    'Dissidia 1 discs (tools/d1_moves.py install). Made by tools/d1_moves.py patches.',
           'folder': 'bartz-dff', 'code': 'fiv100', 'display': m.get('display', 'Bartz: Dissidia (2008) moves'),
           'files': [], 'move_names': {}}
    cache = {}
    for gp in sorted(g for g, n in owner.items() if n == 'bartz-dff'):
        new = open(os.path.join(a.mods, gp), 'rb').read()
        spec = file_sources(gp)
        src = sources(spec, d1, d12, cache)
        d = npk.encode(src, new)
        assert npk.decode(d, src) == new
        name = os.path.basename(gp) + '.npd'
        open(os.path.join(PATCH_DIR, name), 'wb').write(d)
        man['files'].append({'path': gp, 'size': len(new), 'sha256': npk.sha(new), 'patch': name, 'sources': spec,
                             'sources_sha256': [npk.sha(sources([x], d1, d12, cache)) for x in spec]})
        print('%-42s patch %7d B' % (gp, len(d)))
    for k, e in m['move_names'].items():
        if not k.startswith('_') and 'copy' in e:
            e = {x: v for x, v in e.items() if x != 'help'}
            e['help_d1_cmd'] = next(mv[0] for mv in MOVES if mv[1] == e['name'])   # D1's own text, read at install
            man['move_names'][k] = e
    with open(os.path.join(PATCH_DIR, 'manifest.json'), 'w', encoding="utf-8") as f:
        json.dump(man, f, indent=1)
        f.write('\n')
    print('wrote', os.path.normpath(PATCH_DIR))


def cmd_install(a):
    """The installer's step: the mod's files into <work>/mods from the shipped patches and the player's discs, checked
    byte for byte (SHA-256); the new abilities into main.bin (roster_mainbin.py), the Settings switch (retail_moves.txt)
    and the online deltas (netpack.py)."""
    import netpack as npk
    man = json.load(open(os.path.join(PATCH_DIR, 'manifest.json'), encoding="utf-8"))
    d12 = pe.Package(discdir.disc_dir(a.work))
    d1 = IsoPackage(a.d1)
    disc, title = d1.disc_id()        # Dissidia 012 has obj/p_fiv100.objx too: the disc's own id tells the games apart
    if disc != D1_DISC_ID:
        what = '%s (%s)' % (title or 'this disc', disc or 'no PARAM.SFO')
        if 'duodecim' in title.lower():
            raise SystemExit('%s is Dissidia 012, %s: the moves need the first Dissidia (USA, ULUS-10437)' % (a.d1, what))
        raise SystemExit('%s is %s, not Dissidia Final Fantasy (USA, ULUS-10437)' % (a.d1, what))
    if pe.package_crc('obj/p_fiv100.objx') not in d1.entries:
        raise SystemExit('%s is not a Dissidia Final Fantasy (2008) disc image' % a.d1)
    mods = os.path.join(a.work, 'mods')
    cache = {}
    print('PROGRESS 5', flush=True)
    for i, f in enumerate(man['files']):
        for spec, want in zip(f['sources'], f['sources_sha256']):
            got = npk.sha(sources([spec], d1, d12, cache))
            if got != want:
                raise SystemExit('%s differs from the one the patches were made from (%s): the moves need the USA '
                                 'releases of both games' % (spec, 'your Dissidia 012' if spec.startswith('012') else 'your Dissidia 1 disc'))
        out = npk.decode(open(os.path.join(PATCH_DIR, f['patch']), 'rb').read(), sources(f['sources'], d1, d12, cache))
        if len(out) != f['size'] or npk.sha(out) != f['sha256']:
            raise SystemExit('%s did not rebuild to the expected file' % f['path'])
        p = os.path.join(mods, f['path'])
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as fh:
            fh.write(out)
        print('rebuilt %s' % f['path'])
        print('PROGRESS %d' % (10 + 60 * (i + 1) // len(man['files'])), flush=True)
    helps = d1_help(d1)
    names = {}
    for k, e in man['move_names'].items():
        e = dict(e)
        cmd = e.pop('help_d1_cmd')
        e['help'] = helps.get(cmd, next(mv[4] for mv in MOVES if mv[0] == cmd))
        rm.expand_help(e['help'], '')
        names[k] = e
    ab_p = os.path.join(mods, 'extra_abilities.json')
    ab = {}
    if os.path.exists(ab_p):
        try:
            ab = json.load(open(ab_p, encoding="utf-8"))
        except ValueError:      # cut short by a crash of an earlier install (v0.1.18 on Windows wrote it in cp1252 and stopped)
            os.replace(ab_p, ab_p + '.bad')
            print('%s could not be read (kept as %s.bad): written again' % (ab_p, os.path.basename(ab_p)), flush=True)
    ab[man['code']] = names
    with open(ab_p + '.tmp', 'w', encoding="utf-8") as fh:     # whole or not at all
        json.dump(ab, fh, indent=2, ensure_ascii=False)
        fh.write('\n')
    os.replace(ab_p + '.tmp', ab_p)
    ids = ','.join('%04X' % ((9 << 8) | int(k, 16)) for k in sorted(names))
    # files=: the mod's own files, left out by the game while the line is off (hooks_io.cpp)
    line = '%s 9 %s nm=%s files=%s' % (man['folder'], ids, man['display'].replace(' ', '_'), ';'.join(f['path'] for f in man['files']))
    rt = os.path.join(mods, 'retail_moves.txt')
    lines = open(rt, encoding="utf-8").read().splitlines() if os.path.exists(rt) else []
    off = any(l.startswith('#off# ' + man['folder'] + ' ') for l in lines)
    lines = [l for l in lines if not (l.split() and l.replace('#off# ', '').split()[0] == man['folder'])]
    lines.append(('#off# ' if off else '') + line)
    with open(rt, 'w', encoding="utf-8") as fh:
        fh.write('\n'.join(lines) + '\n')
    if not os.path.exists(os.path.join(mods, 'roster.txt')):
        open(os.path.join(mods, 'roster.txt'), 'w', encoding="utf-8").close()       # (roster_mainbin would make up a character without one)
    print('PROGRESS 75', flush=True)
    if subprocess.call(PYEXE + [os.path.join(HERE, 'roster_mainbin.py'), discdir.disc_dir(a.work), mods]):
        raise SystemExit('roster_mainbin.py failed')
    print('PROGRESS 90', flush=True)
    subprocess.call(PYEXE + [os.path.join(HERE, 'netpack.py'), 'deltas', mods, '--disc', discdir.disc_dir(a.work)])
    print('PROGRESS 100', flush=True)
    print('Bartz\'s Dissidia 1 moves installed (Settings > Mods switches them)')


def main(argv):
    if argv and argv[0] in ('install', 'patches'):
        ap = argparse.ArgumentParser(prog='d1_moves.py ' + argv[0])
        ap.add_argument('--d1', required=True, help='Dissidia Final Fantasy (2008) disc image (.iso)')
        if argv[0] == 'install':
            ap.add_argument('--work', required=True, help='the installed game folder (disc0/, mods/)')
            cmd_install(ap.parse_args(argv[1:]))
        else:
            ap.add_argument('--mods', required=True, help='a mods dir with bartz-dff built in')
            ap.add_argument('--disc', required=True, help='its disc0 folder')
            cmd_patches(ap.parse_args(argv[1:]))
        return
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--d1', required=True, help='Dissidia Final Fantasy (2008) ISO')
    ap.add_argument('--iso012', required=True, help='Dissidia 012 ISO (the one your game was set up from)')
    ap.add_argument('--out', default=os.path.join(REPO, 'mods', 'bartz-dff'))
    ap.add_argument('--keep', action='store_true', help='keep the scratch folder')
    a = ap.parse_args(argv)
    if os.path.exists(a.out):
        raise SystemExit('%s exists: delete it first' % a.out)
    d1, d12 = IsoPackage(a.d1), IsoPackage(a.iso012)
    if not d1.entries or rm.MAIN_CRC not in d12.entries:
        raise SystemExit('not the expected discs')
    tmp = tempfile.mkdtemp(prefix='d1moves_')
    try:
        src, out = tmp + '/src', tmp + '/out'
        prepare(src, d1, d12)
        js = os.path.join(HERE, 'd1_moves_studio.js')
        env = dict(os.environ)
        env.setdefault('NODE_PATH', os.path.join(HERE, 'node_modules'))
        r = subprocess.run(['node', js, os.path.join(REPO, 'studio', 'index.html'), src, os.path.abspath(a.iso012), out,
                            ','.join('%X' % mv[0] for mv in MOVES)], env=env, capture_output=True, text=True)
        log = r.stdout + r.stderr
        for line in log.splitlines():
            if 'TOAST' in line and 'retail characters' not in line or 'pageerror' in line:
                print(line.replace('[page] TOAST : ', '  '))
        if r.returncode or not os.path.exists(out + '/bartz/mod.json'):
            print(log[-3000:])
            raise SystemExit('the Move Studio step failed')
        finish(out + '/bartz', d1, d12)
        shutil.copytree(out + '/bartz', a.out)
        print('wrote %s: start the game (run.sh builds it in), or charmod.py build' % a.out)
    finally:
        if a.keep:
            print('scratch kept in', tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    main(sys.argv[1:])

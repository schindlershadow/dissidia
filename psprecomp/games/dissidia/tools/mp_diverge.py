#!/usr/bin/env python3
"""Divergent two-copy online test: two copies of the game on this host that differ the way two real machines do, so a per-machine local
decision (VS line-up, banters, roster, result) cannot match by coincidence. Walks both through the real menus (Communications > Tag Team
Battle > Mode Select > Group Battle room > Tournament), fights, then compares what the two copies logged and fails on any difference.

  mp_diverge.py --work DIR --mode team2|team3|ffa [--net lan|internet] [--out DIR] [--slot N] [--secs N] [--slow a|b|none]
                [--save-b DIR] [--env 'K=V ...'] [--env-a ...] [--env-b ...]

What differs between the copies (unlike mp_test.py, where both run the same save, settings and speed):
  * the save: A uses the work folder's save, B the Deck's (--save-b, default $PSPRECOMP_TEST_SAVE_B or ~/dissidia/tt/s2/saves/deck/ms0; a
    missing second save fails the test unless --allow-same-save),
  * settings: A 30 fps, render scale 1; B 120 fps, render scale 2 (--fps-a/--fps-b/--rs-a/--rs-b), voice: B Japanese (PSPRECOMP_VOICE=jp), A default,
  * speed: the --slow copy (default A, the host; --slow b = the joiner) is pinned to one CPU core (taskset, --slow-cpus) like a phone,
  * the lobby: random CPU fill on (the host's saved lobby), tag-ins on (KOs by KOLIST_NAT swap the mate in), random fill characters,
  * Internet mode goes through a local adhoc_relay (own port, own room), LAN mode through the loopback span of own UDP ports.
Ports: --slot N picks the debug ports (9600 + 2N, +1), the UDP port span (48000 + 32N) and the relay port (47900 + N) so several runs can go in
parallel; without --slot a free slot is claimed (/tmp/mp_diverge_slot_N.lock).

Compared across the copies (every difference is a FAIL, all listed in report.txt):
  NETSYNC   frames compared / mismatched (0 expected), DESYNC lines,   TTVS   the [TTVS] line-up (names normalised: "You" = own nick),
  BANTER    which banters played (character names, caption texts, order),   ROSTER  the host's roster list as each copy saw it,
  RESULT    winner / placing lines at the end of the battle.
Exit status 0 = PASS, 1 = FAIL (differences, a desync, or the battle never finished), 2 = could not run (no second save, no relay build...)."""
import argparse
import fcntl
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mp_test as m  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DECK_SAVE = os.path.expanduser(os.environ.get('PSPRECOMP_TEST_SAVE_B', '~/dissidia/tt/s2/saves/deck/ms0'))
# KOs by battle time (PSPRECOMP_TAGTEAM_KOLIST_NAT: like a natural lethal hit, the mate comes in; lock-step safe, side-based names)
KOLIST = {'team2': 'slot0:22,slot1:36,slot0:50,slot1:64', 'team3': 'slot0:22,slot1:34,slot0:46,slot1:58,slot0:70,slot1:82',
          'ffa': 'extra0:22,extra1:34,slot1:46'}
# the pad moves of the fight: mp_test's list plus an L press now and then (--lpress) (the human's lock switch: the lock cycle goes through retail's L handler on the copy
# whose pad it is, and through the lock-stepped input on the other; a local-only write there is a NETSYNC desync, tt-lockdesync)
LEVERY = int(os.environ.get('MP_LEVERY', '3'))   # an L press after every Nth move
LMASK = os.environ.get('MP_LMASK', '100')       # its buttons (hex PSP mask: 100 = L; 8100 = L + square, a lock switch together with an attack)


def exercise(a, b, secs, lpress):
    """mp_test's moves on both copies; lpress 'a' / 'b' / 'both' / 'none': which human also presses L (switches the lock) after every third move."""
    end, i = time.time() + secs, 0
    while time.time() < end:
        for c, off in ((a, 0), (b, 3)):
            kind, arg = m.MOVES[(i + off) % len(m.MOVES)]
            c.cmd(('A ' if kind == 'A' else 'A2 ' if kind == 'R' else 'B ') + arg)
            if (i + off) % LEVERY == LEVERY - 1 and lpress in ('both', c.side.lower()):
                c.cmd('B %s 150' % LMASK)
        i += 1
        time.sleep(0.7)


# lines that say the battle was decided / its result (both copies must show the same ones)
RESULT_RE = re.compile(r'^\[(?:TAGTEAM\] (?:t=[\d.]+s side \d has nobody left|group battle: winners|result menu)|FFARES\]|FFA\] result rows)')


def log(msg):
    print('[mp_diverge] ' + msg, file=sys.stderr, flush=True)


def claim_slot(want):
    """A free slot number: the debug ports, the UDP span and the relay port are derived from it. Held by a flock on /tmp/mp_diverge_slot_N.lock."""
    for n in ([want] if want is not None else range(0, 60)):
        f = open('/tmp/mp_diverge_slot_%d.lock' % n, 'w')
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            f.close()
            if want is not None:
                raise SystemExit('slot %d is taken' % n)
            continue
        ok = True
        for p, kind in ((9600 + 2 * n, socket.SOCK_STREAM), (9601 + 2 * n, socket.SOCK_STREAM), (47900 + n, socket.SOCK_DGRAM), (48000 + 32 * n, socket.SOCK_DGRAM),
                        (48008 + 32 * n, socket.SOCK_DGRAM)):   # the ports must be free as well (other sessions' rigs)
            s = socket.socket(socket.AF_INET, kind)
            try:
                s.bind(('127.0.0.1', p))
            except OSError:
                ok = False
            s.close()
        if ok:
            return n, f
        f.close()
    raise SystemExit('no free slot')


_CORE_LOCKS = []


def pick_core(slot):
    """The core the slow copy is pinned to: the idlest one right now (a 0.6 s /proc/stat sample), not a fixed number: on a shared host another session's rig can sit on
    the fixed core and the pinned copy then runs at a few frames a second. Claimed with a flock (/tmp/mp_diverge_core_N.lock) so two parallel checks never share one."""
    def sample():
        d = {}
        for l in open('/proc/stat'):
            if l.startswith('cpu') and l[3].isdigit():
                f = l.split()
                v = list(map(int, f[1:9]))
                d[int(f[0][3:])] = (v[3] + v[4], sum(v))
        return d
    try:
        a = sample()
        time.sleep(0.6)
        b = sample()
        busy = {c: 1.0 - (b[c][0] - a[c][0]) / max(1, b[c][1] - a[c][1]) for c in b}
    except (OSError, KeyError, ValueError):
        return str(2 + (slot * 3) % max(1, (os.cpu_count() or 18) - 2))
    for c in sorted((c for c in busy if c >= 2), key=lambda c: (round(busy[c], 1), c)):
        f = open('/tmp/mp_diverge_core_%d.lock' % c, 'w')
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            f.close()
            continue
        _CORE_LOCKS.append(f)
        return str(c)
    return str(2 + (slot * 3) % max(1, (os.cpu_count() or 18) - 2))


def build_relay():
    """tools/adhoc_relay built once into a cache dir (own copy: another session's rebuild cannot change it under us)."""
    src = os.path.join(HERE, '..', '..', '..', 'tools', 'adhoc_relay', 'adhoc_relay.cpp')
    d = os.path.expanduser('~/.cache/dissidia_mp_diverge')
    os.makedirs(d, exist_ok=True)
    exe = os.path.join(d, 'adhoc_relay')
    with open(os.path.join(d, 'build.lock'), 'w') as lk:
        fcntl.flock(lk, fcntl.LOCK_EX)
        if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
            tmp = exe + '.%d' % os.getpid()
            r = subprocess.run(['g++', '-O2', '-std=c++17', '-o', tmp, src], capture_output=True, text=True)
            if r.returncode:
                raise SystemExit('relay build failed: ' + r.stderr[-500:])
            os.replace(tmp, exe)
    return exe


# ---------------------------------------------------------------- extraction from a copy's log
def norm_lineup(line, nick):
    """'[TTVS] line-up: (free-for-all) | team 0: S31/You/L100/j0/pl ...' -> list of teams (card lists); "You" becomes the copy's own nick, the teams are
    sorted (each copy lists its own team first)."""
    parts = line.split(' | ')[1:]
    teams = []
    for p in parts:
        cards = p.split(':', 1)[1].split()
        teams.append(tuple(c.replace('/You/', '/%s/' % nick) for c in cards))
    return sorted(teams), '(free-for-all)' in line


def extract(text, nick):
    lines = text.splitlines()
    r = {'netsync': None, 'mismatch_lines': [], 'lineup': [], 'banter': [], 'roster': [], 'result': [], 'won': None}
    for l in lines:
        mm = re.match(r'\[NETSYNC\] so far: frames compared (\d+), mismatched (\d+)', l)
        if mm:
            r['netsync'] = (int(mm[1]), int(mm[2]))
        if ('the battles differ' in l or 'DESYNC' in l) and len(r['mismatch_lines']) < 5:
            r['mismatch_lines'].append(l[:200])
        if l.startswith('[TTVS] line-up:'):
            teams, ffa = norm_lineup(l, nick)
            r['lineup'].append({'ffa': ffa, 'teams': teams})
        mb = re.match(r'\[TAGTEAM\] banter of [0-9A-F]{8} \((.*?)\): pose', l)
        if mb:
            r['banter'].append('banter ' + mb[1])
        mb = re.match(r'\[TAGTEAM\] banter caption: text (\d+) for \d+ ticks \((.*)\)', l)
        if mb:
            r['banter'].append('caption %s (%s)' % (mb[1], mb[2]))
        mr = re.match(r'\[TTCPU\] roster from the host \(\d+ player\(s\)[^)]*\): (.*)$', l)   # the joiner says whether the host's list equals its own
        if mr:
            r['roster'].append(re.sub(r'\b[0-9a-f]{2}(?::[0-9a-f]{2}){5}\b', 'MAC', mr[1]).strip())
        mr = re.match(r'\[TTCPU\]   this machine had:(.*)$', l)
        if mr:
            r['roster'].append('this machine had:' + re.sub(r'\b[0-9a-f]{2}(?::[0-9a-f]{2}){5}\b', 'MAC', mr[1]).strip())
        if RESULT_RE.match(l):
            t = re.sub(r'\bYou\b', nick, re.sub(r't=[\d.]+s ?', '', re.sub(r'\b[0-9A-F]{8}\b', 'PTR', l))).strip()   # ("You" = the copy's own player)
            mw = re.search(r' \| you (won|did not win)$', t)   # perspective of this copy: compared as a pair below
            if mw:
                r['won'] = mw[1] == 'won'
                t = t[:mw.start()]
            r['result'].append(t)
    return r


def compare(ea, eb, min_frames, nores=False):
    """List of failure strings: everything that must be equal on both copies."""
    bad = []
    for side, e in (('A', ea), ('B', eb)):
        if e['netsync'] is None:
            bad.append('NETSYNC: copy %s logged no frame comparison (the lock-step never ran)' % side)
        else:
            if e['netsync'][1]:
                bad.append('NETSYNC: copy %s: %d of %d frames mismatched' % (side, e['netsync'][1], e['netsync'][0]))
            if e['netsync'][0] < min_frames:
                bad.append('NETSYNC: copy %s compared only %d frames (< %d): the battle was too short' % (side, e['netsync'][0], min_frames))
        for l in e['mismatch_lines']:
            bad.append('DESYNC (%s): %s' % (side, l))
    if ea['won'] and eb['won']:
        bad.append('RESULT: both copies say "you won"')
    for key, name in (('lineup', 'TTVS line-up'), ('banter', 'BANTER'), ('roster', 'ROSTER'), ('result', 'RESULT')):
        a, b = ea[key], eb[key]
        if key == 'roster':   # the host prints nothing comparable: a joiner's line must say "the same as this machine's"
            for side, e in (('A', ea), ('B', eb)):
                bad += ['ROSTER (%s): %s' % (side, x) for x in e['roster'] if not x.startswith("the same as")]
            continue
        if a != b:
            bad.append('%s differs: A has %d entries, B has %d' % (name, len(a), len(b)))   # (entries are shown first-difference only)
            n = max(len(a), len(b))
            for i in range(n):
                x = a[i] if i < len(a) else '(none)'
                y = b[i] if i < len(b) else '(none)'
                if x != y:
                    bad.append('   %s #%d  A: %s' % (name, i, x))
                    bad.append('   %s #%d  B: %s' % (name, i, y))
                    break
        elif key in ('lineup', 'result') and not a and not (nores and key == 'result'):
            bad.append('%s: no entry on either copy (the battle never got that far)' % name)
    return bad


# ---------------------------------------------------------------- the run
def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', required=True, help='a built work folder (its ms0 is copy A\'s save)')
    ap.add_argument('--mode', choices=sorted(KOLIST), default='team2')
    ap.add_argument('--net', choices=('lan', 'internet'), default='lan')
    ap.add_argument('--out', default=None)
    ap.add_argument('--slot', type=int, default=None, help='port slot (parallel runs need different ones; default: the first free)')
    ap.add_argument('--secs', type=float, default=110, help='most seconds of fighting before the battle must be decided')
    ap.add_argument('--slow', choices=('a', 'b', 'none'), default='a', help='the copy pinned to --slow-cpus (a = host, b = joiner)')
    ap.add_argument('--slow-cpus', default=None, help='taskset cpu list for the slow copy (default: one core derived from the slot)')
    ap.add_argument('--save-b', default=DECK_SAVE)
    ap.add_argument('--allow-same-save', action='store_true', help='B uses A\'s save when the second save is missing (weakens the test)')
    ap.add_argument('--same-save', action='store_true', help='B uses A\'s save (settings and speed still differ)')
    ap.add_argument('--same', action='store_true', help='control run: identical copies (same save, 30 fps, scale 1, no slow copy), like mp_test.py')
    ap.add_argument('--fps-a', type=int, default=30)
    ap.add_argument('--fps-b', type=int, default=120)
    ap.add_argument('--rs-a', default='1')
    ap.add_argument('--rs-b', default='2')
    ap.add_argument('--stale', action='store_true', help="copy B plays as a machine that already played a battle (PSPRECOMP_TAGTEAM_STALEREC: the overlay's team records hold the previous battle's parties); with PSPRECOMP_TAGTEAM_VSRECORDS_OLD=1 in --env this must FAIL (the old VS line-up bug)")
    ap.add_argument('--pack', action='store_true', help="the Bartz Dissidia 1 moves pack (net_packs.json): A has the mod installed (--pack-mods), B has an empty mods folder and gets the pack online "
                    "(netpack side channel) for the session; a Bartz CPU fighter is forced into the match (FILLCHAR) so a record / data difference between a native install and a received pack desyncs it")
    ap.add_argument('--pack-mods', default=os.environ.get('PSPRECOMP_TEST_PACK_MODS', '~/dissidia/tt/s2/packmods_bartz'), help='a mods folder with bartz-dff installed (default: ~/dissidia/tt/s2/packmods_bartz, a copy of a built mods folder without textures)')
    ap.add_argument('--pack-disc', default=os.environ.get('PSPRECOMP_TEST_PACK_DISC', '~/dissidia/work_tagslot/disc0'), help='the 012 disc folder netpack.py deltas reads the retail files from')
    ap.add_argument('--kolist', default=None, help="override the KO list ('none' = no KOs, the battle just runs --secs and no result is required)")
    ap.add_argument('--min-frames', type=int, default=600)
    ap.add_argument('--lpress', choices=('both', 'a', 'b', 'none'), default='both', help='which copy\'s human also presses L (switches the lock) during the fight')
    ap.add_argument('--env', default='')
    ap.add_argument('--env-a', default='')
    ap.add_argument('--env-b', default='')
    ap.add_argument('--shots', action='store_true')
    a = ap.parse_args(argv)
    work = os.path.abspath(os.path.expanduser(a.work))
    if a.same:
        a.same_save, a.fps_b, a.rs_b, a.slow = True, a.fps_a, a.rs_a, 'none'
    out = os.path.abspath(os.path.expanduser(a.out or 'mp_diverge_%s_%s' % (a.mode, a.net)))
    os.makedirs(out, exist_ok=True)
    for f in ('report.txt', 'extract.json'):   # (not the whole dir: tagteam_test.py keeps check.log in it)
        if os.path.exists(os.path.join(out, f)):
            os.remove(os.path.join(out, f))
    save_b = os.path.abspath(os.path.expanduser(a.save_b))
    if a.same_save:
        save_b = os.path.join(work, 'ms0')
    elif not os.path.isdir(os.path.join(save_b, 'PSP')):
        if not a.allow_same_save:
            print('mp_diverge: SKIP/ERROR: the second save %s is missing (copy the Deck\'s ms0 there, or set PSPRECOMP_TEST_SAVE_B)' % save_b)
            return 2
        save_b = os.path.join(work, 'ms0')
    slot, lock = claim_slot(a.slot)
    m.PORT['A'], m.PORT['B'] = 9600 + 2 * slot, 9601 + 2 * slot
    udp = 48000 + 32 * slot
    relay_port = 47900 + slot
    room = 'mpdiv%d_%d' % (slot, os.getpid())
    kv = lambda s: dict(x.split('=', 1) for x in s.split() if '=' in x)
    nores = a.kolist == 'none'
    base = {'PSPRECOMP_CLOUD': '0', 'PSPRECOMP_TAGTEAM_LOG': '1', 'PSPRECOMP_TAGTEAM_KOLIST': a.kolist or KOLIST[a.mode], 'PSPRECOMP_TAGTEAM_KOLIST_NAT': '1', 'PSPRECOMP_TAGTEAM_KOLIST_LSFRAME': '1',
            'PSPRECOMP_ADHOC_SPAN': '4', 'MP_NETMODE': a.net, **kv(a.env)}
    if nores:
        base.pop('PSPRECOMP_TAGTEAM_KOLIST'); base.pop('PSPRECOMP_TAGTEAM_KOLIST_NAT'); base.pop('PSPRECOMP_TAGTEAM_KOLIST_LSFRAME')
    cpus = a.slow_cpus or (pick_core(slot) if a.slow != 'none' else '2')
    base.setdefault('PSPRECOMP_VK_RECORD', '1')
    envA = {**base, 'MP_FILL': '1', 'MP_FPS': str(a.fps_a), 'MP_RS': a.rs_a, 'PSPRECOMP_ADHOC_PORT': str(udp), **kv(a.env_a)}
    envB = {**base, 'MP_SAVE': save_b, 'MP_FPS': str(a.fps_b), 'MP_INI': 'voice=jp', 'MP_RS': a.rs_b, 'PSPRECOMP_ADHOC_PORT': str(udp + 8), **kv(a.env_b)}
    if a.net == 'lan':   # LAN: each copy knows the other's span (no broadcast to the real LAN reaches another job)
        envA['PSPRECOMP_ADHOC_PEERS'] = '127.0.0.1:%d' % (udp + 8)
        envB['PSPRECOMP_ADHOC_PEERS'] = '127.0.0.1:%d' % udp
    else:
        envA['PSPRECOMP_ADHOC_SERVER'] = envB['PSPRECOMP_ADHOC_SERVER'] = '127.0.0.1:%d' % relay_port
        envA['PSPRECOMP_ADHOC_ROOM'] = envB['PSPRECOMP_ADHOC_ROOM'] = room
    if a.pack:   # A: the mod installed (+ the deltas it sends), B: an empty mods folder that receives the pack
        src = os.path.abspath(os.path.expanduser(a.pack_mods))
        if not os.path.isfile(os.path.join(src, 'retail_moves.txt')):
            print('mp_diverge: SKIP/ERROR: %s has no bartz-dff install (retail_moves.txt); set --pack-mods / PSPRECOMP_TEST_PACK_MODS' % src)
            return 2
        ma, mb = os.path.join(out, 'mods_A'), os.path.join(out, 'mods_B')
        for d in (ma, mb):
            shutil.rmtree(d, ignore_errors=True)
        shutil.copytree(src, ma, ignore=shutil.ignore_patterns('textures', '.net_cache', 'cloud*'))
        os.makedirs(mb)
        r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'netpack.py'), 'deltas', ma, '--disc', os.path.abspath(os.path.expanduser(a.pack_disc))], capture_output=True, text=True)
        if 'shareable online' not in r.stdout:
            print('mp_diverge: SKIP/ERROR: the pack cannot be made shareable from %s: %s' % (src, (r.stdout + r.stderr)[-300:]))
            return 2
        envA['PSPRECOMP_MODS'], envB['PSPRECOMP_MODS'] = ma, mb
        envA.setdefault('PSPRECOMP_TAGTEAM_FILLCHAR', '0,9')   # side 1's first CPU fill is Bartz (charId 9)
        envB.setdefault('PSPRECOMP_TAGTEAM_FILLCHAR', '0,9')
    if a.stale:
        envB['PSPRECOMP_TAGTEAM_STALEREC'] = '24,11,4,31,28;31,30,26,21,5'
    if a.slow != 'none':
        (envA if a.slow == 'a' else envB)['MP_TASKSET'] = cpus
    relay = None
    A = m.Copy('A', work, out, envA)
    B = m.Copy('B', work, out, envB)
    verdict, bad, ea, eb = 'FAIL', [], None, None
    try:
        if a.net == 'internet':
            relay = subprocess.Popen([build_relay(), '--port', str(relay_port), '--bind', '127.0.0.1'], stdout=open(os.path.join(out, 'relay.log'), 'w'),
                                     stderr=subprocess.STDOUT, start_new_session=True)
            time.sleep(0.5)
        log('slot %d: ports debug %d/%d udp %d%s, A %d fps rs %s%s, B %d fps rs %s%s, save B %s' % (slot, m.PORT['A'], m.PORT['B'], udp,
            ' relay %d' % relay_port if relay else '', a.fps_a, a.rs_a, ' (1 core: %s)' % cpus if a.slow == 'a' else '', a.fps_b, a.rs_b,
            ' (1 core: %s)' % cpus if a.slow == 'b' else '', save_b))
        A.start()
        B.start()
        t0 = time.time()
        m.walk(A, B, 4.0 if a.slow != 'none' else 0.0, a.mode, out if a.shots else None)
        log('in battle after %.0f s' % (time.time() - t0))
        # the fight budget is wall time, but a loaded host runs fewer frames in it: the budget scales with the host load, and goes on past it while the
        # battle still progresses (compared frames rising) up to 3x. A real mismatch / DESYNC ends the fight at once (never waited out).
        sc = m.load_scale()
        end = time.time() + a.secs * sc
        hard = time.time() + a.secs * max(sc, 1.0) * 3
        if sc > 1.0:
            log('host load %.1f on %d cores: fight budget %.0f s (x%.1f), up to %.0f s while progressing' % (os.getloadavg()[0], os.cpu_count() or 0, a.secs * sc, sc, hard - time.time()))
        last_frames, last_prog = None, time.time()
        while True:
            exercise(A, B, 5, a.lpress)
            texts = [c.text() for c in (A, B)]
            if not nores and all(any(RESULT_RE.match(l) for l in t.splitlines()) for t in texts) and all('[NETSYNC] so far' in t for t in texts):
                break
            ex = [extract(t, 'X') for t in texts]
            if any(e['mismatch_lines'] or (e['netsync'] or (0, 0))[1] for e in ex):
                log('mismatch seen: ending the fight now')
                break
            now = time.time()
            frames = tuple((e['netsync'] or (0, 0))[0] for e in ex)   # (a pinned slow copy on a loaded host may run at a few frames a second: it still counts as progress)
            if frames != last_frames:
                last_frames, last_prog = frames, now
            if now >= hard or (now >= end and (nores or now - last_prog > 60)):   # (past the nominal budget only while the frames keep coming)
                break
        time.sleep(6)   # the last NETSYNC summary
        ea, eb = extract(A.text(), 'A'), extract(B.text(), 'B')
        bad = compare(ea, eb, a.min_frames, nores)
        if 'Google Drive sync on' in A.text() + B.text():
            bad.append('CLOUD: a copy synced to Google Drive (the test dirs must carry no cloud.ini)')
        if a.pack:
            if 'applied for this session' not in B.text():
                bad.append('PACK: copy B never applied the online pack (the test did not exercise it)')
            if not re.search(r'extra\w* \w+ c9 ', A.text()) and not re.search(r'\(char 9\)', A.text()):
                bad.append('PACK: Bartz (charId 9) was not in the fight (FILLCHAR not honoured): the test does not exercise the pack records')
        verdict = 'FAIL' if bad else 'PASS'
    except RuntimeError as e:
        bad.append('walk failed: %s' % e)
    finally:
        A.stop()
        B.stop()
        if relay:
            try:
                os.killpg(relay.pid, signal.SIGTERM)
            except OSError:
                pass
        lock.close()
    rep = ['%s  mp_diverge mode=%s net=%s slot=%d slow=%s' % (verdict, a.mode, a.net, slot, a.slow)]
    if ea and eb:
        rep.append('A: netsync %s, %d line-up, %d banter, %d roster, %d result' % (ea['netsync'], len(ea['lineup']), len(ea['banter']), len(ea['roster']), len(ea['result'])))
        rep.append('B: netsync %s, %d line-up, %d banter, %d roster, %d result' % (eb['netsync'], len(eb['lineup']), len(eb['banter']), len(eb['roster']), len(eb['result'])))
    rep += bad
    open(os.path.join(out, 'report.txt'), 'w').write('\n'.join(rep) + '\n')
    if ea and eb:
        json.dump({'A': ea, 'B': eb}, open(os.path.join(out, 'extract.json'), 'w'), indent=1, default=list)
    print('\n'.join(rep))
    return 0 if verdict == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

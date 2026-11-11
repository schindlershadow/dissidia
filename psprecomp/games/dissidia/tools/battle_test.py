#!/usr/bin/env python3
"""Automated battle tests: boot the game headless, walk the menus to a Quick Battle (Squall vs a CPU fighter), play a
scripted pad input in game time, quit, and compare the same script at several frame rates.

  battle_test.py run     --work DIR --script FILE [--fps 120] [--out DIR] [--char Squall] [--timeout 240]
  battle_test.py compare --work DIR --script FILE [--rates 30,120] [--out DIR]
  battle_test.py actions --work DIR --actions 2422,2423,... | --all [--rates 30,120] [--gap 4500] [--who 0] [--hold TRIANGLE --stick 'x,y;x,y']
                                            start each action by id (no button presses, whatever the setup) and compare
                                            how it moves at each rate; --all lists the fighter's actions first
  battle_test.py analyze LOG30 LOG120       (compare two finished logs)

What it needs: a work folder with a save (the Quick Battle has to be unlocked, `ms0/PSP/SAVEDATA/*GameData00`), the
runtime built with the test driver (games/dissidia/runtime/framerate.cpp), Xvfb and python3 with Pillow (for the menu
checks). The game runs on its own virtual X display (a GPU is used when the box has one; nothing appears on a real
screen), with its own settings file (--fps, no fullscreen, no HD textures) and a private copy of the work folder's
memory stick, so the user's settings and saves are never touched.

The menu trip is the one in docs/testing.md: title (CROSS), save list (CROSS), main menu (DOWN, CROSS, CROSS),
Player Select (cursor to the character, CROSS, YES), CPU Select (CROSS, YES), battle setup (CROSS = Enter Battle), then
CROSS on the loading / opponent info pages until the runtime logs "[TEST] battle started". Each press waits for the
picture to change and settle, so a slow load does not eat a press.

A test script (see framerate.cpp 'Test driver') is a list of `<game ms> set|tap|analog|mark|quit ...` lines; times count
from the first update of the fight, in game time, so they mean the same at every frame rate.

`compare` runs the script at each rate and lines the runs up by game time: for every fighter and spawned object it
compares where it was at the same game moments, and reports the largest horizontal-distance difference and the first
moment the paths part. Rates that disagree by more than --tolerance metres fail (exit 1)."""
import argparse
import math
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

BTN = {'select': 0x1, 'start': 0x8, 'up': 0x10, 'right': 0x20, 'down': 0x40, 'left': 0x80, 'l': 0x100, 'r': 0x200,
       'triangle': 0x1000, 'circle': 0x2000, 'cross': 0x4000, 'square': 0x8000}
HERE = os.path.dirname(os.path.abspath(__file__))

# Player Select grid: (row, column) of each character with the cursor starting on (0, 0); read off the screen
GRID = {'lightning': (0, 0), 'vaan': (0, 1), 'squall': (1, 5)}
CHAR_ID = {'lightning': 31, 'squall': 15}      # the game's charId (the fighter the tests drive; the other one is the dummy)


class Game:
    def __init__(self, work, fps, out, display, port, script=None, extra_env=None, record=None):
        self.work, self.fps, self.out, self.display, self.port = work, fps, out, display, port
        self.log = os.path.join(out, 'run_%d.log' % fps)
        self.script, self.extra_env, self.record = script, extra_env or {}, record
        self.proc = None
        self.go_file = os.path.join(out, 'go_%d' % fps)
        self.pos = 0
        self.n = 0

    def start(self):
        ms0 = os.path.join(self.out, 'ms0_%d' % self.fps)
        if os.path.exists(ms0):
            shutil.rmtree(ms0)
        shutil.copytree(os.path.join(self.work, 'ms0'), ms0)
        ini = os.path.join(self.out, 'settings_%d.ini' % self.fps)
        with open(ini, 'w') as f:
            f.write('display=window\nfps=%d\nrender_scale=1\nwidescreen=0\nvoice=en\ncheck_updates=0\nhd_textures=0\n' % self.fps)
        env = dict(os.environ, DISPLAY=self.display, PSPRECOMP_SETTINGS=ini, PSPRECOMP_MS0=ms0, PSPRECOMP_FIGHTER_LOG='1',
                   PSPRECOMP_DEBUG_PORT=str(self.port), PSPRECOMP_SAVEDATA_AUTO='0', PSPRECOMP_TEST_FREEZE_CPU='1')
        env.pop('PSPRECOMP_FPS', None)
        if os.path.exists(self.go_file):
            os.remove(self.go_file)
        env['PSPRECOMP_TEST_GO'] = self.go_file
        if self.script:
            env['PSPRECOMP_TEST_SCRIPT'] = self.script
        if self.record:
            env['PSPRECOMP_INPUT_RECORD'] = self.record
        env.update(self.extra_env)
        self.proc = subprocess.Popen([os.path.join(HERE, 'run.sh'), self.work, '--window', '480x272', '--log', self.log], env=env,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def stop(self):
        if self.proc and self.alive():
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except OSError:
                pass
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)

    def cmd(self, line):
        with socket.create_connection(('127.0.0.1', self.port), timeout=15) as s:
            s.sendall(line.encode() + b'\n')
            return s.makefile('rb').readline().decode(errors='replace').strip()

    def press(self, *names, ms=120):
        mask = 0
        for n in names:
            mask |= BTN[n]
        self.cmd('B %X %d' % (mask, ms))

    def shot(self):
        from PIL import Image
        self.n += 1
        p = os.path.join(self.out, 'shot_%d.tga' % (self.n % 3))
        self.cmd('S ' + p)
        return Image.open(p).convert('RGB')

    def lines(self):
        try:
            with open(self.log, 'rb') as f:
                f.seek(self.pos)
                data = f.read()
        except OSError:
            return []
        cut = data.rfind(b'\n') + 1
        self.pos += cut
        return data[:cut].decode(errors='replace').splitlines()

    def wait_log(self, pattern, timeout):
        end = time.time() + timeout
        rx = re.compile(pattern)
        while time.time() < end:
            if not self.alive():
                raise RuntimeError('the game exited')
            for l in self.lines():
                if rx.search(l):
                    return l
            time.sleep(0.25)
        raise RuntimeError('timeout waiting for /%s/' % pattern)

    def settle(self, timeout=12, quiet=0.7):
        """wait until the picture stops changing (two shots `quiet` s apart are the same)"""
        end = time.time() + timeout
        prev = None
        while time.time() < end:
            cur = self.shot().resize((60, 34)).tobytes()
            if prev is not None and sum(abs(a - b) for a, b in zip(cur, prev)) < 600:
                return
            prev = cur
            time.sleep(quiet)

    def step(self, *names, wait=1.0):
        self.press(*names)
        time.sleep(0.35)
        self.settle(quiet=wait)


def free_display():
    for n in range(90, 120):
        if not os.path.exists('/tmp/.X11-unix/X%d' % n) and not os.path.exists('/tmp/.X%d-lock' % n):
            return n
    raise RuntimeError('no free X display')


def start_xvfb():
    n = free_display()
    p = subprocess.Popen(['Xvfb', ':%d' % n, '-screen', '0', '1280x720x24', '-nolisten', 'tcp', '-ac', '+extension', 'GLX'],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    for _ in range(50):
        if os.path.exists('/tmp/.X11-unix/X%d' % n):
            break
        time.sleep(0.1)
    return p, ':%d' % n


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def walk_menus(g, char, tries=14, stage=12):
    """boot -> battle. Raises when a stage does not come up."""
    g.wait_log(r'Debug socket listening', 60)
    # title: CROSS until the save list opens
    end = time.time() + 120
    while time.time() < end:
        g.press('cross')
        time.sleep(1.5)
        if any('list dialog opened' in l or 'complete: mode=4' in l for l in g.lines()):
            break
    else:
        raise RuntimeError('the save list never opened')
    time.sleep(1.0)
    g.press('cross')                                   # the save list
    g.wait_log(r'sceUtilitySavedata complete: mode=4 result=0x0+\b', 40)
    time.sleep(2.0)
    g.settle(20)
    g.step('down')                                     # Story Mode -> Battle Mode
    g.step('cross')
    g.step('cross')                                    # Quick Battle (1-on-1) -> Player Select
    time.sleep(1.0)
    row, col = GRID[char.lower()]
    for _ in range(row):
        g.step('down', wait=0.3)
    for _ in range(col):
        g.step('right', wait=0.3)
    g.step('cross')
    g.step('cross')                                    # YES
    g.step('cross')                                    # CPU Select: Lightning
    g.step('cross')                                    # YES
    time.sleep(2.0)
    g.settle(10)
    if stage:                                          # battle setup: STAGE row, entry `stage` of 20 (1 = Random), back to Enter Battle
        g.step('down', wait=0.4)
        for _ in range(stage - 1):
            g.step('right', wait=0.25)
        g.step('up', wait=0.4)
    g.press('cross')                                   # Enter Battle
    # loading / opponent pages: CROSS ("Next") until the fight's HUD (dark gauge bar along the bottom) is on screen
    seen = 0
    for _ in range(tries * 6):
        if not g.alive():
            raise RuntimeError('the game exited')
        time.sleep(1.0)
        im = g.shot()
        dark = sum(1 for p in ((120, 262), (360, 262), (30, 262), (450, 262)) if sum(im.getpixel(p)) < 330)
        seen = seen + 1 if dark >= 3 else 0
        if seen >= 2:
            open(g.go_file, 'w').close()                   # the runtime starts its game clock now
            g.wait_log(r'\[TEST\] battle started', 10)
            return
        if seen == 0:
            g.press('cross')
    raise RuntimeError('the battle never started')


def run(a, fps=None):
    fps = fps or a.fps
    os.makedirs(a.out, exist_ok=True)
    xp, disp = start_xvfb()
    g = Game(os.path.abspath(a.work), fps, a.out, disp, free_port(), script=os.path.abspath(a.script) if a.script else None,
             record=os.path.abspath(a.record) if getattr(a, 'record', None) else None,
             extra_env={'PSPRECOMP_TEST_PLAYER_CHAR': str(CHAR_ID[a.char.lower()])})
    try:
        g.start()
        t0 = time.time()
        walk_menus(g, a.char, stage=getattr(a, 'stage', 12))
        print('[battle_test] %d fps: in battle after %.0f s' % (fps, time.time() - t0), file=sys.stderr)
        if getattr(a, 'watch', None):                  # arm the runtime's memory-write watch (debug socket M) `watch_after` s after the clock starts
            time.sleep(a.watch_after)
            g.cmd('M ' + a.watch)
        if a.script:
            end = time.time() + a.timeout
            while g.alive() and time.time() < end:
                time.sleep(0.5)
            if g.alive():
                raise RuntimeError('the script did not finish (no quit) within %d s' % a.timeout)
        else:
            input('in battle (virtual display %s, debug port %d); Enter to quit ' % (disp, g.port))
        return g.log
    finally:
        g.stop()
        xp.terminate()


# ---- per-action tests ------------------------------------------------------------------------------------------------

def list_actions(a, who):
    """run a short battle and return [(action id, script id)] of fighter slot `who` (0 = the player)"""
    path = os.path.join(a.out, 'list.txt')
    with open(path, 'w') as f:
        f.write('2000 action -1 %d\n3000 quit\n' % who)
    a2 = argparse.Namespace(**vars(a))
    a2.script, a2.fps = path, 30
    a2.out = os.path.join(a.out, 'list')
    log = run(a2)
    acts, take = [], False
    for l in open(log, errors='replace'):
        if '[TEST] plya actor' in l:
            take = not acts
        m = re.search(r'\[TEST\] plya action (\d+) script (-?\d+)', l)
        if m and take:
            acts.append((int(m[1]), int(m[2])))
    return acts


def action_windows(path):
    """[(action id, gt, fighter address)] from the [TEST] action lines, and the fighter tracks {addr: [(gt, x, z, scr)]}"""
    wins, tracks = [], {}
    for l in open(path, errors='replace'):
        m = re.match(r'\[TEST\] gt=(\d+) action (-?\d+) on ([0-9A-F]{8}) .*: (started|NOT started|no such record)', l)
        if m:
            wins.append((int(m[2]), int(m[1]), m[3], m[4]))
        elif l.startswith('[FIGHTER]'):
            m = re.search(r'gt=(\S+) t=\d+ ([0-9A-F]{8}) scr=(\d+) (?:cur=(-?\d+) )?.*?pos=(\S+),(\S+) y=', l)
            if m:
                tracks.setdefault(m[2], []).append((float(m[1]), float(m[5]), float(m[6]), int(m[4]) if m[4] else int(m[3])))
    return wins, tracks


def measure(track, t0, t1):
    """what the fighter did in game time [t0, t1): the scripts in order with their length (ms), and, for the FIRST script
    (the move itself; what follows is recovery and walking), the distance covered (m) and the peak speed (m/s over 200 ms)"""
    seg = [r for r in track if t0 <= r[0] < t1]
    if len(seg) < 3:
        return None
    scripts, last = [], None
    for r in seg:
        if r[3] != last:
            scripts.append([r[3], r[0], r[0]])
            last = r[3]
        scripts[-1][2] = r[0]
    scripts = [x for x in scripts if x[0] not in (65535, -1)]
    if not scripts:
        return None
    first = [r for r in seg if scripts[0][1] <= r[0] <= scripts[0][2]]
    dist = sum(math.hypot(v[1] - u[1], v[2] - u[2]) for u, v in zip(first, first[1:]))
    peak = 0.0
    for i, u in enumerate(first):
        for v in first[i + 1:]:
            if v[0] - u[0] >= 200:
                peak = max(peak, math.hypot(v[1] - u[1], v[2] - u[2]) / ((v[0] - u[0]) / 1000.0))
                break
    return [(x[0], x[2] - x[1]) for x in scripts], dist, peak


def actions_compare(a):
    rates = [int(x) for x in a.rates.split(',')]
    if a.all:
        acts = list_actions(a, a.who)
        print('%d actions' % len(acts), file=sys.stderr)
        ids = [i for i, _ in acts]
        scr_of = dict(acts)
    else:
        ids = [int(x) for x in a.actions.split(',')]
        scr_of = {}
    path = os.path.join(a.out, 'actions.txt')
    with open(path, 'w') as f:
        t = 3000
        sticks = a.stick.split(';')                     # one stick position per action in turn: '128,0;255,128'
        for k, i in enumerate(ids):
            f.write('%d action %d %d\n' % (t, i, a.who))
            if a.hold:                                  # keep the button (and stick) down, as a player holding the move on
                f.write('%d set %s %s\n' % (t + 40, a.hold, sticks[k % len(sticks)].replace(',', ' ')))
                f.write('%d set - 128 128\n' % (t + 40 + a.hold_ms))
            t += a.gap
        f.write('%d quit\n' % (t + 1000))
    a.script = path
    a.timeout = max(a.timeout, int(len(ids) * a.gap / 1000 * 6) + 120)
    logs = {}
    for r in rates:
        a.fps = r
        logs[r] = run(a, r)
    data = {r: action_windows(logs[r]) for r in rates}
    bad = 0
    print('%-7s %-7s %-6s  %s' % ('action', 'script', 'rate', 'result'))
    for k, i in enumerate(ids):
        rows = {}
        for r in rates:
            wins, tracks = data[r]
            w = [x for x in wins if x[0] == i][:]
            same = [x for x in ids[:k] if x == i]       # the same action listed again: take its n-th run
            w = w[len(same):] if len(w) > len(same) else []
            if not w:
                rows[r] = 'missing'
                continue
            _, gt, addr, state = w[0]
            if state != 'started':
                rows[r] = state
                continue
            m = measure(tracks.get(addr, []), gt, gt + a.gap)
            rows[r] = m if m else 'no data'
        base = rows[rates[0]]
        line_bad = False
        for r in rates:
            v = rows[r]
            if isinstance(v, str):
                print('%-7d %-7s %-6d  %s' % (i, scr_of.get(i, ''), r, v))
                line_bad = line_bad or r == rates[0] and False
                continue
            sc, dist, peak = v
            note = ''
            if isinstance(base, tuple) and r != rates[0]:
                b = base
                first = lambda m: m[0][0][1] if m[0] else 0
                if b[2] > 0.5 and peak > b[2] * a.speed_tol:
                    note += ' PEAK SPEED x%.2f' % (peak / b[2]); line_bad = True
                if b[1] > 0.5 and not (1 / a.dist_tol <= dist / b[1] <= a.dist_tol):
                    note += ' DISTANCE x%.2f' % (dist / b[1]); line_bad = True
                if b[0] and sc and b[0][0][0] == sc[0][0] and b[0][0][1] > 300 and abs(sc[0][1] - b[0][0][1]) > 0.25 * b[0][0][1] + 120:
                    note += ' DURATION %d vs %d ms' % (sc[0][1], b[0][0][1]); line_bad = True
            print('%-7d %-7s %-6d  scripts %s  moved %.2f m  peak %.2f m/s%s' % (
                i, scr_of.get(i, ''), r, ','.join('%d(%d)' % s for s in sc[:4]), dist, peak, note))
        bad += line_bad
    print('%d of %d actions differ between %s fps' % (bad, len(ids), '/'.join(map(str, rates))))
    return 1 if bad else 0


# ---- analysis -------------------------------------------------------------------------------------------------------

def parse_log(path):
    """{actor: [(gt, x, z, scr)]} from the [FIGHTER] and [MOTION] lines"""
    tracks = {}
    for l in open(path, errors='replace'):
        if l.startswith('[FIGHTER]'):
            m = re.search(r'gt=(\S+) t=\d+ ([0-9A-F]{8}) scr=(\d+) (?:cur=(-?\d+) )?.*?pos=(\S+),(\S+) y=(\S+)', l)
            if m:
                tracks.setdefault('fighter ' + m[2], []).append((float(m[1]), float(m[5]), float(m[6]), int(m[4]) if m[4] else int(m[3])))
        elif l.startswith('[MOTION]'):
            m = re.search(r'gt=(\S+) pos=(\S+),(\S+),(\S+) t=\d+ ([0-9A-F]{8}) kind=(\d+).*scr=(\d+)', l)
            if m:
                tracks.setdefault('object %s kind %s' % (m[5], m[6]), []).append((float(m[1]), float(m[2]), float(m[4]), int(m[7])))
    return tracks


def at(track, t):
    """position at game time t (linear between samples; None outside the track)"""
    if not track or t < track[0][0] or t > track[-1][0]:
        return None
    lo, hi = 0, len(track) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if track[mid][0] <= t:
            lo = mid
        else:
            hi = mid
    a, b = track[lo], track[hi]
    k = 0 if b[0] == a[0] else (t - a[0]) / (b[0] - a[0])
    return a[1] + k * (b[1] - a[1]), a[2] + k * (b[2] - a[2])


def analyze(logs, tolerance=0.5, quiet=False):
    """compare the same script run at different rates. Returns the worst disagreement (metres)."""
    runs = [(p, parse_log(p)) for p in logs]
    base_p, base = runs[0]
    worst = 0.0
    for p, other in runs[1:]:
        print('== %s vs %s' % (os.path.basename(base_p), os.path.basename(p)))
        # fighters/objects are matched by their rank in first appearance (addresses differ between runs)
        bk = sorted(base, key=lambda k: base[k][0][0])
        ok_ = sorted(other, key=lambda k: other[k][0][0])
        for i, k in enumerate(bk):
            if i >= len(ok_):
                break
            ta, tb = base[k], other[ok_[i]]
            if len(ta) < 5 or len(tb) < 5:
                continue
            t0, t1 = max(ta[0][0], tb[0][0]), min(ta[-1][0], tb[-1][0])
            maxd, first, t = 0.0, None, t0
            while t <= t1:
                pa, pb = at(ta, t), at(tb, t)
                if pa and pb:
                    d = math.hypot(pa[0] - pb[0], pa[1] - pb[1])
                    if d > maxd:
                        maxd = d
                    if first is None and d > tolerance:
                        first = t
                t += 33.3
            # total path length per game second: the rate the thing moves at
            def speed(tr):
                tot = 0.0
                for u, v in zip(tr, tr[1:]):
                    if u[0] >= t0 and v[0] <= t1:
                        tot += math.hypot(v[1] - u[1], v[2] - u[2])
                return tot / max((t1 - t0) / 1000.0, 1e-6)
            sa, sb = speed(ta), speed(tb)
            flag = 'FAIL' if maxd > tolerance else 'ok'
            print('  %-34s -> %-34s max gap %.2f m%s, travelled %.2f vs %.2f m/s  %s' % (
                k, ok_[i], maxd, (' from gt=%.0f' % first) if first is not None else '', sa, sb, flag))
            worst = max(worst, maxd)
    return worst


def compare(a):
    logs = []
    for r in [int(x) for x in a.rates.split(',')]:
        logs.append(run(a, r))
    worst = analyze(logs, a.tolerance)
    print('worst gap %.2f m (tolerance %.2f)' % (worst, a.tolerance))
    return 1 if worst > a.tolerance else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='what', required=True)
    for name in ('run', 'compare'):
        p = sub.add_parser(name)
        p.add_argument('--work', required=True)
        p.add_argument('--script')
        p.add_argument('--out', default=tempfile.mkdtemp(prefix='battle_test_'))
        p.add_argument('--char', default='squall')
        p.add_argument('--stage', type=int, default=12, help='Quick Battle stage, 1-20 (1 = Random); default 12')
        p.add_argument('--timeout', type=int, default=240)
        if name == 'run':
            p.add_argument('--fps', type=int, default=30)
            p.add_argument('--record', help='write the pad input of this session as a test script')
            p.add_argument('--watch', help='hex guest address: log which functions change it ([MEM_WATCH] lines)')
            p.add_argument('--watch-after', dest='watch_after', type=float, default=4.0)
        else:
            p.add_argument('--rates', default='30,120')
            p.add_argument('--tolerance', type=float, default=0.5)
    p = sub.add_parser('actions')
    p.add_argument('--work', required=True)
    p.add_argument('--out', default=tempfile.mkdtemp(prefix='battle_test_'))
    p.add_argument('--char', default='squall')
    p.add_argument('--stage', type=int, default=12)
    p.add_argument('--actions', default='')
    p.add_argument('--all', action='store_true')
    p.add_argument('--who', type=int, default=0)
    p.add_argument('--rates', default='30,120')
    p.add_argument('--gap', type=int, default=4500)
    p.add_argument('--hold', default='', help='button(s) held down after the action starts, e.g. SQUARE (moves that stop when it is released)')
    p.add_argument('--hold-ms', dest='hold_ms', type=int, default=3000)
    p.add_argument('--stick', default='128,128', help='stick while holding, x,y (255 = down / back)')
    p.add_argument('--timeout', type=int, default=240)
    p.add_argument('--speed-tol', dest='speed_tol', type=float, default=1.35)
    p.add_argument('--dist-tol', dest='dist_tol', type=float, default=1.5)
    p.add_argument('--record', default=None)
    p = sub.add_parser('analyze')
    p.add_argument('logs', nargs='+')
    p.add_argument('--tolerance', type=float, default=0.5)
    a = ap.parse_args(argv)
    if a.what == 'run':
        print(run(a))
        return 0
    if a.what == 'compare':
        return compare(a)
    if a.what == 'actions':
        os.makedirs(a.out, exist_ok=True)
        return actions_compare(a)
    return 1 if analyze(a.logs, a.tolerance) > a.tolerance else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

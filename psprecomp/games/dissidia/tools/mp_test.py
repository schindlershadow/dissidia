#!/usr/bin/env python3
"""Two-copy multiplayer test (issue #66): boot two copies headless in LAN mode on this machine, walk both into a
Communications > Group Battle fight (A creates the room, B joins, Party A / Party B, Tournament, both Lightning), press a
fixed list of moves on both through the debug socket, then report the lock-step statistics and the desync check.

  mp_test.py --work-a DIR --work-b DIR [--secs 60] [--env 'K=V ...'] [--env-a ...] [--env-b ...] [--out DIR]

Examples:
  mp_test.py --work-a ~/dissidia/work --work-b ~/dissidia/work2                                    # plain match
  mp_test.py ... --env 'PSPRECOMP_NET_LAG_MS=100'                                                  # 200 ms round trip
  mp_test.py ... --env 'PSPRECOMP_NET_LAG_MS=100 PSPRECOMP_NET_INPUT_DELAY=3'                      # with input delay
  mp_test.py ... --env-a 'PSPRECOMP_NET_PERTURB=600'                                               # must report frame 601
  mp_test.py ... --rstick                                                                          # views turned while moving

Each work folder needs a save in ms0/ (copied for the run, the folder's own is not touched) and a runtime built with
games/dissidia/runtime/netplay.cpp. The copies get a private settings file (30 fps, LAN), debug ports 9311 / 9312 and
PSPRECOMP_NET_TRACE=1. Rendering goes to the GPU without a window (SDL offscreen + the NVIDIA EGL vendor file when present).
Prints, per copy: the last lock-step statistics line (frame, checks that agreed, frames waited for the other copy) and
a check mismatch if there was one. Exit 1 when the walk fails or the desync check found a mismatch (unless --expect-desync)."""
import argparse
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BTN = {'up': 0x10, 'down': 0x40, 'cross': 0x4000, 'circle': 0x2000, 'right': 0x20, 'left': 0x80}
PORT = {'A': int(os.environ.get('MP_PORT_A', 9311)), 'B': int(os.environ.get('MP_PORT_B', 9312))}   # MP_PORT_A / MP_PORT_B: when another rig holds 9311 / 9312
# the moves pressed on both sides: (stick x y ms) or (buttons ms); B runs the list three entries later
MOVES = [('A', '255 128 900'), ('B', '8000 150'), ('A', '128 0 700'), ('B', '2000 150'), ('B', '0 128 600'), ('B', '4000 150'),
         ('B', '1000 150'), ('A', '128 255 800'), ('B', '8000 150'), ('B', '2000 150'), ('A', '255 255 500'), ('B', '2000 150')]
# --rstick: also turn each player's view with the right stick ('R': x y ms) while the left stick moves (the camera must not
# leak into the simulation: docs/multiplayer.md "Camera and stick")
RSTICK_MOVES = [('R', '255 128 500'), ('A', '128 0 900'), ('R', '0 100 300')]


class Copy:
    def __init__(self, side, work, out, env):
        self.side, self.work, self.out, self.env = side, work, out, env
        self.log = os.path.join(out, 'run_%s.log' % side)
        self.proc = None

    def start(self):
        # per-copy keys of self.env (divergent runs, tools/mp_diverge.py): MP_SAVE (source ms0 dir instead of the work folder's), MP_FPS, MP_RS (render_scale),
        # MP_NETMODE (lan | internet), MP_INI (extra settings.ini lines, ';'-separated), MP_TASKSET (cpu list for taskset: a slower copy), MP_PORT (debug port)
        ms0 = os.path.join(self.out, 'ms0_' + self.side)
        shutil.rmtree(ms0, ignore_errors=True)
        shutil.copytree(self.env.get('MP_SAVE') or os.path.join(self.work, 'ms0'), ms0, ignore=shutil.ignore_patterns('cloud*', 'SAVEDATA_BACKUP'))   # never a cloud file into a test dir
        ini = os.path.join(self.out, 'settings_%s.ini' % self.side)
        netmode = self.env.get('MP_NETMODE', 'lan')
        with open(ini, 'w') as f:
            f.write('display=window\nfps=%d\nrender_scale=%s\nwidescreen=0\nvoice=en\ncheck_updates=0\nhd_textures=0\nnet_mode=%s\n'
                    % (int(self.env.get('MP_FPS', os.environ.get('MP_FPS', '30'))), self.env.get('MP_RS', '1'), netmode))   # MP_FPS=60: both copies at 60 fps (30 Hz logic)
            if self.env.get('MP_FILL'):
                f.write('tagteam_lobby=1,2,1,5,0,0,0,0,0\n')
            for l in self.env.get('MP_INI', '').split(';'):
                if l.strip():
                    f.write(l.strip() + '\n')
        env = dict(os.environ, PSPRECOMP_SETTINGS=ini, PSPRECOMP_MS0=ms0, PSPRECOMP_ADHOC_MODE=netmode, PSPRECOMP_ADHOC_NICK=self.side,
                   PSPRECOMP_DEBUG_PORT=str(PORT[self.side]), PSPRECOMP_CHARMOD='0', PSPRECOMP_AUDIO='0', PSPRECOMP_SAVEDATA_AUTO='0',
                   PSPRECOMP_NET_TRACE='1', SDL_VIDEODRIVER='offscreen')
        egl = '/usr/share/glvnd/egl_vendor.d/10_nvidia.json'
        if os.path.exists(egl):
            env['__EGL_VENDOR_LIBRARY_FILENAMES'] = egl
        env.update(self.env)
        if os.path.exists(self.log):
            os.remove(self.log)
        cmd = [os.path.join(HERE, 'run.sh'), self.work, '--window', '480x272', '--log', self.log]
        if self.env.get('MP_TASKSET'):
            cmd = ['taskset', '-c', self.env['MP_TASKSET']] + cmd
        self.proc = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
                self.proc.wait(10)
            except (OSError, subprocess.TimeoutExpired):
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except OSError:
                    pass

    def text(self):
        try:
            with open(self.log, 'rb') as f:
                return f.read().decode(errors='replace')
        except OSError:
            return ''

    def cmd(self, line):
        with socket.create_connection(('127.0.0.1', PORT[self.side]), timeout=15) as s:
            s.sendall(line.encode() + b'\n')
            return s.makefile('rb').readline().decode(errors='replace').strip()

    def press(self, *names, gap=0.8):
        for n in names:
            self.cmd('B %X 120' % BTN[n])
            time.sleep(gap)


def load_scale():
    """How much slower than an idle host this one is running the games, as a factor for open-loop waits (menu sleeps, timeouts) and fight budgets:
    1.0 up to half the cores' worth of load, then 1 per extra half-core-load, at most 3. TT_LOAD_SCALE=x forces it."""
    v = os.environ.get('TT_LOAD_SCALE')
    if v:
        return max(0.5, float(v))
    try:
        load = os.getloadavg()[0]
    except OSError:
        return 1.0
    return max(1.0, min(3.0, load / ((os.cpu_count() or 4) * 0.5)))


SCALE = 1.0   # set by walk()


def nap(secs):
    time.sleep(secs * SCALE)


def wait(pred, timeout, what):
    end = time.time() + timeout * SCALE
    while time.time() < end:
        if pred():
            return
        time.sleep(0.5)
    raise RuntimeError('timeout: ' + what)


MENU_MODE_KEYS = {'team2': [], 'team3': ['down'], 'tagduel': ['down', 'down'], 'ffa': ['down', 'down', 'down']}   # Mode Select starts on Team 2 vs 2


def shot(c, out, name):   # a screenshot with the game layer (the debug socket's V command)
    if not out:
        return
    try:
        from PIL import Image
        tga = os.path.join(out, '%s_%s.tga' % (name, c.side))
        c.cmd('V ' + tga)
        Image.open(tga).convert('RGB').save(tga[:-4] + '.png')
        os.remove(tga)
    except Exception as e:   # a screenshot must not fail the walk
        print('[mp_test] shot %s failed: %s' % (name, e), file=sys.stderr)


def walk(a, b, slack=0.0, menu_mode=None, shots=None):   # slack: extra seconds at the lobby steps that go through the network (--slack, for runs with PSPRECOMP_NET_LAG_MS)
    global SCALE
    SCALE = load_scale()
    if SCALE > 1.0:
        print('[mp_test] host load %.1f: menu waits x%.1f' % (os.getloadavg()[0], SCALE), file=sys.stderr)
    both = (a, b)
    for c in both:
        wait(lambda: 'Debug socket listening' in c.text(), 90, 'debug socket ' + c.side)
    for c in both:   # title: CROSS until the save list opens, CROSS loads the save
        end = time.time() + 150 * SCALE
        while not ('list dialog opened' in c.text() or 'complete: mode=4' in c.text()):
            if time.time() > end:
                raise RuntimeError('the save list never opened (%s)' % c.side)
            c.press('cross', gap=1.0)
        time.sleep(0.6)
        c.press('cross')
    for c in both:
        wait(lambda: 'complete: mode=4 result=0x00000000' in c.text(), 60, 'save load ' + c.side)
    nap(15)
    for c in both:
        c.press('down', 'down', 'cross', gap=1.2)          # Communications Mode
    nap(2)
    if menu_mode:   # the MENU route: Communications > Tag Team Battle (the last row: Up from the first), Mode Select, then the game's Group Battle
        for c in both:
            c.press('up', 'cross', gap=1.2)
        nap(2)
        shot(a, shots, 'modeselect')
        for c in both:   # (the joiner's choice does not matter: the host's lobby decides)
            c.press(*MENU_MODE_KEYS[menu_mode], gap=0.5)
        shot(a, shots, 'modeselect_choice')
        for c in both:
            c.press('cross', gap=1.2)
    else:
        for c in both:
            c.press('down', 'cross', gap=1.2)              # Group Battle
    nap(4)
    a.press('cross'); nap(4)                        # A: Create Room (Custom)
    b.press('down', 'down', 'cross'); nap(6)        # B: Search for a Room
    b.press('cross'); nap(5)                        # B: request entry
    a.press('up', 'cross'); nap(2)                  # A: Start Group Battle -> YES
    a.press('cross'); nap(8 + slack)
    a.press('cross'); time.sleep(slack); b.press('right', 'cross')   # Party A / Party B
    nap(12 + slack)
    shot(a, shots, 'partyselect'); shot(b, shots, 'partyselect')
    if '[TTFMT] battle format dialog answered' not in a.text():   # (the tag modes answer the dialog themselves: PSPRECOMP_TAGTEAM_FMTDLG_OLD=1 shows it)
        a.press('cross')                                   # A: Tournament
    nap(5)
    shot(a, shots, 'setup'); shot(b, shots, 'setup')
    for c in both:
        c.press('cross', gap=2.0)                          # the 1st slot -> Player Select
    for c in both:
        if c.side == 'B' and int(os.environ.get('MP_B_RIGHT', '0')):
            c.press(*(['right'] * int(os.environ['MP_B_RIGHT'])), gap=0.5)   # MP_B_RIGHT=n: B picks the character n to the right of Lightning (default: both Lightning)
        c.press('cross', gap=1.5)                          # Lightning
    for c in both:
        c.press('cross', gap=3.0)                          # YES
    for c in both:
        c.press('down', 'down', 'down', 'down', 'down', gap=0.5)
    shot(a, shots, 'before_enter'); shot(b, shots, 'before_enter')
    for c in both:
        c.press('cross', gap=0.2)                          # Enter Battle
    end = time.time() + 90 * SCALE
    while time.time() < end:                               # "Next" on the opponent pages until the lock-step runs
        busy = [c for c in both if not any(m in c.text() for m in IN_BATTLE)]
        if not busy:
            return
        nap(3)
        for c in busy:
            c.press('cross', gap=0.1)
    raise RuntimeError('the battle never started')


def exercise(a, b, secs, moves=MOVES):
    end, i = time.time() + secs, 0
    while time.time() < end:
        for c, off in ((a, 0), (b, 3)):
            kind, arg = moves[(i + off) % len(moves)]
            c.cmd(('A ' if kind == 'A' else 'A2 ' if kind == 'R' else 'B ') + arg)
        i += 1
        time.sleep(0.7)


# log lines that say the lock-step battle runs: plain battles link the two copies' pads, a tag battle prints its network delay
IN_BATTLE = ('[NETLINK] linked with', '[NETPLAY] tag lock-step battle: network delay')


def desynced(c):
    t = c.text()
    return 'the battles differ' in t or 'DESYNC' in t


def stats(c):
    last = None
    for l in c.text().splitlines():
        m = re.match(r'\[NETLINK\] (?:watching )?frame (\d+): (\d+) checks agreed, (\d+) frames waited', l)
        if m:
            last = tuple(int(x) for x in m.groups())
    if not last:
        return '%s: no lock-step statistics yet (one line per 900 frames)' % c.side
    bad = [l for l in c.text().splitlines() if 'the battles differ' in l or 'DESYNC' in l]
    return '%s: frame %d, %d checks agreed, %d frames waited%s' % (c.side, last[0], last[1], last[2], ('\n   ' + bad[0]) if bad else '')


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work-a', required=True)
    ap.add_argument('--work-b', required=True)
    ap.add_argument('--secs', type=float, default=60)
    ap.add_argument('--env', default='')
    ap.add_argument('--env-a', default='')
    ap.add_argument('--env-b', default='')
    ap.add_argument('--out', default=os.path.join(os.getcwd(), 'mp_test_out'))
    ap.add_argument('--rstick', action='store_true', help='also turn both views with the right stick while moving')
    ap.add_argument('--slack', type=float, default=0.0, help='extra seconds at the lobby steps (lag runs: the party screen comes later)')
    ap.add_argument('--menu-mode', choices=sorted(MENU_MODE_KEYS), help='go through Communications > Tag Team Battle > Mode Select (the menu route, no PSPRECOMP_TAGTEAM) and pick this mode')
    ap.add_argument('--fill', action='store_true', help='the host starts with CPU fill on (saved lobby in its settings.ini)')
    ap.add_argument('--shots', action='store_true', help='screenshots of the flow steps and the battle into --out')
    ap.add_argument('--keep', action='store_true', help='leave both copies running afterwards')
    ap.add_argument('--expect-desync', action='store_true', help='exit 0 only when a desync was reported (the perturbation test)')
    a = ap.parse_args(argv)
    a.out = os.path.abspath(os.path.expanduser(a.out))
    os.makedirs(a.out, exist_ok=True)
    kv = lambda s: dict(x.split('=', 1) for x in s.split() if '=' in x)
    A = Copy('A', os.path.abspath(os.path.expanduser(a.work_a)), a.out, {**kv(a.env), **kv(a.env_a), **({'MP_FILL': '1'} if a.fill else {})})
    B = Copy('B', os.path.abspath(os.path.expanduser(a.work_b)), a.out, {**kv(a.env), **kv(a.env_b)})
    try:
        A.start()
        B.start()
        t0 = time.time()
        walk(A, B, a.slack, a.menu_mode, a.out if a.shots else None)
        print('[mp_test] in battle after %.0f s' % (time.time() - t0), file=sys.stderr)
        shot(A, a.out if a.shots else None, 'battle'); shot(B, a.out if a.shots else None, 'battle')
        exercise(A, B, a.secs, RSTICK_MOVES + MOVES if a.rstick else MOVES)
        time.sleep(11)   # one more desync summary
        for c in (A, B):
            print(stats(c))
        desync = any(desynced(c) for c in (A, B))
        return 0 if desync == a.expect_desync else 1
    finally:
        if not a.keep:
            A.stop()
            B.stop()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

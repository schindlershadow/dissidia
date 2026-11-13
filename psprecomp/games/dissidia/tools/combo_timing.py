#!/usr/bin/env python3
"""Combo input-window timing across frame rates (Jecht by default).

One battle per frame rate is booted (own Xvfb display, own debug port, own copy of the memory stick, so it can run next to other
sessions' tests and next to the other rates); the CPU fighter has no AI input (PSPRECOMP_TEST_AI_OFF: its pad never gets a
button) but runs normally, so it takes hits and reacts like in a real fight (a frozen dummy stalls the attacker's hit-stop). Trials are then fed into the
running battle as script batches (`batch <file>` in the test driver, runtime/framerate.cpp), so the menus are walked once per rate
and everything after that runs fast-forwarded (debug socket FF).

A trial: wait for the player to be idle, `place` him in melee range of the dummy, press the prefix inputs of the combo (each at a
fixed delay after the previous move STARTED, i.e. after its script id appeared: `sync <script>`), then press the probe input D ms
after the last move started and log which script ids follow. A window is the set of D that start a given script. The same D grid
(default every 1/120 s) is used at every rate, so the windows can be compared directly: they should have the same start and end
at 30, 60 and 120 fps (at 30 fps the edges can only fall on update boundaries, 33.3 ms apart).

  combo_timing.py sweep   --work W --rates 30,120 --prefix 'CIRCLE@800=10001' --probe CIRCLE --from 150 --to 700   # one transition
  combo_timing.py combos  --work W --rates 30,60,120                                                              # every wiki combo

Step syntax: BUTTON[:STICK][@delay][=script]   STICK n (neutral) | u (up) | d (down) | l | r   delay in ms after the previous move
started (default 400); `=script` is the script id the press must start (the next step is timed from that start).
"""
import argparse
import json
import math
import os
import re
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import battle_test as bt            # noqa: E402

STICK = {'n': (128, 128), 'u': (128, 0), 'd': (128, 255), 'l': (0, 128), 'r': (255, 128)}
TAP_MS = 67                         # a press: two updates at 30 fps (a 1-update tap is sometimes missed by the game)
GRID = 1000.0 / 120.0               # default probe grid: one update at 120 fps


def parse_step(s):
    m = re.match(r'^([A-Z+]+)(?::([nudlr]))?(?:@([0-9.]+))?(?:=(\d+))?$', s.strip())
    if not m:
        raise ValueError('bad step %r' % s)
    return dict(btn=m[1], stick=m[2] or 'n', delay=float(m[3]) if m[3] else 400.0, scr=int(m[4]) if m[4] else None)


def press_lines(delay, btn, stick, hold=TAP_MS):
    ax, ay = STICK[stick]
    return ['+%.1f set %s %d %d' % (delay, btn, ax, ay), '+%.1f set - 128 128' % (delay + hold)]


def trial_script(i, prefix, probe, d, dist=3.0, tail=900):
    """script lines of trial i. prefix: steps (parse_step) pressed in order; probe: a step whose delay is replaced by d"""
    L = ['+0 waitidle 8000', '+400 place %g' % dist, '+700 mark trial %d' % i]
    first = True
    for st in prefix:
        L += press_lines(st['delay'] if not first else max(st['delay'], 800), st['btn'], st['stick'])
        L.append('+0 sync 1500' + (' %d' % st['scr'] if st['scr'] is not None else ''))
        first = False
    if first:                         # no prefix: the probe is pressed d ms after the idle moment + 800
        d += 800
    L.append('+%.1f mark probe %d' % (d, i))
    L += press_lines(d, probe['btn'], probe['stick'])
    L.append('+%.1f mark end %d' % (d + tail, i))
    return L


class Session(threading.Thread):
    """a running battle at one frame rate"""

    def __init__(self, work, fps, out, char='jecht', ff_menus=0):
        super().__init__(daemon=True)
        self.work, self.fps, self.out, self.char, self.ff_menus = work, fps, out, char, ff_menus
        self.ready = threading.Event()
        self.error = None
        self.n = 0
        self.lines = []
        self.g = None
        self.stop_flag = False

    def batch_path(self, n):
        return os.path.join(self.out, 'batch_%d_%d.txt' % (self.fps, n))

    def run(self):
        os.makedirs(self.out, exist_ok=True)
        xp, disp = bt.start_xvfb()
        try:
            init = os.path.join(self.out, 'init_%d.txt' % self.fps)
            with open(init, 'w') as f:
                f.write('5000 waitidle 8000\n+0 batch %s\n' % self.batch_path(0))
            self.g = bt.Game(self.work, self.fps, self.out, disp, bt.free_port(), script=init,
                             extra_env={'PSPRECOMP_TEST_PLAYER_CHAR': str(bt.CHAR_ID[self.char]), 'PSPRECOMP_TEST_FREEZE_CPU': '0', 'PSPRECOMP_TEST_AI_OFF': '1',
                                        'PSPRECOMP_FIGHTER_LOG': '0'})   # the fighter log is a line per fighter per update: tens of GB at 120 fps
            self.g.start()
            bt.walk_menus(self.g, self.char, ff_menus=self.ff_menus)
            self.ready.set()
            last = 0
            while not self.stop_flag and self.g.alive():
                if time.time() - last > 50:           # fast-forward (as fast as frames render), renewed before its 120 s cap
                    try:
                        self.g.cmd('FF 120000')
                    except OSError:
                        pass
                    last = time.time()
                if os.path.getsize(self.g.log) > 6e9:     # a runaway log (the game stuck in unrecompiled code logs a line per call): stop, do not fill the disk
                    raise RuntimeError('rate %d: the log is over 6 GB, game stopped' % self.fps)
                new = self.g.lines()
                if any(l.startswith('X connection') for l in new):   # the display was killed (low memory, someone's cleanup): the game then spins in unrecompiled code logging GBs
                    raise RuntimeError('rate %d: the X display died' % self.fps)
                self.lines += [l for l in new if l.startswith('[TEST]')]   # only the driver's lines: the per-update fighter log is gigabytes
                time.sleep(0.1)
        except Exception as e:                        # noqa: BLE001
            self.error = e
            self.ready.set()
        finally:
            if self.g:
                self.g.stop()
            xp.terminate()

    def submit(self, lines, timeout=1800, retries=6):
        """play a batch of script lines; returns the log lines it produced. If the fight ends meanwhile (the game has a time limit,
        and starts a new fight, which replays the initial script: it waits for batch 0 again) the batch is played again in the new fight."""
        n = self.n
        self.n += 1
        text = '\n'.join(lines + ['+0 mark batchdone %d' % n, '+0 batch %s' % self.batch_path(n + 1)]) + '\n'

        def put(path):
            tmp = path + '.tmp'
            with open(tmp, 'w') as f:
                f.write(text)
            os.rename(tmp, path)
        start = len(self.lines)
        put(self.batch_path(n))
        end, ended_at, pos = time.time() + timeout, None, start
        while time.time() < end:
            if self.error or (self.g and not self.g.alive()):
                raise RuntimeError('rate %d: the game ended (%s)' % (self.fps, self.error))
            while pos < len(self.lines):
                l = self.lines[pos]
                pos += 1
                if 'mark batchdone %d' % n in l:
                    return self.lines[start:pos]
                if 'battle ended' in l:
                    ended_at = pos
                elif 'battle started' in l and ended_at is not None:
                    if retries <= 0:
                        raise RuntimeError('rate %d: the fight keeps ending' % self.fps)
                    retries -= 1
                    print('rate %d: the fight ended, batch %d played again in the new one' % (self.fps, n), file=sys.stderr)
                    if os.path.exists(self.batch_path(n)):   # (still there if the fight ended before it was reached)
                        os.remove(self.batch_path(n))
                    start, ended_at = pos, None              # results of the new fight only
                    put(self.batch_path(0))                  # the replayed initial script waits for this one
            time.sleep(0.2)
        raise RuntimeError('rate %d: batch %d timed out' % (self.fps, n))

    def finish(self):
        try:
            self.submit(['+0 quit'], timeout=30)
        except Exception:                             # noqa: BLE001
            pass
        self.stop_flag = True


def parse_trials(lines):
    """{trial: {'outcome': [script ids after the probe], 'timeouts': n, 'probe_gt': ms}} from a batch's log lines"""
    res, cur = {}, None
    for l in lines:
        m = re.match(r'\[TEST\] gt=(\d+) mark trial (\d+)', l)
        if m:
            cur = int(m[2]); res[cur] = dict(outcome=[], timeouts=0, probe_gt=None, scr=[]); continue
        if cur is None:
            continue
        m = re.match(r'\[TEST\] gt=(\d+) mark probe (\d+)', l)
        if m:
            res[cur]['probe_gt'] = int(m[1]); continue
        if re.match(r'\[TEST\] gt=\d+ (sync|waitidle) TIMEOUT', l):
            res[cur]['timeouts'] += 1
            continue
        m = re.match(r'\[TEST\] gt=(\d+) scr (-?\d+) -> (-?\d+)', l)
        if m:
            t = res[cur]
            t['scr'].append((int(m[1]), int(m[3])))
            if t['probe_gt'] is not None and int(m[1]) >= t['probe_gt'] and int(m[3]) != -1:
                t['outcome'].append(int(m[3]))
        if re.match(r'\[TEST\] gt=\d+ mark end', l):
            cur = None
    return res


def sweep(sess, prefix, probe, ds, dist=3.0, chunk=24):
    """probe at each D in ds; returns [(d, outcome list, ok)] (ok False if a prefix step did not start its move)"""
    out = []
    for c in range(0, len(ds), chunk):
        part = ds[c:c + chunk]
        L = []
        for i, d in enumerate(part):
            L += trial_script(i, prefix, probe, d, dist)
        res = parse_trials(sess.submit(L))
        for i, d in enumerate(part):
            t = res.get(i)
            out.append((d, t['outcome'] if t else None, bool(t) and t['timeouts'] == 0))
    return out


def windows(rows):
    """{first outcome script id: (min D, max D, count, contiguous)} from sweep rows"""
    by = {}
    for d, oc, ok in rows:
        if not ok or oc is None:
            continue
        key = oc[0] if oc else None
        by.setdefault(key, []).append(d)
    res = {}
    allds = [d for d, _, _ in rows]
    for k, ds in by.items():
        lo, hi = min(ds), max(ds)
        inside = [d for d in allds if lo <= d <= hi]
        res[k] = (lo, hi, len(ds), len(inside) == len(ds))
    return res


def run_rates(work, rates, out, fn, char='jecht', ff_menus=0, attempts=3):
    """boot one session per rate (in parallel), run fn(session) in a thread each, return {rate: result}. A session whose display was killed
    (this host: low memory, other sessions' cleanups) is started again from the menus, up to `attempts` times."""
    results, errs = {}, {}

    def go(r):
        for attempt in range(attempts):
            s = Session(work, r, os.path.join(out, 'r%d' % r if attempt == 0 else 'r%d_try%d' % (r, attempt + 1)), char, ff_menus)
            s.start()
            s.ready.wait()
            try:
                if s.error:
                    raise s.error
                results[r] = fn(s)
                errs.pop(r, None)
                return
            except Exception as e:                    # noqa: BLE001
                errs[r] = e
                print('rate %d: attempt %d failed: %r' % (r, attempt + 1, e), file=sys.stderr)
                if 'display died' not in str(e):
                    return
            finally:
                s.finish()
                s.join(30)
    th = [threading.Thread(target=go, args=(r,)) for r in rates]
    for t in th:
        t.start()
    for t in th:
        t.join()
    for r, e in errs.items():
        print('rate %d failed: %r' % (r, e), file=sys.stderr)
    return results


def fmt_windows(w):
    return ', '.join('%s: %.0f..%.0f ms (%d%s)' % (k, v[0], v[1], v[2], '' if v[3] else ', GAPS') for k, v in sorted(w.items(), key=lambda kv: str(kv[0])))


def grid(lo, hi, step=GRID):
    return [round(lo + k * step, 2) for k in range(int((hi - lo) / step) + 1)]


def pick_success(rows, exclude=()):
    """the script id the narrowest contiguous run of probe delays leads to (the chain move; the wider ones are 'too early / too late' repeats)"""
    best = None
    for k, (lo, hi, n, contiguous) in windows(rows).items():
        if k is None or k in exclude or n < 3:
            continue
        if best is None or n < best[1]:
            best = (k, n, (lo + hi) / 2.0)
    return best


def combos_run(sess):
    """every ground transition of the wiki's solo Jecht combos that starts from Rush (the air ones start from Jecht Stream, which needs a
    jump first: not covered). Each later step is pressed at the middle of the window this rate measured for the one before."""
    out = {}
    rush = parse_step('CIRCLE@800=10001')
    P = parse_step
    out['Rush > Neutral 2 (Circle)'] = sweep(sess, [rush], P('CIRCLE:n'), grid(150, 500))
    out['Rush > Up 2 (Circle+Up)'] = sweep(sess, [rush], P('CIRCLE:u'), grid(150, 500))
    pk = pick_success(out['Rush > Neutral 2 (Circle)'])
    if not pk:
        return out
    n2 = P('CIRCLE:n@%.1f=%d' % (pk[2], pk[0]))
    out['Neutral 2 > Neutral 3 (Circle)'] = sweep(sess, [rush, n2], P('CIRCLE:n'), grid(300, 900))
    out['Neutral 2 > Up 3 (Circle+Up)'] = sweep(sess, [rush, n2], P('CIRCLE:u'), grid(300, 900))
    pk3 = pick_success(out['Neutral 2 > Neutral 3 (Circle)'], exclude=(pk[0],))
    if pk3:
        n3 = P('CIRCLE:n@%.1f=%d' % (pk3[2], pk3[0]))
        out['Neutral 3 > HP (Square)'] = sweep(sess, [rush, n2, n3], P('SQUARE'), grid(300, 900))
    return out


def report(res, rates):
    names = []
    for r in rates:
        for k in res.get(r, {}):
            if k not in names:
                names.append(k)
    lines = []
    for name in names:
        lines.append('%s' % name)
        for r in rates:
            if name in res.get(r, {}):
                w = windows(res[r][name])
                lines.append('  %3d fps  %s' % (r, fmt_windows(w)))
    return '\n'.join(lines)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='what', required=True)
    p = sub.add_parser('sweep')
    p.add_argument('--work', required=True)
    p.add_argument('--out', default='/tmp/combo_timing')
    p.add_argument('--rates', default='30,120')
    p.add_argument('--char', default='jecht')
    p.add_argument('--prefix', default='CIRCLE@800=10001', help='comma separated steps')
    p.add_argument('--probe', default='CIRCLE')
    p.add_argument('--from', dest='lo', type=float, default=150)
    p.add_argument('--to', dest='hi', type=float, default=700)
    p.add_argument('--step', type=float, default=GRID)
    p.add_argument('--dist', type=float, default=3.0)
    p.add_argument('--ff-menus', type=int, default=0)
    p = sub.add_parser('combos')
    p.add_argument('--work', required=True)
    p.add_argument('--out', default='/tmp/combo_timing')
    p.add_argument('--rates', default='30,120')
    p.add_argument('--char', default='jecht')
    p.add_argument('--ff-menus', type=int, default=0)
    a = ap.parse_args(argv)
    if a.what == 'combos':
        work, out = os.path.abspath(os.path.expanduser(a.work)), os.path.abspath(a.out)
        rates = [int(x) for x in a.rates.split(',')]
        t0 = time.time()
        res = run_rates(work, rates, out, combos_run, a.char, a.ff_menus)
        print(report(res, rates))
        with open(os.path.join(out, 'combos.json'), 'w') as f:
            json.dump({str(r): res[r] for r in res}, f)
        print('took %.0f s' % (time.time() - t0))
        return 0
    work, out = os.path.abspath(os.path.expanduser(a.work)), os.path.abspath(a.out)
    rates = [int(x) for x in a.rates.split(',')]
    prefix = [parse_step(x) for x in a.prefix.split(',')] if a.prefix else []
    probe = parse_step(a.probe)
    ds = [round(a.lo + k * a.step, 2) for k in range(int((a.hi - a.lo) / a.step) + 1)]
    t0 = time.time()
    res = run_rates(work, rates, out, lambda s: sweep(s, prefix, probe, ds, a.dist), a.char, a.ff_menus)
    for r in rates:
        if r in res:
            print('%3d fps  %s' % (r, fmt_windows(windows(res[r]))))
    with open(os.path.join(out, 'sweep.json'), 'w') as f:
        json.dump({str(r): res[r] for r in res}, f)
    print('took %.0f s' % (time.time() - t0))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

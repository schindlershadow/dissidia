#!/usr/bin/env python3
"""Tag team (2v2) regression test: boots the game headless straight into a Quick Battle (battle_test.py --direct), with the tag team
on, lets the four fighters fight for a while and checks, from the runtime log:

  * four fighters were seen and each one changed its script (the extras think and act: an idle extra changes it only once or twice),
  * the game camera stayed sane (the camera log's eye = (0,0,0) or -480 garbage frames stay under --max-bad-cam),
  * the extra fighters were spawned and started.
With --baseline it also runs the same battle with the tag team off and checks the 1v1 is unchanged (two fighters, no garbage frames), and the
two-copy ONLINE checks (tools/mp_diverge.py): identical copies (control) and divergent copies (A: the work folder's save, 30 fps, one CPU core;
B: the Deck's save, 120 fps, render scale 2) in Team 2v2, 3v3 and Free-for-all, LAN and Internet (local relay), through the real menus, comparing
NETSYNC, the VS line-up, the banters, the roster and the result of the two copies.

  tagteam_test.py --work DIR [--out DIR] [--seconds 24] [--baseline] [--jobs 4] [--no-online] [--only NAME[,NAME]] [--max-bad-cam 5] [--min-changes 8]
                  [--remote HOST [--remote-jobs 3]]

The checks are independent and run in parallel (--jobs N, default 4; each in its own subdir of --out with its own log, display, debug port and
UDP / relay ports). The summary lists every check as PASS / FAIL with its first failure line; a failing online check has report.txt in its subdir
(docs/tag-team.md, "Regression: reading a failure"). --only runs the named checks (sp_tag, sp_plain, mp_same_lan, mp_div_<mode>_<lan|internet>, mp_div_stale_team2_lan).

The work folder is a built one (see docs/testing.md); the runtime needs tagteam.cpp, framerate.cpp's test driver and direct_battle.cpp.
Load awareness: --jobs is a ceiling; a new local check starts only while the other sessions' load (load average minus this run's own) leaves room on the
cores (--no-throttle turns it off), and the walk waits / fight budget / single-player seconds scale with the host load (mp_test.load_scale: x1 up to half the cores
busy, x3 at most; TT_LOAD_SCALE=x forces it); the online fight also goes on past its budget while the battle still progresses, and a mismatch ends it at once.
--remote HOST (the desktop, 192.168.2.131): a share of the checks runs there over ssh (tools/tt_remote.py), only when it is idle (no psprecomp_runtime / play_*.sh /
run.sh running, low load, ssh works; else everything runs locally). The checks are handed out dynamically (a free slot, local or remote, takes the next one); each
check's dir is copied back so the summary is unchanged; "placement:" lists where each ran. A remote failure that is not a real desync / line-up / banter / roster / result
difference is rerun locally (a desktop timing or ssh problem must not fail the merge).
Needs Xvfb locally. Exit status 0 = pass."""
import argparse
import os
import re
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import battle_test as bt  # noqa: E402
import mp_test  # noqa: E402
import tt_remote  # noqa: E402


def run_battle(work, out, seconds, tag, fps=30, partner=0, enemy2=0):
    os.makedirs(out, exist_ok=True)
    script = os.path.join(out, 'script.txt')
    with open(script, 'w') as f:
        f.write('%d quit\n' % ((seconds + 6) * 1000))
    os.environ['BT_NO_FREEZE'] = '1'   # read by battle_test.py itself: the CPU fighters fight (the default is a standing dummy)
    env = {'PSPRECOMP_CAMX_LOG': '800', 'PSPRECOMP_FIGHTER_LOG': '1', 'PSPRECOMP_TAGTEAM_LOG': '1'}
    if tag:
        env['PSPRECOMP_TAGTEAM'] = '1'
        env['PSPRECOMP_TAGTEAM_CLONES'] = '1'   # the quick battle has no party: the extras are copies (off in play: a team without a second member has no partner)
        if partner: env['PSPRECOMP_TAGTEAM_PARTNER_CHAR'] = str(partner)   # extras of other characters (load_character)
        if enemy2: env['PSPRECOMP_TAGTEAM_ENEMY2_CHAR'] = str(enemy2)
    a = argparse.Namespace(work=work, script=script, fps=fps, out=out, char='Squall', timeout=400, ff=True, ff_menus=4,
                           direct=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tests', 'replays', 'quick_battle_walk.txt'),
                           env=env, stage=12, record=None, watch=None, shots=str(seconds))
    bt.run(a)
    return os.path.join(out, 'run_%d.log' % fps)


def analyse(log):
    per, cam, bad, spawn, started = {}, 0, 0, 0, 0
    for l in open(log, errors='replace'):
        m = re.match(r'\[FIGHTER\] gt=(\d+) .*? (0[0-9A-F]{7}) .*?cur=(-?\d+)', l)
        if m:
            per.setdefault(m[2], []).append(m[3])
        if '[CAMX]' in l:
            cam += 1
            if 'eye=(0.00,0.00,0.00)' in l or '-480.00' in l:
                bad += 1
        if '[TAGTEAM] spawn' in l:
            spawn += 1
        if re.search(r'\[TAGTEAM\] extra \S+ \(side \d\) started', l):
            started += 1
    changes = {a: len([x for i, x in enumerate(v) if i == 0 or x != v[i - 1]]) for a, v in per.items()}
    return changes, cam, bad, spawn, started



ONLINE = [  # name, mp_diverge.py arguments (the slow copy alternates between host and joiner)
    ('mp_same_lan', ['--same', '--mode', 'team2', '--net', 'lan']),
    ('mp_div_team2_lan', ['--mode', 'team2', '--net', 'lan', '--slow', 'a']),
    ('mp_div_team2_internet', ['--mode', 'team2', '--net', 'internet', '--slow', 'b']),
    ('mp_div_stale_team2_lan', ['--mode', 'team2', '--net', 'lan', '--slow', 'b', '--stale']),   # B has played before (stale team records): the 26 Nov VS line-up / banter bug
    ('mp_div_team3_lan', ['--mode', 'team3', '--net', 'lan', '--slow', 'b']),
    ('mp_div_team3_internet', ['--mode', 'team3', '--net', 'internet', '--slow', 'a']),
    ('mp_div_ffa_lan', ['--mode', 'ffa', '--net', 'lan', '--slow', 'a']),
    ('mp_pack_bartz_internet', ['--same', '--mode', 'team2', '--net', 'internet', '--pack']),   # A has the Bartz D1 moves installed, B gets the pack online (27 Nov: ability costs + chrl row differed -> desync); fails (rc 2) without ~/dissidia/tt/s2/packmods_bartz (a built mods folder with bartz-dff)
    ('mp_div_ffa_internet', ['--mode', 'ffa', '--net', 'internet', '--slow', 'b']),
]


def check_sp(a, out, tag):
    """One single-player battle: (ok, lines)."""
    lines, ok = [], True

    def check(cond, msg):
        nonlocal ok
        lines.append(('PASS  ' if cond else 'FAIL  ') + msg)
        ok = ok and cond
    changes, cam, bad, spawn, started = analyse(run_battle(os.path.abspath(a.work), out, a.seconds, tag, partner=a.partner_char if tag else 0, enemy2=a.enemy2_char if tag else 0))
    if tag:
        lines.append('tag team on: fighters %d, script changes %s, camera lines %d (bad %d), spawned %d, started %d' % (len(changes), sorted(changes.values()), cam, bad, spawn, started))
        check(len(changes) == 4, 'four fighters were seen')
        check(spawn == 2 and started == 2, 'both extras were spawned and started')
        # the human has no scripted input: the three AI-driven fighters (the CPU and both extras) must act
        check(len(changes) == 4 and sorted(changes.values())[1] >= a.min_changes, 'the three AI-driven fighters acted (>= %d script changes each)' % a.min_changes)
        check(cam > 100 and bad <= a.max_bad_cam, 'the camera stayed sane (%d bad of %d logged updates, limit %d)' % (bad, cam, a.max_bad_cam))
    else:
        lines.append('tag team off: fighters %d, camera lines %d (bad %d)' % (len(changes), cam, bad))
        check(len(changes) == 2 and spawn == 0, 'without the tag team the battle is the plain 1v1')
        check(bad == 0, 'no camera garbage in the plain battle')
    return ok, lines


def retryable(lines):
    """The failure is only a walk timeout (a menu step that missed its timing on a loaded host). Every other difference (a NETSYNC mismatch of any size, line-up, banter,
    roster, result) fails at once: the one-frame call-in transient this used to forgive was the battle age counting from each machine's own start (fixed in batch eighty-two:
    online the age follows the lock-step frame)."""
    if any(l.startswith(('TTVS', 'BANTER', 'ROSTER', 'RESULT', 'CLOUD', 'NETSYNC', 'DESYNC')) for l in lines):
        return False
    return any(l.startswith('walk failed') for l in lines)


def bt_save_b():
    return os.path.expanduser(os.environ.get('PSPRECOMP_TEST_SAVE_B', '~/dissidia/tt/s2/saves/deck/ms0'))


def run_check(a, name, out):
    """Run one named check in this process; (ok, lines)."""
    if name in ('sp_tag', 'sp_plain'):
        sc = mp_test.load_scale()   # a loaded host runs fewer frames in the same wall seconds
        if sc > 1.0:
            a.seconds = int(a.seconds * sc)
        ok, lines = check_sp(a, out, name == 'sp_tag')
        if not ok and os.environ.get('TT_CHILD'):   # a loaded host runs fewer frames in the same wall seconds ("acted" counts, camera lines): one retry with 1.5x the time
            a.seconds = int(a.seconds * 1.5)
            ok2, lines2 = check_sp(a, out + '_retry', name == 'sp_tag')
            lines += ['-- retry with %d s --' % a.seconds] + lines2
            return ok2, lines
        return ok, lines
    for n, args in ONLINE:
        if n == name:
            cmd = [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mp_diverge.py'), '--work', os.path.abspath(a.work), '--out', out] + args
            if a.save_b:
                cmd += ['--save-b', a.save_b]
            r = subprocess.run(cmd, capture_output=True, text=True)
            lines = (r.stdout + r.stderr[-1500:]).splitlines()
            if r.returncode == 2:   # could not run (no second save): a failure of the check, never a silent pass
                return False, lines
            if r.returncode and retryable(lines):   # one retry for a menu step that missed its timing on a loaded host (never for a desync)
                lines = ['-- first run failed only with: %s; retrying once --' % ('; '.join(l.strip()[:90] for l in lines if l.startswith(('NETSYNC', 'walk failed'))))] + lines
                r = subprocess.run(cmd, capture_output=True, text=True)
                lines += ['-- retry --'] + (r.stdout + r.stderr[-1500:]).splitlines()
            return r.returncode == 0, lines
    raise SystemExit('unknown check ' + name)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', required=True)
    ap.add_argument('--out', default=None)
    ap.add_argument('--seconds', type=int, default=24)
    ap.add_argument('--baseline', action='store_true', help='also run the plain 1v1 check and the online two-copy checks')
    ap.add_argument('--jobs', type=int, default=4, help='checks run in parallel (default 4: other sessions\' rigs run on this host too)')
    ap.add_argument('--remote', default=None, help='run a share of the checks on this host over ssh (the desktop, 192.168.2.131) when it is idle; see the docstring')
    ap.add_argument('--remote-jobs', type=int, default=3, help='checks in parallel on the remote (default 3)')
    ap.add_argument('--no-throttle', action='store_true', help='start local checks up to --jobs whatever the host load')
    ap.add_argument('--no-xdev', action='store_true', help='with --baseline: skip the cross-device online test (tools/xdev_test.py: the real machines, run when reachable and idle)')
    ap.add_argument('--xdev-targets', default='desktop,deck,laptop,rp6', help='xdev_test.py --targets')
    ap.add_argument('--no-online', action='store_true', help='with --baseline: skip the two-copy online checks (single-player checks only)')
    ap.add_argument('--only', default=None, help='comma-separated check names to run (instead of the default set)')
    ap.add_argument('--save-b', default=None, help='the second save (a Deck ms0) for the divergent online checks (default: $PSPRECOMP_TEST_SAVE_B or ~/dissidia/tt/s2/saves/deck/ms0)')
    ap.add_argument('--partner-char', type=int, default=0, help='charId of the human\'s partner (default: a copy of your fighter)')
    ap.add_argument('--enemy2-char', type=int, default=0, help='charId of the enemy\'s second fighter')
    ap.add_argument('--max-bad-cam', type=int, default=5)
    ap.add_argument('--min-changes', type=int, default=8)
    a = ap.parse_args()
    out = os.path.abspath(a.out or tempfile.mkdtemp(prefix='tagteam_test_'))
    os.makedirs(out, exist_ok=True)
    names = a.only.split(',') if a.only else ['sp_tag'] + (['sp_plain'] + ([] if a.no_online else [n for n, _ in ONLINE]) if a.baseline else [])
    results = {}
    t0 = time.time()
    if os.environ.get('TT_CHILD') and len(names) == 1:   # a child of the parallel run: one check, result in the exit status
        ok, lines = run_check(a, names[0], os.path.join(out, names[0]))
        print('\n'.join(lines))
        print('CHECK %s: %s' % (names[0], 'PASS' if ok else 'FAIL'))
        sys.exit(0 if ok else 1)
    if names == ['sp_tag'] and not a.only:   # the plain single-player run (no --baseline): in this process, logs in OUT/tag
        ok, lines = run_check(a, 'sp_tag', os.path.join(out, 'tag'))
        print('\n'.join(lines))
        print('RESULT: ' + ('PASS' if ok else 'FAIL') + '  (logs in %s)' % out)
        sys.exit(0 if ok else 1)
    # parallel: every check is a child process of this script (--only NAME) with its own out subdir and log; the longest checks first
    order = sorted(names, key=lambda n: 0 if n.startswith('mp_') else 1)
    pending, running = list(order), {}
    where = {}
    rem, rem_ok, retried, used = None, False, set(), set()
    if a.remote:
        rem = tt_remote.Remote(a.remote)
        ok_r, why = rem.probe()
        print('remote %s: %s' % (a.remote, why if ok_r else 'NOT used: ' + why), flush=True)
        if ok_r:
            err = rem.prepare(a.work, os.path.dirname(os.path.abspath(__file__)), a.save_b or bt_save_b())
            if err:
                print('remote %s: NOT used: setup failed: %s' % (a.remote, err), flush=True)
                ok_r = False
        rem_ok = ok_r
    cores = os.cpu_count() or 4
    last_start, last_rstart, throttled, last_watch = 0.0, 0.0, False, 0.0
    child_args = ['--seconds', str(a.seconds), '--max-bad-cam', str(a.max_bad_cam), '--min-changes', str(a.min_changes), '--partner-char', str(a.partner_char),
                  '--enemy2-char', str(a.enemy2_char)]

    def local_room():
        """True when another local check may start: under --jobs and (unless --no-throttle) the load other sessions put on the host (load average minus about 2.2 per
        running check of this run) leaves room for it on the cores."""
        nonlocal throttled
        nloc = len([1 for v in running.values() if not v[4]])
        if nloc >= max(1, a.jobs):
            return False
        if a.no_throttle or nloc == 0:
            return True
        ext = max(0.0, os.getloadavg()[0] - 2.2 * nloc)
        if ext + 2.2 * (nloc + 1) > cores:
            if not throttled:
                print('THROTTLE: other load %.1f on %d cores, %d local check(s) running: waiting before the next local start' % (ext, cores, nloc), flush=True)
                throttled = True
            return False
        throttled = False
        return True

    def start_local(n):
        d = os.path.join(out, n)
        os.makedirs(d, exist_ok=True)
        cmd = [sys.executable, os.path.abspath(__file__), '--work', a.work, '--out', out, '--only', n] + child_args + (['--save-b', a.save_b] if a.save_b else [])
        lf = open(os.path.join(d, 'check.log'), 'w')
        running[n] = (subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT, env=dict(os.environ, TT_CHILD='1'), start_new_session=True), time.time(), lf, 'local', False)
        where[n] = 'local (after a desktop failure)' if n in retried else 'local'
        print('START %s on local (%d running)' % (n, len(running)), flush=True)

    def start_remote(n):
        d = os.path.join(out, n)
        os.makedirs(d, exist_ok=True)
        lf = open(os.path.join(d, 'check.log'), 'w')
        running[n] = (rem.start(n, child_args, lf), time.time(), lf, 'desktop', True)
        where[n] = 'desktop'
        used.add('desktop')
        print('START %s on desktop (%d running)' % (n, len(running)), flush=True)

    HARD = ('TTVS', 'BANTER', 'ROSTER', 'RESULT', 'CLOUD', 'DESYNC')

    def real_difference(n):
        """The remote check failed because the two copies really differ (not a walk timeout / short battle / ssh trouble)."""
        try:
            ls = open(os.path.join(out, n, 'check.log'), errors='replace').read().splitlines()
        except OSError:
            return False
        for l in ls:
            if l.startswith(HARD) or (l.startswith('NETSYNC') and 'mismatch' in l and 'too short' not in l):
                return True
        return False

    while pending or running:
        now = time.time()
        if rem_ok and any(v[4] for v in running.values()) and now - last_watch >= 15:   # the user starts a game there: off the desktop at once (its checks run locally instead)
            last_watch = now
            why = rem.busy_now()
            if why:
                print('remote %s: busy now (%s): stopping its checks, rerunning them locally, desktop off for the rest of this run' % (a.remote, why), flush=True)
                rem.kill()
                rem_ok = False
                for n in [n for n, v in running.items() if v[4]]:
                    p_, t_, lf_, wh_, _ = running.pop(n)
                    lf_.close()
                    try:
                        os.killpg(p_.pid, signal.SIGTERM)
                    except OSError:
                        pass
                    rem.fetch(out)
                    os.replace(os.path.join(out, n, 'check.log'), os.path.join(out, n, 'check.desktop_stopped.log'))
                    pending.insert(0, n)
                    retried.add(n)
        if rem_ok and pending and pending[0] not in retried and len([1 for v in running.values() if v[4]]) < max(1, a.remote_jobs) and now - last_rstart >= 10:
            start_remote(pending.pop(0))
            last_rstart = time.time()
        elif pending and now - last_start >= 15 and local_room():
            start_local(pending.pop(0))
            last_start = time.time()
        time.sleep(2)
        for n in list(running):
            p, t, lf, wh, is_rem = running[n]
            if p.poll() is None:
                continue
            lf.close()
            del running[n]
            rc = p.returncode
            if is_rem:
                got = rem.fetch(out)
                txt = open(os.path.join(out, n, 'check.log'), errors='replace').read()
                if rc == 255 or not got or ('CHECK %s:' % n) not in txt:   # ssh / setup trouble, not a verdict
                    print('remote %s: ssh/rsync trouble (rc %d) for %s: rerunning it locally, desktop off for the rest of this run' % (a.remote, rc, n), flush=True)
                    rem.cleanup()
                    rem_ok = False
                    os.replace(os.path.join(out, n, 'check.log'), os.path.join(out, n, 'check.remote_failed.log'))
                    pending.insert(0, n)
                    retried.add(n)
                    continue
                if rc != 0 and not real_difference(n):
                    print('FAIL  %s on desktop (%.0f s) without a real difference (timing): rerunning it locally' % (n, time.time() - t), flush=True)
                    os.replace(os.path.join(out, n, 'check.log'), os.path.join(out, n, 'check.desktop.log'))
                    pending.insert(0, n)
                    retried.add(n)
                    continue
            results[n] = (rc == 0, time.time() - t)
            print('%s %s on %s (%.0f s)' % ('PASS ' if rc == 0 else 'FAIL ', n, where[n], time.time() - t), flush=True)
    if rem and used:
        rem.cleanup()   # (also when it was switched off: kills anything left of this run there)
    print('placement: ' + ', '.join('%s=%s' % (n, where.get(n, '?')) for n in names), flush=True)
    ok = all(v[0] for v in results.values())
    print('---- summary (%d checks, --jobs %d, wall %.0f s, sum of checks %.0f s) ----' % (len(results), a.jobs, time.time() - t0, sum(v[1] for v in results.values())))
    for n in names:
        okn, secs = results[n]
        first = ''
        try:
            ls = open(os.path.join(out, n, 'check.log'), errors='replace').read().splitlines()
            fails = [l for l in ls if l.startswith(('FAIL', 'NETSYNC', 'DESYNC', 'TTVS', 'BANTER', 'ROSTER', 'RESULT', 'CLOUD', 'walk failed')) and not l.startswith('FAIL  mp_diverge')]
            first = (fails[0] if fails else '')[:150]
        except OSError:
            pass
        print('%s  %-24s %4.0f s  %s' % ('PASS' if okn else 'FAIL', n, secs, '' if okn else first + '   [' + os.path.join(out, n) + ']'))
    if a.baseline and not a.no_online and not a.no_xdev and not a.only:   # the real devices (own dirs there; skipped with a printed reason when offline / busy)
        try:
            r = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'xdev_test.py'), '--work', a.work, '--targets', a.xdev_targets, '--out', os.path.join(out, 'xdev')], timeout=4500)
            print('xdev (cross-device online test): ' + {0: 'PASS or skipped', 1: 'FAIL', 2: 'could not run (not counted)'}.get(r.returncode, 'rc %d' % r.returncode))
            ok = ok and r.returncode != 1
        except subprocess.TimeoutExpired:
            print('xdev: timed out (not counted)')
    print('RESULT: ' + ('PASS' if ok else 'FAIL') + '  (logs in %s)' % out)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()

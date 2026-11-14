#!/usr/bin/env python3
"""Tag team (2v2) regression test: boots the game headless straight into a Quick Battle (battle_test.py --direct), with the tag team
on, lets the four fighters fight for a while and checks, from the runtime log:

  * four fighters were seen and each one changed its script (the extras think and act: an idle extra changes it only once or twice),
  * the game camera stayed sane (the camera log's eye = (0,0,0) or -480 garbage frames stay under --max-bad-cam),
  * the extra fighters were spawned and started.
With --baseline it also runs the same battle with the tag team off and checks the 1v1 is unchanged (two fighters, no garbage frames).

  tagteam_test.py --work DIR [--out DIR] [--seconds 24] [--baseline] [--max-bad-cam 5] [--min-changes 8]

The work folder is a built one (see docs/testing.md); the runtime needs tagteam.cpp, framerate.cpp's test driver and direct_battle.cpp.
Needs Xvfb. Exit status 0 = pass."""
import argparse
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import battle_test as bt  # noqa: E402


def run_battle(work, out, seconds, tag, fps=30):
    os.makedirs(out, exist_ok=True)
    script = os.path.join(out, 'script.txt')
    with open(script, 'w') as f:
        f.write('%d quit\n' % ((seconds + 6) * 1000))
    os.environ['BT_NO_FREEZE'] = '1'   # read by battle_test.py itself: the CPU fighters fight (the default is a standing dummy)
    env = {'PSPRECOMP_CAMX_LOG': '800', 'PSPRECOMP_FIGHTER_LOG': '1', 'PSPRECOMP_TAGTEAM_LOG': '1'}
    if tag:
        env['PSPRECOMP_TAGTEAM'] = '1'
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', required=True)
    ap.add_argument('--out', default=None)
    ap.add_argument('--seconds', type=int, default=24)
    ap.add_argument('--baseline', action='store_true', help='also check that the same battle without the tag team is the plain 1v1')
    ap.add_argument('--max-bad-cam', type=int, default=5)
    ap.add_argument('--min-changes', type=int, default=8)
    a = ap.parse_args()
    out = os.path.abspath(a.out or tempfile.mkdtemp(prefix='tagteam_test_'))
    work = os.path.abspath(a.work)
    ok = True

    def check(cond, msg):
        nonlocal ok
        print(('PASS  ' if cond else 'FAIL  ') + msg)
        ok = ok and cond

    changes, cam, bad, spawn, started = analyse(run_battle(work, os.path.join(out, 'tag'), a.seconds, True))
    print('tag team on: fighters %d, script changes %s, camera lines %d (bad %d), spawned %d, started %d' % (len(changes), sorted(changes.values()), cam, bad, spawn, started))
    check(len(changes) == 4, 'four fighters were seen')
    check(spawn == 2 and started == 2, 'both extras were spawned and started')
    # the human has no scripted input: the three AI-driven fighters (the CPU and both extras) must act
    check(len(changes) == 4 and sorted(changes.values())[1] >= a.min_changes, 'the three AI-driven fighters acted (>= %d script changes each)' % a.min_changes)
    check(cam > 100 and bad <= a.max_bad_cam, 'the camera stayed sane (%d bad of %d logged updates, limit %d)' % (bad, cam, a.max_bad_cam))
    if a.baseline:
        changes, cam, bad, spawn, started = analyse(run_battle(work, os.path.join(out, 'plain'), a.seconds, False))
        print('tag team off: fighters %d, camera lines %d (bad %d)' % (len(changes), cam, bad))
        check(len(changes) == 2 and spawn == 0, 'without the tag team the battle is the plain 1v1')
        check(bad == 0, 'no camera garbage in the plain battle')
    print('RESULT: ' + ('PASS' if ok else 'FAIL') + '  (logs in %s)' % out)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()

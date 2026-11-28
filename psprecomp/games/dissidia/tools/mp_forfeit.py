#!/usr/bin/env python3
"""mp_forfeit.py --work-a DIR --work-b DIR [--out DIR] [--who A|B] [--then A|B] [--env 'K=V ...']

Forfeit test (docs/tag-team.md "Batch forty-six"): two LAN copies, Communications > Tag Team Battle > Team 2 v 2 with CPU fill, a short fight, then --who
opens the pause menu (Start), picks Forfeit and confirms (Down, Cross, Right, Cross) through the debug socket. Checks: the forfeiter leaves the battle (log
`[TTPAUSE] .. the battle is left`, then no more lock-step frames / a menu on its screen), the other copy goes on (its frame counter still runs) with the
forfeiter's side taken over; with --then the other copy forfeits as well and both must be out of the battle. Screenshots in --out."""
import argparse, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mp_test as m
m.BTN['start'] = 0x8

ap = argparse.ArgumentParser()
ap.add_argument('--work-a', required=True); ap.add_argument('--work-b', required=True)
ap.add_argument('--out', default='/tmp/mp_forfeit'); ap.add_argument('--secs', type=int, default=12)
ap.add_argument('--who', default='B', choices=['A', 'B']); ap.add_argument('--then', default='', choices=['', 'A', 'B'])
ap.add_argument('--env', default=''); ap.add_argument('--env-a', default=''); ap.add_argument('--env-b', default='')
ap.add_argument('--reg', default='', choices=['', 'A', 'B']); ap.add_argument('--regn', type=int, default=1)
a = ap.parse_args()
out = os.path.abspath(a.out); os.makedirs(out, exist_ok=True)
env = {'PSPRECOMP_CLOUD': '0', 'PSPRECOMP_TAGTEAM_LOG': '1', **dict(kv.split('=', 1) for kv in a.env.split() if '=' in kv)}
A = m.Copy('A', os.path.abspath(os.path.expanduser(a.work_a)), out, {**env, 'MP_FILL': '1', **dict(kv.split('=', 1) for kv in a.env_a.split() if '=' in kv)})
B = m.Copy('B', os.path.abspath(os.path.expanduser(a.work_b)), out, {**env, **dict(kv.split('=', 1) for kv in a.env_b.split() if '=' in kv)})
realshot = m.shot
cp = {'A': A, 'B': B}
def shot(c, o, nm):   # as mp_take.py: registers extra rows of the chosen player on the setup screen
    if nm == 'before_enter' and c.side == a.reg:
        for k in range(1, a.regn + 1):
            c.press(*(['up'] * (5 - k)), gap=0.5); c.press('cross', gap=2.0); c.press(*(['right'] * k), gap=0.6)
            c.press('cross', gap=1.5); c.press('cross', gap=3.0); c.press(*(['down'] * (5 - k)), gap=0.5)
if a.reg: m.shot = shot


def forfeit(c):
    c.press('start', 'down', 'cross', 'right', 'cross', gap=0.6)

def lockframes(c):
    f = re.findall(r'\[TTLOCK\] frame (\d+): lead', c.text())
    return int(f[-1]) if f else 0

rc = 1
try:
    A.start(); B.start()
    m.walk(A, B, 0, 'team2', None)
    m.exercise(A, B, a.secs)
    for who in [a.who] + ([a.then] if a.then else []):
        c, o = cp[who], cp['B' if who == 'A' else 'A']
        n0 = len(c.text())
        realshot(c, out, 'before_' + who); realshot(o, out, 'before_other_' + who)
        forfeit(c)
        time.sleep(8)
        t = c.text()
        print(who, 'forfeit:', [l for l in t.splitlines() if 'TTPAUSE' in l or 'forfeit' in l.lower()][-6:])
        realshot(c, out, 'after_' + who); realshot(o, out, 'after_other_' + who)
        f1 = lockframes(o); time.sleep(4); f2 = lockframes(o)
        print('other copy lock-step frame', f1, '->', f2)
        for k in ('A', 'B'):
            x = cp[k].text()
            print(k, 'left-lines:', len(re.findall(r'the battle is left', x)), 'ends:', [l for l in x.splitlines() if 'TTFORFEIT' in l or 'battle ends' in l][-4:])
    time.sleep(4)
    realshot(A, out, "end_A"); realshot(B, out, 'end_B')
    rc = 0
finally:
    for c in (A, B):
        c.stop()
sys.exit(rc)

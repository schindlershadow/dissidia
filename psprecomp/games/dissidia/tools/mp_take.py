#!/usr/bin/env python3
"""mp_take.py --work-a DIR --work-b DIR [--out DIR] [--secs 70] [--kolist slot0:20] [--reg A|B] [--regn 1] [--env 'K=V ...']

Slot take test (docs/tag-team.md "Spectating after death online"): two LAN copies, Communications > Tag Team Battle > Team 2 v 2 with CPU fill,
one player (--reg, default A) registers --regn more characters on the setup screen, the slot is KO'd at the --kolist times
(PSPRECOMP_TAGTEAM_KOLIST, natural KO), the CPU team mate takes the slot, and the player's next member must take it back: look for
`[TTLOCK] .. slot take: ..` on both copies (and `slot take refused (why ..)` when a guard said no), NETSYNC 0 mismatched."""
import argparse, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mp_test as m

ap = argparse.ArgumentParser()
ap.add_argument('--work-a', required=True); ap.add_argument('--work-b', required=True)
ap.add_argument('--out', default='/tmp/mp_take'); ap.add_argument('--secs', type=int, default=70)
ap.add_argument('--kolist', default='slot0:20'); ap.add_argument('--reg', default='A', choices=['A', 'B'])
ap.add_argument('--regn', type=int, default=1); ap.add_argument('--env', default='')
a = ap.parse_args()
out = os.path.abspath(a.out); os.makedirs(out, exist_ok=True)
def shot(c, o, nm):   # replaces mp_test.shot: the 'before_enter' step registers the extra rows of the chosen player
    if nm == 'before_enter' and c.side == a.reg:
        for k in range(1, a.regn + 1):
            c.press(*(['up'] * (5 - k)), gap=0.5); c.press('cross', gap=2.0); c.press(*(['right'] * k), gap=0.6)
            c.press('cross', gap=1.5); c.press('cross', gap=3.0); c.press(*(['down'] * (5 - k)), gap=0.5)
m.shot = shot
env = {'PSPRECOMP_CLOUD': '0', 'PSPRECOMP_TAGTEAM_LOG': '1', 'PSPRECOMP_TAGTEAM_KOLIST': a.kolist, 'PSPRECOMP_TAGTEAM_KOLIST_NAT': '1',
       **dict(kv.split('=', 1) for kv in a.env.split() if '=' in kv)}
A = m.Copy('A', os.path.abspath(os.path.expanduser(a.work_a)), out, {**env, 'MP_FILL': '1'})
B = m.Copy('B', os.path.abspath(os.path.expanduser(a.work_b)), out, dict(env))
rc = 1
try:
    A.start(); B.start()
    m.walk(A, B, 0, 'team2', None)
    m.exercise(A, B, a.secs); time.sleep(3)
    takes = [[l for l in c.text().splitlines() if 'slot take' in l] for c in (A, B)]
    for c, t in zip((A, B), takes):
        print(c.side, ':', *(t or ['no slot take line']), sep='\n  ')
        print(m.stats(c))
    nets = [re.findall(r'\[NETSYNC\] so far: frames compared (\d+), mismatched (\d+)', c.text()) for c in (A, B)]
    print('NETSYNC', [n[-1] if n else None for n in nets])
    ok = all(any('slot take:' in l for l in t) for t in takes) and all(n and n[-1][1] == '0' for n in nets)
    print('RESULT:', 'PASS' if ok else 'FAIL'); rc = 0 if ok else 1
finally:
    A.stop(); B.stop()
sys.exit(rc)

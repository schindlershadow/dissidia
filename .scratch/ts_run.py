#!/usr/bin/env python3
"""tt_run.py OUT SCRIPT [fps] [extra KEY=VAL env...]: battle_test.run with fast-forward (menus 4x, script unthrottled)."""
import sys, os, argparse
sys.path.insert(0, os.path.expanduser('~/code/dissidia-tagslot/psprecomp/games/dissidia/tools'))
import battle_test as bt
out, script = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
fps = int(sys.argv[3]) if len(sys.argv) > 3 else 30
env = dict(kv.split('=', 1) for kv in sys.argv[4:])
a = argparse.Namespace(work=os.path.expanduser('~/dissidia/work_tagslot'), script=script, fps=fps, out=out, char='Squall',
                       timeout=400, ff=True, ff_menus=4, direct=os.path.expanduser('~/code/dissidia-tagslot/psprecomp/games/dissidia/tests/replays/'+os.environ.get('WALK','quick_battle_walk.txt')+''), env=env, stage=12, record=None, shots=os.environ.get('TT_SHOTS'), watch=os.environ.get('TT_WATCH'), watch_after=float(os.environ.get('TT_WATCH_AFTER', '4')))
sys.exit(bt.run(a) or 0)

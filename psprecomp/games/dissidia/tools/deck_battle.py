#!/usr/bin/env python3
"""Battle test on the Steam Deck over ssh (desktop mode, real display): the same menu walk as battle_test.py, driven through the runtime's
debug socket (ssh -L) and the game's log, with a per-second sampler (deck_sampler.py) running on the Deck beside it.

  deck_battle.py --out DIR [--host deck] [--use-settings] [--fps 60] [--render-scale 3] [--hd 0|1] [--env K=V ...] [--secs 40]

The Deck's own settings.ini is not touched (a private one is pushed). Saves: Quick Battle must be unlocked; nothing is written."""
import argparse, os, re, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import battle_test as bt   # noqa: E402

W = '$HOME/dissidia/work'


class DeckGame(bt.Game):
    def __init__(self, a, out, extra):
        super().__init__('.', a.fps, out, '', a.port, script=None, extra_env=extra)
        self.a, self.host = a, a.host
        self.log = out + '/run.log'
        self.rlog = '/tmp/deck_bt.log'
        self.fwd = None

    def ssh(self, cmd, **kw):
        return subprocess.run(['ssh', '-o', 'ControlMaster=auto', '-o', 'ControlPath=/tmp/deckbt-%C', '-o', 'ControlPersist=600', self.host, cmd],
                              capture_output=True, text=kw.pop('text', True), **kw)

    def start(self):
        a = self.a
        if a.use_settings:
            ini = self.ssh('cat %s/settings.ini' % W).stdout
            ini = ''.join(l + '\n' for l in ini.splitlines() if not l.startswith(('check_updates=', 'display=', 'fps=', 'render_scale=') if False else ('check_updates=', 'display=')))
            ini += 'check_updates=0\ndisplay=fullscreen\n'
        else:
            ini = 'display=fullscreen\nfps=%d\nrender_scale=%d\nvoice=en\ncheck_updates=0\nhd_textures=%d\n' % (a.fps, a.render_scale, a.hd)
        for kv in a.set:
            k = kv.split('=')[0]
            ini = ''.join(l + '\n' for l in ini.splitlines() if not l.startswith(k + '=')) + kv + '\n'
        open(self.out + '/settings.ini', 'w').write(ini)
        script = self.out + '/idle.txt'
        open(script, 'w').write('1000 mark start\n900000 quit\n')
        subprocess.run(['scp', '-q', self.out + '/settings.ini', script, '%s:/tmp/' % self.host], check=True)
        subprocess.run(['scp', '-q', os.path.join(HERE, 'deck_sampler.py'), '%s:/tmp/' % self.host], check=True)
        env = dict(PSPRECOMP_SETTINGS='/tmp/settings.ini', PSPRECOMP_FIGHTER_LOG='1', PSPRECOMP_SAVEDATA_AUTO='0', PSPRECOMP_TEST_FREEZE_CPU='1',
                   PSPRECOMP_TEST_NO_AI='1', PSPRECOMP_DEBUG_PORT=str(self.port), PSPRECOMP_TEST_GO='/tmp/deck_bt_go', PSPRECOMP_TEST_SCRIPT='/tmp/idle.txt',
                   PSPRECOMP_TEST_PLAYER_CHAR=str(bt.CHAR_ID['squall']), PSPRECOMP_RENDERER='vulkan', PSPRECOMP_MS0='/tmp/deck_bt_ms0')
        env.update(self.extra_env)
        exports = ' '.join('%s=%s' % kv for kv in env.items())
        self.ssh('rm -rf /tmp/deck_bt_ms0 /tmp/deck_bt_go; cp -r %s/ms0 /tmp/deck_bt_ms0' % W)
        # the desktop session's display (Wayland; Xwayland as the fallback)
        cmd = ('export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 DISPLAY=:0 XAUTHORITY=$(ls /run/user/1000/xauth_* | head -1); %s; cd ~/dissidia/work; '
               'nohup ~/dissidia/game_dissidia/tools/run.sh ~/dissidia/work %s --log %s </dev/null >/dev/null 2>&1 &' % (
                   ' '.join('export ' + e for e in exports.split(' ')) if False else ';'.join('export ' + e for e in exports.split(' ')), ('--ge-sync ' + a.ge_sync) if a.ge_sync else '', self.rlog))
        self.ssh('rm -f %s' % self.rlog)
        self.ssh(cmd)
        self.fwd = subprocess.Popen(['ssh', '-N', '-o', 'ExitOnForwardFailure=yes', '-o', 'ServerAliveInterval=5', '-L', '%d:127.0.0.1:%d' % (self.port, self.port), self.host])
        self.ssh('nohup python3 /tmp/deck_sampler.py /tmp/deck_samples.txt </dev/null >/dev/null 2>&1 &')

    def cmd(self, line):
        for i in range(6):
            try:
                return super().cmd(line)
            except OSError as e:
                print('[deck_battle] cmd %r failed (%s), fwd rc=%s; retry' % (line, e, self.fwd.poll()), file=sys.stderr)
                if self.fwd.poll() is not None:
                    self.fwd = subprocess.Popen(['ssh', '-N', '-o', 'ExitOnForwardFailure=yes', '-L', '%d:127.0.0.1:%d' % (self.port, self.port), self.host])
                time.sleep(2)
        raise RuntimeError('debug socket unreachable')

    def alive(self):
        return self.ssh('pgrep -x psprecomp_runti >/dev/null && echo y').stdout.strip() == 'y'   # comm is truncated to 15 chars

    def stop(self):
        self.ssh('pkill -x psprecomp_runti; sleep 1; pkill -9 -x psprecomp_runti; true')
        if self.fwd:
            self.fwd.terminate()

    def touch_go(self):
        self.ssh('touch /tmp/deck_bt_go')

    def lines(self):
        r = self.ssh('tail -c +%d %s' % (self.pos + 1, self.rlog), text=False)
        data = r.stdout or b''
        cut = data.rfind(b'\n') + 1
        self.pos += cut
        return data[:cut].decode(errors='replace').splitlines()

    def shot(self):
        from PIL import Image
        self.n += 1
        self.cmd('S /tmp/deck_bt_shot.tga')
        p = os.path.join(self.out, 'shot_%d.tga' % (self.n % 3))
        subprocess.run(['scp', '-q', '%s:/tmp/deck_bt_shot.tga' % self.host, p], check=True)
        return Image.open(p).convert('RGB').resize((480, 272))

    def keep_shot(self, name):
        self.cmd('S /tmp/deck_bt_keep.tga')
        subprocess.run(['scp', '-q', '%s:/tmp/deck_bt_keep.tga' % self.host, os.path.join(self.out, name + '.tga')])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--host', default='deck')
    ap.add_argument('--use-settings', action='store_true')
    ap.add_argument('--set', action='append', default=[], metavar='K=V', help='settings.ini override')
    ap.add_argument('--fps', type=int, default=60)
    ap.add_argument('--render-scale', type=int, default=3)
    ap.add_argument('--hd', type=int, default=1)
    ap.add_argument('--env', action='append', default=[], metavar='K=V')
    ap.add_argument('--secs', type=int, default=40, help='measure this long after the fight starts')
    ap.add_argument('--perf', type=int, default=0, help='perf record the game for this many seconds in the fight')
    ap.add_argument('--mash', action='store_true', help='button-mash a combo through --secs (pair with --env PSPRECOMP_TEST_NO_AI=0 PSPRECOMP_TEST_FREEZE_CPU=0)')
    ap.add_argument('--ge-sync', default='', help='run.sh --ge-sync value (off|flush|1|2|3); default: run.sh decides (flush on a Deck)')
    ap.add_argument('--ff-menus', type=int, default=0, help='run the menu trip at this multiple of real time (-1 = as fast as frames render)')
    ap.add_argument('--series', type=int, default=0, help='after the measuring time, capture this many consecutive screenshots (flicker check)')
    ap.add_argument('--perf-args', default='-F 1999')
    ap.add_argument('--port', type=int, default=29999)
    ap.add_argument('--stage', type=int, default=12)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    a.out = os.path.abspath(a.out)
    os.makedirs(a.out, exist_ok=True)
    g = DeckGame(a, a.out, dict(kv.split('=', 1) for kv in a.env))
    try:
        g.start()
        t0 = time.time()
        bt.walk_menus(g, 'squall', stage=a.stage, ff_menus=a.ff_menus)
        time.sleep(2)
        fl = g.ssh('grep -c "^\\[FIGHTER\\]" %s' % g.rlog).stdout.strip()
        if not fl.isdigit() or int(fl) < 50:
            raise RuntimeError('not in a fight: no [FIGHTER] lines in the log (menu walk went wrong)')
        print('[deck_battle] in battle after %.0f s' % (time.time() - t0), file=sys.stderr)
        if a.perf:
            time.sleep(8)
            g.ssh('perf record %s -o /tmp/perf.data -p $(pgrep -x psprecomp_runti) -- sleep %d >/tmp/perf_rec.txt 2>&1; '
                  'perf report -i /tmp/perf.data --sort pid,symbol --stdio --children -g none 2>/dev/null | grep -v "^#" | head -120 > /tmp/perf_rep.txt' % (a.perf_args, a.perf))
            subprocess.run(['scp', '-q', '%s:/tmp/perf_rep.txt' % a.host, a.out + '/perf.txt'])
        if a.mash:
            end, i = time.time() + a.secs, 0
            seq = [('cross',), ('cross',), ('triangle',), ('right',), ('cross',), ('square',), ('left',), ('triangle',)]
            while time.time() < end:
                g.press(*seq[i % len(seq)], ms=90)
                i += 1
                time.sleep(0.25)
        else:
            time.sleep(a.secs)
        g.keep_shot('battle')
        if a.series:
            for i in range(a.series):
                g.press(*(('cross',), ('right',), ('triangle',))[i % 3], ms=90)
                g.keep_shot('series_%02d' % i)
    finally:
        subprocess.run(['scp', '-q', '%s:/tmp/deck_samples.txt' % a.host, a.out + '/samples.txt'])
        subprocess.run(['scp', '-q', '%s:%s' % (a.host, g.rlog), a.out + '/run.log'])
        g.stop()
        g.ssh('pkill -f deck_sampler.py; true')
    fps = re.findall(r'\[FPS\] ([\d.]+) GE frames/s', open(a.out + '/run.log', errors='replace').read())
    print('FPS lines:', ' '.join(fps))
    n = max(2, a.secs // 5 - 1)
    f = [float(x) for x in fps[-n:]]
    sm = [l for l in open(a.out + '/samples.txt', errors='replace')][-a.secs:]
    gb = [int(m.group(1)) for l in sm for m in [re.search(r'gpu_busy=(\d+)%', l)] if m]
    lst = re.findall(r'list=([\d.]+)', open(a.out + '/run.log', errors='replace').read())
    gpu = re.findall(r'GPU ([\d.]+) ms/frame', open(a.out + '/run.log', errors='replace').read())
    print('SUMMARY fps mean %.1f min %.1f | gpu_busy %.0f%% | render list ms %s | VK GPU ms %s' % (sum(f) / len(f), min(f), sum(gb) / max(1, len(gb)), lst[-1] if lst else '-', gpu[-1] if gpu else '-'))


if __name__ == '__main__':
    main()

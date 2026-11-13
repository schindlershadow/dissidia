#!/usr/bin/env python3
"""Battle test on an Android device over adb: the same menu walk as battle_test.py (Quick Battle, Squall vs the dummy Lightning), driven
through the runtime's debug socket (adb forward) and the game's log, with screenshots pulled back. Used to check graphics settings on
the phone itself (Vulkan, custom GPU driver, ray-traced shadows).

  android_battle.py --serial 192.168.2.53:38921 --renderer vulkan [--driver ID] [--rt 1] [--fps 30] [--shots 3,8] [--script S] --out DIR

The game's env.txt is replaced for the run (the old one is kept in DIR and put back afterwards); the game's own settings.ini is not touched
(a private settings file is used). Saves are the device's own: Quick Battle must be unlocked, and nothing is written (autosave is off).
"""
import argparse, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import battle_test as bt   # noqa: E402

PKG = 'org.psprecomp.game'
DEV = '/sdcard/Android/data/%s/files/game' % PKG


class AndroidGame(bt.Game):
    def __init__(self, serial, out, args, extra_env):
        super().__init__('.', args.fps, out, '', args.port, script=args.script, extra_env=extra_env)
        self.serial, self.args = serial, args
        self.log = DEV + '/last_run.log'
        self.go_file = DEV + '/bt_go'

    def adb(self, *a, check=True, text=True, **kw):
        for attempt in range(3):   # wireless adb drops a pull now and then (the adb client aborts): try again
            r = subprocess.run(['adb', '-s', self.serial] + list(a), capture_output=True, text=text, check=False, **kw)
            if r.returncode == 0 or attempt == 2:
                break
            time.sleep(1.5)
        if check and r.returncode != 0:
            raise subprocess.CalledProcessError(r.returncode, r.args, output=r.stdout, stderr=r.stderr)
        return r

    def sh(self, cmd):
        return self.adb('shell', cmd, check=False).stdout

    def start(self):
        a = self.args
        env_old = os.path.join(self.out, 'env.txt.orig')
        if not os.path.exists(env_old):
            self.adb('pull', DEV + '/env.txt', env_old)
        ini = os.path.join(self.out, 'bt_settings.ini')
        if a.use_settings:          # the player's own settings.ini (render scale, SSAO, RT rows ...): what they see is what is tested
            self.adb('pull', DEV + '/settings.ini', ini)
            with open(ini) as f: kept = f.read()
            drop = tuple(k + '=' for k in a.drop)
            kept = ''.join(l + '\n' for l in kept.splitlines() if not l.startswith(drop or ('\0',)) and not l.startswith(('touch=', 'check_updates=', 'display=', 'fps=') if a.fps_override else ('touch=', 'check_updates=', 'display=')))
            with open(ini, 'w') as f: f.write(kept + 'touch=off\ncheck_updates=0\ndisplay=fullscreen\n' + ('fps=%d\n' % a.fps if a.fps_override else ''))
        else:
          with open(ini, 'w') as f:
            f.write('display=fullscreen\nfps=%d\nrender_scale=%d\nvoice=en\ncheck_updates=0\nhd_textures=0\nrenderer=%s\ngpu_driver=%s\nrt_shadows=%s\n'
                    'contact_shadows=%d\ntouch=off\n' % (a.fps, a.render_scale, a.renderer, a.driver, a.rt, 1 if a.rt == '1' else a.contact))
        env = {'PSPRECOMP_SETTINGS': DEV + '/bt_settings.ini', 'PSPRECOMP_FIGHTER_LOG': '1', 'PSPRECOMP_SAVEDATA_AUTO': '0',
               'PSPRECOMP_TEST_FREEZE_CPU': '1', 'PSPRECOMP_TEST_GO': self.go_file, 'PSPRECOMP_DEBUG_PORT': '9999'}
        if not self.script:       # the runtime's test driver (game clock, "[TEST] battle started") is only on with a script: an idle one
            self.script = os.path.join(self.out, 'idle_script.txt')
            with open(self.script, 'w') as f:
                f.write('1000 mark start\n600000 quit\n')
        self.adb('push', self.script, DEV + '/bt_script.txt')
        env['PSPRECOMP_TEST_SCRIPT'] = DEV + '/bt_script.txt'
        env.update(self.extra_env)
        envf = os.path.join(self.out, 'env.txt.bt')
        with open(envf, 'w') as f:
            f.write(''.join('%s=%s\n' % kv for kv in env.items()))
        self.adb('push', ini, DEV + '/bt_settings.ini')
        self.adb('push', envf, DEV + '/env.txt')
        self.adb('shell', 'rm -f ' + self.go_file)
        self.adb('shell', 'am force-stop ' + PKG)
        self.adb('forward', 'tcp:%d' % self.port, 'tcp:9999')
        self.adb('shell', 'monkey -p %s -c android.intent.category.LAUNCHER 1' % PKG)
        for _ in range(30):                      # wait for the process
            if self.alive():
                break
            time.sleep(0.5)

    def restore(self):
        orig = os.path.join(self.out, 'env.txt.orig')
        if os.path.exists(orig):
            self.adb('push', orig, DEV + '/env.txt', check=False)
            self.adb('shell', 'chmod 666 ' + DEV + '/env.txt', check=False)   # a push leaves it shell-owned and read-only for the app
        self.adb('forward', '--remove', 'tcp:%d' % self.port, check=False)

    def alive(self):
        return bool(self.sh('pidof ' + PKG).strip())

    def stop(self):
        self.adb('shell', 'am force-stop ' + PKG, check=False)

    def touch_go(self):
        self.adb('shell', 'touch %s; chmod 666 %s' % (self.go_file, self.go_file))   # world-readable: the app is another uid than the shell

    def lines(self):
        r = self.adb('exec-out', 'tail -c +%d %s' % (self.pos + 1, self.log), check=False, text=False)
        data = r.stdout or b''
        cut = data.rfind(b'\n') + 1
        self.pos += cut
        return data[:cut].decode(errors='replace').splitlines()

    def shot(self):
        from PIL import Image
        self.n += 1
        self.cmd('S ' + DEV + '/bt_shot.tga')
        p = os.path.join(self.out, 'shot_%d.tga' % (self.n % 3))
        self.adb('pull', DEV + '/bt_shot.tga', p)
        return Image.open(p).convert('RGB').resize((480, 272))   # the walk's pixel checks assume the PSP-sized picture

    def keep_shot(self, name):
        self.cmd('S ' + DEV + '/bt_keep.tga')
        self.adb('pull', DEV + '/bt_keep.tga', os.path.join(self.out, name + '.tga'))


def hdr_summary(log, scene):
    import re
    txt = open(log, errors='replace').read()
    st = [(float(m[0]), int(m[1]), int(m[2]), int(m[3])) for m in re.findall(r'\[HDRSTATS\] max=([\d.]+) above1=(\d+) above1\.5=(\d+) of (\d+)', txt)]
    print('[hdr] HDR Scene=%s: scene targets %s; HDR swapchain %s' % (scene, ','.join(re.findall(r'scene targets: (\S+)', txt)) or '?',
          'live' if 'HDR scRGB' in txt or 'HDR10' in txt else 'NOT live'))
    if st:
        print('[hdr] %d samples: max channel %.2f, peak pixels above 1.0: %d (above 1.5: %d) of %d' %
              (len(st), max(x[0] for x in st), max(x[1] for x in st), max(x[2] for x in st), st[0][3]))
    else:
        print('[hdr] no [HDRSTATS] samples (expected with HDR Scene off: the targets are 8-bit and cannot exceed 1.0)')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--serial', required=True)
    ap.add_argument('--fps-override', action='store_true', help='with --use-settings: still apply --fps instead of the saved one')
    ap.add_argument('--drop', action='append', default=[], metavar='KEY', help='with --use-settings: leave this saved setting out (bisecting)')
    ap.add_argument('--use-settings', action='store_true', help="test with the device's own settings.ini (render scale, SSAO, RT rows, driver ...) instead of the private minimal one")
    ap.add_argument('--renderer', default='vulkan', choices=['gl', 'vulkan'])
    ap.add_argument('--driver', default='', help='installed GPU driver id ("" = the phone\'s own)')
    ap.add_argument('--rt', default='0', choices=['0', '1'], help='Ray-Traced Shadows')
    ap.add_argument('--contact', type=int, default=1)
    ap.add_argument('--fps', type=int, default=30)
    ap.add_argument('--render-scale', type=int, default=4)
    ap.add_argument('--char', default='squall')
    ap.add_argument('--stage', type=int, default=12)
    ap.add_argument('--shots', default='3,8', help='seconds after the fight starts')
    ap.add_argument('--script')
    ap.add_argument('--ai', action='store_true', help='let the CPU fighter act (default: PSPRECOMP_TEST_NO_AI=1, it stands still)')
    ap.add_argument('--env', action='append', default=[], metavar='KEY=VALUE', help='extra PSPRECOMP_* variable for the run')
    ap.add_argument('--wait-script', action='store_true', help='wait for the script to quit the game before finishing')
    ap.add_argument('--hdr-scene', choices=['0', '1'], help='HDR output on with HDR Scene off/on; logs [HDRSTATS] (pixels above 1.0 in the scene target) '
                    'and prints a summary. Use with --script tests/hdr_scene_spells.txt --char lightning --wait-script')
    ap.add_argument('--port', type=int, default=19999)
    ap.add_argument('--timeout', type=int, default=120)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    a.out = os.path.abspath(a.out)
    os.makedirs(a.out, exist_ok=True)
    if a.script:
        a.script = os.path.abspath(a.script)
    extra = {'PSPRECOMP_TEST_PLAYER_CHAR': str(bt.CHAR_ID[a.char.lower()])}
    if not a.ai:
        extra['PSPRECOMP_TEST_NO_AI'] = '1'      # the CPU fighter does nothing (like the desktop harness's dummy, but from the first frame)
    if a.hdr_scene is not None:
        extra.update({'PSPRECOMP_HDR': '1', 'PSPRECOMP_HDR_SCENE': a.hdr_scene, 'PSPRECOMP_HDR_STATS': '1'})
    extra.update(kv.split('=', 1) for kv in a.env)
    g = AndroidGame(a.serial, a.out, a, extra)
    try:
        g.start()
        t0 = time.time()
        bt.walk_menus(g, a.char, stage=a.stage)
        print('[android_battle] in battle after %.0f s' % (time.time() - t0), file=sys.stderr)
        t_start = time.time()
        for sec in [float(x) for x in a.shots.split(',') if x.strip()]:
            time.sleep(max(0.0, t_start + sec - time.time()))
            g.keep_shot('battle_%s_rt%s_%03d' % (a.renderer, a.rt, int(sec)))
        if a.wait_script:
            end = time.time() + a.timeout
            while g.alive() and time.time() < end:
                time.sleep(0.5)
        log = os.path.join(a.out, 'run.log')
        g.adb('pull', DEV + '/last_run.log', log, check=False)
        print(log)
        if a.hdr_scene is not None:
            hdr_summary(log, a.hdr_scene)
    finally:
        g.stop()
        g.restore()


if __name__ == '__main__':
    main()

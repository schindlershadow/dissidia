#!/usr/bin/env python3
"""Cross-device online test: the SAME scripted tag battle (mp_diverge.py's walk, KO list on the lock-step frame, CPU fill, deterministic input through
the debug socket) with copy A on this host and copy B on a REAL other machine, through a local relay (Internet mode, own port) on this host.
What two copies on one host cannot catch (Windows libm vs Linux, stale heap on a long-running Deck, Android arm64) shows up as a NETSYNC mismatch.

  xdev_test.py --work DIR [--targets desktop,deck,laptop,rp6] [--mode team2|team3|ffa] [--out DIR] [--secs N] [--neg] [--env-b 'K=V ...'] [--probe]
  --probe   only say which targets are reachable and idle (exit 0)
  --neg     fail-ability check: copy B gets PSPRECOMP_DMATH_OLD=1 when the binary knows it, else PSPRECOMP_NET_PERTURB=600 (a mismatch must be reported;
            the target then PASSes only if it DID mismatch)

Targets (never touches anything outside its own dir; test saves only, no cloud.ini; muted, nice, no pads):
  desktop  ssh schindler@192.168.2.131, ~/dissidia/xdev/ (runs even while the user plays: nice, dummy audio, PSPRECOMP_NO_PADS=1)
  deck     ssh deck, ~/dissidia/xdev/ (only when no psprecomp_runtime of the user runs there; the cross-built Deck binary, never built on the Deck)
  laptop   ssh schindler@192.168.2.55 (Windows, cmd), C:\\Users\\Schindler\\xdev\\ with its own disc0/ms0 copy; the exe is started through a scheduled task
           (a game started from an ssh session dies with it / has no desktop); skipped when a psprecomp_runtime.exe is running
  rp6      Retroid Pocket 6 over wireless adb (APK installed over, test env.txt moved aside afterwards)
Binaries: --work/build/psprecomp_runtime (this host + desktop), $XDEV_DECK_BIN (default ~/dissidia/build_devtest_deck/psprecomp_runtime), $XDEV_WIN_EXE
(default ~/dissidia/build_winrt_tt/psprecomp_runtime.exe). All must be built from the same commit (the online build id).
Exit: 0 = every reachable target PASSed (none reachable = 0 with a SKIP line), 1 = a FAIL, 2 = could not run."""
import argparse
import fcntl
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mp_test as m  # noqa: E402
import mp_diverge as d  # noqa: E402

HOST_IP = os.environ.get('XDEV_HOST_IP', '192.168.2.130')
SSH = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=6', '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=6']
WINDIR = 'C:/Users/Schindler/xdev'
TARGETS = {'desktop': {'ssh': 'schindler@192.168.2.131', 'os': 'linux', 'cores': 12},
           'deck': {'ssh': 'deck', 'os': 'linux'},
           'laptop': {'ssh': 'schindler@192.168.2.55', 'os': 'win'},
           'rp6': {'os': 'android'}}


def log(msg):
    print('[xdev] ' + msg, file=sys.stderr, flush=True)


def sh(host, cmd, timeout=60, **kw):
    return subprocess.run(SSH + [host, cmd], capture_output=True, text=True, timeout=timeout, **kw)


def binary_of(work):
    p = os.path.realpath(os.path.join(work, 'build', 'psprecomp_runtime'))
    with open(p, 'rb') as f:
        if f.read(2) == b'#!':   # mkx.sh wrapper: `exec -a NAME .../NAME`
            mm = re.search(r'exec -a (\S+)', open(p, errors='replace').read())
            if mm:
                p = os.path.join(os.path.dirname(p), mm[1])
    return p


ANDROID_PKG = 'org.psprecomp.game'
ANDROID_DEV = '/sdcard/Android/media/%s/game' % ANDROID_PKG


def find_rp6():
    """adb serial of the RP6 (mdns name adb-c2aba36c..., or 192.168.2.53:port); NEVER the Shield (192.168.2.37)."""
    out = subprocess.run(['adb', 'devices'], capture_output=True, text=True, timeout=20).stdout
    for l in out.splitlines():
        if ('c2aba36c' in l or l.startswith('192.168.2.53:')) and l.rstrip().endswith('device'):
            return l.split()[0]
    mm = re.search(r'(\S*c2aba36c\S*)\s', subprocess.run(['adb', 'mdns', 'services'], capture_output=True, text=True, timeout=20).stdout)
    if mm:
        return mm[1]
    raise RuntimeError('RP6 not found over adb')


# ---------------------------------------------------------------- probes: (ok, reason)
LINUX_PROBE = r'''busy=""
for p in /proc/[0-9]*; do
  exe=$(readlink $p/exe 2>/dev/null); args=$(tr '\0' ' ' < $p/cmdline 2>/dev/null)
  case "$exe $args" in *psprecomp_runtime*|*/regrt\ *|*play_*.sh*|*tools/run.sh*) ;; *) continue;; esac
  case "$args" in *probe_marker*) continue;; esac
  case "$(readlink $p/cwd 2>/dev/null)" in */dissidia/xdev/*) continue;; esac   # (an xdev game of ours)
  busy="$busy ${p#/proc/}"
done
echo "$(cut -d' ' -f1 /proc/loadavg) $(nproc) [$busy]"'''


def user_game_running(name):
    """'' while the machine is free of the user's game, else a reason. Ours (cwd under dissidia/xdev/) never counts; an ssh failure counts as busy."""
    r = sh(TARGETS[name]['ssh'], 'bash -s', timeout=30, input=LINUX_PROBE)
    mm = re.search(r'([\d.]+) (\d+) \[(.*)\]', r.stdout)
    if r.returncode or not mm:
        return 'ssh probe failed'
    return mm[3].strip() and 'a game/runtime is running there (pid %s)' % mm[3].strip()


def probe(name):
    t = TARGETS[name]
    try:
        if t['os'] == 'linux':
            r = sh(t['ssh'], 'bash -s', timeout=30, input=LINUX_PROBE)
            mm = re.search(r'([\d.]+) (\d+) \[(.*)\]', r.stdout)
            if r.returncode or not mm:
                return False, 'ssh failed'
            load, cores, busy = float(mm[1]), int(mm[2]), mm[3].strip()
            if busy and name == 'deck':
                return False, 'a game/runtime is running on the Deck (pid %s; the user plays)' % busy
            if load > cores * 0.5:
                return False, 'load %.1f on %d cores' % (load, cores)
            return True, 'idle (load %.1f on %d cores%s)' % (load, cores, ', the user\'s game runs there (pid %s)' % busy if busy else '')
        if t['os'] == 'win':
            r = sh(t['ssh'], 'tasklist /fi "imagename eq psprecomp_runtime.exe" /nh', timeout=25)
            if r.returncode:
                return False, 'ssh failed'
            if 'psprecomp_runtime' in r.stdout:
                return False, 'a psprecomp_runtime.exe is running on the laptop'
            return True, 'idle'
        find_rp6()
        return True, 'found over adb'
    except (subprocess.TimeoutExpired, OSError, ValueError, RuntimeError) as e:
        return False, 'probe failed: %s' % e


# ---------------------------------------------------------------- a copy on another machine
class XCopy(m.Copy):
    """Copy B on a real machine. Settings + a test save are staged locally (like m.Copy), pushed to the target's own dir, the game runs there; its stderr log is
    mirrored to self.log (so text() works), the debug socket reached through an ssh -L port forward on this host (PORT[side])."""

    def __init__(self, side, work, out, env, name, run_id, rport):
        super().__init__(side, work, out, env)
        if name == 'rp6':
            rport = 9999
        self.name, self.t, self.run_id, self.rport = name, TARGETS[name], run_id, rport
        self.procs = []
        self.serial = None

    def stage(self):
        ms0 = os.path.join(self.out, 'ms0_' + self.side)
        shutil.rmtree(ms0, ignore_errors=True)
        shutil.copytree(self.env.get('MP_SAVE') or os.path.join(self.work, 'ms0'), ms0, ignore=shutil.ignore_patterns('cloud*', 'SAVEDATA_BACKUP'))
        ini = os.path.join(self.out, 'settings_%s.ini' % self.side)
        with open(ini, 'w') as f:
            f.write('display=window\nfps=%d\nrender_scale=%s\nwidescreen=0\nvoice=en\ncheck_updates=0\nhd_textures=0\nnet_mode=%s\n'
                    % (int(self.env.get('MP_FPS', '30')), self.env.get('MP_RS', '1'), self.env.get('MP_NETMODE', 'internet')))
        return ms0, ini

    def game_env(self, base, paths):
        e = {k: v for k, v in self.env.items() if k.startswith('PSPRECOMP_') or k.startswith('MP_NET')}
        e.update(PSPRECOMP_SETTINGS=paths['ini'], PSPRECOMP_MS0=paths['ms0'], PSPRECOMP_DISC0=paths['disc0'], PSPRECOMP_ADHOC_MODE=self.env.get('MP_NETMODE', 'internet'),
                 PSPRECOMP_ADHOC_NICK=self.side, PSPRECOMP_DEBUG_PORT=str(self.rport), PSPRECOMP_CHARMOD='0', PSPRECOMP_AUDIO='0', PSPRECOMP_SAVEDATA_AUTO='0',
                 PSPRECOMP_NET_TRACE='1', PSPRECOMP_CLOUD='0', PSPRECOMP_CROSS_MID='1', PSPRECOMP_NO_PADS='1', SDL_AUDIODRIVER='dummy',
                 PSPRECOMP_TAGTEAM_LOG='1')
        e.update(base)
        e.pop('PSPRECOMP_DEBUG_BIND', None)
        return e

    def start(self):
        ms0, ini = self.stage()
        if os.path.exists(self.log):
            os.remove(self.log)
        open(self.log, 'w').close()
        fwd = ['-L', '127.0.0.1:%d:127.0.0.1:%d' % (m.PORT[self.side], self.rport)]
        host = self.t.get('ssh')
        if self.t['os'] == 'linux':
            R = 'dissidia/xdev'   # relative to the remote's home
            sh(host, 'mkdir -p %s/work/build %s/run_%s' % (R, R, self.run_id))
            binp = os.environ.get('XDEV_DECK_BIN', os.path.expanduser('~/dissidia/build_devtest_deck/psprecomp_runtime')) if self.name == 'deck' else binary_of(self.work)
            rs = lambda *a: subprocess.run(['rsync', '-a', '-e', ' '.join(SSH)] + list(a), capture_output=True, text=True)
            if self.name == 'deck':   # (a stripped copy: the Deck's /home is nearly full)
                st = os.path.join(self.out, 'deck_runtime')
                if not os.path.exists(st) or os.path.getmtime(st) < os.path.getmtime(binp):
                    subprocess.run(['strip', '-o', st, binp], check=False)
                binp = st if os.path.exists(st) else binp
            for r in (rs(binp, '%s:%s/work/build/psprecomp_runtime' % (host, R)), rs(ms0 + '/', '%s:%s/run_%s/ms0/' % (host, R, self.run_id)),
                      rs(ini, '%s:%s/run_%s/settings.ini' % (host, R, self.run_id))):
                if r.returncode:
                    raise RuntimeError('rsync to %s failed: %s' % (self.name, r.stderr[:200]))
            r = sh(host, 'cd ~/%s/work && for f in ~/dissidia/work/*; do n=$(basename $f); case $n in build|build_*|ms0|cloud*|mods|*.log|frame.tga|settings.ini|play.sh|savemgr_tmp|breakshots) continue;; esac; ln -sfn $f $n; done; chmod +x build/psprecomp_runtime; rm -rf cloud cloud.ini; echo OK' % R)
            if 'OK' not in r.stdout:
                raise RuntimeError('setup on %s failed: %s' % (self.name, r.stderr[:200]))
            home = sh(host, 'echo $HOME').stdout.strip()
            paths = {'ini': '%s/%s/run_%s/settings.ini' % (home, R, self.run_id), 'ms0': '%s/%s/run_%s/ms0' % (home, R, self.run_id), 'disc0': '%s/%s/work/disc0' % (home, R)}
            e = self.game_env({'SDL_VIDEODRIVER': 'offscreen', 'PSPRECOMP_XDEV_RUN': self.run_id}, paths)
            if 'MP_TASKSET' in e:
                e.pop('MP_TASKSET')
            # (exec -a would rename it: bash `exec -a` is a builtin option, not env's; keep the plain name, kill by the run dir in the environment)
            line = 'cd ~/%s/work && unset DISPLAY WAYLAND_DISPLAY; for j in /usr/share/glvnd/egl_vendor.d/50_mesa.json; do [ -f $j ] && export __EGL_VENDOR_LIBRARY_FILENAMES=$j; done; exec nice -n 10 env %s ./build/psprecomp_runtime' % (
                R, ' '.join('%s=%s' % (k, shlex.quote(v)) for k, v in e.items()))
            self.procs.append(subprocess.Popen(SSH + fwd + [host, line], stdout=subprocess.DEVNULL, stderr=open(self.log, 'ab'), start_new_session=True))
        elif self.t['os'] == 'win':
            self.start_win(ms0, ini, fwd)
        else:
            self.start_android(ms0, ini, fwd)

    def adb(self, *a, **kw):
        for _ in range(3):   # wireless adb drops a push / pull now and then
            r = subprocess.run(['adb', '-s', self.serial] + list(a), capture_output=True, text=True, timeout=300, **kw)
            if r.returncode == 0:
                break
            time.sleep(1.5)
        return r

    def start_android(self, ms0, ini, fwd):
        self.serial = find_rp6()
        dev = ANDROID_DEV
        if not getattr(XCopy, '_installed', False):   # the APK of the tag-slot build (same commit as the other legs); installed over, nothing cleared
            apk = os.environ.get('XDEV_APK', os.path.expanduser('~/dissidia/tt/dissidia_tt.apk'))
            r = self.adb('install', '-r', apk)
            if 'Success' not in r.stdout:
                raise RuntimeError('adb install failed: %s' % (r.stdout + r.stderr)[-200:])
            XCopy._installed = True
        self.env_old = os.path.join(self.out, 'env.txt.orig')
        if self.adb('pull', dev + '/env.txt', self.env_old).returncode != 0:
            open(self.env_old + '.none', 'w').close()
        self.adb('shell', 'am force-stop ' + ANDROID_PKG)
        self.adb('shell', 'rm -rf %s/xdev; mkdir -p %s/xdev; rm -f %s/last_run.log' % (dev, dev, dev))
        for src, dst in ((ms0, dev + '/xdev/ms0'), (ini, dev + '/xdev/settings.ini')):
            if self.adb('push', src, dst).returncode:
                raise RuntimeError('adb push failed')
        self.adb('shell', 'chmod -R 777 %s/xdev' % dev)
        e = self.game_env({}, {'ini': dev + '/xdev/settings.ini', 'ms0': dev + '/xdev/ms0', 'disc0': ''})
        e.pop('PSPRECOMP_DISC0'); e.pop('PSPRECOMP_NO_PADS'); e.pop('SDL_AUDIODRIVER')
        envf = os.path.join(self.out, 'env.txt.xdev')
        open(envf, 'w').write(''.join('%s=%s\n' % kv for kv in e.items()))
        self.adb('push', envf, dev + '/env.txt')
        self.adb('shell', 'chmod 666 ' + dev + '/env.txt')
        self.adb('forward', 'tcp:%d' % m.PORT[self.side], 'tcp:%d' % self.rport)
        self.adb('shell', 'monkey -p %s -c android.intent.category.LAUNCHER 1' % ANDROID_PKG)
        self.procs.append(subprocess.Popen(['adb', '-s', self.serial, 'exec-out', 'tail -F -n +1 %s/last_run.log' % dev], stdout=open(self.log, 'ab'), stderr=subprocess.DEVNULL, start_new_session=True))

    def stop_android(self):
        if not self.serial:
            return
        dev = ANDROID_DEV
        self.adb('shell', 'am force-stop ' + ANDROID_PKG)
        if os.path.exists(self.env_old):
            self.adb('push', self.env_old, dev + '/env.txt')
            self.adb('shell', 'chmod 666 ' + dev + '/env.txt')
        else:
            self.adb('shell', 'rm -f ' + dev + '/env.txt')
        self.adb('shell', 'rm -rf %s/xdev' % dev)
        self.adb('forward', '--remove', 'tcp:%d' % m.PORT[self.side])

    def start_win(self, ms0, ini, fwd):
        host = self.t['ssh']
        exe = os.environ.get('XDEV_WIN_EXE', os.path.expanduser('~/dissidia/build_winrt_tt/psprecomp_runtime.exe'))
        sh(host, 'mkdir C:\\Users\\Schindler\\xdev & rmdir /s /q C:\\Users\\Schindler\\xdev\\run_%s' % self.run_id)
        r = sh(host, 'if not exist C:\\Users\\Schindler\\xdev\\disc0 robocopy C:\\Users\\Schindler\\Games\\Dissidia012_tt\\disc0 C:\\Users\\Schindler\\xdev\\disc0 /E /NFL /NDL /NJH /NJS /NP', timeout=1500)
        scp = lambda *a: subprocess.run(['scp', '-q', '-o', 'BatchMode=yes'] + list(a), capture_output=True, text=True, timeout=900)
        sh(host, 'mkdir C:\\Users\\Schindler\\xdev\\run_%s' % self.run_id)
        for src, dst in ((exe, WINDIR + '/psprecomp_runtime.exe'), (ini, '%s/run_%s/settings.ini' % (WINDIR, self.run_id))):
            r = scp(src, '%s:%s' % (host, dst))
            if r.returncode:
                raise RuntimeError('scp to laptop failed: %s' % r.stderr[:200])
        r = scp('-r', ms0, '%s:%s/run_%s/' % (host, WINDIR, self.run_id))
        if r.returncode:
            raise RuntimeError('scp ms0 to laptop failed: %s' % r.stderr[:200])
        w = lambda p: p.replace('/', '\\')
        paths = {'ini': w('%s/run_%s/settings.ini' % (WINDIR, self.run_id)), 'ms0': w('%s/run_%s/ms0_%s' % (WINDIR, self.run_id, self.side)), 'disc0': w(WINDIR + '/disc0')}
        e = self.game_env({}, paths)
        bat = os.path.join(self.out, 'xdev_%s.bat' % self.side)
        with open(bat, 'w', newline='\r\n') as f:
            f.write('@echo off\ncd /d %s\n' % w(WINDIR))
            for k, v in e.items():
                f.write('set "%s=%s"\n' % (k, v))
            f.write('psprecomp_runtime.exe 2> %s\n' % w('%s/run_%s/run.log' % (WINDIR, self.run_id)))
        if scp(bat, '%s:%s/run_%s/run.bat' % (host, WINDIR, self.run_id)).returncode:
            raise RuntimeError('scp bat failed')
        tr = w('%s/run_%s/run.bat' % (WINDIR, self.run_id))
        r = sh(host, 'schtasks /create /tn xdev_%s /tr "%s" /sc once /st 23:59 /it /f & schtasks /run /tn xdev_%s' % (self.run_id, tr, self.run_id))
        if 'SUCCESS' not in r.stdout.upper():
            raise RuntimeError('schtasks failed: %s %s' % (r.stdout[-200:], r.stderr[-200:]))
        ps = ("[Console]::OutputEncoding=[Text.Encoding]::UTF8; $f='%s'; while(!(Test-Path $f)){Start-Sleep 1}; Get-Content -Wait -ReadCount 1 -Encoding UTF8 $f"
              % w('%s/run_%s/run.log' % (WINDIR, self.run_id)))
        self.procs.append(subprocess.Popen(SSH + fwd + [host, 'powershell -NoProfile -Command "%s"' % ps], stdout=open(self.log, 'ab'), stderr=subprocess.DEVNULL, start_new_session=True))

    def stop(self):
        host = self.t.get('ssh')
        try:
            if self.t['os'] == 'android':
                self.stop_android()
            elif self.t['os'] == 'linux':   # the game was exec'd by the ssh session: the SIGHUP of closing it ends it; make sure (by the run dir in its environment)
                sh(host, "for p in $(pgrep -u $USER -f 'work/build/psprecomp_runtime'); do tr '\\0' '\\n' < /proc/$p/environ 2>/dev/null | grep -q 'xdev/run_%s' && kill $p; done; true" % self.run_id, timeout=30)
            elif self.t['os'] == 'win':
                sh(host, 'powershell -NoProfile -Command "Get-Process psprecomp_runtime -ErrorAction SilentlyContinue | Where-Object {$_.Path -like \'C:\\Users\\Schindler\\xdev\\*\'} | Stop-Process -Force"; schtasks /end /tn xdev_%s & schtasks /delete /tn xdev_%s /f' % (self.run_id, self.run_id), timeout=40)
        except (subprocess.TimeoutExpired, OSError):
            pass
        for p in self.procs:
            if p.poll() is None:
                try:
                    os.killpg(p.pid, signal.SIGTERM)
                except OSError:
                    pass


def pick_neg_env(binary, kind='auto'):
    if kind == 'perturb':
        return 'PSPRECOMP_NET_PERTURB=600', 'perturb'
    try:
        if b'PSPRECOMP_DMATH_OLD' in open(binary, 'rb').read():
            return 'PSPRECOMP_DMATH_OLD=1', 'dmath'
    except OSError:
        pass
    return 'PSPRECOMP_NET_PERTURB=600', 'perturb'


# ---------------------------------------------------------------- one pair
def run_pair(a, name, out, work, slot, relay_port):
    kv = lambda s: dict(x.split('=', 1) for x in s.split() if '=' in x)
    room = 'xdev%d_%d' % (slot, os.getpid())
    server = '%s:%d' % (HOST_IP, relay_port)
    neg_env, neg_kind = pick_neg_env(binary_of(work), a.neg_kind) if a.neg else ('', '')
    base = {'PSPRECOMP_CLOUD': '0', 'PSPRECOMP_TAGTEAM_LOG': '1', 'PSPRECOMP_TAGTEAM_KOLIST': d.KOLIST[a.mode], 'PSPRECOMP_TAGTEAM_KOLIST_NAT': '1', 'PSPRECOMP_TAGTEAM_KOLIST_LSFRAME': '1',
            'PSPRECOMP_ADHOC_SPAN': '4', 'MP_NETMODE': 'internet', 'PSPRECOMP_ADHOC_SERVER': server, 'PSPRECOMP_ADHOC_ROOM': room, **kv(a.env)}
    envA = {**base, 'MP_FILL': '1', 'MP_FPS': '30', 'MP_RS': '1', 'PSPRECOMP_ADHOC_PORT': str(48000 + 32 * slot), **kv(a.env_a)}
    envB = {**base, 'MP_SAVE': os.path.abspath(os.path.expanduser(a.save_b)), 'MP_FPS': '30', 'MP_RS': '1', 'PSPRECOMP_ADHOC_PORT': str(48008 + 32 * slot), **kv(neg_env), **kv(a.env_b)}
    m.PORT['A'], m.PORT['B'] = 9600 + 2 * slot, 9601 + 2 * slot
    A = m.Copy('A', work, out, envA)
    B = XCopy('B', work, out, envB, name, '%d_%d' % (slot, os.getpid()), 9601 + 2 * slot)
    res = {'target': name, 'neg': neg_kind}
    bad, ea, eb = [], None, None
    try:
        A.start()
        B.start()
        t0 = time.time()
        m.walk(A, B, 4.0, a.mode, None)
        log('%s: in battle after %.0f s' % (name, time.time() - t0))
        sc = m.load_scale()
        end = time.time() + a.secs * sc
        hard = time.time() + a.secs * max(sc, 1.0) * 3
        last_frames, last_prog, last_chk = None, time.time(), time.time()
        while True:
            m.exercise(A, B, 5)
            if name == 'deck' and time.time() - last_chk > 40:   # the user may start their game on the Deck meanwhile: leave at once
                last_chk = time.time()
                why = user_game_running(name)
                if why and 'ssh probe failed' not in why:
                    bad.append('SKIPPED-BUSY: %s' % why)
                    break
            texts = [c.text() for c in (A, B)]
            if all(any(d.RESULT_RE.match(l) for l in t.splitlines()) for t in texts) and all('[NETSYNC] so far' in t for t in texts):
                break
            ex = [d.extract(t, 'X') for t in texts]
            if any(e['mismatch_lines'] or (e['netsync'] or (0, 0))[1] for e in ex):
                log('%s: mismatch seen: ending the fight now' % name)
                break
            now = time.time()
            frames = tuple((e['netsync'] or (0, 0))[0] for e in ex)
            if frames != last_frames:
                last_frames, last_prog = frames, now
            if now >= hard or (now >= end and now - last_prog > 60):
                break
        time.sleep(6)
        ea, eb = d.extract(A.text(), 'A'), d.extract(B.text(), 'B')
        bad = d.compare(ea, eb, a.min_frames, False)
        if 'Google Drive sync on' in A.text() + B.text():
            bad.append('CLOUD: a copy synced to Google Drive')
    except (RuntimeError, OSError) as e:
        bad.append('run failed: %s' % e)
    finally:
        A.stop()
        B.stop()
        if sys.platform != 'win32':
            subprocess.run(['pkill', '-f', 'run.sh %s' % work], capture_output=True) if False else None
    if any(x.startswith('SKIPPED-BUSY') for x in bad):
        return {'target': name, 'neg': neg_kind, 'verdict': 'SKIP', 'A': None, 'B': None, 'bad': bad, 'lineup': False, 'banter': False, 'result': False}
    mism = bool(bad) and any(x.startswith(('NETSYNC: copy', 'DESYNC')) and ('mismatched' in x or 'DESYNC' in x) for x in bad)
    if a.neg:
        verdict = 'PASS(neg: mismatch detected via %s)' % neg_kind if mism else 'FAIL(neg: no mismatch detected via %s)' % neg_kind
    else:
        verdict = 'FAIL' if bad else 'PASS'
    res.update(verdict=verdict, A=ea and ea['netsync'], B=eb and eb['netsync'], bad=bad,
               lineup=ea and eb and ea['lineup'] == eb['lineup'] and bool(ea['lineup']), banter=ea and eb and ea['banter'] == eb['banter'],
               result=ea and eb and ea['result'] == eb['result'] and bool(ea['result']))
    return res


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', required=True)
    ap.add_argument('--targets', default='desktop,deck,laptop,rp6')
    ap.add_argument('--mode', choices=sorted(d.KOLIST), default='team2')
    ap.add_argument('--out', default=None)
    ap.add_argument('--secs', type=float, default=110)
    ap.add_argument('--min-frames', type=int, default=600)
    ap.add_argument('--save-b', default=d.DECK_SAVE)
    ap.add_argument('--env', default='')
    ap.add_argument('--env-a', default='')
    ap.add_argument('--env-b', default='')
    ap.add_argument('--neg', action='store_true')
    ap.add_argument('--neg-kind', choices=('auto', 'perturb'), default='auto', help='auto = PSPRECOMP_DMATH_OLD=1 when the binary has it (the old C-library maths: differs only where the libms differ), perturb = PSPRECOMP_NET_PERTURB=600 (always differs)')
    ap.add_argument('--probe', action='store_true')
    a = ap.parse_args(argv)
    work = os.path.abspath(os.path.expanduser(a.work))
    out0 = os.path.abspath(os.path.expanduser(a.out or '~/dissidia/tt/s2/xdev/auto'))
    names = [x for x in a.targets.split(',') if x in TARGETS]
    ready = []
    for n in names:
        ok, why = probe(n)
        print('xdev: %-8s %s: %s' % (n, 'READY' if ok else 'SKIP', why), flush=True)
        if ok:
            ready.append(n)
    if a.probe:
        return 0
    if not ready:
        print('xdev: SKIP: no other device reachable and idle')
        return 0
    if not os.path.isdir(os.path.join(os.path.expanduser(a.save_b), 'PSP')):
        print('xdev: SKIP/ERROR: the test save %s is missing' % a.save_b)
        return 2
    xl = open('/tmp/xdev_run.lock', 'w')   # one xdev run at a time on this host: two runs share the devices' dirs / exe / relay ports and corrupt each other
    try:
        fcntl.flock(xl, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        print('xdev: SKIP: another xdev run is in progress on this host')
        return 0
    slot, lock = d.claim_slot(None)
    relay_port = 47900 + slot
    os.makedirs(out0, exist_ok=True)
    relay = subprocess.Popen([d.build_relay(), '--port', str(relay_port), '--bind', '0.0.0.0'], stdout=open(os.path.join(out0, 'relay.log'), 'w'), stderr=subprocess.STDOUT, start_new_session=True)
    time.sleep(0.5)
    results = []
    try:
        for n in ready:
            out = os.path.join(out0, n + ('_neg' if a.neg else ''))
            os.makedirs(out, exist_ok=True)
            r = run_pair(a, n, out, work, slot, relay_port)
            results.append(r)
            line = 'xdev: host<->%-8s %s  NETSYNC A %s B %s (frames compared, mismatched)  line-up %s banter %s result %s' % (
                n, r['verdict'], r['A'], r['B'], 'equal' if r['lineup'] else 'DIFF/none', 'equal' if r['banter'] else 'DIFF', 'equal' if r['result'] else 'DIFF/none')
            print(line, flush=True)
            for b in r['bad'][:8]:
                print('   ' + b)
            open(os.path.join(out, 'report.txt'), 'w').write(line + '\n' + '\n'.join(r['bad']) + '\n')
    finally:
        try:
            os.killpg(relay.pid, signal.SIGTERM)
        except OSError:
            pass
        lock.close()
    json.dump(results, open(os.path.join(out0, 'results.json'), 'w'), indent=1, default=list)
    return 1 if any(r['verdict'].startswith('FAIL') for r in results) else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

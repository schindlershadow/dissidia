#!/usr/bin/env python3
"""Run tagteam_test.py checks on another machine over ssh (the desktop), headless, and copy each check's directory back.

Used by tagteam_test.py --remote HOST. Everything lives in ~/dissidia/reg_remote/ on the remote (never the user's own work folders, saves, settings or a
running game): tools + replay scripts + relay source (rsync of this tree), a work folder whose data files are symlinks to the remote's ~/dissidia/work (the
generated game data) with its own ms0 (a copy of the local test save) and the runtime copied from the local work folder's build (the SAME binary the local
checks run), the Deck save in saves/deck/ms0, and one out/<run id>/ per run. No cloud.ini / cloud/ anywhere. The games render with SDL's offscreen video
driver on the remote's Mesa GPU (no X server needed: the desktop has no Xvfb), audio is the dummy driver, the process is named regrt.

The remote is used only when idle (probe()): no psprecomp_runtime / play_* / run.sh of the user running, load low, ssh works. Otherwise the caller runs
everything locally."""
import os
import re
import shlex
import subprocess
import time

BASE = 'dissidia/reg_remote'   # relative to the remote's home
SSH = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=4']
PROBE = r'''
cores=$(nproc); load=$(cut -d' ' -f1 /proc/loadavg)
busy=""
for p in /proc/[0-9]*; do
  set -f
  pid=${p#/proc/}; exe=$(readlink $p/exe 2>/dev/null); args=$(tr '\0' ' ' < $p/cmdline 2>/dev/null)
  case "$args" in *reg_remote*) continue;; esac           # (our own earlier run)
  case "$exe" in */psprecomp_runtime) busy="$busy runtime:$pid";; esac
  set -- $args; case "$1 $2" in *play_*.sh*|*tools/run.sh*) busy="$busy $(echo $1_$2 | cut -c1-60)";; esac
done
echo "cores=$cores load=$load busy=[$busy]"
'''


class Remote:
    def __init__(self, host, log=print):
        self.host, self.log, self.ok, self.run_id = host, log, False, time.strftime('%H%M%S') + '_%d' % os.getpid()
        self.cores = 0

    def ssh(self, cmd, **kw):
        return subprocess.run(SSH + [self.host, cmd], capture_output=True, text=True, **kw)

    def probe(self, max_load_frac=0.5):
        """(ok, reason): the remote is reachable and idle."""
        try:
            r = self.ssh('bash -s', input=PROBE, timeout=40)
        except subprocess.TimeoutExpired:
            return False, 'ssh probe timed out'
        if r.returncode:
            return False, 'ssh failed (rc %d): %s' % (r.returncode, r.stderr.strip()[:100])
        m = re.search(r'cores=(\d+) load=([\d.]+) busy=\[(.*)\]', r.stdout)
        if not m:
            return False, 'probe gave no answer: ' + r.stdout[:100]
        self.cores = int(m[1])
        if m[3].strip():
            return False, 'the user\'s game or another runtime is running there (%s)' % m[3].strip()
        if float(m[2]) > self.cores * max_load_frac:
            return False, 'load %s on %d cores' % (m[2], self.cores)
        return True, 'idle (load %s on %d cores)' % (m[2], self.cores)

    def busy_now(self):
        """'' while the desktop stays free of the user's game (checked every few seconds while checks run there), else the reason (also when ssh fails: the checks then
        go local). Load is not looked at: our own checks make it."""
        try:
            r = self.ssh('bash -s', input=PROBE, timeout=25)
        except subprocess.TimeoutExpired:
            return 'ssh probe timed out'
        m = re.search(r'busy=\[(.*)\]', r.stdout)
        if r.returncode or not m:
            return 'ssh failed (rc %d)' % r.returncode
        return m[1].strip()

    def prepare(self, work, tools_dir, save_b):
        """Sync the tools / runtime / saves. work = the local built work folder (its build/psprecomp_runtime is the binary, its ms0 copy A's save)."""
        tools_dir = os.path.abspath(tools_dir)
        root = os.path.abspath(os.path.join(tools_dir, '..', '..', '..'))   # .../psprecomp
        rt = os.path.realpath(os.path.join(work, 'build', 'psprecomp_runtime'))
        with open(rt, 'rb') as f:   # a mkx.sh work folder: build/psprecomp_runtime is a wrapper script `exec -a NAME .../NAME`, the binary is next to it
            if f.read(2) == b'#!':
                mm = re.search(r'exec -a (\S+)', open(rt, errors='replace').read())
                rt = os.path.join(os.path.dirname(rt), mm[1]) if mm else rt
        ms0 = os.path.realpath(os.path.join(work, 'ms0'))
        h, B = self.host, BASE
        steps = [
            'mkdir -p %s/psprecomp/games/dissidia/tests %s/psprecomp/tools/adhoc_relay %s/work/build %s/saves/deck %s/out' % ((B,) * 5),
        ]
        r = self.ssh(' && '.join(steps))
        if r.returncode:
            return 'mkdir: ' + r.stderr[:200]
        rs = lambda *a: subprocess.run(['rsync', '-a', '-e', ' '.join(SSH)] + list(a), capture_output=True, text=True)
        for src, dst in ((tools_dir + '/', '%s/psprecomp/games/dissidia/tools/' % B), (os.path.join(root, 'games/dissidia/tests/replays') + '/', '%s/psprecomp/games/dissidia/tests/replays/' % B),
                         (os.path.join(root, 'tools/adhoc_relay/adhoc_relay.cpp'), '%s/psprecomp/tools/adhoc_relay/' % B), (rt, '%s/work/build/regrt' % B),
                         (ms0 + '/', '%s/work/ms0/' % B), (os.path.abspath(save_b) + '/', '%s/saves/deck/ms0/' % B)):
            ex = ['--exclude', '__pycache__', '--delete'] if src.endswith('/') else []
            r = rs(*ex, src, '%s:%s' % (h, dst))
            if r.returncode:
                return 'rsync %s: %s' % (src, r.stderr[:200])
        setup = r'''cd ~/%s/work && for f in ~/dissidia/work/*; do n=$(basename $f); case $n in build|build_*|ms0|cloud*|mods|last_run.log|frame.tga|settings.ini|play.sh) continue;; esac; ln -sfn $f $n; done
printf '#!/bin/bash\nexec -a regrt "$(dirname "$0")/regrt" "$@"\n' > build/psprecomp_runtime; chmod +x build/psprecomp_runtime build/regrt
rm -rf cloud cloud.ini ms0/cloud.ini; test ! -e cloud.ini && test ! -e cloud && echo SETUP_OK''' % B
        r = self.ssh(setup)
        if 'SETUP_OK' not in r.stdout:
            return 'setup: ' + (r.stdout + r.stderr)[-200:]
        return ''

    def command(self, name, args):
        """The remote shell command running one check (a child of the parallel run: result in the exit status, report on stdout)."""
        # nice: the desktop is the user's; SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT: the test games must not open the user's real pad (its input would steer them)
        env = 'nice -n 10 env -u DISPLAY SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0001/0x0001 BT_GPU=1 BT_EGL_VENDOR=/usr/share/glvnd/egl_vendor.d/50_mesa.json SDL_AUDIODRIVER=dummy PSPRECOMP_CLOUD=0 TT_CHILD=1'
        out = '$HOME/%s/out/%s' % (BASE, self.run_id)   # (absolute: every command line of the run carries it, kill() finds them by it)
        return 'cd ~/%s && mkdir -p %s && %s python3 psprecomp/games/dissidia/tools/tagteam_test.py --work work --out %s --only %s %s --save-b saves/deck/ms0' % (
            BASE, out, env, out, name, ' '.join(shlex.quote(x) for x in args))

    def start(self, name, args, logfile):
        return subprocess.Popen(SSH + [self.host, self.command(name, args)], stdout=logfile, stderr=subprocess.STDOUT, start_new_session=True)

    def fetch(self, out):
        """Copy the run's check dirs (<name>, <name>_retry) into the local out dir, then remove them from the remote."""
        src = '%s:%s/out/%s/' % (self.host, BASE, self.run_id)
        r = subprocess.run(['rsync', '-a', '-e', ' '.join(SSH), src, out + '/'], capture_output=True, text=True)
        return r.returncode == 0

    def kill(self):
        """Stop everything of this run on the remote (games, python, relay): the run id is in every command line / log path."""
        pat = 'reg_remote/out/%s' % self.run_id
        self.ssh("pkill -f '[%s]%s'; sleep 2; pkill -9 -f '[%s]%s'; true" % (pat[0], pat[1:], pat[0], pat[1:]), timeout=30)

    def cleanup(self):
        self.kill()
        self.ssh('rm -rf ~/%s/out/%s; rm -f /tmp/bt_x_* 2>/dev/null; true' % (BASE, self.run_id), timeout=60)

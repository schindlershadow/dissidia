#!/usr/bin/env python3
"""Runs ON the Steam Deck: once a second, per-thread CPU % of the game, GPU busy %, GPU/CPU clocks, power. Output is one line per second.
  deck_sampler.py OUTFILE   (stops when the game exits or on SIGTERM)"""
import glob, os, sys, time

def pid():
    for p in os.listdir('/proc'):
        if p.isdigit():
            try:
                if open('/proc/%s/comm' % p).read().strip().startswith('psprecomp_runti'):
                    return int(p)
            except OSError:
                pass

def threads(p):
    out = {}
    for t in glob.glob('/proc/%d/task/*' % p):
        try:
            s = open(t + '/stat').read()
            name = s[s.index('(') + 1:s.rindex(')')]
            f = s[s.rindex(')') + 2:].split()
            out[int(os.path.basename(t))] = (name, int(f[11]) + int(f[12]))
        except (OSError, ValueError):
            pass
    return out

def rd(path, default=''):
    try:
        return open(path).read().strip()
    except OSError:
        return default

def sclk():
    for l in rd('/sys/class/drm/card0/device/pp_dpm_sclk').splitlines():
        if l.endswith('*'):
            return l.split()[1]
    return '?'

def hwmon(name, f):
    for h in glob.glob('/sys/class/hwmon/hwmon*'):
        if rd(h + '/name') == name:
            return rd(h + '/' + f)
    return ''

def metrics():
    # amdgpu gpu_metrics v2.x (APU): socket/cpu/gfx power (mW), average clocks, throttle bits
    import struct
    try:
        b = open('/sys/class/drm/card0/device/gpu_metrics', 'rb').read()
    except OSError:
        return ''
    if len(b) < 112 or b[2] != 2:
        return 'gm(rev %d/%d size %d)' % (b[2], b[3], len(b)) if len(b) > 3 else ''
    sock, cpu, soc, gfx = struct.unpack_from('<4H', b, 40)
    core_p = struct.unpack_from('<8H', b, 48)
    gclk, = struct.unpack_from('<H', b, 76)
    ccl = struct.unpack_from('<8H', b, 88)
    thr, = struct.unpack_from('<I', b, 108)
    return 'sock=%dmW cpu=%dmW gfx=%dmW gfxclk=%d cclk=%s throttle=0x%x' % (sock, cpu, gfx, gclk, '/'.join(map(str, ccl)), thr)

def main():
    out = open(sys.argv[1], 'w', buffering=1)
    p = None
    while p is None:
        p = pid(); time.sleep(0.3)
    hz = os.sysconf('SC_CLK_TCK')
    prev, t0 = threads(p), time.time()
    while os.path.exists('/proc/%d' % p):
        time.sleep(1.0)
        now = time.time()
        cur = threads(p)
        dt = now - t0
        rows = sorted(((cur[t][1] - prev[t][1]) / hz / dt * 100, cur[t][0], t) for t in cur if t in prev)[::-1]
        prev, t0 = cur, now
        freqs = [int(float(l.split(':')[1])) for l in rd('/proc/cpuinfo').splitlines() if l.startswith('cpu MHz')]
        pw = hwmon('amdgpu', 'power1_average')
        gf = hwmon('amdgpu', 'freq1_input')
        out.write('t=%.0f gpu_busy=%s%% sclk=%sMHz cpu_mhz=%s total_cpu=%.0f%% ppt=%sW | %s' % (
            now, rd('/sys/class/drm/card0/device/gpu_busy_percent'), (int(gf) // 1000000 if gf.isdigit() else sclk()), '/'.join(map(str, freqs)),
            sum(r[0] for r in rows), (int(pw) // 1000000) if pw.isdigit() else '?',
            ' '.join('%d:%.0f' % (t, c) for c, n, t in rows[:8] if c >= 3)) + ' ' + metrics() + '\n')
        sys.stdout.flush()

main()

#!/usr/bin/env python3
"""pull_log.py: fetch the game's log files through its debug socket (TCP 9999), no adb / ssh needed.

  pull_log.py HOST[:PORT]                  last_run.log -> ~/dissidia/tt/desk_logs/HOST_DATE/
  pull_log.py HOST --prev                  prev_run.log instead
  pull_log.py HOST --tail 5000000          only the last ~5 MB of it (cut at a line start)
  pull_log.py HOST --all                   every log the game keeps (last_run, prev_run, crash*, hang*, netdump*, ...)
  pull_log.py HOST --list                  list the files (name, size, age) and exit
  pull_log.py HOST -o out.log              output file (one file) or directory (with --all)
  pull_log.py --scan 192.168.2            who on that /24 answers on the debug port (needs a dev build / PSPRECOMP_DEBUG_BIND=lan)
  pull_log.py --scan 192.168.2.0/24 --pull   ... and pull last_run.log from each

Read-only: the runtime serves only log-like files of its own folder (LOGLS / LOGGET / LOGGZ in psprecomp/runtime/src/psp_debug_socket.cpp).
A log that is still growing is fetched up to its size at the last chunk. The transfer is gzip-compressed per chunk unless --raw.
"""
import argparse, datetime, ipaddress, json, os, socket, sys, threading, time, zlib

CHUNK = 4 << 20


class Dbg:
    def __init__(self, host, port, timeout=15):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.f = self.s.makefile('rb')

    def cmd(self, line):
        self.s.sendall(line.encode() + b'\n')
        hdr = self.f.readline().decode().strip()
        if not hdr:
            raise IOError('connection closed')
        parts = hdr.split()
        if parts[0] != 'OK':
            raise IOError(hdr)
        n = int(parts[1])
        data = self.f.read(n)
        if len(data) != n:
            raise IOError('short read')
        return data, [int(x) for x in parts[2:]]

    def ls(self):
        return json.loads(self.cmd('LOGLS')[0])

    def get(self, name, off, length, gz=True):
        data, extra = self.cmd('%s %s %d %d' % ('LOGGZ' if gz else 'LOGGET', name, off, length))
        return zlib.decompress(data, 31) if gz and data else data


def fetch(d, name, size, dest, tail=0, gz=True, quiet=False):
    start = max(0, size - tail) if tail else 0
    t0, got, off = time.time(), 0, start
    cut_line = start > 0
    with open(dest, 'wb') as out:
        while True:
            blk = d.get(name, off, CHUNK, gz)
            if not blk:
                break
            if cut_line:   # drop the partial line the tail started in
                nl = blk.find(b'\n')
                blk = blk[nl + 1:] if nl >= 0 else b''
                cut_line = False
            out.write(blk)
            off += CHUNK if len(blk) else 0
            got += len(blk)
            if not quiet:
                print('\r  %s: %.1f MB (%.1f MB/s)' % (name, got / 1e6, got / 1e6 / max(time.time() - t0, 1e-3)), end='', file=sys.stderr)
            if len(blk) < CHUNK and off - start >= size - start:
                # at or past the size seen in LOGLS; a growing log may have more: loop until an empty read
                pass
    if not quiet:
        print('', file=sys.stderr)
    return got


def split_host(h):
    if h.count(':') == 1:
        a, p = h.split(':')
        return a, int(p)
    return h, 9999


def scan(net, port, pull, args):
    if '/' not in net:
        net = (net.rstrip('.') + '.0/24') if net.count('.') == 2 else net + '/32'
    ips = [str(i) for i in ipaddress.ip_network(net, strict=False).hosts()]
    found, lock = [], threading.Lock()

    def probe(ip):
        try:
            s = socket.create_connection((ip, port), timeout=0.4)
            s.settimeout(2)
            s.sendall(b'I\n')
            f = s.makefile('rb')
            hdr = f.readline().decode().split()
            body = f.read(int(hdr[1])).decode(errors='replace') if hdr and hdr[0] == 'OK' else ''
            s.close()
            with lock:
                found.append((ip, body))
        except Exception:
            pass

    ts = [threading.Thread(target=probe, args=(ip,)) for ip in ips]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    found.sort(key=lambda x: tuple(int(p) for p in x[0].split('.')))
    for ip, body in found:
        try:
            j = json.loads(body)
            info = 'uptime %.0fs frames %s' % (j.get('uptime_s', j.get('uptime', 0)), j.get('frames', j.get('ge', {}).get('frames', '?')))
        except Exception:
            info = body[:80].replace('\n', ' ')
        print('%s:%d  %s' % (ip, port, info))
        if pull:
            pull_host(ip, port, args)
    if not found:
        print('nothing answers on %s:%d (the game must be running a dev build, or have PSPRECOMP_DEBUG_BIND=lan)' % (net, port))
    return 0 if found else 1


def pull_host(host, port, args):
    d = Dbg(host, port)
    info = d.ls()
    files = {f['name']: f for f in info['files']}
    if args.list:
        now = time.time()
        print('%s (%s)' % (info['dir'], host))
        for n, f in sorted(files.items()):
            print('  %-32s %12d bytes  %s' % (n, f['size'], time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(f['mtime']))))
        return 0
    if args.all:
        want = sorted(files)
    else:
        want = ['prev_run.log' if args.prev else 'last_run.log']
    for n in want:
        if n not in files:
            print('%s: no %s there (have: %s)' % (host, n, ', '.join(sorted(files)) or 'nothing'), file=sys.stderr)
            return 1
    day = datetime.date.today().strftime('%Y%m%d')
    outdir = os.path.expanduser('~/dissidia/tt/desk_logs/%s_%s' % (host.replace(':', '_'), day))
    if args.o and (args.all or len(want) != 1):
        outdir = args.o
    for n in want:
        if args.o and not args.all:
            dest = args.o
            if os.path.isdir(dest):
                dest = os.path.join(dest, n)
        else:
            dest = os.path.join(outdir, n.replace('/', '_'))
        os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)
        got = fetch(d, n, files[n]['size'], dest, args.tail, not args.raw, args.quiet)
        print('%s -> %s (%d bytes)' % (n, dest, got))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('host', nargs='?', help='HOST or HOST:PORT (default port 9999)')
    ap.add_argument('--prev', action='store_true', help='prev_run.log')
    ap.add_argument('--tail', type=int, default=0, metavar='N', help='only the last N bytes')
    ap.add_argument('-o', metavar='PATH', help='output file (one log) or directory (--all)')
    ap.add_argument('--all', action='store_true', help='every log file the game lists')
    ap.add_argument('--list', action='store_true', help='list the files and exit')
    ap.add_argument('--raw', action='store_true', help='no gzip on the wire')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--scan', metavar='NET', help='probe a /24 (192.168.2 or 192.168.2.0/24) for the debug port')
    ap.add_argument('--pull', action='store_true', help='with --scan: pull from every responder')
    ap.add_argument('--port', type=int, default=9999, help='port for --scan')
    a = ap.parse_args()
    if a.scan:
        return scan(a.scan, a.port, a.pull, a)
    if not a.host:
        ap.error('host (or --scan) required')
    host, port = split_host(a.host)
    try:
        return pull_host(host, port, a)
    except (OSError, IOError) as e:
        print('%s:%d: %s' % (host, port, e), file=sys.stderr)
        print('(debug socket must listen on the LAN: Android dev builds do; desktop and releases need PSPRECOMP_DEBUG_BIND=lan in the environment (Android: a line in env.txt); same Wi-Fi', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Tests for adhoc_relay.

  python3 test_relay.py [host] [port]       protocol test against a running relay
  python3 test_relay.py --spawn ./adhoc_relay   starts its own relay on a free port with a temporary log dir, runs the protocol
                                            test, then a ~40 s scenario with two simulated clients (lobby, introductions, relayed
                                            then silent unicast, battle traffic, garbage packets) and checks the JSON log lines and
                                            the output of adhoc_relay_stats (text, --json, --abuse)
"""
import json, os, signal, socket, struct, subprocess, sys, tempfile, time

args = sys.argv[1:]
SPAWN = None
if "--spawn" in args:
    i = args.index("--spawn")
    SPAWN = os.path.abspath(args[i + 1])
    del args[i:i + 2]
HERE = os.path.dirname(os.path.abspath(__file__))
LOGDIR = None
PROC = None
if SPAWN:
    LOGDIR = tempfile.mkdtemp(prefix="relaylog_") + "/logs"
    _s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); _s.bind(("127.0.0.1", 0)); _p = _s.getsockname()[1]; _s.close()
    PROC = subprocess.Popen([SPAWN, "--port", str(_p), "--bind", "127.0.0.1", "--log-dir", LOGDIR, "--rate-secs", "2"], stdout=subprocess.DEVNULL)
    time.sleep(0.5)
    HOST, PORT = "127.0.0.1", _p
else:
    HOST = args[0] if len(args) > 0 else "127.0.0.1"
    PORT = int(args[1]) if len(args) > 1 else 27500
T_BEACON, T_PDP, T_MATCH, T_PTP, T_RELAY, T_FRAG = 1, 2, 3, 4, 5, 6
BCAST = b"\xff" * 6

def pkt(ptype, mac, body):
    return b"DSAH" + bytes([ptype]) + mac + struct.pack("<H", len(body)) + body

class Client:
    cookie_cache = 0
    def __init__(self, mac, room):
        self.mac, self.room = mac, room
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.settimeout(0.3)
        self.srv = (HOST, PORT)
    def hello(self, cookie=0):
        body = bytes([1]) + self.room.encode().ljust(32, b"\0") + struct.pack("<Q", cookie)
        self.s.sendto(pkt(T_RELAY, self.mac, body), self.srv)
    def recv(self):
        try:
            d, _ = self.s.recvfrom(65536)
            return d
        except socket.timeout:
            return None
    def register(self):
        self.hello(0)
        d = self.recv()
        assert d and d[4] == T_RELAY and d[HDR] == 2, "expected CHALLENGE, got %r" % d
        cookie = struct.unpack("<Q", d[HDR + 1:HDR + 9])[0]
        self.hello(cookie)
        d = self.recv()
        assert d and d[4] == T_RELAY and d[HDR] == 3, "expected OK, got %r" % d
        return struct.unpack("<H", d[HDR + 1:HDR + 3])[0]
    def send(self, ptype, body):
        self.s.sendto(pkt(ptype, self.mac, body), self.srv)

HDR = 13
fails = 0
def check(name, cond):
    global fails
    print(("ok   " if cond else "FAIL ") + name)
    if not cond: fails += 1

ma, mb, mc, md = (bytes([2, 0, 0, 0, 0, i]) for i in (1, 2, 3, 4))
room = "t%d" % int(time.time())
a, b, c = Client(ma, room), Client(mb, room), Client(mc, room + "x")
check("A registers (1 in room)", a.register() == 1)
check("B registers (2 in room)", b.register() == 2)
check("C registers in another room", c.register() == 1)

a.send(T_BEACON, b"beacon-A")
d = b.recv(); check("beacon from A reaches B", d is not None and d[4] == T_BEACON and d.endswith(b"beacon-A"))
check("beacon not seen by A itself", a.recv() is None)
check("beacon not seen in the other room", c.recv() is None)

a.send(T_MATCH, mb + b"hello-B")
d = b.recv(); check("directed packet reaches B", d is not None and d[4] == T_MATCH and d.endswith(b"hello-B"))
check("directed packet not seen by C", c.recv() is None)

a.send(T_FRAG, mb + struct.pack('<IBBB', 7, 0, 2, T_MATCH) + b"fragment-0")
d = b.recv(); check("fragment is routed by destination MAC", d is not None and d[4] == T_FRAG and d.endswith(b"fragment-0"))
check("fragment not seen by C", c.recv() is None)

a.send(T_PDP, BCAST + b"to-all")
d = b.recv(); check("broadcast reaches B", d is not None and d.endswith(b"to-all"))
check("broadcast not seen by A", a.recv() is None)

a.send(T_PTP, mc + b"cross-room")
check("packet to a MAC in another room is dropped", c.recv() is None)

# Spoofed MAC from B's socket claiming to be A
spoof = pkt(T_MATCH, ma, mb + b"spoof")
b.s.sendto(spoof, b.srv)
check("spoofed source MAC is dropped", b.recv() is None)

# Unregistered sender is ignored
x = Client(md, room)
x.send(T_BEACON, b"nope")
check("unregistered sender is ignored", b.recv() is None)

# Bad cookie gets a fresh challenge, not registration
y = Client(md, room)
y.hello(0x1234567890abcdef)
d = y.recv(); check("wrong cookie is challenged again", d is not None and d[HDR] == 2)

# Keep-alive keeps the count, a second hello from A does not duplicate it
check("A keep-alive", a.register() == 2)

# Bye removes the player
a.s.sendto(pkt(T_RELAY, ma, bytes([4])), a.srv)
time.sleep(0.2)
b2 = Client(mb, room)
check("after A's bye, the room has one player", b2.register() == 1)

def scenario():
    """Two simulated players for ~40 s; returns nothing, the log is checked afterwards."""
    room = "lg"
    mp, mq = bytes([2, 9, 0, 0, 0, 1]), bytes([2, 9, 0, 0, 0, 2])
    P, Q = Client(mp, room), Client(mq, room)
    P.s.settimeout(0.005); Q.s.settimeout(0.005)
    for c in (P, Q):
        c.s.settimeout(0.3); c.register(); c.s.settimeout(0.005)

    def beacon(c, nick):
        b = nick.encode().ljust(32, b"\0") + b"GRP".ljust(8, b"\0") + bytes([1, 30]) + bytes([0]) + b"GD" + bytes([4]) + b"\x01\x02\x03\x04"
        c.send(T_BEACON, b)

    def pump(c):   # answer a CHALLENGE (the relay's RTT probe) at once, like the game
        while True:
            d = c.recv()
            if d is None: return
            if d[4] == T_RELAY and d[HDR] == 2:
                c.hello(struct.unpack("<Q", d[HDR + 1:HDR + 9])[0])

    def intro(c, target):
        c.send(T_RELAY, bytes([5]) + target + bytes([192, 168, 1, 7]) + struct.pack("<H", 40000 + c.mac[5]))

    ping = lambda c, to: c.send(T_PDP, to + struct.pack("<HH", 0xD1D2, 0xD1D2) + bytes([0]) + b"\0" * 12)
    game = lambda c, to: c.send(T_PDP, to + struct.pack("<HH", 0xFFF0, 0xFFF0) + b"g" * 60)
    t0 = time.time(); last_b = last_h = -9; step = 0
    n = 0
    while True:
        t = time.time() - t0
        if t > 36: break
        if t - last_b >= 1 and t < 31:   # beacons stop at 31 s: the match ends
            last_b = t; beacon(P, "Alice"); beacon(Q, "Bob")
        if t - last_h >= 1:
            last_h = t
            if t < 31: P.hello(P.cookie_cache); Q.hello(Q.cookie_cache)
        pump(P); pump(Q)
        if t > 0.5 and step == 0:
            step = 1
            # build ids: 'TV' on the roster channel from P, a manifest with the 'V' trailer from Q
            P.send(T_PDP, mq + struct.pack("<HH", 0xFF03, 0xFF03) + b"TV" + bytes([1, 7]) + b"abc1234")
            Q.send(T_PDP, mp + struct.pack("<HH", 0xD1D1, 0xD1D1) + b"M\x01\x00\x00V" + struct.pack("<H", 1) + bytes([7]) + b"def5678")
            intro(P, mq); intro(Q, mp)
            # garbage: not DSAH, truncated, an unregistered sender, a spoofed MAC, an unknown relay op, an unknown packet type
            g = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            for junk in (b"hello world", b"DSAH\x02", pkt(T_BEACON, md, b"x" * 20), pkt(T_RELAY, mp, bytes([99])), pkt(99, mp, b"zz")):
                g.sendto(junk, (HOST, PORT))
            Q.s.sendto(pkt(T_MATCH, mp, mq + b"spoof"), (HOST, PORT))
        if step == 1 and t > 2: step = 2; intro(P, mq); intro(Q, mp)
        # phase A (to 8 s): relayed unicast pings; B (8-16 s): silence = direct; C (16-20 s): battle traffic; then silence
        n += 1
        if t < 8 and n % 5 == 0: ping(P, mq); ping(Q, mp)
        if 16 < t < 20: game(P, mq); game(Q, mp)
        time.sleep(0.04)
    for c in (P, Q):
        c.s.sendto(pkt(T_RELAY, c.mac, bytes([4])), c.srv)

# remember the cookie the client got so scenario keep-alives work
_reg = Client.register
def _register(self):
    n = _reg(self)
    self.cookie_cache = self.last_cookie
    return n
Client.register = _register
_recv = Client.recv
def _recv2(self):
    d = _recv(self)
    if d and d[4] == T_RELAY and d[HDR] == 2: self.last_cookie = struct.unpack("<Q", d[HDR + 1:HDR + 9])[0]
    return d
Client.recv = _recv2

if SPAWN:
    # the cookie is in the first CHALLENGE; re-run the registration steps through the patched methods
    scenario_ok = True
    scenario()
    time.sleep(1.5)
    PROC.send_signal(signal.SIGTERM); PROC.wait(5)
    check("log dir is private (0700)", (os.stat(LOGDIR).st_mode & 0o777) == 0o700)
    files = sorted(f for f in os.listdir(LOGDIR) if f.startswith("relay-") and f.endswith(".jsonl"))
    check("one day file, 0600", len(files) == 1 and (os.stat(os.path.join(LOGDIR, files[0])).st_mode & 0o777) == 0o600)
    ev = [json.loads(l) for f in files for l in open(os.path.join(LOGDIR, f))]
    kinds = {}
    for e in ev: kinds.setdefault(e["ev"], []).append(e)
    has = lambda k, **kw: any(all(e.get(a) == b for a, b in kw.items()) for e in kinds.get(k, []))
    check("start and stop events", "start" in kinds and "stop" in kinds)
    check("joins for both players with IP:port and nat class", len([e for e in kinds.get("join", []) if e["room"] == "lg"]) == 2 and all(e.get("ip") == "127.0.0.1" and e.get("port") and e.get("nat") == "loopback" for e in kinds["join"]))
    check("ident carries nickname, group and game data", has("ident", nick="Alice", group="GRP", game_data="01020304") and has("ident", nick="Bob"))
    check("build ids sniffed (roster TV and manifest V)", has("build", build="abc1234", proto=1) and has("build", build="def5678", proto=1))
    check("client event: relay version 2 + port mapping", has("client", client_v=2, mapping="port_remapped"))
    check("intro logged with both IPs", any(e.get("from_ip", "").startswith("127.0.0.1:") and e.get("to_ip", "").startswith("127.0.0.1:") for e in kinds.get("intro", [])))
    check("match_start for the group", any(e["room"] == "lg" and e["group"] == "GRP" and e["n"] == 2 for e in kinds.get("match_start", [])))
    check("path: relay while pings are relayed", has("path", state="relay", reason="no_hole"))
    check("path: direct once relayed unicast stops", has("path", state="direct", reason="silent_on_relay"))
    check("battle start and end from relayed game traffic", "battle_start" in kinds and len(kinds.get("battle_end", [])) == 1 and kinds["battle_end"][0]["relayed_game_pkts"] >= 20)
    me = kinds.get("match_end", [])
    check("match_end: duration, players, bytes, pairs", len(me) == 1 and me[0]["dur_s"] > 20 and len(me[0]["players"]) == 2 and me[0]["relayed_bytes"] > 1000 and len(me[0]["pairs"]) == 1 and me[0]["peak_bps"] > 0 and me[0]["battles"] == 1)
    rates = [e for e in kinds.get("rate", []) if e["room"] == "lg"]
    check("rate events with relayed bytes/packets/peaks", rates and sum(e["pkts"] for e in rates) > 100 and max(e["peak_pps"] for e in rates) > 0 and max(e["peak_bps"] for e in rates) > 0)
    rtts = [e for e in rates if e.get("rtt_n")]
    check("RTT measured by the relay (probe answered)", rtts and all(0 <= e["rtt_avg"] < 500 for e in rtts))
    errk = {e["kind"] for e in kinds.get("error", [])}
    check("errors: malformed, unregistered, spoofed, bad op, bad type", {"malformed", "unregistered", "spoofed_mac", "bad_op", "bad_type"} <= errk)
    check("leave events carry totals and client version", len([e for e in kinds.get("leave", []) if e["room"] == "lg"]) == 2 and all(e["rx_pkts"] > 0 and e["client_v"] == 2 and e["tx_bytes"] > 0 for e in kinds["leave"] if e["room"] == "lg"))
    stats = os.path.join(HERE, "adhoc_relay_stats")
    out = subprocess.run([sys.executable, stats, "--dir", LOGDIR, "--json"], capture_output=True, text=True)
    R = json.loads(out.stdout) if out.returncode == 0 else {}
    check("stats --json parses", bool(R))
    check("stats: matches, players, builds", R.get("matches", {}).get("count") == 1 and R.get("unique_players", 0) >= 2 and {b["build"] for b in R.get("builds", [])} == {"abc1234", "def5678"})
    check("stats: direct share + reasons", R.get("paths", {}).get("pairs") == 1 and "direct_share" in R.get("paths", {}))
    check("stats: ping, bandwidth, rooms", R.get("rtt", {}).get("samples", 0) > 0 and R.get("bandwidth", {}).get("rx_bytes", 0) > 0 and any(r.get("room") == "lg" for r in R.get("top_rooms", [])))
    txt = subprocess.run([sys.executable, stats, "--dir", LOGDIR], capture_output=True, text=True)
    check("stats text report", txt.returncode == 0 and "Matches" in txt.stdout and "Top rooms" in txt.stdout and "Builds in use" in txt.stdout)
    ab = subprocess.run([sys.executable, stats, "--dir", LOGDIR, "--abuse", "--all", "--flag-bad", "3"], capture_output=True, text=True)
    check("stats --abuse lists the IP and flags the garbage", ab.returncode == 0 and "127.0.0.1" in ab.stdout and "malformed/unregistered/spoofed" in ab.stdout)
    print(txt.stdout)
    print(ab.stdout)
    print("log:", LOGDIR)

print("PASSED" if not fails else "%d FAILED" % fails)
sys.exit(1 if fails else 0)

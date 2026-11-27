# adhoc_relay

UDP relay for the native build's ad-hoc multiplayer. Players behind NAT send all their game traffic here instead of to each other, so nobody has to forward a port. The default server is `party.schindlershadow.com:27500`. Protocol and usage: [games/dissidia/docs/multiplayer.md](../../games/dissidia/docs/multiplayer.md).

```
g++ -O2 -std=c++17 -pthread -o adhoc_relay adhoc_relay.cpp
./adhoc_relay --port 27500                 # UDP; forward this port to the machine
python3 test_relay.py 127.0.0.1 27500      # protocol test against a running relay
python3 test_relay.py --spawn ./adhoc_relay  # own relay + temp log dir: protocol test, a 40 s two-player scenario, log and stats checks
```

Options: `--bind IP`, `--max-room N` (32), `--max-clients N` (2000), `--pps N` and `--kbps N` (per-client rate limits, 6000 packets/s and 32000 kbit/s), `--log-dir DIR` / `--no-log`, `--keep-days N` (90), `--rate-secs N` (30), `--verbose`.

How it behaves:

- A client says hello with a room name (empty = the public room) and answers a cookie challenge, so a forged source address cannot register and the server cannot be used to flood a third party.
- Packets are forwarded untouched: a beacon goes to everyone else in the room, anything else to the client whose MAC is the destination (or to the whole room for the broadcast MAC). A packet whose source MAC is not the one registered for that address is dropped, and nothing crosses between rooms.
- A client that is silent for 8 s is removed. A room holds up to 32 players.
- It prints joins, leaves and a one-line summary every minute to stdout (the journal), and writes the JSON event log below. It does not log message contents, and it has no authentication or encryption: a room name is the only secret.

Run it as a service: `adhoc-relay.service` is a systemd user unit (`cp` it to `~/.config/systemd/user/`, put the binary at `~/.local/lib/adhoc_relay/adhoc_relay`, `systemctl --user enable --now adhoc-relay`, `loginctl enable-linger $USER`). Forward UDP 27500 on the router to the host. The unit sets `ReadWritePaths` to `~/.local/state/adhoc_relay` (create the directory once: `mkdir -p ~/.local/state/adhoc_relay`), because it otherwise runs read-only.

## Version 2: introductions for direct paths

A client may send op 5 INTRO (target MAC, its LAN address and port); the server answers both players of that room with op 6 PEER
(the other's MAC, public address and port as the server sees them, and the LAN address it reported), so they can open a direct
path (hole punching; `games/dissidia/docs/multiplayer.md`). The server still forwards everything else as before, and version 1
clients never send INTRO. OK carries version 2.


## Event log (27 Nov)

One JSON object per line in `--log-dir` (default `~/.local/state/adhoc_relay/`), file `relay-YYYY-MM-DD.jsonl` (UTC day, rotated at midnight), directory mode 0700, files 0600, files older than `--keep-days` (90) deleted when a day file opens. A background thread writes it (batches every 0.5 s); the packet path only formats a line and queues it (up to 200 000 lines, then they are dropped and counted in `stats.log_lost`). Every line has `ts` (ISO UTC), `t` (unix seconds) and `ev`. **It contains the raw IP:port of every client** (for abuse detection): keep the directory private.

| `ev` | fields |
|---|---|
| `start` / `stop` | bind, port, version / - |
| `join` | room, mac, ip, port, nat (`public` `private` `cgnat` `loopback` `linklocal`; `nat_range` for cgnat: 100.64.0.0/10 and the 172.56.0.0/13 mobile range), in_room, total |
| `ident` | first beacon of a client: room, mac, nick, group, fps, game_data (hex of the beacon's game trailer) |
| `build` | room, mac, nick, build, proto: the game's build id, read from the side-channel manifest (`V` trailer, port 0xD1D1) or the tag roster (`TV`, port 0xFF03) the game sends (added 26 Nov by the netdesync2 work; older builds send none) |
| `client` | first INTRO of a client: client_v (2), lan address, nat, mapping (`no_nat` `port_preserved` `port_remapped`: its LAN port against the public one) |
| `leave` | room, mac, nick, ip, port, reason (`bye` `timeout` `re-registered` `same player reconnected` `shutdown`), dur_s, rx_pkts, rx_bytes, fwd_pkts, tx_pkts, tx_bytes, dropped, game_pkts, peak_bps, client_v (1 = never sent INTRO: the relay cannot see the client's version, 2 = sent one), build, rtt_n/min/avg/max |
| `match_start` / `match_players` / `match_end` | a *match* is a group of 2 or more connected players with the same group name in one room (from their beacons). start/players: room, group, players [{mac, nick}], n. end: dur_s, max_players, players [{mac, nick, ip, rx_pkts, rx_bytes, tx_bytes, game_pkts, peak_bps, avg_bps, rtt_*}], pairs [{a, b, state, reason}], relayed_bytes, relayed_game_pkts, avg_bps, peak_bps, peak_pps, battles, battle_s |
| `battle_start` / `battle_end` | relayed battle traffic (PDP other than ping/pack transfer/roster, PTP, fragments: 10+ packets/s for 2 s starts one, 5 s without ends it). end: dur_s, relayed_bytes, relayed_game_pkts, avg_bps, peak_bps, peak_pps. A battle whose players are all on direct paths is not seen |
| `intro` | a pair introduced (one line per pair every 10 s): from, from_ip, from_nat, to, to_ip, to_nat, n |
| `path` | direct path result of a pair: state `relay` or `direct`, reason, intros, after_intro_s, after_group_s. `direct` (`silent_on_relay`) means relayed player-to-player traffic (pings, 1/s) stopped for 4 s while both still beacon; `relay` reasons: `no_hole` (introduced, still relayed after 6 s: symmetric NAT or firewall), `cgnat` (one side is behind carrier NAT), `peer_no_intro` (one side never sent INTRO), `no_intro_one_side` / `no_intro_either_side` / `no_intro` (no introduction happened: old client or direct paths off), `direct_lost` (relayed traffic came back) |
| `rate` | every `--rate-secs` per client and once more at leave: room, mac, ip, port, secs, pkts, bytes (from the client), tx_pkts, tx_bytes (to the client), dropped (rate limited), game_pkts, peak_pps, peak_bps (best second of the window), rtt_n/rtt_min/rtt_avg/rtt_max |
| `error` | every window, per IP and kind: kind (`malformed` `unregistered` `spoofed_mac` `challenge` `room_full` `server_full` `bad_room` `bad_type` `bad_op` `bad_intro`), ip, port, room, count, secs. `bad_op` / `bad_type` is what a newer client than this server sends (version mismatch) |
| `ipstat` | per IP and window when something stands out (errors, 3+ joins, 3+ rooms, 3+ re-registrations): unreg_pkts, unreg_bytes, joins, leaves, rereg, rooms, errors |
| `stats` | every minute: clients, rooms, matches, rx, fwd, dropped_total, log_lost |

RTT is measured by the relay: right after a keep-alive HELLO it sends the client an unsolicited CHALLENGE (every 5 s), and the client answers with a HELLO at once; the time to that HELLO is the round trip (it includes up to 10 ms of the client's tick; no client change is needed). Direct-path detection and battle detection are inferences from what the relay still sees; a client pair on a direct path is invisible to the relay apart from the beacons.

## adhoc_relay_stats

```
adhoc_relay_stats                      # report over everything in ~/.local/state/adhoc_relay/
adhoc_relay_stats --days 7 --json      # last 7 days, as JSON
adhoc_relay_stats --since 2026-11-01 --until 2026-11-30 --dir /path/to/logs
adhoc_relay_stats --abuse              # per-IP view: flagged IPs only (--all: every IP, --limit N)
```

The report has: players (joins, unique MACs / nicknames / IPs, network classes, leave reasons), matches (count, average / median / longest, per day with battles, players, joins, time), direct vs relay share per pair with the reasons for relay, the ping distribution (percentiles of the 30 s window averages and a histogram), bandwidth (received / sent bytes, busiest second, match and battle averages), the builds in use (and how many players sent none), top rooms, and error counts. `--abuse` groups by IP: players (MACs), joins, most joins in a minute, rooms, mean and peak packets/s, bytes, rate-limited packets, error counts, and flags (high packet rate / bandwidth, rate limiting, malformed / unregistered / spoofed packets, many joins in a minute, room hopping, many MACs, challenge flood, hitting a full room). The thresholds are options (`--flag-pps`, `--flag-kbps`, `--flag-bad`, `--flag-joins`, `--flag-rooms`, `--flag-macs`, `--flag-challenges`).

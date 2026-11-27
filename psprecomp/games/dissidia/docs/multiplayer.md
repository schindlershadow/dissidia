# Multiplayer (ad-hoc wireless over UDP)

Dissidia 012's "Communications" mode was written for two or more PSPs talking over ad-hoc WiFi. The native build
emulates that layer on top of UDP, so players on the same network, on one machine, or across the internet can play
each other. It is **experimental**: a full match has been played between two instances on one machine, and nothing
has been tested with more than two players, with real WiFi/LAN hardware between machines, or across the internet.

The code is generic runtime code (`runtime/src/hle/psp_hle_adhoc.cpp`), not Dissidia-specific. The only game-specific
part is the frame-rate rule in `runtime/framerate.cpp`.

## Playing

Open Communications mode in the menu and challenge a player as on a PSP. Nothing else needs turning on: the network
layer starts the first time the game uses it. Players need the same game version and character set (do not mix a
modded roster with an unmodded one). Packs listed below are the exception: they are sent to players who lack them.

### Internet (the default)

By default the game connects to a **party server**, `party.schindlershadow.com`, which relays the game traffic
between players. Nobody has to forward a port or know anyone's address.

- Players in the same **room** see each other in the lobby. The default is the public room, where everyone using the
  server is. To play with friends only, all of you set the same room name (any name only you know).
- The settings page is **Settings (Esc / F1 / Guide) > Network** (L / R shoulder, Q / E or Tab switch pages):

  | Row | Meaning |
  |---|---|
  | Network | **Internet** (relay server, default) or **LAN** |
  | Server | the relay: a host name or `host:port` (default `party.schindlershadow.com:27500`); Cross edits |
  | Room | blank = the public room; Cross edits |
  | Direct Peers | LAN only: extra addresses to contact directly; Cross edits |
  | Network Delay | **Auto** (default) or 1-8 battle frames; see "Input delay" below |
  | Status | `Connected, 3 online`, `No answer from the server yet`, `Cannot find ...`, `LAN: 2 nearby` |

  Editing text: type on a keyboard (Enter saves, Esc cancels), or with a controller pick a letter with left / right,
  add it with Cross, delete with Circle, and save with Start or the Done row. Changes apply at once and are saved
  in `settings.ini` (`net_mode`, `net_server`, `net_room`, `net_peers`, `net_input_delay`).
- Everything you send goes through the server, and the public room shows your player name to whoever is in it. The
  server never looks inside a game message and does not log them, but it has no accounts and no encryption: use a
  private room name for friends.

### LAN

Set Network to **LAN**. Players on the same network find each other by broadcast and show up in the lobby by name
(some routers and Wi-Fi isolation block broadcast; list the other player under Direct Peers then). No server is
involved, so it has the lowest latency.

Two copies on one machine (for testing) need LAN mode and separate work folders, because the game saves into its
folder:

```
cp -a ~/dissidia/work ~/dissidia/work2        # or symlink everything but ms0/, settings.ini
PSPRECOMP_ADHOC_MODE=lan PSPRECOMP_ADHOC_NICK=A tools/run.sh ~/dissidia/work  --window 960x544 &
PSPRECOMP_ADHOC_MODE=lan PSPRECOMP_ADHOC_NICK=B PSPRECOMP_DEBUG_PORT=10000 tools/run.sh ~/dissidia/work2 --window 960x544 &
```

The copies find each other over `127.0.0.1`: each takes the first free UDP port from 27312. Controllers only act on the
window that has focus (set `PSPRECOMP_PAD_BACKGROUND=1` to listen while unfocused). To test through a relay instead,
use `PSPRECOMP_ADHOC_MODE=internet PSPRECOMP_ADHOC_SERVER=127.0.0.1:27500`.

### Direct internet play without the server

In LAN mode a player can also be given an address directly (`PSPRECOMP_ADHOC_PEERS=friend.example.org`, or Direct
Peers in the menu). That needs UDP port 27312 reachable on both sides (forward it, or use a VPN such as ZeroTier,
Tailscale or WireGuard). Both players should list each other; a third and later player needs only one address,
because beacons carry the addresses of the peers each player hears (unknown addresses are probed for 15 s and dropped
if they do not answer).

**UPnP.** Turn on UPnP Port Forward on the Network page (LAN mode; or `PSPRECOMP_ADHOC_UPNP=1`) and the game asks the router to
forward its UDP port to this machine (`runtime/src/hle/psp_upnp.cpp`: SSDP discovery, the router's WANIPConnection or
WANPPPConnection service, AddPortMapping with a one-hour lease renewed every 30 minutes). The UPnP row then shows the public address
(`1.2.3.4:27312`) to give to friends for their Direct Peers. The forward is removed when UPnP is turned off or the game quits. Most
routers have UPnP off by default (UniFi among them); when no router answers or it refuses, the game says so and the port has to be
forwarded by hand. Only the router on the local network is contacted.

**Expect latency to matter.** The game runs a lock-step battle over reliable streams, and nothing here delays or
predicts input. It was built for a few milliseconds of WiFi latency. A relay adds the trip to the server on top of the
direct path; how much round-trip time the game tolerates has not been measured, so try a nearby partner first.
**Expect latency to matter.** Battles run on our pad link (see "Battles: our pad link"). The default network delay, Auto,
picks the delay from the measured ping at the start of every battle; a fixed 1-8 still works as before.

### Input delay

There is ONE network delay: Settings > Network > **Network Delay** (`PSPRECOMP_NET_DELAY=auto|1..8`, default Auto; saved as
`net_input_delay`, 0 = Auto; the old name `PSPRECOMP_NET_INPUT_DELAY` is still read when the new one is not set).
`runtime/src/hle/psp_netplay.cpp` (`psp_netplay_input_delay`) holds it; both online battle engines use it:

- plain battles: `comm_battle.cpp` (read again at every battle start, so a change in the menu applies to the next fight);
- tag team Communications battles: `tagteam_net.cpp` (the tag lock-step, see tag-team.md "Online: pad link and tag lock-step").

With N frames your own presses take effect N battle frames later and the other player's input has N frames longer to
arrive. The delays of both players add up; a rough guide is one frame per 70 ms of round-trip ping at 30 fps, the same
value on both sides (the two copies work with different values too, each pad is labelled by its frame). There is no
"Off" any more: 0 was the game's own exchange, which main's pad link replaces.

**Auto** (26 Nov). At every battle start `psp_netplay_pick_delay()` reads the ad-hoc pings of the slowest path
(`psp_hle_adhoc.cpp`: the smoothed round trip, its jitter as TCP's RTTVAR, and the share of pings lost since the players
joined) and takes the smallest delay whose budget (two delays = 66.7 ms per frame at the 30 Hz logic) covers round trip +
4 x jitter + 12 ms; one frame more off a LAN (round trip over 20 ms: a lost packet's words come a frame later with the next
one), and one more when 1 % or more of the pings were lost. With no ping yet it is 2. The log says what it chose:
`[NETPLAY] network delay Auto: 4 frame(s) for a round trip of 110 ms, jitter 8 ms, 0.0 % of the pings lost`.
Plain battles keep it for the whole fight. The tag lock-step starts there and then follows the link (`tagteam_net.cpp`):

- the lead (the input delay felt) grows only when the other machine reports that our words keep arriving late (two holds
  of 8 ms or more in its last 5 s; at most once per 5.5 s, so the same holds are not counted twice), up to 3 above the start;
- the retail rule that a waited frame samples the pad anyway (the lead climbed to 5 even on a LAN) is replaced: no new
  sample after a wait while every peer still holds 2 or more of our words;
- a late word is waited for up to 25 ms (resending every 8 ms) before a whole frame is given up;
- a copy that runs ahead (it keeps holding frames for a player that has plenty of its words) gives up one frame itself,
  which moves the slack to its own side;
- shortening in the middle of a fight is off (it made a jittery link worse): `PSPRECOMP_TTLOCK_SHORTEN=1`.

Measured with two copies on one PC (`PSPRECOMP_NET_LAG_MS` / `_JITTER_MS` / `_LOSS`, 60 s of moves, 30 fps, tag battle;
"over 50 ms" = delivered frames more than 50 ms apart in the whole battle, copy A / copy B; 0 mismatched frames everywhere):

| Link | Old: lead | Old: over 50 ms | Auto: lead | Auto: over 50 ms |
|---|---|---|---|---|
| clean (LAN) | 2 -> 5 | 27 / 25 (6 / 8 at full PC speed) | 1 | 4 / 7 |
| 80 ms round trip | 2 -> 4 | 3 / 3 | 3 | 13 / 13 (2 / 3 at full PC speed) |
| 80 ms + 30 ms jitter + 2 % loss (several runs each) | 2 -> 5 | 4 to 28 | 4-5 | 3 to 15 |
| 140 ms + 20 ms jitter | 2 -> 6 | 24 / 37 (9 / 22 at full PC speed) | 4-5 | 2 / 6 |

Frames over 50 ms also come from the PC itself: whenever the two copies dropped below 150 game frames per 5 s both had them,
whatever the netcode (old code included); compare runs only where the PC kept full speed.

Android: the ad-hoc layer also takes a `WIFI_MODE_FULL_LOW_LATENCY` Wi-Fi lock while online
(`psp_android_wifi_low_latency_lock`, `PSPRECOMP_ADHOC_NOWIFILOCK=1` = off; the manifest has `WAKE_LOCK`). Not tested on a phone.

### Desync check (tag team battles)

Plain battles are checked by the pad link (the game's own check value every 10 frames, 'checks agreed' in the
`PSPRECOMP_NET_LINK_LOG=1` log). The runtime's own per-field check below runs in tag team battles only (`tagteam_net.cpp` submits
the state of every delivered frame; `netplay.cpp` no longer hooks the game's exchange). Both copies hash the battle state at every frame the lock-step plays and send the hashes to each other (on by default;
`PSPRECOMP_NET_SYNCCHECK=0` turns it off). Every 10 s the log says `[NETSYNC] so far: frames compared N, mismatched M`;
the first frame that differs is logged with every field that differs, and a message appears on screen:

```
[NETSYNC] DESYNC at frame 601 with 02:aa:be:3e:ad:25: 7 of 65 fields differ, first fighter0.pos.x local C1A7D9AD remote C1A9D9AD
[NETSYNC]   fighter0.pos.x           local C1A7D9AD remote C1A9D9AD
[NETSYNC]   fighter0.body.x          local C1A7FE53 remote C1A9FE96
...
```

The values: the battle clock; for each fighter its origin and body position, current script, motion time, the HP / brave
block (+0x10A8..+0x10B8), the assist gauge, and FNV hashes of the fighter struct in 256-byte blocks (`fighter1.block300`
= bytes +0x300..+0x3FF). Fields that differ between the two players by design (data only the local player's fighter
fills: camera, lock-on, the pad number, and any word holding a heap pointer, because the HUD and effects allocate per
view) are left out; the list is in `runtime/netplay.cpp` (`viewer_mask`). `PSPRECOMP_NETSYNC_DUMP=300,500` logs those
blocks word by word every frame (`[NETDUMP]`) so the two logs can be diffed to find the word behind a block mismatch.

The game has a much coarser check of its own (a sum of both fighters' positions and the battle clock every 10 frames,
sent in its input packets): when that one fails, the game ends the match (`[NETPLAY] the game's own lock-step checksum
failed at frame N`). Our check reports the exact frame and field before that happens.

## Ping and network warnings

Each copy pings the players in its group once a second (a PDP datagram to a reserved port, which the relay forwards like any other;
older builds do not answer and show `--`). **Show Ping** on the Network page (off by default; `PSPRECOMP_PING_OVERLAY=1`) puts the
round trip to the slowest other player at the top right while you are in a group, next to the frame rate if that is shown.

Network trouble is shown on screen for six seconds (and logged as `[NET]`), each kind at most every 10 to 60 s:

| Message | When |
|---|---|
| no answer from the party server | Internet mode, 6 s after Communications mode started without an answer |
| lost the connection to the party server | nothing from the server for 6 s (it answers every second) |
| nothing from *player* for N s (unstable connection) | a player in your group silent for 2.5 s |
| lost *player* (nothing received for 4 s) | the player is dropped |
| Ping to *player* is N ms (direct / through the relay / LAN) + a Network Delay tip | the round trip over 120 ms for 3 s (each 45 s; was 150 ms "high latency"). The tip says what Network Delay is (Auto: frames now; or a fixed value and what this ping needs) and to raise it in Settings > Network (it cannot change during a session). `PSPRECOMP_ADHOC_MSG_OLD=1` = the old text at 150 ms |
| Playing through the relay with *player* (ping N ms) + the cause | no direct path after the hole punch. The cause names a carrier-grade NAT when the introduced address is 100.64.0.0/10 or T-Mobile's 172.56.0.0/13 (or your own address is in 100.64/10, or a 10.x LAN address sits behind a public one that never answers: "probably"), else the firewall / router wording. Test aid `PSPRECOMP_ADHOC_FAKE_NAT=a.b.c.d` stands in for the player's public address in the message |
| packet loss to *player*: N % | 15 % or more of the last pings unanswered |
| waiting for the other player's input | the fight had to wait more than 5 frames within 2 s (battle link) |
| the other player's game is an older version | the battle link fell back to the game's exchange |
| *reason*. The battle ends. | the battle link ended a fight (the other player left, out of sync, nothing for 10 s) |
| UPnP: ... | the router forwarded the port (with your public address), or why not |

A network warning is not replaced by the frame-rate note when a session ends, so the reason stays on screen.

## Settings menu

The in-game settings menu (Esc / F1, Guide) pauses the game, which would stall the other players, so it does not open while you are in a group with others: a message says so and the game carries on. Change settings before joining a room.

## Direct paths through the party server (hole punching)

In Internet mode everything used to go through the party server, so every packet paid the extra trip. Now, once two players are in
one group, each asks the server to introduce it to the other (relay op INTRO, server version 2). The server sends both of them the
other's public address (what it sees) and the LAN address the other reported (relay op PEER); both then send small T_PUNCH packets
to those addresses every 100 ms for up to 6 s, which opens most home routers to the other side. The first punch that arrives sets
the direct path, and from then on packets to that player go straight to the address it came from; broadcasts (lobby beacons) still
go through the server. A direct path is kept alive with a punch every 0.5 s; silent for 3 s, it is dropped and traffic goes through
the server again (tried again after 15 s). Packets from outside the server are accepted only from the address a punch confirmed
for that player. An older server ignores INTRO and an older game never sends it: they play through the server as before.
`PSPRECOMP_ADHOC_DIRECT=0` keeps everything on the server. The `[ADHOC] direct:` log lines say what happened.

Tested 19 Nov 2026 with two copies and a local server: introduced, the direct path up on both, the battle link linked over it;
in the fight's minute the server forwarded 357 packets (the presence beacons) where a fight through it forwards about 4,200. With
one copy at `PSPRECOMP_ADHOC_DIRECT=0` the punches got no answer, both stayed on the server and the fight ran (359 checks agreed).
Not tested: real routers (strict or carrier-grade NAT gets no direct path and stays on the server), two players behind one router.
The party server has to run the new `tools/adhoc_relay` for any of this.

## Battles: our pad link

A 1-on-1 fight nobody watches runs on our own pad link (`games/dissidia/runtime/comm_battle.cpp`) instead of the game's exchange. The
game's exchange (`FUN_0890D664`) sends a 16-bit word a frame (12 buttons and 4 stick directions after its dead zone), so online
fighters only moved 8 ways; its input delay grows to 2 frames and never shrinks, and every late word freezes the fight. The link
replaces that function while the fight runs: each frame this copy's whole pad (buttons, stick at full resolution, turned by this copy's
free camera before sending) for frame f + delay goes over the ad-hoc side channel (type byte `B`), with this copy's last 8 frames in
every packet, so a lost packet is covered by the next. The other copy's pad for frame f is injected next to ours, and the analogue walk
works online. Every tenth frame the game's own check value (`FUN_0890CD90`) is compared; a difference ends the fight with the game's
error. When a pad is late the game waits that frame, as before, so the game's own timeout still ends a fight whose peer is gone (and
the link ends it after 10 s with nothing heard). The lobby, setup, results and Group Battle fights with watchers stay the game's.

**Watched fights (Group Battle).** The members who are not fighting watch each fight (the game's role 1, `FUN_0890DF58`). The
leading fighter (the game's player 0) keeps both pads of every frame it played and streams them to each watcher (STREAM, up to 36
frames a packet, from the first frame the watcher lacks; its WATCH packets say which); a watcher runs the fight from the stream,
injecting both pads as the fighters do, and compares the game's check values with the leader's. The fighters announce themselves to
the whole group and learn which member is the other fighter from its answer; they only link once both have seen every watcher the
game counts (`net+0x358`) announce itself, otherwise the fight (and its watchers) stays on the game's exchange. Link version 2;
version 1 copies count as older builds. Tested 19 Nov 2026 with three copies on one machine (Group Battle, Tournament, Lightning
against Lightning, one watcher): all three linked, 90 checks agreed on the watcher by frame 900, no waits, the watcher's picture
the leader's; with the watcher at `PSPRECOMP_NET_LINK=0` the fighters fell back after 5 s and the fight ran on the game's exchange
on all three.

**Group Battle rooms.** Until 19 November 2026 starting a Group Battle always failed with "Room has been deleted due to a
connection error": a member's request to enter waits for the host's answer without sending anything, and the matching layer
dropped any peer silent for 6 s, so by the time the host started, its `MatchingSelectTarget` (the accept) found nobody and
returned an error. Waiting requests are now dropped only when the player is gone from the network (its beacons stop).

Both copies announce the link at the fight's first frame; one that hears nothing within 5 s (an older build, or `PSPRECOMP_NET_LINK=0`)
keeps the game's exchange for that fight, so mixed versions still play. Injected sticks are never turned by the receiving copy's camera
and, while the link runs, are processed with the default stick constants whatever the local settings, so both copies compute the same
fight.

| Variable | Meaning |
|---|---|
| `PSPRECOMP_NET_LINK=0` | the game's own exchange |
| `PSPRECOMP_NET_DELAY=auto\|<frames>` | input delay: Auto (default, from the ping at battle start) or 1 to 8 frames |
| `PSPRECOMP_TTLOCK_ADAPT=0` | tag lock-step: the old fixed lead (retail sampling, no reports) |
| `PSPRECOMP_TTLOCK_SHORTWAIT_MS=<ms>` | tag lock-step: how long a late word is waited for before a frame is given up (25; 0 = off) |
| `PSPRECOMP_TTLOCK_LATE_MIN=<n>`, `_SHORTEN=1`, `_SHRINK_S=<s>`, `_SHRINK_MIN=<n>` | tag lock-step lead tuning (2 late holds; shortening off; 10 s; 3 words) |
| `PSPRECOMP_NET_LINK_LOG=1` | log the link (linked, every 900 frames: checks agreed, frames waited, how a fight ended) |
| `PSPRECOMP_NET_LINK_DROP=<percent>` | drop that share of the pad packets sent (tests) |

Tested 19 Nov 2026, two copies on one machine (LAN, Online Lobby 1-on-1, Lightning vs Lightning): linked, fights in sync with both
attacking (same Bravery and HP on both screens), 1,800 frames with no wait; copy A at 60 fps and copy B at 240 with 30 % of the pad
packets dropped: 79 checks agreed by frame 900, 16 frames waited; a copy with `PSPRECOMP_NET_LINK=0` against one with the link: the
link gave up after 5 s and the fight ran on the game's exchange. Not tested yet: two machines, through the relay, real latency.
Design from VCKing101's port (https://github.com/VCKing101/Dissidia012-pc, `docs/port/communication-mode.md`, MIT); our own code.

## Frame rate

Every player's wanted frame rate (from the settings menu) is shared in the beacons. When every player in a group asked for more
than 30, each keeps their own rate: above 30 a fight's logic runs at 30 Hz (30 Hz logic) and only the frames between are drawn
(tested: 60 against 240 fps, in sync). If anyone asked for 30 the game runs at the **lowest rate any of them asked for**, so a
60 fps player and a 30 fps player both run at 30 (plain 30 fps is not bit-identical to 30 Hz logic in every move).
`PSPRECOMP_NET_SAME_FPS=1` always uses the lowest rate. It returns to the player's own setting when the group ends. A message at the top of the screen tells you the rate for five seconds ("Online: 30 fps (lowest player's setting)"), and again when the group ends. This logic is in
`runtime/framerate.cpp` (`psp_adhoc_group_fps_cap()`).

Above 30 fps a fight runs on 30 Hz logic ([frame-rate.md](frame-rate.md)): the game advances on one displayed frame in two, three or
four. Its network manager task (`FUN_0890E794`, which runs the battle's per-frame pad exchange `FUN_0890D664`) only runs on those logic
frames. Until 19 November 2026 it ran on every displayed frame; on the in-between frames the other copy's pad for the next frame was
not there yet, the exchange counted a waiting frame, and after the game's 900 waiting frames it ended the session without a message:
about 10 s into every online fight at 120 fps (two copies on one machine, LAN; 30 fps was fine). `PSPRECOMP_L30_NET_EVERY_FRAME=1`
brings the old behaviour back for tests.

## Options

| Variable | Meaning |
|---|---|
| `PSPRECOMP_ADHOC=0` | turn the emulation off (the old no-op stubs return, Communications mode then cannot connect) |
| `PSPRECOMP_ADHOC_MODE=lan\|internet` | how players find each other; default `internet` (`lan` if `PSPRECOMP_ADHOC_PEERS` is set and no mode is given). Saved settings win |
| `PSPRECOMP_ADHOC_SERVER=host[:port]` | the relay for Internet mode (default `party.schindlershadow.com:27500`) |
| `PSPRECOMP_ADHOC_ROOM=<name>` | room on the relay; empty = public |
| `PSPRECOMP_ADHOC_PORT=<n>` | LAN mode: first UDP port (default 27312) |
| `PSPRECOMP_ADHOC_SPAN=<n>` | how many consecutive ports are tried and probed (default 16, up to 64) |
| `PSPRECOMP_ADHOC_PEERS=a,b` | LAN mode: extra peers to contact directly: `host` or `host:port` (a bare host probes the whole port span) |
| `PSPRECOMP_ADHOC_NICK=<name>` | nickname other adhocctl clients see (default: the save's player name once a save is loaded, else "Player"; Discord status names players by it) |
| `PSPRECOMP_ADHOC_MAC=aa:bb:cc:dd:ee:ff` | virtual MAC; default is random per launch (a host-name hash collides: every Steam Deck is called `steamdeck`) |
| `PSPRECOMP_ADHOC_LOG=1` | log every call and event with arguments, results and the first bytes of each message |
| `PSPRECOMP_PAD_BACKGROUND=1` | let controllers act while the window is unfocused |
| `PSPRECOMP_NET_INPUT_DELAY` | old name of `PSPRECOMP_NET_DELAY` |
| `PSPRECOMP_NET_SYNCCHECK=0` | turn the desync check off |
| `PSPRECOMP_NET_TRACE=1` | log every PTP / PDP message with the frame number (`[NETTRACE]`) and the lock-step state of every frame (`[NETLS] rc= frame= seq= lead=`) |
| `PSPRECOMP_NET_LAG_MS=<ms>`, `PSPRECOMP_NET_JITTER_MS=<ms>`, `PSPRECOMP_NET_LOSS=<percent>` | testing: delay (plus 0..jitter) or drop the game packets this copy receives; set on both copies (round trip = twice the delay) |
| `PSPRECOMP_NET_PERTURB=<frame>` | testing: move fighter 0 by 0.25 on this copy only after that lock-step frame (the desync check must report frame+1) |
| `PSPRECOMP_NETSYNC_DUMP=<hex offsets>` | log those fighter blocks word by word every frame (`[NETDUMP]`) |
| `PSPRECOMP_NET_VIEW_STICK=1` | testing: turn every pad slot's stick by the local camera again (the old desync, see "Camera and stick") |

Log lines start with `[ADHOC]` in `last_run.log`.

## How it works

The game uses four layers of the PSP network API; each is emulated:

| Layer | Calls | Emulation |
|---|---|---|
| Wlan / net helpers | `sceWlanGetEtherAddr`, `sceNetGetLocalEtherAddr`, `sceNetEtherNtostr` | a virtual MAC per instance |
| Ad-hoc control (adhocctl) | `Init/Term/AddHandler/DelHandler`, `Create/Connect/Join/Disconnect/Scan`, `GetScanInfo/GetPeerList/GetPeerInfo` | a "group" is a name; peers advertise their group in beacons. Events (connect, disconnect, scan) are queued and delivered to the game's handlers |
| Matching | `MatchingInit/Create/Start/Stop/Delete`, `SelectTarget/CancelTarget/SendData/SetHelloOpt` | the lobby state machine: hello, request, accept, established, leave, cancel, data and ping/timeout, as events on the game's callback |
| PDP / PTP | `PdpCreate/Send/Recv`, `PtpListen/Open/Connect/Accept/Send/Recv/Flush/Close` | PDP is a datagram per call; PTP is a TCP-like reliable stream (connection ids, go-back-N retransmission, partial sends) |

Messages larger than 1.3 KB (the lobby hello is 4.7 KB) are split into 1.1 KB `T_FRAG` packets and reassembled by the receiver, because IP-fragmented datagrams are dropped by many routers (a home router's loop-back NAT did exactly that). Transport: one UDP socket and one network thread. In LAN mode the socket takes the first free port from 27312 and sends to the LAN, loopback and listed peers; in Internet mode every packet goes to the relay server. Packets start with `DSAH`, a type, the sender's MAC and a body
length. Types: beacon (nickname, group, wanted frame rate, known peers), PDP, matching, PTP. Beacons go out twice a
second to the local ports, the LAN broadcast address and any listed peers; a peer is dropped after 4 s of silence.

Guest callbacks never run on the network thread. They are queued and run on a game thread, from
`sceKernelCheckCallback` and the vblank wait (`psp_adhoc_pump`), because they call back into recompiled code.

Details worth knowing when changing it:

- A matching request that is still pending (sent, received, or accepted but not yet established) stays until it is
  accepted or cancelled, as on a PSP: both sides ping each other every second while it waits (a ping flag marks it as
  pending), so a host may take as long as it likes to start a room. Only when a side's pings stop for 6 s is the other
  side told (cancel for a requester that is gone, timeout for a target that is gone); a pending ping to a side that has
  no such request is answered with a cancel, so no stale entry is left when a cancel is lost.
- The game checks every peer with `sceNetAdhocctlGetPeerInfo` and drops anyone whose `last_recv` (game clock) is older
  than 20 s, so the field is filled from the beacon time.
- After the PTP handshake the game keeps opening new sockets on the same ports whenever a connect fails. A SYN with no
  listener must be refused (RST) so it retries, and every packet carries a connection id so replies reach the right
  socket.
- `sceNetAdhocPtpSend` is `send()`-like: it queues what fits in the socket's buffer and writes the count into `*size`
  (0 on would-block). The game adds that count to what it has sent.
- Struct layouts follow PPSSPP: scan info 28 bytes, peer info 152 bytes, PDP stat 20, PTP stat 36 (the last two are
  unchecked against the game).

## Battle protocol

What the two PSPs exchange during a Communications fight (Group Battle; found with `PSPRECOMP_NET_TRACE=1`):

- Before the fight, over PTP (ports 0x64-0x66): the setup and each player's data (20 KB blocks on 0x64, a 4-byte hello
  and a 64-byte answer on 0x66).
- During the fight, nothing but pad input, over PDP port 0x67: every frame one 36-byte datagram to every peer. The
  battle's net object (EBOOT `FUN_0890D664`, called once per frame) reads the pad itself (`sceCtrlReadBufferPositive`),
  packs it into a 16-bit input word for frame `seq` and sends the newest 8 words (so a lost datagram is covered by the
  next ones), the oldest frame of that window, and the game's checksum with its frame (every 10 frames). It then plays
  frame `frame` if every player's input for it has arrived: it copies the inputs into the pad manager's per-player slots
  (`[0x089D6008] + 96 x player`, +64) and returns 0; otherwise it returns 1 and the battle waits for a frame. 2 = no
  battle, 3 = the checksums disagree (the match ends).
- The run-ahead (`seq - frame`, the local input delay) is 0 on a LAN. The pad is read only while
  `seq - (last frame played) < 4` (`FUN_0890D214`), so during waits the input can get up to 3 frames ahead, and stays
  there. That is the latency the stock game absorbs. Input Delay N holds the battle back at the start until the input
  runs N frames ahead (`FUN_0890D524`, "is this frame's input here", answers no) and raises the limit to 4 + N.
- A tag team battle (docs/tag-team.md "Multiplayer: N-player lock-step") replaces this with the runtime's own protocol on every member
  (the game's dispatcher `FUN_0890C5BC`): every member sends its input words to everyone, only the fighters' drivers are waited for.
- Both copies simulate the whole fight from the same inputs; nothing of the state is sent. So any difference in the
  simulation (an uninitialised value, a host-side random number, a frame-rate fix that depends on local timing) is a
  desync. The negotiated frame rate (both play at the lowest rate either asked for) is what keeps the step identical.

(The rows of the table below, and `mp_test.py`'s `NET_INPUT_DELAY` runs, measured the OLD input-delay hook in `netplay.cpp`, which is retired; plain battles are measured in "Battles: our pad link".)

### Testing (two copies, headless)

`tools/mp_test.py --work-a W1 --work-b W2 [--env 'PSPRECOMP_NET_LAG_MS=100 PSPRECOMP_NET_INPUT_DELAY=3'] [--secs 60]` boots
two copies on this machine (LAN mode, no window: `SDL_VIDEODRIVER=offscreen` with the NVIDIA EGL vendor file, private
settings and memory stick copies), walks both into a fight (A: Communications Mode > Group Battle > Create Room (Custom);
B: Search for a Room, join; A starts; A takes Party A, B Party B; Tournament; both pick Lightning; Enter Battle; Next until
the lock-step runs), presses a fixed list of moves on both through the debug socket, and prints the waits, the lead and
the desync summary of each copy. `--env-a 'PSPRECOMP_NET_PERTURB=600' --expect-desync` is the desync-check self test.
Results (30 fps, 60-120 s of moves each):

| Case (`PSPRECOMP_NET_*` on both copies) | Waits | Lead A / B | Desync check |
|---|---|---|---|
| (old netplay.cpp hook) none, Input Delay Off | 0 % | 0 / 1 | 3600 frames, 0 mismatched |
| none, Input Delay 3 | 0 % | 3 / 3 | 1800 frames, 0 mismatched |
| LAG_MS=100 (200 ms round trip), Off | 33 % | 2-3 / 2-3 | 1200 frames, 0 mismatched |
| LAG_MS=100, Input Delay 3 | 0.1 % | 5 / 3 | 1800 frames, 0 mismatched |
| LAG_MS=60 JITTER_MS=40 LOSS=2, Input Delay 3 | 0.1 % | 5 / 4-5 | 1760 frames, 0 mismatched |
| LAG_MS=60 JITTER_MS=40 LOSS=2, Off | 26-30 % | 2-3 / 2-3 | before the stick fix 3 of 6 runs desynced at frame 235 (see "Camera and stick") |
| none, A: PERTURB=600 | 0 % | 0 / 1 | both report frame 601, `fighter0.pos.x` first; the game's own check ends the match at frame 611 |

### Camera and stick (the jitter desync, fixed)

Three of six runs with 60 ms + 0-40 ms jitter + 2 % loss and Input Delay Off once desynced at frame 235, right after copy
A's first stick input, with identical inputs on both copies; a tag battle desynced the same way when a fighter got up from
a knockdown with the stick held (yaw -1.99 on one copy, -1.15 on the other). Cause: **the runtime turned every player's
stick by the local view.**

- The stick reaches the battle like this: the net object (`FUN_0890D664`) reads the local pad and packs it into the 16-bit
  input word (`FUN_0890CE58`), which runs the stick processor `FUN_08872138` on the local stick; only its 4 virtual D-pad
  bits (0x1000-0x8000) are sent. Every machine unpacks every player's word into the pad slots (`FUN_0890D090`: each axis
  becomes 0x00 / 0x80 / 0xFF) and runs `FUN_08872138` again on each slot.
- `input.cpp` wraps `FUN_08872138` and turns the stick by `dissidia_camera_turn()`, how far this player's free camera
  (right stick) or held camera (no lock-on, e.g. after a knockdown or before the first lock) is turned from the game's
  camera, so that "up" goes into the screen. That ran for the slots too: on each machine both fighters' sticks were turned
  by that machine's view, which differs between the players (and is advanced by the wall clock, also on held-back frames).
- Fix (`input.cpp`): in a lock-step battle (`FUN_0890CE58` has run in this battle, or the player is in an ad-hoc group) the
  turn is applied only inside `FUN_0890CE58`, to the local stick before it is packed, so it is part of the input every
  machine receives; the slots are processed the same way everywhere. Per fighter by construction: each player's own word
  carries their own turn, so a human-driven tag partner works the same way. Single player is unchanged (that path is not
  taken). `PSPRECOMP_NET_VIEW_STICK=1` restores the old behaviour for A/B tests.
- The game's own conversion does not read the displayed camera: with the left stick held in a single-player battle, gdb
  read watchpoints on the camera manager's matrix / eye / target (`[0x08B3D6B8]`, +0x00..+0x5F) saw reads only inside
  the camera update `FUN_08B3CA24` and its copy-outs (render camera `FUN_088A5F1C`, global camera `[0x089D5EC0]+0x720` by
  `FUN_08B3C0D8`), and the global copy is read by the render-matrix builders and two unidentified battle-overlay readers
  (`FUN_08B53C20` eye, `FUN_08ABBC48` matrix). Two copies with one player's view turned by the right stick while moving
  stay in sync (below), which a movement conversion through the displayed camera could not. So the camera module's view
  polish (free camera, hold, smoothing, own collision, wall fade, hitcam= and tag entrance close-ups) stays on in
  multiplayer; none of it is disabled.

Tests, 30 fps, two copies on one machine (`mp_test.py --rstick`: the usual moves plus right-stick turns on both copies while
the left stick is held; tag battles: the same with `PSPRECOMP_TAGTEAM=1`, 2 v 2 with CPU partners; runs that stalled under
the GPU load of other sessions are left out):

| Case | Old stick turn (`PSPRECOMP_NET_VIEW_STICK=1`) | Fixed |
|---|---|---|
| Group Battle, right stick, LAN | 2 of 2 desynced at frame 345, the game ended the match at 352 | 2 of 2 in sync (2100 frames each) |
| Group Battle, right stick, LAG 60 + JITTER 40 + LOSS 2 | (the run stalled) | 2 of 2 in sync (1446-1521 frames) |
| Group Battle, no right stick, LAG 60 + JITTER 40 + LOSS 2 | 2 runs: no position desync | 2 runs: no position desync |
| Tag battle, right stick, LAN | 1 of 1 desynced at frame 435, the game ended the match at 441 | 2 of 2 in sync (2094-2100 frames) |
| Group Battle, right stick, copy A's view on the opponent's camera (`PSPRECOMP_CAM_VIEW_SLOT=1`) | - | in sync (2100 frames) |

Without the right stick the old frame-235 desync did not reproduce in two runs with the old turn (the camera code changed
since; not investigated), so the right stick is the repeatable pressure: it makes the turn non-zero at once.

**Showing another fighter (view only).** The camera manager's slot table (`mgr + 0x6D0 + 4 * i`) is simulation state:
every update `FUN_08B3CA24` steps the camera of every lock block listed there (`FUN_08B3C760`, `FUN_08B3B4FC`), and
movement uses that fighter's lock-block camera (inferred from that test; the per-slot reads were not traced). Re-pointing a side's entry to show another fighter (a test in the
lock-step tree) split the positions at the first stick input. Which slot is *shown* is per viewer: the update picks the
slot whose lock block has byte `+0x125` set (`FUN_08B3B308`, index stored at `mgr + 0x8B8`). `camera.cpp`
`dissidia_camera_set_view_slot(i)` (`PSPRECOMP_CAM_VIEW_SLOT=<i>` for tests) shows slot `i`'s camera by writing its
eye / look-at (the update's copy at `mgr + 0x110 + 0x30 * i`) into the final eye, view only. That copy is the slot's raw
camera, before the manager's final blend / pull-in (a metre or so from the game's own framing of the same slot), and a
fighter needs a slot entry (made the same way on every machine) for his camera to be stepped at all.

Still open: under jitter and loss a mismatch in `fighter1.blockD00` (one field, ~24-40 frames, positions and the game's
own checksum unaffected) was seen with and without the fix (1 of 2 runs each, frames 960 / 1274); the word is not identified
(`PSPRECOMP_NETSYNC_DUMP=D00` names it). Some jitter / loss runs stall at the start of the fight (both copies at under
1 fps, no lock-step frames) with and without the fix; not looked into.
## Mods that change a character (verified packs)

A player who uses a known pack (today: Bartz's Dissidia 1 moves, `mods/bartz-dff`) can play online with players who
never installed it: as soon as both are in the same lobby, the other game fetches what it needs from that player
(about 760 KB: the moveset, the models and the voice bank, as deltas against the retail 012 files every player has),
caches it in `<mods>/.net_cache/`, and uses it for that session. A player's own characters are not changed by it.
Only the player who wants the moves needs the Dissidia 1 disc.

Nothing a peer sends is trusted. The packs a game accepts are compiled into it (`games/dissidia/net_packs.json` ->
`runtime/net_packs.inc`, written by `tools/netpack.py record`): every file with its size and SHA-256, the SHA-256 of the
retail file the delta applies to, and the abilities. The receiving game applies the delta to its own retail file
(`runtime/net_delta.h`: every copy bounds-checked, output capped at the listed size, inflate capped; host test
`tests/run_net_delta_test.sh`, run under AddressSanitizer with corrupted and hostile deltas), keeps the result only if
size and SHA-256 match, stores it under that hash (never a name a peer sent), and builds the ability records itself
from the list. So a peer can make another game use exactly the listed files and abilities, or nothing.

Who may start a match (`net_packs.cpp` `allow_match`, through the ad-hoc layer's match filter): a player who uses a
pack this game does not know (another version, an edited mod, a build that is not the listed one) or has not been
received and verified yet is refused, and a player who uses a pack only plays with players who confirmed having it. A
refused player is not shown in the lobby. Two players without packs play as before.

Transport: a side channel of the ad-hoc layer (PDP to the reserved port 0xD1D1, `psp_adhoc_side_*`), so the party
server needs no change. Messages: a manifest every second (packs in use, packs ready), requests and 1 KB pieces of a
delta (24 in flight, resent after 0.4 s, given up after 90 s). A sender serves only the deltas `netpack.py deltas`
wrote (run by `charmod.py build`) for packs whose files it has installed byte for byte (`<mods>/.net/<sha256>.dlt`).

Tested 16 Nov 2026 with two instances in LAN mode: A with the pack and Ascendant Lance / Soul Eater equipped, B with an
empty mods folder. B fetched and verified all six files in the lobby within seconds, the match started, A picked Bartz
and B Lightning, and both screens showed the same Brave and HP values through Ascendant Lance and Soul Eater. With
one of A's deltas altered (valid format, different bytes), B threw that file away and the two players no longer saw
each other in the lobby. Not done yet: an on-screen note telling a refused player why.

Adding a pack: build the mod, `netpack.py record <mods dir> <folder> --version N` (updates the JSON and the .inc), rebuild
the game. The recorded hashes must be what `d1_moves.py` / the mod build makes on every machine (they are deterministic
for the same discs: rebuilding from scratch gave identical files).

**A received pack must equal a native install (27 Nov, pack v2).** A native install changes the game's data through the mod's main.bin: the new `ablt` records and the character's `chrl` row (abilities learned per level, used by CPU records). A pack received online makes both locally from the compiled-in list: the records copy a retail record and get the listed command / type / phase and **CP / AP costs** (`cp`, `cp_mastered`, `ap`), and the `chrl` row is replaced by the listed one (`netpack.py record` reads both from the mod's built main.bin; `dissidia_chrl_set_row`). Before v2 the costs and the `chrl` row differed, so a Bartz CPU fighter had other abilities on a machine that got the pack online and the fights desynced. `[ABLT]` log lines show the table hash and the records. Test: `tools/mp_diverge.py --pack` (in `tagteam_test.py --baseline` as `mp_pack_bartz_internet`). `PSPRECOMP_NETPACK_COSTS_OLD=1` = the old behaviour. When a pack changes anything else a fight reads (another table), add it to the pack too, or the players desync.

## Persistent desync (27 Nov)

The desync check cannot repair a fight, it only reports it. A fight that stays out of sync is a different fight on each machine (a player can be stunned or unable to move on one of them while the others play on). The first three reports show a 6 s toast; after that a toast every ~150 mismatched frames says "Out of sync with another player: ... Leave from the pause menu" and the log has `[NETSYNC] still out of sync with <mac> after N mismatched frames`. No automatic resync or match end yet (needs state transfer / rollback). `PSPRECOMP_DESYNC_TOAST_OLD=1` = only the first three reports.

## Build id (26 Nov)

Both players should run the same build: a different one can desync (plans, line-ups, banters, lock-step state). Every build sends its git commit and a netcode protocol version to the other players: at the end of the pack manifest (once a second while in a lobby; `'V'` u16 protocol, u8 length, commit string - older builds stop reading before it and show up as "an older build") and, in a tag battle, as a `'TV'` message on the roster channel. The log has `[BUILD]` lines. A different commit with the same protocol only warns on screen ("Online: another player runs build X (netcode 1), this game build Y (netcode 1). Fights may desync."). A different protocol version refuses the match on both sides, like a mod pack mismatch ("no match with a player in the lobby: ..."). Bump `kNetProtocolReal` (`runtime/net_packs.cpp`) when a change makes lock-step play incompatible. `PSPRECOMP_NETBUILD_FAKE=<id>[:<protocol>]` announces another build (tests).

## The party server

`tools/adhoc_relay/` ([README](../../../../tools/adhoc_relay/README.md)) is a small C++ UDP relay. A client registers into a
room with a cookie handshake (so a forged source address cannot make the server flood someone), then the server
copies its beacons to the rest of the room and its PDP / matching / PTP packets to the player whose MAC is the
destination (everyone else for the broadcast MAC). Packets are forwarded untouched; the client sends one copy and
the server fans out, so a 10-player room costs each player the same upstream as two. Per-client rate limits and a
32-player room size protect it. The client keeps the registration alive with a hello every second and treats 5 s of
silence as a lost server. `party.schindlershadow.com` runs it as a user service on a home server, with UDP 27500
forwarded; run your own with the README's two lines and point Server at it.

## Not implemented

- **TODO (large, not started; user 26 Nov): rollback netcode for high-ping play.** Players on mobile / carrier-grade NAT links play through the relay at 150-250 ms, where input delay (Network Delay) alone stalls or feels sluggish. See "Rollback: what it would need" below; the tag team lock-step (`tagteam_net.cpp`) would need the same snapshot / predict / re-simulate layer.

- `sceNetAdhocDiscover*` and game mode (`sceNetAdhocctlCreateEnterGameMode` and friends): the game imports Discover but
  nothing in Communications mode was seen to need it.
- Rollback (issue #66); see "Rollback: what it would need" below. Any form of authentication or encryption. A single
  relay host is a single point of failure.
- The Online Lobby (Communications Mode > Online Lobby) stops on a white screen: it jumps to `0x08A11ED8` in overlay 2,
  a function the analysis did not find (`[LOOKUP_MISS]`). Group Battle works.
- Infrastructure (WiFi access point) mode: the game does not use it.

## Rollback: what it would need

Notes from the input-delay / desync-check work (issue #66), for whoever builds rollback on top of it:

- **Inputs are already isolated.** The battle reads its inputs only from the pad slots `FUN_0890D664` fills, one frame
  at a time, and the frame number is explicit (`frame`, net object +72). A rollback layer can own that function: deliver
  a predicted input for the remote player when the real one is missing (return 0 instead of 1), remember what was
  predicted, and roll back when the real input differs.
- **Frame boundary.** `FUN_0890D664` runs at the same point of the main thread's frame every frame, with the same native
  call stack under it. That is the one place where a snapshot / restore of guest state can work despite the native-stack
  execution model (`docs/savestates.md`): restore guest memory and the main thread's registers there and return, and the
  main thread continues from a state it was in before. Other guest threads (audio, file loading) must not be in the
  middle of touching battle memory then; that has not been checked.
- **What to snapshot.** Not measured yet beyond the fighters: each fighter struct is about 0x1100 bytes, plus the battle
  object (`[0x089D6A10]`), the slow-motion object, effect / projectile actors, the RNG the battle uses (not found yet), the
  battle overlay's data section, and the runtime's own per-frame hook state in `games/dissidia/runtime` (frame-rate
  compensation, press latch, camera). The desync check's block hashes can be extended to find what else changes per
  frame.
- **View-side state is separate.** The fields the desync check masks (camera, lock-on, HUD, effect pointers) differ
  between the players and need not (and must not) be rolled back with the simulation; re-simulated frames must not draw
  or play sounds (the 30 Hz logic work's "drawn-only frames" skip list is the opposite case and a starting point).
- **The game's own checksum** compares positions every 10 frames and ends the match on a mismatch: a rollback
  implementation has to send its checksums from confirmed frames only, or turn it off.
- **Determinism risks seen in the code** (already a desync risk without rollback): `roster.cpp` `magi_pick` (modded
  characters whose magic picks a random element) advances a process-wide xorshift (`g_pick_rng`) and a call counter that
  live for the whole session, mixed with the actor address; two players whose games made a different number of picks
  before the match (a single-player fight first) pick differently. It should be reseeded from battle state at the start
  of each battle. Every native frame-rate compensation in `framerate.cpp` runs per update and must behave identically on
  both sides (both play at the negotiated rate, so they do today); `camera.cpp` and `framerate.cpp` read the wall clock,
  which is fine only while it feeds view-side state.
- **The camera.** The displayed camera is view state and the simulation does not read it (see "Camera and stick"); the
  stick turn by the local view is applied before the input is packed, so a rollback layer predicting or replaying inputs
  sees the turned stick as plain input. Re-simulated frames must not run the camera module's per-frame hook (it also
  calls `dissidia_tagteam_frame`, the tag team's swap / call-in safe point).
- **Re-simulation cost** (up to 8 frames per displayed frame) is unknown; the battle update was never timed without
  drawing.

**Tag battles (our lock-step, 20 Nov):** the end of a tag battle and a silent peer are handled by `tagteam_net.cpp` (lock-step stands down at the decision so retail's end-of-battle exchange can run; `PSPRECOMP_TTLOCK_TIMEOUT` s of silence from a needed player = quit like above), see tag-team.md "Lock-step end of battle".

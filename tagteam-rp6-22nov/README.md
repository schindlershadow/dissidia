# Tag team play-test logs, RP6 (Android), 22 Nov, online via the relay against one other player

Build: tag-slot around the v0.1.25 release (summonstones off, KO hold default). Each log is the game's last_run.log of one session.

| File | Report |
|---|---|
| rp6_feral_void_1122_2259.log + .png | The CPU partner (extraP Feral Chaos, char 32) EX Burst: everyone stays in the burst "void" forever (screenshot). `EX Burst over: the extra burster's lock block ... taken out of its camera slot` at 270.61, but no `EX Burst over: <fighter> back from under the stage` lines. |
| rp6_desync_1122_2242.log | `[NETSYNC] DESYNC at frame 0 ... 16 of 66 fields differ, first fighter0.block100` (the human's Terra) before any input; every frame after mismatches. Opponent saw Terra die while alive here; humans looked AI-driven; froze when Feral Chaos was called in on the opponent's screen. Cannot pause in this battle. |
| rp6_2v2_1122_2255.log | Communications 2v2: the Battle Lobby closes instantly. `[NETSYNC] DESYNC at frame 1 ... first fighter0.body.x` (both slot fighters' positions + blocks 0x100/0x300/0x800). `[LOBBY] single player: ...` logged in an online session. Also contains the Feral Chaos burst (264-270 s). |
| rp6_spectate_terra_1122_2207.log | After the human died the spectate view was wrong; he came back as TERRA as an extra this machine drives (`chase camera on the extra this machine drives`) instead of in his slot: camera did not follow / turn, L could not change the lock. Slot rebuilt as Terra at 229.6 s. |
| rp6_whitescreen_1122_2228.log | Leaving Communications mode after the tag battle: adhocctl event 2, full battle teardown at t=342.4, then `[HANGDUMP] game thread stuck` forever (white screen, GPU still drawing). |
| rp6_lan_1122_2246.log | Network = LAN: joined "MLOBBY", no peer ever found; via the relay the same peer appeared at once (LAN address 192.168.2.120). Generic multiplayer. |

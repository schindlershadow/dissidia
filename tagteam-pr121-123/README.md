# Logs for PR #121 (free-for-all) and PR #123 (3 v 3)

All runs: 10 game copies on ONE host (Linux, headless), own relay, branch tag-slot at ea1cc5f, rig `run10.py` variant.
Ten copies share one machine, so every copy runs slowly; timings are not comparable with 2 real machines.

## FFA (4), 10 humans, run c2b
- Decided at t=138.0 s on the host.
- `ffa4_c2b_h1_from_decided.log`: host (h1) from the "decided" line to the end of the run.
- `ffa4_c2b_h6_from_decided.log`: a member (h6), same stretch.
- No `[TTGB] setup screen` line follows in either log: the run ended (observation time over) before any copy was back on the setup screen.
- What was slow: 60 s after the decision (`c2b_post1_montage.png`, h1..h10 left to right, top row first) h1-h5 show the EXP / result screen ("Lightning obtained 3698 EXP"), h6-h7 still show the battlefield with "Hold: Cancel wireless" (waiting on the link), h8-h10 drew nothing. So: members waiting on the wireless link, not the victory pose or the tally. `c2b_late_montage.png` / `c2b_end_montage.png`: later shots.

## 3 v 3, 10 humans, run c1b (GE fault on h1, h3, h4)
- First `[GE] Hit command limit (4194304) without END at pc=0x015DB408` at log line 5963 (h1), 6288 (h3), 5985 (h4).
- `3v3_c1b_hN_before_slowdown.log`: the 200 lines before it plus 20 after.
- `3v3_c1b_hN_filtered.log`: only `[GE_` / `[GE]` / `[TAGTEAM]` / `[MODELS]` / `[EXCORE]` lines from the 2000 lines before it.
- Call-ins just before: t=29.6 s CALLIN queued KAIN (char 24) for side 1 (entrance 29.7 s); t=31.6 s slot rebuild "char 31 stands". The fault started right after that rebuild. The same rebuild in runs c1a / c1c had no fault.
- `c1b_t30_montage.png` / `c1b_t100_montage.png`: all 10 copies at 30 s / 100 s.

## Sync check note
Agreed: "0 mismatches" does not catch a different fighter count. The FFA (3) split was caught by other lines (different extras spawned, `[NETSYNC] DESYNC at frame 235`, different decision times). Adding the fighter count / extras mask to the sync hash would close that.

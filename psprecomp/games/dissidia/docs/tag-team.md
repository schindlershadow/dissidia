# Tag team battles (2 v 2): research notes and plan

Status: **research only, nothing implemented yet** (branch `tag-team`, based on `origin/main` 00b9d41, 14 Nov).
Everything below comes from reading the recompiled code (`psprecomp dump <analysis> 0xADDR`) and the existing
runtime hooks. "Confirmed" = read in the code; "inferred" = a guess to check in a run.

Goal: Budokai Tenkaichi style tag battles. Two fighters per side on the stage at once, the rest of the party roster
comes in when a fighter is KO'd, the existing lock-on cycling switches between the enemy fighters.

## What the battle looks like today (confirmed)

**Two fighters, hard-wired.** The battle setup `FUN_0888BA0C(bs)` (EBOOT, called from `FUN_0888C4C8`) builds the whole
cast:

| bs field | meaning |
|---|---|
| `+0x4C` (76) | battle mode (5, 6, 0xD, 0 are handled here; 6 swaps the per-side data) |
| `+0x38` / `+0x3C` (56 / 60) | the two fighter actors (slot 0 / slot 1) |
| `+0x54` (84) | the slot the human plays |
| `+0x7D8` / `+0x7DC` (2008 / 2012) | the two fighters' setup data (char info) |

Each actor is made the same way: `FUN_08831320(0x12D0)` (allocate), `FUN_088729A0(side)`, `FUN_088470B8(setup)`, then the
fighter constructor `FUN_08B39E5C(mem, a, b, side, 0)`. The code after that is all `for (i = 0; i < 2; i++)`: the
foes are cross-linked with `FUN_08836DA4(a, b)` (`a+0x300 = b`, so `+0x300` is "my enemy"), per-fighter records
(stride 0x1060 in `bs`) are copied into each, the lock-on block is initialised (`FUN_08B3B1B8`), and the camera /
HUD take the two actors from `bs+0x38`, `bs+0x3C`.

**Actor kinds** (byte `actor+0x542`, set by the object constructor `FUN_0883975C`): 0 fighter, 1 assist (master at
`+0x1108`), 3 summoned / thrown object (master at `+0x5D4`), 5 bullet, 6 the dash's fixed-point target.
`FUN_0883739C` (is fighter) is true for kinds 0 and 1. Assists are real, fully loaded second characters that
already live in the battle next to the two fighters, so the engine can run more than two character actors.

**Actor fields used for targeting:**

| offset | meaning |
|---|---|
| `+0x300` | the enemy fighter (set at setup; `FUN_08AB32B8(actor)` returns it, kind 0/1 only) |
| `+0x1108` | the "partner": an assist's master; for a fighter it is also what lock-on falls back to |
| `+0x2FC` | the current target (what `turn_to_target`, homing dashes and projectiles read; getter `FUN_08836D8C`) |
| `+0x5AC` | the lock-on block (see below) |

**The lock-on block** (battle overlay, `FUN_08B3B0B4` .. `FUN_08B3B550`). This is the "cycle lock" system:

| block field | meaning |
|---|---|
| `+0xF8` (248) | owner actor |
| `+0xFC` (252) | the locked target actor |
| `+0x108` (264) | mode (0..3); `+0x10C` (268) previous mode; `+0x110` (272) / `+0x114` (276) index into the mode table |
| `+0x118` (280) | "changing" flag |
| `+0x12C` (300) | table of mode handler objects (each has virtual functions at vtable +8 / +16 / +112: enter, update, get-target) |

`FUN_08B3B280(block)` = current target (asks the active mode handler), `FUN_08B3B0B4(block, target)` = set the target
(also copies it into `owner+0x2FC` through `FUN_08B2F17C`, which clears it again in `FUN_08B2F22C`),
`FUN_08B3B3EC(block, mode, flag)` = change mode. The target the block hands out is "any kind", so it is not limited to
the one enemy: this is the part to reuse for cycling between two enemies. The mode handlers that enumerate the
candidates have not been read yet.

## What has to change for 2 v 2

1. **More than two fighter actors.** The setup builds exactly two. We need four kind-0 actors (two per side) from
   the same constructor, with their model / motion / effect data loaded (see risks).
2. **"The enemy" stops being one actor.** `+0x300` is single-valued. Each fighter needs a current foe (the locked
   enemy of the pair) and `+0x1108` has to be right for the lock fallback.
3. **Control.** One human fighter at a time (the active one), the partner on CPU AI; a tag button swaps who the
   pad drives and who the AI drives. The pad owner flag lives on the fighter's controller object (`+0x5C4`).
4. **Lock-on candidates.** Teach the lock block's cycle to walk the live enemy fighters (and skip KO'd ones).
5. **Win / lose and roster.** KO of the active fighter brings the next roster member in (position, intro, keep the
   bravery / HP rules) and only the last KO ends the round. The party roster (Quick Battle Party "Party A")
   already exists in the menu; today each member is fought in its own scene (inferred, check).
6. **HUD and camera.** Both the HUD (bravery / HP gauges, portraits) and the camera frame two fighters; the camera
   hook in `runtime/camera.cpp` already pivots on the nearest fighter and asks the player's lock state.
7. **Assist / summons / EX Revenge / break logic** read "the" opponent in many places (`+0x300`, `+0x1108`,
   `FUN_08AB32B8`, the EX Revenge manager); each needs a check for the 4-actor case.

## Risks, most likely first

- **Asset loading.** Two more characters means their models, motions, effects, sounds and `plya` tables have to be
  loaded in the same scene budget. Assists show it can be done, but how the loader picks what to load is unread.
- **The 0x12D0-byte actor and shared singletons** (HUD, camera, AI "brain" per fighter, replay recorder) may
  assume exactly slots 0 and 1.
- **Frame-rate patches** (`runtime/framerate.cpp`) track a fixed `g_fighters[4]`, one human and one CPU; a third
  and fourth fighter needs the same time step.
- **Online play** (the adhoc layer) syncs two fighters; tag battles are offline only to begin with.

## Plan (each step is checked in the game before the next)

1. **Spike: a third actor.** Hook the setup, create a third kind-0 fighter on the CPU side with the same
   constructor, link it, and see whether it loads and stands in the battle. Learn what the loader needs.
2. **Foe selection.** Make each fighter's foe the locked enemy; cycle the lock between the two enemies with the
   existing system.
3. **Fourth actor and partner AI.** Player team of two, only one human, the partner runs the CPU brain.
4. **Tag swap button** and active-fighter HUD.
5. **KO flow and roster call-ins**, then round end rules.
6. **Camera and HUD polish**, settings menu option, docs, tests.

## Spike results (14 Nov, `runtime/tagteam.cpp`, `PSPRECOMP_TAGTEAM=1`)

A third kind-0 fighter (a copy of the CPU fighter's setup data, so no new assets) is made after the setup and the
engine accepts it: it is updated every frame, stands where it is placed, becomes "ready" and is started with the others,
and the fight, HUD and result screen carry on. It does nothing yet (idle action, no AI choices, not on the camera).

What it takes (all confirmed in a run):
1. The construction: `FUN_08831320(0x12D0)`, `FUN_088729A0(side)`, `FUN_088470B8(bs + 0x7DC)`, `FUN_08B39E5C(mem, siderec, info, side, 0)`.
2. The setup's per-fighter init for that side: `FUN_08B01090(buf, side)`, then the actor's init through
   `*(actor + 0x544)`-style vtable slot at `*(actor + 1348) + 256` with `(actor + off, G + 0x1060 * side, buf)` where
   `G = 0x09AFB8A0` (per-side records), the two flag bits (`actor + 1456` bytes 7 / 8), and the lock block init
   `FUN_08B3B1B8(actor +0x5AC block, 3 words)`. Without it the actor ticks but never gets a script.
3. Placement `FUN_088359A8(actor, vec4, 1)` and the brain `FUN_08A64078(brain, 2)` / `FUN_08A64100(brain, 15)`
   (`brain = actor + 0x5C4`).
4. **The battle flow step `FUN_088887A4(bs)`** drives the start: it is called every frame until both fighters in
   `bs+56` / `bs+60` are ready (`FUN_08B2F9E0(actor)` = bit 0x10 of byte `actor+1456`), then calls
   `FUN_0882FA10(actor + 304, 1, 0)` (start action 1) for each and sets `bs+72 = 1`. The spike wraps it: it holds the
   flow until the third is ready too, lets the original run, then starts the third.

Test notes: actor addresses differ between runs (do not watch fixed addresses; `PSPRECOMP_TAGTEAM_WATCH=<hex off>` arms
the write watch on the human actor + offset from inside the hook). `PSPRECOMP_TAGTEAM_LOG=1` logs the steps.
Menu fast-forward above 1x misnavigated the menu walk (landed in the story), so the walk runs at normal speed.

Still open for step 1: the third fighter gets no AI actions, is not framed by the camera, and the lock block / foe
cycle is not wired to it.

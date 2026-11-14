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

5. **AI.** The brain (`actor + 0x5C4`) is given the setup's CPU settings: `FUN_08B2F2E4(actor, 0)` and, for control types 1
   and 2, `FUN_08A640CC` / `FUN_08A64100` with record halfwords +3894 / +3896. Two more things were needed: the target
   (`FUN_08B2F17C(actor)` copies the lock block's result into `+0x2FC`; the lock block target is set with
   `FUN_08B3B0B4(block, foe)`), and clearing **bit 2 of `actor+0x380`**, which the CPU brain tick `FUN_08A64400`
   treats as "skip this actor" (set at construction; the retail two fighters lose it after the intro; what clears it
   there is not found yet, the spike clears it every tick).

With all five the third fighter fights: in a run Squall faced two Lightnings and the new one dashed and attacked
(script ids 400 / 460 / 1 / 2 repeating). The brain's per-fighter `partner +0x1108` of a retail fighter is its **assist
actor** (kind 1), which is why `+0x1108` is not a team mate pointer.

Still open: the camera does not frame the third fighter, the lock block's cycle is not wired to it, the human has no
partner yet.

## HUD for the extra fighters and the reserve (14 Nov)

The retail HUD only knows the two original fighters (it reads them by slot through `FUN_089E95B0(side)` = the actor in `bs+56+4*side`).
The extras get their own: **`runtime/tagteam_hud.cpp`** draws a panel for each extra (name, HP bar, KO state; HP is the live word
`actor+0x10B0`, its maximum is the largest value seen) above the retail HUD, and a **reserve strip** per side under the top row
(the waiting fighters, next one highlighted). It needed a small engine feature: `ge_draw_set_game_overlay(rgba, w, h)`
(`runtime/include/psp_ge_draw.h`), a game module's own 480x272 canvas that the presenter blends over the picture (centred on the
480-pixel area in widescreen) and that screenshots include.

- Names: retail charIds 1..33 come from a built-in name table (the roster order; 15 Squall and 31 Lightning are confirmed in runs, the
  other ids are from the roster order and the series codes, so check them in game); mods fall back to their code string.
- The reserve lists are a **placeholder** (`PSPRECOMP_TAGTEAM_RESERVE="id,id;id,id"`, default `5,9;20,24`) until the KO call-ins
  read the real party. `PSPRECOMP_TAGTEAM_HUD=0` hides the HUD.
- Not done yet: bravery (the live BRV is not in the actor; the status object at `actor+0x52C` has floats at +0x18 / +0x1C that may be
  it, unverified), the "active" arrow for the pad-driven fighter, portraits.

## Camera and side-0 extras (tag-camera fork, 14 Nov)

Symptom: with a second fighter on the human's side the game's camera manager (`FUN_08B3CA24`, `[CAMX]` logs) computed
eye = target = (0, 0, 0) (later garbage), so the 3D scene came out grey / white. An extra on the enemy side looked fine.
Reproduced by the constructor alone (`PSPRECOMP_TAGTEAM_STOP=2`), with either character.

Cause (confirmed with RAM diffs around the constructor, `PSPRECOMP_TAGTEAM_REGDUMP/PTRDIFF=1`): `FUN_08B39E5C` registers the
new fighter in shared tables, replacing the retail pair's entries:
- the **camera manager's slot table at `mgr + 0x6D0 + 4 * side`** holds the fighters' lock blocks (slot 0 the human's,
  slot 1 the CPU's, 2 / 3 the assists'); the new fighter's lock block (`actor + 0x5AC`) takes the slot of its side, so the camera
  follows the new, not yet placed fighter. The camera uses side 0 only, which is why the enemy side looked fine;
- it resets four words of the old lock block (`+0x274 +0x278 +0x2A0 +0x314`) and links a new node into the old block's list
  (head at `old_lb - 0x30`);
- a pair table at `0x08BAE9FC / 0x08BAEA08` (CPU, human, human, CPU) gets the new actor instead of the CPU;
- EBOOT global `0x089D4774` (the last created fighter's controller-side object) and a node in the resource pool at `0x08C00xxx`.

The registration has to stay while the fighter loads (without it the actor's ready bit, `FUN_08B2F9E0`, never sets and the flow
waits forever), so `spawn_fighter` records an undo list (snapshot of 0x08800000..0x0A000000 before the constructor, diff after)
and `apply_undo` writes the old values back when the extra is released, in reverse spawn order: before the original flow step
for extras made at setup, in `start_extra` for late ones. Result: the gameplay camera is the retail one again.

Remaining: the resource node (`HE_D001T` / `HE_P001T_P1` for the human's side, `SG_D...` for the enemy's) holds the side's
**intro camera data**. A second fighter on a side re-loads it (the old node is released by `FUN_0886CAA0` / `FUN_0886A140`
and reused, even for a different character), so that side's intro cut shows eye = target = 0 for ~80 frames (and a second
~70 frames for the human's side). Skipping the two releases does not help (the node is reset anyway). Two ways around it:
make the extras after the intro (`PSPRECOMP_TAGTEAM_LATE=2`: the partner is made once the human's script ids >= 1000 have
run and it is idle again: 1 zero camera frame of 622, the frame it loads) or give the extras their own slot objects
(not done). Measured, `[CAMX]` zero frames in the first 400 updates: baseline 0; partner at setup, no fix 400 (all);
with the undo 151; enemy extra with the undo 80; partner late after the intro 1.

Debug switches: `PSPRECOMP_TAGTEAM_NOFIX=1` (no undo), `_KEEP_LAST=1`, `_FREE=1` (do not skip the releases), `_STOP=n`,
`_LATE=1|2`, `_REGDUMP=1`, `_PTRDIFF=1`, `_SCAN=1`, `_WATCHRES=<hex>`.

## 2v2 works (14 Nov, `PSPRECOMP_TAGTEAM=1`)

State of `runtime/tagteam.cpp`: both extras (the enemy team's second fighter and the human's partner, each a copy of the existing
fighter's setup data on a CPU brain) are made **after the intro** (the intro cut's camera data is loaded per fighter, and a second
fighter spawned during it zeroes the camera for ~80 frames; `PSPRECOMP_TAGTEAM_LATE=0` spawns at setup, `=1` right when the battle
starts), started together once both are loaded, and fight: in a 30 s run camera garbage is 1 frame in 500 (the load frame) and the four
fighters change script 22-41 times each. HUD panels and reserve strips show (see the HUD section).

- The fork's undo of the constructor's registrations (camera slot table, lock block, "last fighter" global) stays, **except the pair
  table at 0x08BAE9FC / 0x08BAEA08**: the CPU brain tick of an extra needs its place in it (with it undone the extras idle, only
  the first tick runs). `PSPRECOMP_TAGTEAM_UNDO_PAIR=1` restores the old behaviour for experiments.
- **Lock-on.** L toggles the human's lock through `FUN_08B3B3EC(block, mode, flag)` (mode 1 locked, 0 released; the retail cycle selector
  at mode handler [1] +28 is reset by the press itself). In a tag battle the hook `lock_mode_hook` turns the press that would release
  the lock into a cycle first: foe -> the other living enemy (`FUN_08B40994`, retail "the foe's assist", is hooked to return it, and
  `FUN_08B2F17C` is hooked to write the locked fighter into `+0x2FC`) -> release -> foe. Seen in a run: `lock cycle: foe -> other
  enemy`, `+0x2FC` changed to the extra. Not verified yet: that attacks and the camera really follow the second target in play.

## Tag swap (first attempt, 14 Nov: not working yet, off by default)

`PSPRECOMP_TAGTEAM_SWAP_BUTTON=1` (hex PSP button mask, 1 = SELECT) turns it on; the press is taken out of the pad input by the pad filter
(`dissidia_tagteam_pad`, called from framerate.cpp) and served by the next tick of the human's brain (`swap_roles`). The swap trades the
two fighters' brains (+0x5C4, brain +4 owner) and their places in the battle slot table (`bs+56+4*slot`, `[0x089D6A10]` is the same
`bs`), and re-points the camera manager's lock block slot (`mgr+0x6D0+4*side`). `PSPRECOMP_TAGTEAM_SWAP_MODE` = 1 brains only, 2 slots only,
3 both. Result: the pad does go to the other fighter (the swap logs `pad H -> P`), a second press swaps back and everything recovers, but
- brains only: HUD fine, **the game camera produces garbage** (eye 0 / grey world, ~46 of 700 logged updates);
- slots only: camera garbage too and **the retail HUD breaks** (left portrait / bars missing, right side shows the wrong fighter);
- so the camera and the HUD keep references to the human fighter (cached actor pointers in camera mode objects, HUD widgets) beyond the
  slot table, brain and camera slot.

## Tag swap: findings of the tag-swap fork (14 Nov; the swap still does not work)

**Fixed on the way: the extras' bravery was 0.** The retail HUD reads a fighter's current bravery as the halfword at
`*(actor + 0x52C) + 0x14` (`FUN_089E978C(side)`; the word is `<initial><current>`, both halves = the character's base bravery
`actor + 0x10AC` at battle start: human 0x0434 = 1076, CPU 0x0406 = 1030). The extras were left at 0, i.e. permanently broken
(`FUN_089E9A4C` flips to 1). `start_extra` now writes `(base << 16) | base` when the word is 0. (`status + 0x1C` is not bravery:
it is the assist gauge's draining bar; HP is `status + 0xC` = `actor + 0x10B0`.) A 2v2 run is unchanged otherwise (camera 1 bad
frame in 500, all four fighters act).

**HUD accessors** (`PSPRECOMP_TAGTEAM_HUDPROBE=1` calls the retail HUD getters `0x089E95B0..0x089E9EF4(side)` before and after the
swap): `FUN_089E95B0(side)` = `bs[56 + 4*side]`; `FUN_089E95E0 / 089E9628` = "side is CPU-controlled" (reads the brain: slots-only
swap makes both sides CPU, brains-only keeps them right); `089E96DC / 973C` = HP, `089E978C` = bravery. So the HUD does read the slot
actors live; with brains and slots both swapped and the bravery fixed the getters return the right numbers for both sides, yet the
left HUD still disappears and the right portrait shows the wrong character, so the **portraits / widgets are bound at setup** and not
re-read.

**Shared references to the human** (`PSPRECOMP_TAGTEAM_SWAPSCAN=1`, scans RAM for words equal to H, P, their lock blocks and brains
at swap time): H is held by the slot table (`bs+56`), a pair table at `0x08BAE9FC..0x08BAEA08`, a resource node near `0x08C00720`,
`0x08B63310`, a word near the camera manager (`mgr + 0x6360`, listed as `[CPU, human]`), `0x089D4890`, and ~50 words inside H's own
sub-objects, the CPU's, the extras' and a per-fighter record array at `0x09744xxx..0x09747xxx` (which holds P once per record, so
it is an interaction matrix, not a list to swap). The lock block of H is held only by the camera manager slot (`mgr + 0x6D0 + 4*side`)
and two words in a pool. `swap_roles` now also swaps `mgr + 0x6360`, `0x089D4890`, `0x08B63310` (`PSPRECOMP_TAGTEAM_SWAP_LISTS=<hex mask>`);
the addresses that hold H vary between runs once the extras spawn late (heap layout), so fixed addresses are unreliable and the
camera still shows garbage in 36 of 700 updates after a swap (46 before this change; baseline 1).

Conclusion: moving the retail slot between two actors needs the same bookkeeping the constructor does for a new fighter (camera
slot table, lock block list, pair table, "last fighter" global, the HUD's setup-time bindings), found by RAM diffs at the right moment, not by
patching a fixed list. Suggested next step: instead of swapping existing actors, **re-run the retail registrations** for the pair:
snapshot RAM before and after constructing a fighter into slot 0 (the fork's undo machinery does exactly this) and, at swap time,
write P's "after" values for the retail registrations and H's "before" values for the extra, or give the HUD and camera a single
fixed "player" actor and swap the character data under it (clones only).

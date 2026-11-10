# Savestates (task 11): why this is tabled

Wanted: save and load states in single player (snapshot, restore, disabled in multiplayer). Status: **not started, blocked by the execution model**.

## The blocker

A PSP thread is a real OS thread running recompiled C++ (`docs/SCHEDULER-DESIGN.md` §1.1: one `std::thread` per guest thread, each with its
own `recomp_context`). Where a guest thread is "in" the program (which recompiled function it is inside, its locals, the return addresses
of the nested C++ calls the translation turns guest `jal`s into) lives on the thread's native stack, not in guest memory or in
`recomp_context`. Snapshotting `rdram` (128 MB) and the registers is therefore not enough: after a restore the native stacks would have to
come back too, and a C++ stack cannot be serialized and re-entered portably.

(An emulator such as PPSSPP can save states because its guest threads are data: registers + stack in emulated memory + a scheduler queue.)

## What a savestate would have to capture anyway

`rdram`; every guest thread's registers and wait state (`PspThread`); kernel objects (semaphores, event flags, mutexes, lw-mutexes,
message pipes), their wait queues and timers; the scheduler's virtual time; HLE state (open files and positions, `sceGe` list queue and the
whole `GeState`, texture cache contents are rebuildable, display/vblank counters, audio/SAS voices and the ATRAC decoder positions, MPEG
streams, savedata dialog state, OSK, ad-hoc sockets); the game module's own hook state (frame-rate hook counters, input latches, camera).

## Ways forward (each is a project of its own)

1. **Process-level checkpoint** (CRIU on Linux; needs privileges and is not available on Windows). Captures everything including native stacks,
   but a GPU context cannot be checkpointed: the GL/SDL side would have to be separated into another process first.
2. **Stackless guest threads**: change the emitter so a call to another guest function is not a native call but returns to a trampoline with
   the guest stack in `rdram` (the scheduler design doc's §3 discusses PPSSPP's single-thread model). That also fixes the scheduling races
   the design doc describes, but it touches every generated function and every HLE blocking call.
3. **Quick-restart only**: save at points where every guest thread is parked in a known HLE wait (for example at a vblank wait with the
   menu paused: `psp_settings_guest_wait` already stops the guest there). Only `rdram` + registers of those wait points + kernel objects
   would be needed, but each thread's native stack would still have to be unwound to that point, so this needs option 2's trampolines.

Until one of these is chosen there is nothing useful to implement here; the in-game "settings" pause (all guest threads wait at their next
vblank) is the closest existing facility.

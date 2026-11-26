#pragma once
// Netplay helpers on top of the ad-hoc emulation (issue #66, first steps): an input-delay setting and a desync check.
//
// Game-agnostic. The game module knows where its lock-step battle lives: it applies the input delay to the game's own
// protocol (psp_netplay_input_delay) and hands this module the battle state of every logic frame it runs
// (psp_netplay_submit_state). This module sends those values to the other players over the ad-hoc transport (a
// reserved PDP port the game never uses, so it also works through the party relay), compares them frame by frame and
// reports the first frame and field that differ:
//   [NETSYNC] DESYNC at frame 1234 with 02:..: 3 of 40 fields differ, first fighter0.pos.x local 3F800000 remote 3F800100
// plus a toast, and a summary line every 10 s and when a battle ends: [NETSYNC] frames compared 900, mismatched 0.
//
// Environment / settings:
//   PSPRECOMP_NET_DELAY=1..8         the network input delay in frames (default 2; settings.ini net_input_delay;
//                                    Settings > Network > Network Delay). Old name: PSPRECOMP_NET_INPUT_DELAY.
//   PSPRECOMP_NET_SYNCCHECK=0|1      the desync check (default 1)
//   PSPRECOMP_NET_TRACE=1            log every PTP / PDP message with the frame number ([NETTRACE]); the game module adds
//                                    its lock-step state per update ([NETLS])
#include <cstddef>
#include <cstdint>

/// The game module calls this once per frame end. `battle`: a battle is running.
void psp_netplay_frame_end(bool battle);
/// Frame ends seen so far.
uint64_t psp_netplay_frame();

/// THE input delay of online battles, in frames (1..8, default 2): the game module's pad link and the tag team's lock-step
/// both use it. Env PSPRECOMP_NET_DELAY (old name PSPRECOMP_NET_INPUT_DELAY); Settings > Network > Network Delay.
int psp_netplay_input_delay();
void psp_netplay_set_input_delay(int frames);
constexpr int PSP_NETPLAY_MIN_DELAY = 1, PSP_NETPLAY_DEFAULT_DELAY = 2, PSP_NETPLAY_MAX_DELAY = 8;

/// Desync check on / off (on by default; PSPRECOMP_NET_SYNCCHECK=0 or the Debug page turn it off).
bool psp_netplay_sync_check();
void psp_netplay_set_sync_check(bool on);
/// Names of the values psp_netplay_submit_state sends, in order (static strings; used in the report only).
void psp_netplay_set_fields(const char* const* names, int count);
/// The state at the start of logic frame `frame` of the current lock-step battle (before that frame's update). Frames
/// must be numbered the same way on every player (the game's lock-step frame counter). A frame number lower than the
/// previous one starts a new battle (the old one's summary is logged).
void psp_netplay_submit_state(uint32_t frame, const uint32_t* values, int count);

/// The first frame of the current battle whose values differed from another player's (0xFFFFFFFF = none yet).
uint32_t psp_netplay_first_mismatch();

/// Called by the ad-hoc layer for a desync-check message from another player.
void psp_netplay_on_message(const uint8_t src_mac[6], const uint8_t* data, size_t n);

/// PSPRECOMP_NET_TRACE=1: every PTP / PDP send and receive with data, with the frame number and the first bytes.
bool psp_netplay_trace_on();
void psp_netplay_trace_ptp(const char* what, int sock, const uint8_t* data, uint32_t n);

#pragma once
// PSP ad-hoc networking emulation (sceNetAdhoc*, sceNetAdhocctl*, sceNetAdhocMatching*).
//
// Transport: UDP. Instances find each other with broadcast beacons on the LAN (and on
// 127.0.0.1 for several instances on one machine), or by direct address through
// PSPRECOMP_ADHOC_PEERS for internet play (forward the UDP port or use a VPN).
//
// Environment:
//   PSPRECOMP_ADHOC=0            turn the ad-hoc emulation off (the old no-op stubs remain)
//   PSPRECOMP_ADHOC_MODE=lan|internet  how players find each other (default internet; lan when
//                                 PSPRECOMP_ADHOC_PEERS is set and no mode is given)
//   PSPRECOMP_ADHOC_SERVER=h[:p] the relay server for Internet mode (default party.schindlershadow.com:27500)
//   PSPRECOMP_ADHOC_ROOM=<name>  room on the server; players in one room see each other (default: the public room)
//   PSPRECOMP_ADHOC_PORT=<n>     first UDP port to try (default 27312; tries n..n+7)
//   PSPRECOMP_ADHOC_PEERS=a,b    LAN mode: extra peers to contact directly: host or host:port, comma separated
//   PSPRECOMP_ADHOC_NICK=<name>  nickname shown to other players (default: what the game module passes to psp_adhoc_set_nickname
//                                 (Dissidia: the save's player name), else "Player")
//   PSPRECOMP_ADHOC_MAC=aa:bb:.. override the virtual MAC (default: random per launch)
//   PSPRECOMP_ADHOC_LOG=1        log every packet-level event
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct recomp_context;

void psp_hle_register_adhoc();

/// Run queued guest callbacks (adhocctl handlers, matching callbacks) on the calling guest
/// thread. Safe to call often; does nothing when the queue is empty or a dispatch is running.
void psp_adhoc_pump(uint8_t* rdram, recomp_context* ctx);

/// The frame rate (30 or 60) this instance would like. 0 = unknown. Sent to other players.
void psp_adhoc_set_local_fps(int fps);
// A game module's own bytes in the beacons (at most 24): what it wants the other players in the group to know (a mode, a checksum).
void psp_adhoc_set_game_data(const uint8_t* d, int n);
// The game bytes of the other players in this player's group: returns how many are in the group (0 = not in a group).
int psp_adhoc_group_game_data(uint8_t (*out)[48], int* lens, int max, uint8_t (*macs)[6] = nullptr);   // macs: each player's MAC, same order

/// The frame rate the whole connected group should run at: the lowest of every player's
/// preference, or 0 when not in a group with anyone else (no limit).
int psp_adhoc_group_fps_cap();

/// True while this instance is in an ad-hoc group with at least one other player.
bool psp_adhoc_in_session();
bool psp_adhoc_connected();
/// Players in this instance's ad-hoc group, itself included; 0 when not in a group (the Discord party size).
int psp_adhoc_group_size();
/// The nicknames of the other players in this instance's group (UTF-8, as they sent them).
std::vector<std::string> psp_adhoc_group_nicks();
/// A string every member of the group gets the same (room + group name), "" when not in a group (the Discord party id).
std::string psp_adhoc_group_key();
/// The nickname other players see: a game module passes the player name of the save (UTF-8, cut to 31 bytes). Empty = "Player".
/// PSPRECOMP_ADHOC_NICK, when set, wins.
void psp_adhoc_set_nickname(const char* utf8);

/// Change the network setup at run time (the settings menu). `internet` selects relay-server mode;
/// `server`/`room` apply to it, `peers` (comma separated) to LAN mode. Takes effect within a few
/// milliseconds; players found so far are forgotten.
void psp_adhoc_configure(bool internet, const char* server, const char* room, const char* peers);

/// Side channel for a game module (e.g. sharing verified mod packs with the other players): datagrams to the
/// reserved PDP port PSP_ADHOC_SIDE_PORT go to `handler` instead of the game's sockets. The handler runs on the
/// network thread: it must not touch guest memory and must treat the bytes as untrusted. Only players in the same
/// ad-hoc group are heard. Messages up to PSP_ADHOC_SIDE_MAX bytes; `mac` all 0xFF = everyone in the group.
constexpr uint16_t PSP_ADHOC_SIDE_PORT = 0xD1D1;
constexpr size_t PSP_ADHOC_SIDE_MAX = 16000;
void psp_adhoc_side_set_handler(void (*handler)(const uint8_t mac[6], const uint8_t* data, size_t len));
bool psp_adhoc_side_send(const uint8_t mac[6], const uint8_t* data, size_t len);
/// The MACs of the players in this instance's ad-hoc group (empty when not in one); `own` gets this instance's.
void psp_adhoc_group_peers(uint8_t (*out)[6], size_t max, size_t* count, uint8_t own[6]);
/// A game module's say on who may start a match with this player: matching packets from a player for whom
/// `allow` returns false are dropped (they never reach the game, so no match with them can start).
void psp_adhoc_set_match_filter(bool (*allow)(const uint8_t mac[6]));

/// Called on a guest thread at the start of every psp_adhoc_pump (while the network is up), before the queued
/// callbacks run: where a game module may change guest memory in reaction to the network (side channel).
void psp_adhoc_set_pump_hook(void (*hook)(uint8_t* rdram));

/// A one-line description of the network state for the settings menu.
void psp_adhoc_status(char* out, size_t n);

/// One line on screen and in the log about the network (a lost player, the server not answering, high latency, ...), shown at most
/// once per `min_interval_s` for the same `key`. Also for game modules (their battle link).
void psp_net_notice(const char* key, int min_interval_s, const char* fmt, ...);
/// A network notice is on screen right now (other messages should not replace it).
bool psp_net_notice_showing();
/// The round trip to the slowest player in our group, in ms (smoothed, measured once a second), or -1 when unknown (no group, or the
/// others run a build that does not answer pings).
int psp_adhoc_ping_ms();
int psp_adhoc_ping_loss_permille();   // lost pings of the worst path since the players joined, in 1/1000 (0: not known yet)
int psp_adhoc_ping_jitter_ms();   // jitter of that path (mean deviation of single round trips, ms); -1 with no ping
// Side channel for a game module: datagrams to one player (or the broadcast MAC ff:..:ff) of the group on channel `chan`, carried like the
// game's PDP traffic on ports the game never uses. _recv pops the oldest message of that channel (-1 = none). Not reliable, not ordered.
void psp_adhoc_local_mac(uint8_t out[6]);
int psp_adhoc_group_macs(uint8_t (*out)[6], char (*nicks)[32], int max);   // the other members of this player's group (MAC, adhocctl nickname)
void psp_adhoc_side_send(const uint8_t mac[6], uint16_t chan, const uint8_t* d, int n);
int psp_adhoc_side_recv(uint16_t chan, uint8_t src[6], uint8_t* buf, int max);
/// Send a runtime message (not the game's) to every player in this instance's group: a PDP datagram on a reserved
/// port; the receivers hand it to psp_netplay_on_message. Does nothing when not in a group.
void psp_adhoc_send_aux(const uint8_t* data, size_t n);

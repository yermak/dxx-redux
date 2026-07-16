# Dedicated Game Server for DXX-Redux (d1) — Design

Date: 2026-07-12
Status: Approved (brainstorming complete)
Branch: `gameserver`

## Problem

Descent multiplayer today requires one player to host: everyone else connects to that
player's machine. Restrictive ISPs (CGNAT, blocked inbound UDP, symmetric NAT) often make
a player unhostable, and sometimes make any pair of players unable to reach each other.
The existing tracker/NAT-punch assists don't help when the provider blocks inbound
traffic outright.

## Goals (v1)

- A separate **server app** deployable on a machine with a public address (Docker image
  on a cloud provider; also runnable locally for testing).
- A player "creates a game" from the client UI; this **creates the session on the
  server**. Players — including the creator — connect **to the server**, never to each
  other's home connections.
- The server **hosts each game but is not a player**: no ship, no player slot consumed,
  absent from the kill matrix.
- The server runs **multiple games in parallel** and **closes a session when everyone
  has left**.
- **All game modes** work, including co-op and robo-anarchy.
- Keep it simple; improvements (auth, admin tools, d2) come later.

## Non-goals / future work

- d2 port (follow-up after d1 is validated, mirroring the minimap precedent).
- Create-password / authentication, admin commands (kick, close session), server-side
  bans.
- Tracker registration of server-hosted games (discovery is via the in-client server
  browser in v1).
- Orphaned-child watchdog (children already self-terminate when empty).
- Host migration (not applicable — the host never leaves).

## Architecture

**Chosen: headless engine instances behind a small broker.**

Each game session is the real `d1x-redux` engine running headless as a non-playing
observer-host. A small broker process accepts create/list requests, spawns one engine
process per session, and hands players the port to join. Rationale: co-op and robot
modes work by construction (the server runs the actual simulation), "host but not a
player" reuses the engine's existing `host_is_obs` observer-host support, and the
in-game wire protocol stays untouched.

Alternatives rejected:

- *Pure relay server*: creator would remain the authoritative host, so the game dies
  when the creator quits, and the host stays a player. Fails two core requirements.
- *Reimplemented standalone protocol server*: acting as host without the engine means
  reimplementing robot AI/physics/level logic — a fork of the simulation with permanent
  protocol-drift risk. Not viable with co-op in scope.

```
                 ┌────────────────────────── server machine / container ──┐
                 │  d1x-gameserver (broker)          UDP :42424           │
 client ────────►│   • GSP_CREATE_REQ / GSP_LIST_REQ                      │
 "Game Server"   │   • session table {port, pid}                          │
 menu            │   • spawns / reaps children, polls them with           │
                 │     existing UPID_GAME_INFO_REQ (lite)                 │
                 │                                                        │
                 │  d1x-redux -dedicated             UDP :42425           │
 client ────────►│   (session 1: full engine, headless observer-host)    │
 joins game      │  d1x-redux -dedicated             UDP :42426           │
 directly        │   (session 2)                     ...                  │
                 └────────────────────────────────────────────────────────┘
```

## Components

### 1. Broker: `d1x-gameserver` (new binary, new `gameserver/` source dir)

- Single-threaded UDP server on the control port (default **42424** — players enter just
  the server address).
- No SDL or engine dependencies: sockets plus a process-spawn shim (`fork`/`exec` on
  Linux, `CreateProcess` on Windows for local testing).
- Maintains a session table `{port, pid, created_at}`. Ports for instances come from a
  configurable base/count (default 42425, 16 slots).
- **Stays dumb**: never parses game rules and never speaks the game protocol beyond the
  existing `UPID_GAME_INFO_REQ` (lite info). It polls its own children with that request
  to build browser listings (name, mode, mission, players, level) and to confirm a
  freshly spawned child is up before acknowledging creation. No new parent↔child IPC.
- Configuration via command-line flags and equivalent env vars (Docker-friendly):
  control port, instance port base/count, max sessions (default 8), timeouts, path to
  the engine binary, `-hogdir` pass-through for children.

### 2. Engine `-dedicated` mode (in `d1x-redux`)

Launch: `d1x-redux -dedicated <session-file> -hogdir <dir> -nosound -notitles`.
The session file (written by the broker to a temp dir) is a small `key=value` text
file (`port`, timeouts, optional `blob=<path>` pointing at the packed `netgame_info`
bytes; without a blob, plain keys `game_name`/`mission`/`level`/`mode`/`maxplayers`
configure a defaults game — used by the broker's `--test-create` smoke test and manual
runs). The child deletes both files after reading. Dedicated children do not open the
LAN-broadcast socket (the broker owns the default port on the server machine).

- **No video**: skips window/renderer init entirely and runs its own main loop instead
  of the window-event pump: `calc_frame_time()` → `GameProcessFrame()` → net pump →
  sleep. These are the same per-frame calls `EVENT_WINDOW_DRAW` drives today
  (`main/game.c:1021-1037`); `game_render_frame()` is never invoked, so no GL context or
  SDL video subsystem is created. Works in both OpenGL and software builds. Sim rate
  uses existing `-maxfps` handling (default 60; packet rate maxes at 40/s).
- **Menu-less hosting flow**: load mission → unpack the netgame blob into `Netgame` →
  apply the unchecked-host path of `net_udp_select_players()`
  (`main/net_udp.c:5145-5150`: `Netgame.host_is_obs = 1`, `Host_is_obs = 1`,
  `GM_OBSERVER`, slot 0 reserved without a ship) → open sockets, generate tokens →
  `StartNewLevel(levelnum)` → `NETSTAT_PLAYING` with zero players. Every player join
  uses the existing join-in-progress path (`net_udp_welcome_player` → rejoin sync).
  A synthetic pilot ("SERVER") avoids pilot-selection UI.
- **Auto-behaviors**, gated by one global `Dedicated_server` flag:
  - Join policy forced **Open**: `RefusePlayers` approval prompts and closed-game
    refusal paths never trigger.
  - **Level advance**: where host code today waits in the kmatrix score screen,
    dedicated mode auto-advances after the engine's existing score-display period
    (`KMATRIX_VIEW_SEC`, 7 s — kept identical to what clients already use so both
    sides stay in step), cycling levels in anarchy-type modes (the stock engine ends
    the game at the last level; dedicated wraps to level 1). Co-op mission completion
    ends the session gracefully.
  - Never pauses, ignores all input, records no demos. HUD messages buffer harmlessly
    (they are only drawn in the render frame, which never runs).
  - Self-termination: exits when no player has joined within 5 minutes of spawn, or
    when the last player leaves and a 60-second grace period passes.
  - Robot modes: `MULTI_ROBOT_PRIORITY` (`main/multibot.c:76`) already skips the
    observer host in the control rotation. Add an explicit guard for the
    zero-real-players case (`N_players == 1` with `host_is_obs` makes the existing
    modulus zero — audit/fix as part of implementation).
  - Logs to stdout via existing `con_printf`, so `docker logs` works.
- **Host-side UI audit**: any `nm_messagebox`/menu invocation reachable from the net
  code on the host (join prompts, endlevel/kmatrix windows, error boxes) gets a
  dedicated-mode branch that logs and takes the default action instead of opening UI.

### 3. Client additions (in `d1x-redux`)

- Multiplayer menu gains **"GAME SERVER"** with: `[Server address]` (persisted in the
  machine-level config via `main/config.c` (`GameCfg`), not per-pilot; last-used
  remembered), `[Browse games]`, `[Create game]`.
- **Browse**: `GSP_LIST_REQ` → list of sessions (name, mode, mission, players, level) →
  selecting one joins via the existing direct-join flow to `server_ip:port`, preserving
  the current join-as-player / join-as-observer choice.
- **Create**: opens the existing netgame setup menus unchanged (players keep all their
  options), except the Open/Closed/Restricted choice is forced to Open. On confirm the
  client packs the configured `netgame_info`, sends `GSP_CREATE_REQ`, and on success
  **auto-joins** the new session as its first player. The creator has no special status
  afterward; the session outlives them.
- Errors (unreachable server, full, version mismatch, spawn failure) surface as normal
  message boxes. Request timeout ~3 s with a couple of retries.
- Standard mission rule unchanged: a player must have the mission files locally to play.

## Broker protocol (GSP)

New UDP opcodes on the **broker port only**; the in-game wire protocol is untouched and
`MULTI_PROTO_VERSION` is not bumped. Clients without this feature can still join
server-hosted games by direct IP:port.

All packets start with `{opcode:u8, gsp_version:u8}`; integers little-endian, packed
with the existing `PUT_INTEL_*`/`GET_INTEL_*` conventions.

| Opcode | Direction | Payload |
|---|---|---|
| `GSP_CREATE_REQ` | client→broker | client `MULTI_PROTO_VERSION` (u16, carried for future use), blob length (u16), packed `netgame_info` blob (same packing as `net_udp_send_game_info()`, `UPID_GAME_INFO` format) |
| `GSP_CREATE_ACK` | broker→client | result code (OK / full / spawn failed / rate limited / bad request), game port on success |
| `GSP_LIST_REQ` | client→broker | (header only) |
| `GSP_LIST_ACK` | broker→client | count + per-session `{port, game name, mission name, level, mode, players, max players, status}` from cached polls |

The child unpacks the blob with existing deserialization code, then overrides
host-specific fields: `host_is_obs`, addresses/tokens, forced-Open join policy, port.
The broker parses nothing about the game — not even mission/level: a child that cannot
load its mission exits before answering the readiness poll, which the broker reports as
spawn failure.

Version check (v1): the broker does not compare game-protocol versions. The creator
auto-joins immediately after `GSP_CREATE_ACK`, and the existing join handshake performs
the full version comparison and shows the standard mismatch message; a session created
by an incompatible client simply times out empty. The child additionally validates that
the blob was produced by the same engine version triple.

Listing freshness: the broker polls each child with `UPID_GAME_INFO_REQ` (lite) every
~5 s and answers `GSP_LIST_REQ` from that cache.

Create flow: broker validates version + capacity → spawns child → polls the child's
game port with `UPID_GAME_INFO_REQ` until it answers (ready) or exits/times out (~10 s)
→ replies `GSP_CREATE_ACK`. Mission-availability errors need no special handling in the
broker: a child whose mission fails to load exits quickly, which the broker reports as
spawn failure.

## Session lifecycle

1. `GSP_CREATE_REQ` accepted → child spawned on next free port → `GSP_CREATE_ACK(port)`.
2. Creator auto-joins; others join via Browse (or direct IP). Join/leave freely;
   join-in-progress is the norm.
3. Child self-terminates when empty (never-joined 5 min / all-left 60 s grace) or on
   co-op mission completion. Crash is equivalent to termination.
4. Broker reaps the child and frees the port slot for reuse.
5. Broker shutdown terminates all children.

Abuse limiting in v1: global session cap (default 8) + per-IP create rate limit
(e.g. one create per 5 s per IP). Anything stronger (shared secret) is future work.

## Compatibility notes

- Gameplay protocol unchanged; no `MULTI_PROTO_VERSION` bump. Old redux clients can
  join server games via direct IP (they just lack the browser/create menus).
- GSP carries its own `gsp_version` byte for future evolution; the child additionally
  enforces game-protocol compatibility on join exactly as today.
- RetroProtocol (P2P) games keep working: players who can reach each other directly
  still do, and the existing proxy-through-host fallback automatically relays via the
  server for pairs whose NATs block direct traffic.

## Deployment

- **`d1/Dockerfile`** (multi-stage):
  - Build stage: gcc/cmake/ninja + SDL 1.2 + PhysFS dev packages; configure
    `-DOPENGL=OFF -DOPENGLMERGE=OFF -DSDLMIXER=OFF -DPNG=OFF -DTRACKER=OFF` (server
    never renders; the software build keeps the runtime image free of GL/audio/png
    deps, and tracker code is client-side anyway).
  - Runtime stage: slim base with `d1x-gameserver`, `d1x-redux`, and runtime libs.
  - `EXPOSE 42424/udp 42425-42440/udp`; knobs via env; entrypoint `d1x-gameserver`.
- **Game data is never baked into the image** (copyright): `descent.hog`/`descent.pig`
  and an add-on missions folder are volume-mounted (e.g. at `/data`, passed to children
  via `-hogdir`). Documented in a README section with a `docker-compose.yml` example.
- Local testing: native Windows run (broker spawns children via `CreateProcess`) for
  dev iteration; the compose file covers the containerized path.

## Error handling summary

- Broker ignores malformed/unknown packets; rate-limits creates per IP; caps sessions.
- Child that never becomes ready (bad mission, port bind failure) is killed/reaped and
  reported via `GSP_CREATE_ACK` error.
- Client-side timeouts produce message boxes; joining a full/vanished session falls
  back to existing join-failure behavior.
- In-game failures (player crash/disconnect) use existing timeout-and-dump handling;
  an empty session then closes itself.
- Edge cases noted for implementation: reactor countdown finishing with zero players
  (level advances; empty-timeout still applies), robot-control rotation with zero real
  players (guard the modulus), endlevel while all players simultaneously drop.

## Testing plan

No automated suite exists in this repo; verification is compile + scripted/manual runs:

1. Both ports still compile (d2 untouched but built to confirm).
2. Local loop (Windows native): broker + 2 clients on localhost — create via menu,
   browse, join, play anarchy; both leave → session closes, port reused on next create.
3. Two sessions in parallel; join-in-progress on both.
4. Co-op smoke test: robots sync, level advance on exit, mission completion closes the
   session. Robo-anarchy smoke test.
5. Error paths: server unreachable (timeout box), server full, create with a mission
   the server lacks, client killed mid-game (timeout dump), idle session auto-close.
6. Docker: build image, mount data, run; client on the host machine performs the full
   create/browse/join/play/leave loop against the container.
7. Latency sanity check: in-game ping via server comparable to a locally hosted game on
   the same network.

## Implementation risks / audit items

- `GameProcessFrame()` call graph must be audited for stray draw/UI calls when running
  without video (known suspects: kmatrix/endlevel windows, host join prompts,
  `multi_do_frame` message paths).
- Kmatrix/endlevel auto-advance: reuse the existing host timeout path if present
  (`net_udp_kmatrix_poll1/2`), otherwise add a dedicated-mode timer.
- `MULTI_ROBOT_PRIORITY` division by zero with an empty dedicated server (pre-existing
  observer-host edge case).
- Linux build health: the port is Windows-developed; the Dockerfile build stage is the
  validation vehicle for the Linux target.
- SDL subsystem initialization without video on a headless host (timer works without a
  display in SDL 1.2; verify no code path demands `SDL_INIT_VIDEO` in dedicated mode).

## Key code references

- Observer-host machinery: `main/net_udp.c:5127-5155` (`net_udp_select_players`),
  `Host_is_obs` in `main/multi.c:159`, `main/multi.h:524`, `Netgame.host_is_obs`
  consumers across `player.c:90`, `fireball.c:591`, `gameseq.c:244`, `gauges.c`,
  `multibot.c:76`, `net_udp.c:6968`.
- Game loop separation: `main/game.c:1021-1037` (`EVENT_WINDOW_DRAW` →
  `GameProcessFrame()` / `game_render_frame()`).
- Join handling: `main/net_udp.c:1913` (`net_udp_welcome_player`), open/closed/
  restricted logic at `net_udp.c:1995-2006`, restricted prompt at `net_udp.c:3477`.
- Netgame serialization: `main/net_udp.c:2873` (`net_udp_send_game_info`),
  `net_udp.c:3116` (`net_udp_process_game_info`).
- Default port: `main/net_udp.h:33` (`UDP_PORT_DEFAULT` 42424).

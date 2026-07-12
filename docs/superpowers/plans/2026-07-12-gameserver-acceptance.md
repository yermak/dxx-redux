# Dedicated Game Server — User Acceptance Checklist

The implementation is complete and reviewed (per-task + whole-branch, all clean).
Everything verifiable without a human at a game window was checked automatically
during the build (headless boot, lifecycle self-exit, UDP protocol probes, the broker
spawning a live child on both Windows and inside the Linux Docker image). The items
below need a person driving real game clients and are for you to run before merge.

## Setup

Build is current in the worktree. From `d1/`:
- Broker: `./build/gameserver/d1x-gameserver.exe --engine "$(pwd)/build/main/d1x-redux.exe" --hogdir C:/Users/Yermak/Projects/dxx-redux/d1/hogs`
  (prefix with `PATH=/c/Programs/msys64/mingw64/bin:$PATH` so the child finds its DLLs)
- Client(s): `./build/main/d1x-redux.exe -hogdir C:/Users/Yermak/Projects/dxx-redux/d1/hogs -notitles -pilot <name> -udp_myport <46000+>`
  (give each client a distinct `-udp_myport` so they don't clash with each other or the broker)

## Checklist

### Core loop (anarchy)
- [ ] Client A: Multiplayer → GAME SERVER → address `localhost`, port `42424` → CREATE GAME
      → pick Descent: First Strike → set a game name → Start. Broker logs spawn + ready;
      client auto-joins and spawns in the mine, flying, alone.
- [ ] Confirm the HUD scoreboard does NOT show a "SERVER" player row (observer-host is hidden).
- [ ] Client B: Multiplayer → GAME SERVER → BROWSE GAMES → the session is listed → select → join.
- [ ] A and B see each other, can shoot and kill each other; kill matrix updates on both.
- [ ] Both quit. Within ~60 s (the empty-timeout) the broker logs the session exited and
      the port frees; creating again reuses it.

### Persistence & host-defaults isolation (the one review-fixed risk)
- [ ] After using CREATE GAME once, go to ordinary HOST GAME → confirm your Open/Closed/
      Restricted preference is UNCHANGED (the fix in commit 0c9cf7e). Also confirm the
      GAME SERVER address/port fields are pre-filled from last time.

### Parallel sessions
- [ ] Create a second game while the first runs; both appear in BROWSE; join one from each client.

### Co-op (all-modes requirement)
- [ ] CREATE GAME → Cooperative → join with two clients. Robots present and synced (shoot the
      same robot, HP agrees; kills attributed). Pick up a key → both clients can open its doors.
- [ ] Both players reach the exit → score screen (~7 s) → level 2 loads on both, server following.
- [ ] Join-in-progress: with one player mid-level, join the second via BROWSE → they enter the
      current level with correct robot state.
- [ ] Play the co-op mission to completion → session ends gracefully (server logs mission complete,
      closes; clients return to menu).

### Robo-anarchy
- [ ] CREATE GAME → Robo-anarchy → join → robots attack, robot kills count, no crash.
      (Headless already confirmed the empty-session divide-by-zero guard; this confirms live play.)

### Error paths
- [ ] BROWSE with the broker stopped → "No response from game server" box.
- [ ] Create when the server is full (raise sessions past `--max-sessions`, default 8) → "Server is full".
- [ ] Kill a client process mid-game → the other client sees the disconnect after the normal timeout.

### Docker (deployment target)
- [ ] `docker compose up -d` from `d1/` (with game data in `./hogs`) → connect a client to the
      host machine's address → full create/browse/join/play/leave loop against the container.
      `docker logs` streams the broker's session events in real time.

### Latency sanity
- [ ] In a server game on localhost, the in-game ping display is single-digit ms (no systematic
      delay beyond the host-relay model that already exists).

## Known cosmetic follow-ups (from whole-branch review — not blockers)

- Browser mission column is blank for the builtin Descent mission (broker relays the 9-byte
  mission *filename*, which is empty for builtin; add-on missions show their stem). Could read
  the long `mission_title` instead for parity with the LAN netlist.
- Browser player count includes the non-playing host (empty session shows `1/8`). Pre-existing
  observer-host accounting; the lite packet doesn't carry `host_is_obs` for the broker to subtract.
- GSP replies aren't source-address validated during the create/browse window (spec defers auth).
- Docker image runs as root with no `HEALTHCHECK`/`USER` (standard hardening).
- Windows-only: force-killing the broker orphans child engines (SIGTERM handler is POSIX; the
  Linux/Docker deployment target shuts down cleanly).

## d2 port

Deferred by design (follow-up, mirroring the minimap precedent). This branch also fixed a
pre-existing d2 build break (SEH guard, commit a5fd2f5) so `d2` compiles on the MinGW gcc
toolchain — unrelated to the feature, but required for "both ports compile" verification.

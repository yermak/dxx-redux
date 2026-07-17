# DXX-Redux Game Server — how it works, how it's deployed, and why

This document explains the dedicated game server for Descent 1 (d1x-redux): the
problem it solves, its architecture, the connection flow, how it is deployed, and
the reasoning behind the design.

> See also: [`README.md`](README.md) (running the broker) and
> [`DEPLOY-GCP.md`](DEPLOY-GCP.md) (from-zero Google Cloud setup).

## The problem it solves

Descent's multiplayer is peer-hosted: one player is the "host" and everyone else
connects *to that player's machine*. That breaks constantly on modern home
internet — CGNAT, symmetric NAT, and ISPs that block inbound UDP mean many players
simply can't be reached from outside, and sometimes no two players can reach each
other at all. The tracker / NAT-punch assists don't help when the provider blocks
inbound traffic outright.

The game server flips the direction: it runs on a machine with a **public IP**, and
every player — including whoever "created" the game — makes an *outbound* connection
to that server. Outbound connections pass through NAT/CGNAT fine, so connectivity
stops depending on any player being reachable. The server hosts the game but is
**not a player**: no ship, no slot in the kill matrix.

## How it works

Two pieces run on the server:

```
                    server machine (one public IP)
   ┌──────────────────────────────────────────────────────────┐
   │  d1x-gameserver  (the "broker")        UDP :42424         │
   │    • tiny standalone C program, no game engine inside     │
   │    • speaks GSP (create / list) to clients                │
   │    • spawns + reaps one engine process per game           │
   │                                                           │
   │  d1x-redux -dedicated   :42425   ← session 1 (real engine)│
   │  d1x-redux -dedicated   :42426   ← session 2              │
   │  d1x-redux -dedicated   :4242N   ← ...up to max-sessions  │
   └──────────────────────────────────────────────────────────┘
        ▲ create/browse (:42424)         ▲ actual gameplay (:4242N)
        │                                │
     client ─────────────────────────────
```

### The broker (`d1x-gameserver`)

A small process that only does traffic-directing. It listens on UDP **42424** and
understands a tiny protocol called **GSP** (four message types: create-request/ack,
list-request/ack). It deliberately knows *nothing* about game rules — it never
parses a netgame's settings. Its whole job is: accept a "create a game" request,
launch a game process, and answer "what games are running?". It is standalone C with
no game-engine or SDL dependencies.

### Each game session is the real engine

Rather than reimplement Descent's networking in a lightweight server, **each game is
a real, unmodified engine instance** launched headless as `d1x-redux -dedicated`.
This is the central design decision. Each instance:

- runs with **no video, no sound, no input**, on its own main loop;
- hosts the netgame as a **non-playing observer-host**, reusing an existing engine
  concept (`host_is_obs`) that already meant "this host watches but doesn't play";
- runs the genuine simulation — physics, robots, collisions, everything.

Because it *is* the real engine, every game mode works by construction — anarchy,
team, robo-anarchy, co-op — with no separate server logic to keep in sync.

### The connection flow (from a player's side)

1. In the client, **Multiplayer → GAME SERVER**, with the server address pre-filled
   (defaults to `eu.descent.one`).
2. **Create** packs the chosen netgame settings into a blob and sends it to the
   broker on :42424. The broker spawns a `-dedicated` engine on the next free port
   (42425+), waits until that engine answers a readiness poll, then replies with the
   port. The client then **auto-joins** that port as the first player — using the
   *normal* join handshake, unchanged.
3. **Browse** asks the broker for its list of running games. The broker builds that
   list by periodically polling each child engine with the game-info request the
   engine already answers. Selecting a game joins it via the normal join path.
4. When the **last player leaves**, that engine self-terminates (after a short grace
   period), the broker reaps it, and the port frees for reuse. New sessions that
   nobody joins also time out and close.

### Protocol separation

A crucial property: **the in-game wire protocol is completely unchanged** — the
multiplayer protocol version was not bumped. GSP is a separate small protocol spoken
only on the broker's port (42424). Once you're in a game, it's ordinary Descent
netcode. Even a client without the GAME SERVER menu could join a server-hosted game
by entering the IP and game port directly.

## How it's deployed, and why that way

It ships as a **Docker image** and runs on a **Google Compute Engine VM** (a plain
Linux virtual machine with a public IP), set up by
[`deploy-gcp.sh`](deploy-gcp.sh).

**Why Compute Engine and not Cloud Run** (the obvious "just run a container" GCP
option): Cloud Run only accepts HTTP/gRPC over TCP, on a single port, in stateless
instances that scale to zero. This server is the opposite on all three counts — it
is **UDP**, it needs a whole **port range** (42424 for the broker plus 42425+ for
each concurrent game), and it holds **long-lived stateful sessions** with real child
processes. Any one of those rules Cloud Run out. A VM is the right fit.

**Why Docker:** it bundles the compiled broker and engine with their Linux runtime
libraries so the same artifact runs identically anywhere. The image is built locally
and shipped to the VM directly (`docker save | scp | docker load`) — no container
registry needed.

**Why the game data is *not* in the image:** `descent.hog` / `descent.pig` are
copyrighted retail data. The image is deliberately data-less; the data files are
uploaded to the VM separately and mounted into the container as a read-only volume.
Nothing copyrighted ever touches a shared registry.

The deploy script also reserves a **static public IP** (a stable address for
players) and opens a **firewall rule for UDP 42424–42440**.

### Current live deployments

| Region | VM | Public IP | Notes |
|---|---|---|---|
| `asia-south1-b` | `dxx-gameserver` | `34.180.20.131` | first deploy |
| `europe-west3-a` | `dxx-gameserver-eu` | `34.185.143.72` | `eu.descent.one` → this IP; the client default |

Both are `e2-small` Ubuntu VMs in GCP project `descent-502421`, sharing one global
firewall rule (`dxx-gameserver-udp`). The europe instance is closer to the primary
players, which is why it was added and made the client default.

## Why this shape overall

The design leans on one principle: **reuse the real engine instead of reinventing
it.** That is what makes "the host isn't a player" and "co-op and robots just work"
fall out for free, and it is why the gameplay protocol did not have to change. The
broker stays deliberately dumb, so it is small, robust, and rule-agnostic.
Per-game process isolation means one crashing session cannot take down the others,
and empty sessions clean themselves up so the server does not accumulate dead games.
Putting it on a public-IP VM is the actual fix for the connectivity problem that
motivated the whole feature.

## Related files

- `d1/gameserver/gameserver.c` — the broker (socket loop, session table, spawn/reap,
  GSP handlers, child polling).
- `d1/gameserver/gsp.h` — the GSP wire protocol constants and layout.
- `d1/gameserver/spawn.c` / `spawn.h` — cross-platform process spawn/reap shim.
- `d1/main/dedicated.c` / `dedicated.h` — the engine's `-dedicated` headless host
  mode (session config, main loop, lifecycle).
- `d1/main/gameserver_menus.c` — the client-side GAME SERVER menu (browse / create /
  join) and GSP requests.
- `d1/Dockerfile`, `d1/docker-compose.yml` — the server image and an example
  deployment.
- `docs/superpowers/specs/2026-07-12-gameserver-design.md` — the full design spec.

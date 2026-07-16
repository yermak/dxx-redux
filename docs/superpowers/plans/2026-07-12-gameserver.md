# Dedicated Game Server (d1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A deployable game server: a small broker (`d1x-gameserver`) spawns headless `d1x-redux -dedicated` engine instances that host netgames as non-playing observer-hosts; clients create/browse/join server games from a new in-game menu.

**Architecture:** Per approved spec `docs/superpowers/specs/2026-07-12-gameserver-design.md`. One broker process on UDP 42424 speaking a tiny new GSP protocol (create/list only); one engine child process per session on ports 42425+, reusing the existing `host_is_obs` observer-host machinery and join-in-progress path; the in-game wire protocol is unchanged. Docker image for deployment; native Windows run for dev testing.

**Tech Stack:** Plain C (DOS-era lineage), CMake + MSYS2 MinGW64 (this machine) / gcc (Linux container), SDL 1.2, PhysFS, fixed-point math (`fix`, 16.16), BSD/winsock UDP sockets.

## Global Constraints

- Work in the worktree `C:\Users\Yermak\Projects\dxx-redux\.claude\worktrees\gameserver`, branch `gameserver`. All paths below are relative to `d1/` inside it unless prefixed.
- Build (Git Bash): `PATH=/c/Programs/msys64/mingw64/bin:$PATH cmake --build build -j` from `d1/`. First-time configure: `PATH=/c/Programs/msys64/mingw64/bin:$PATH cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo` from `d1/`.
- Run needs game data: always pass `-hogdir C:/Users/Yermak/Projects/dxx-redux/d1/hogs` (contains `DESCENT.HOG`, `DESCENT.PIG`, add-on missions `CHAOS`, `achtung`, `bigrat`).
- There is NO test suite. Every task's verification = compile cleanly + the concrete run steps given in that task, with the stated observable outcome.
- The engine uses `fix` (16.16) via `maths.h` — `F1_0` is one second in timer values; `timer_query()` returns `fix64`. No floats in game logic.
- Structs sent over the wire are packed byte-by-byte with `PUT_INTEL_*`/`GET_INTEL_*` (`byteswap.h`) — never `memcpy` a struct into a packet (exception: existing code already memcpys `_sockaddr`; don't imitate in new code).
- Do not change the in-game wire protocol. Do NOT bump `MULTI_PROTO_VERSION` (`main/multi.h`). New GSP packets exist only on the broker's port.
- New engine behavior must be gated by the `Dedicated_server` global (Task 3) so normal client behavior is bit-identical when it is 0.
- `d2/` is untouched by this plan (port is a follow-up). Final task verifies `d2` still configures+builds.
- Commit after every task (message prefix `gameserver:`), end commit messages with the `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` trailer.

## File Structure (locked-in decomposition)

| Path (under `d1/`) | Role |
|---|---|
| `main/net_udp.c` (modify) | pack-helper extraction (T1); dedicated hosting flow + sync guards (T3); GSP response dispatch (T6) |
| `main/net_udp.h` (modify) | new exported prototypes, `direct_join` typedef move |
| `main/dedicated.c` (create) | dedicated-mode state, session-file parsing, main loop, lifecycle timers, endlevel wait |
| `main/dedicated.h` (create) | `Dedicated_server` flag, `Dedicated_cfg`, `dedicated_*()` API |
| `main/inferno.c` (modify) | `-dedicated` init branches (skip video/titles/menus → `dedicated_main()`) |
| `misc/args.c`, `include/args.h` (modify) | `-dedicated <file>` argument |
| `arch/sdl/init.c` (modify) | timer-only SDL init in dedicated mode |
| `arch/sdl/gr.c` + OpenGL counterpart under `arch/ogl/` (modify) | early-return guards in `gr_palette_load`/`gr_palette_step_up` |
| canvas files under `2d/` (modify) | early-return guards in `gr_set_current_canvas`/`gr_clear_canvas` |
| `main/game.c` (modify) | skip `diminish_palette_towards_normal` headless |
| `main/songs.c` (modify) | no music in dedicated mode |
| `main/gameseq.c` (modify) | dedicated anarchy level wrap, co-op completion exit, UI skips |
| `main/multi.c` (modify) | obs-host fix in `multi_obs_check_all_escaped`; dedicated branch in `multi_endlevel_score` |
| `main/kmatrix.c`, `main/kmatrix.h` (modify) | obs-host fix in playing-scan; export `KMATRIX_VIEW_SEC` |
| `main/multibot.c` (modify) | robot-rotation divide-by-zero guard |
| `main/menu.c`, `main/menu.h` (modify) | "GAME SERVER" multiplayer menu entry + `MENU_GAMESERVER` |
| `main/config.c`, `main/config.h` (modify) | persisted `GameserverAddr`/`GameserverPort` |
| `main/gameserver_menus.c`/`.h` (create) | client-side Game Server submenu: address, browse, create-and-join; GSP mailboxes |
| `gameserver/gsp.h` (create) | GSP opcodes/layout constants shared by broker and client |
| `gameserver/gameserver.c` (create) | broker: socket loop, session table, spawn/reap, GSP handlers, child polling |
| `gameserver/spawn.c`, `gameserver/spawn.h` (create) | process spawn/kill/poll shim (CreateProcess / fork+exec) |
| `gameserver/CMakeLists.txt` (create) | `d1x-gameserver` target (no SDL/engine deps; ws2_32 on Windows) |
| `CMakeLists.txt` (modify) | `add_subdirectory(gameserver)` |
| `Dockerfile`, `docker-compose.yml`, `.dockerignore`, `gameserver/README.md` (create) | server image + example deployment + docs |

Tasks below are ordered so every task ends runnable: pack helper → dedicated skeleton → dedicated hosting (joinable by an unmodified client!) → lifecycle/auto-behaviors → broker → client menus → co-op validation → Docker/docs.

---

### Task 0: Worktree build baseline

**Files:** none (build only)

**Interfaces:**
- Consumes: nothing
- Produces: a configured `d1/build/` in the worktree that later tasks incrementally rebuild

- [ ] **Step 1: Configure**

Run (Git Bash, from `d1/` in the worktree):
```bash
PATH=/c/Programs/msys64/mingw64/bin:$PATH cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
```
Expected: `-- Configuring done` / `-- Generating done`.

- [ ] **Step 2: Build**

```bash
PATH=/c/Programs/msys64/mingw64/bin:$PATH cmake --build build -j
```
Expected: exits 0, produces `build/main/d1x-redux.exe`.

- [ ] **Step 3: Smoke run**

```bash
./build/main/d1x-redux.exe -hogdir C:/Users/Yermak/Projects/dxx-redux/d1/hogs -notitles &
```
Expected: game window opens to main menu. Close it. No commit (nothing changed).

---

### Task 1: Extract `net_udp_pack_game_info()` from `net_udp_send_game_info()`

Behavior-preserving refactor so the client (Task 7) can serialize `Netgame` into a GSP create payload and the dedicated child (Task 4) can reuse the existing unpacker.

**Files:**
- Modify: `main/net_udp.c:2873-3069` (`net_udp_send_game_info`)
- Modify: `main/net_udp.h` (add prototype)

**Interfaces:**
- Consumes: existing globals `Netgame`, `netgame_token`, packing macros.
- Produces: `int net_udp_pack_game_info(ubyte *buf, ubyte info_upid, struct _sockaddr *sender_addr, uint player_token)` — fills `buf` (caller provides `>= UPID_GAME_INFO_SIZE` bytes) with exactly the bytes `net_udp_send_game_info()` used to send for that `info_upid`, returns the length. `sender_addr` is only used for the per-player `isyou` byte (full-info variants); pass a zeroed `_sockaddr` when there is no meaningful peer.

- [ ] **Step 1: Refactor**

In `main/net_udp.c`, split the existing function. The **entire body** of the `else` branch (full info, lines 2927-3068) plus the lite branch move into the new function; only `net_udp_update_netgame()` and the sends stay in the wrapper:

```c
// Serialize current Netgame into buf in the exact wire format of info_upid
// (UPID_GAME_INFO_LITE, UPID_GAME_INFO or UPID_SYNC). Returns byte length.
// sender_addr is used only to set the per-player isyou byte; player_token
// only for UPID_SYNC. Caller must provide UPID_GAME_INFO_SIZE bytes.
int net_udp_pack_game_info(ubyte *buf, ubyte info_upid, struct _sockaddr *sender_addr, uint player_token)
{
	int len = 0;

	if (info_upid == UPID_GAME_INFO_LITE)
	{
		/* ...moved lite-branch body, unchanged, minus the memset/sendto:
		   keep the memset(buf, 0, UPID_GAME_INFO_LITE_SIZE) as
		   memset(buf, 0, UPID_GAME_INFO_LITE_SIZE); ... */
	}
	else
	{
		/* ...moved full-branch body, unchanged, minus memset/sendto/
		   forward_to_observers; every reference to sender_addr becomes
		   *sender_addr (it is now a pointer)... */
	}
	return len;
}

void net_udp_send_game_info(struct _sockaddr sender_addr, ubyte info_upid, ubyte send_to_observers, uint player_token)
{
	ubyte buf[UPID_GAME_INFO_SIZE];
	int len;

	net_udp_update_netgame(); // Update the values in the netgame struct
	len = net_udp_pack_game_info(buf, info_upid, &sender_addr, player_token);

	if (info_upid == UPID_GAME_INFO_LITE)
	{
		dxx_sendto (UDP_Socket[0], buf, len, 0, (struct sockaddr *)&sender_addr, sizeof(struct _sockaddr));
		return;
	}

	if (send_to_observers != 2)
		dxx_sendto (UDP_Socket[0], buf, len, 0, (struct sockaddr *)&sender_addr, sizeof(struct _sockaddr));
	if (send_to_observers != 0)
		forward_to_observers(buf, len, 0);
}
```

Move both branch bodies verbatim (they already compute `len` incrementally). Inside the moved full branch, the `isyou` comparison line becomes:

```c
			if (!memcmp((struct _sockaddr *)sender_addr, (struct _sockaddr *)&Netgame.players[i].protocol.udp.addr, sizeof(struct _sockaddr))) {
```

Keep the `Assert(len <= UPID_GAME_INFO_SIZE);` at the end of the full branch. Note the lite buffer previously was a smaller local array — after the merge both use the caller's buffer; change the lite branch's `memset(buf, 0, sizeof(buf))` to `memset(buf, 0, UPID_GAME_INFO_LITE_SIZE)` and the full branch's to `memset(buf, 0, UPID_GAME_INFO_SIZE)`.

Add to `main/net_udp.h` after the existing prototypes (line ~26):

```c
int net_udp_pack_game_info(ubyte *buf, ubyte info_upid, struct _sockaddr *sender_addr, uint player_token);
```

- [ ] **Step 2: Build**

```bash
PATH=/c/Programs/msys64/mingw64/bin:$PATH cmake --build build -j
```
Expected: exits 0, no new warnings about `net_udp.c`.

- [ ] **Step 3: Behavior check (host + join on localhost)**

Terminal A:
```bash
./build/main/d1x-redux.exe -hogdir C:/Users/Yermak/Projects/dxx-redux/d1/hogs -notitles -pilot srv1
```
Multiplayer → HOST GAME → defaults → start; in the player-select lobby leave the window open.

Terminal B:
```bash
./build/main/d1x-redux.exe -hogdir C:/Users/Yermak/Projects/dxx-redux/d1/hogs -notitles -pilot cli1 -udp_myport 42425
```
Multiplayer → JOIN GAME MANUALLY → address `localhost` port `42424` → the game-info screen must appear (this exercises the moved full-info packing) and joining must reach the lobby. Quit both.

Expected: info screen shows correct game name/mission/mode; join succeeds.

- [ ] **Step 4: Commit**

```bash
git add main/net_udp.c main/net_udp.h
git commit -m "gameserver: extract net_udp_pack_game_info from send_game_info

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: `-dedicated` argument, headless boot, video-guard chokepoints

Boots the engine with no video/input/sound and no windows. Strategy (from the headless audit): skip `gr_init` entirely and add `Dedicated_server` early-returns at the few gr functions the simulation path can reach (`gr_palette_load`, `gr_palette_step_up`, `gr_set_current_canvas`, `gr_clear_canvas`), rather than guarding every caller. Blocking UI on the frame path is handled in Tasks 3-4.

**Files:**
- Modify: `include/args.h`, `misc/args.c` (add `-dedicated <file>`)
- Create: `main/dedicated.h`, `main/dedicated.c`
- Modify: `main/inferno.c:306-462` (init branches, replace `DoMenu()`)
- Modify: `arch/sdl/init.c:37-58` (`arch_init` dedicated branch)
- Modify: every definition of `gr_palette_load`, `gr_palette_step_up` (in `arch/sdl/gr.c` and the OpenGL build's counterpart under `arch/ogl/`) and of `gr_set_current_canvas`, `gr_clear_canvas` (under `2d/`)
- Modify: `main/game.c:1118` (skip `diminish_palette_towards_normal`)
- Modify: `main/songs.c` (no music in dedicated)
- Modify: `main/CMakeLists.txt` (add `dedicated.c`)

**Interfaces:**
- Consumes: `GameArg` parsing pattern (`misc/args.c:128-145`), `con_printf`.
- Produces: `int Dedicated_server` (global flag, 0 in normal play), `int Dedicated_exit_requested`, `dedicated_config Dedicated_cfg`, `int dedicated_parse_cfg(const char *path)`, `void dedicated_main(void)` (stub in this task; Task 3 fills it). Header `main/dedicated.h`.

- [ ] **Step 1: argument**

`include/args.h`, in `struct Arg` after `SysNoTitles` (line 54):

```c
	char *SysDedicated;
```

`misc/args.c`, in the System Options section (after line 145 `SysAutoDemo`):

```c
	GameArg.SysDedicated 		= get_str_arg("-dedicated", NULL);
```

Also add a help line in `main/inferno.c` `print_commandline_help()` (after `-notitles`, line ~111):

```c
	printf( "  -dedicated <f>                Run headless dedicated game server with session file <f>\n");
```

- [ ] **Step 2: `main/dedicated.h`**

```c
/* Dedicated (headless) game server mode. */
#ifndef DEDICATED_H
#define DEDICATED_H

extern int Dedicated_server;         /* 1 = running headless as a dedicated host */
extern int Dedicated_exit_requested; /* set by signals / endgame to leave the loop */

typedef struct dedicated_config {
	int port;                  /* UDP game port to bind */
	char blob_path[512];       /* optional: packed netgame_info from the broker */
	char game_name[16];        /* NETGAME_NAME_LEN+1; used when no blob */
	char mission[13];          /* mission filename stem; "" = builtin Descent */
	int level;
	int mode;                  /* NETGAME_ANARCHY etc.; used when no blob */
	int maxplayers;
	int timeout_empty_start;   /* s before exiting if nobody ever joined */
	int timeout_empty;         /* s after the last player left */
} dedicated_config;

extern dedicated_config Dedicated_cfg;

int dedicated_parse_cfg(const char *path);
void dedicated_main(void);           /* runs the session; never returns */
int dedicated_should_exit(void);
int dedicated_connected_players(void);
void dedicated_endlevel_wait(void);  /* Task 4 */

#endif
```

- [ ] **Step 3: `main/dedicated.c` (skeleton)**

```c
/* Dedicated (headless) game server mode: session config, main loop,
 * lifecycle. The netcode start sequence lives in net_udp.c
 * (net_udp_dedicated_start_game) where the transport internals are. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "dedicated.h"
#include "console.h"
#include "maths.h"
#include "timer.h"

int Dedicated_server = 0;
int Dedicated_exit_requested = 0;
dedicated_config Dedicated_cfg;

static void ded_signal(int sig)
{
	sig = sig;
	Dedicated_exit_requested = 1;
}

int dedicated_parse_cfg(const char *path)
{
	FILE *f;
	char line[600];

	memset(&Dedicated_cfg, 0, sizeof(Dedicated_cfg));
	Dedicated_cfg.port = 42425;
	strcpy(Dedicated_cfg.game_name, "Dedicated");
	Dedicated_cfg.level = 1;
	Dedicated_cfg.mode = 0; /* NETGAME_ANARCHY */
	Dedicated_cfg.maxplayers = 8;
	Dedicated_cfg.timeout_empty_start = 300;
	Dedicated_cfg.timeout_empty = 60;

	f = fopen(path, "r");
	if (!f) {
		con_printf(CON_URGENT, "[dedicated] cannot open session config %s\n", path);
		return 0;
	}
	while (fgets(line, sizeof(line), f)) {
		char *nl, *eq;
		const char *key, *val;
		nl = strchr(line, '\n');
		if (nl) *nl = 0;
		if (line[0] == '#')
			continue;
		eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		key = line;
		val = eq + 1;
		if (!strcmp(key, "port")) Dedicated_cfg.port = atoi(val);
		else if (!strcmp(key, "blob")) strncpy(Dedicated_cfg.blob_path, val, sizeof(Dedicated_cfg.blob_path) - 1);
		else if (!strcmp(key, "game_name")) strncpy(Dedicated_cfg.game_name, val, sizeof(Dedicated_cfg.game_name) - 1);
		else if (!strcmp(key, "mission")) strncpy(Dedicated_cfg.mission, val, sizeof(Dedicated_cfg.mission) - 1);
		else if (!strcmp(key, "level")) Dedicated_cfg.level = atoi(val);
		else if (!strcmp(key, "mode")) Dedicated_cfg.mode = atoi(val);
		else if (!strcmp(key, "maxplayers")) Dedicated_cfg.maxplayers = atoi(val);
		else if (!strcmp(key, "timeout_empty_start")) Dedicated_cfg.timeout_empty_start = atoi(val);
		else if (!strcmp(key, "timeout_empty")) Dedicated_cfg.timeout_empty = atoi(val);
	}
	fclose(f);
	remove(path); /* single-use, written by the broker */
	return 1;
}

int dedicated_connected_players(void)
{
	return 0; /* real body (players + observers) arrives in Task 3 */
}

int dedicated_should_exit(void)
{
	return Dedicated_exit_requested; /* lifecycle timers arrive in Task 4 */
}

void dedicated_main(void)
{
	signal(SIGINT, ded_signal);
	signal(SIGTERM, ded_signal);
	con_printf(CON_NORMAL, "[dedicated] session: port=%d game='%s' mission='%s' level=%d mode=%d blob='%s'\n",
	           Dedicated_cfg.port, Dedicated_cfg.game_name, Dedicated_cfg.mission,
	           Dedicated_cfg.level, Dedicated_cfg.mode, Dedicated_cfg.blob_path);
	con_printf(CON_NORMAL, "[dedicated] boot OK (hosting arrives in the next task)\n");
	exit(0);
}
```

Add `dedicated.c` to the source list in `main/CMakeLists.txt` (same list that gets `gameserver_menus.c` in Task 6).

- [ ] **Step 4: `main/inferno.c` boot branches**

After `con_init()` (line 312), set the flag and console-safe error handlers:

```c
	if (GameArg.SysDedicated)
	{
		Dedicated_server = 1;
		error_init(NULL);      /* console-only errors; the msgbox handlers */
		set_warn_func(NULL);   /* registered above would pop Win32 dialogs */
	}
```
(`#include "dedicated.h"` at the top of inferno.c.) `error_init(NULL)` is safe: `print_exit_message` checks `ErrorPrintFunc` (`misc/error.c:58`); `Warning()` returns when `warn_func == NULL` (`misc/error.c:86`).

Guard the display-only init calls (audit: `inferno.c:398,405,409,411`):

```c
	if (!Dedicated_server)
	{
		con_printf(CON_VERBOSE, "Going into graphics mode...\n");
		gr_set_mode(Game_screen_mode);
	}

	// Load the palette stuff. Returns non-zero if error.
	con_printf(CON_DEBUG, "Initializing palette system...\n" );
	gr_use_palette_table( "PALETTE.256" );          /* KEEP: pure data, no canvas */

	if (!Dedicated_server)
	{
		con_printf(CON_DEBUG, "Initializing font system...\n" );
		gamefont_init();	// must load after palette data loaded.
	}

	set_default_handler(standard_handler);

	if (!Dedicated_server)
	{
		show_titles();
		set_screen_mode(SCREEN_MENU);
	}
```
(`gamefont_init` dereferences `grd_curscreen` — `main/gamefont.c:184` — so it must be skipped; fonts are render-only. `gamedata_init`, `texmerge_init`, `init_game` stay unconditional: the audit verified they are canvas-independent.)

Replace the menu entry point (line ~456):

```c
	if (Dedicated_server)
		dedicated_main();       /* never returns */

	Game_mode = GM_GAME_OVER;
	DoMenu();
```

- [ ] **Step 5: `arch_init` branch (`arch/sdl/init.c`)**

At the top of `arch_sdl_init()`/`arch_init()` (the function at `arch/sdl/init.c:37` containing `SDL_Init(SDL_INIT_VIDEO)`):

```c
	extern int Dedicated_server;
	if (Dedicated_server)
	{
		if (SDL_Init(SDL_INIT_TIMER) < 0)
			Error("SDL library initialisation failed: %s.", SDL_GetError());
		return; /* headless: no video, no input devices, no sound */
	}
```
(The timer wrappers `timer_update`/`timer_query`/`timer_delay` only need `SDL_GetTicks`/`SDL_Delay` — audited video-free.)

- [ ] **Step 6: gr chokepoint guards**

Locate every **definition** (not caller) of the four functions:

```bash
grep -rn --include=*.c -E "^void gr_palette_load|^void gr_palette_step_up|^void gr_set_current_canvas|^void gr_clear_canvas" 2d/ arch/
```
Expected: `gr_palette_load` + `gr_palette_step_up` in `arch/sdl/gr.c` (software) and once more under `arch/ogl/` (OpenGL build); `gr_set_current_canvas` and `gr_clear_canvas` under `2d/` (likely `2d/canvas.c`). In **each** definition add as the first statement:

```c
	{
		extern int Dedicated_server;
		if (Dedicated_server)
			return; /* headless: no video surface exists */
	}
```
Rationale (audit): `gr_palette_load` dereferences the SDL `canvas` (`arch/sdl/gr.c:287`) and is reached from `DoPlayerDead` (`gameseq.c:1030`) and level load (`gameseq.c:677,1192`); `gr_palette_step_up` dereferences `canvas->format->palette` (`arch/sdl/gr.c:240`) from `diminish_palette_towards_normal`; `cntrlcen.c:193-194` calls `gr_set_current_canvas(NULL)` + `gr_clear_canvas(...)` on the reactor whiteout, which is reachable because the observer host's slot is `CONNECT_PLAYING`.

- [ ] **Step 7: frame-path and music guards**

`main/game.c:1118` in `GameProcessFrame()`:

```c
	if (!Dedicated_server)
		diminish_palette_towards_normal();		//	Should leave palette effect up for as long as possible by putting right before render.
```
(`#include "dedicated.h"` in game.c.)

`main/songs.c`: first statement of `songs_play_song()` and `songs_play_level_song()`:

```c
	{
		extern int Dedicated_server;
		if (Dedicated_server)
			return;
	}
```
(Return the function's zero value instead if its return type is non-void — check each signature when editing.)

- [ ] **Step 8: Build + verify both personalities**

```bash
PATH=/c/Programs/msys64/mingw64/bin:$PATH cmake --build build -j
```
Expected: exits 0.

Write `C:/Users/Yermak/AppData/Local/Temp/ded_boot.cfg`:
```
port=42425
game_name=boottest
```
Run:
```bash
./build/main/d1x-redux.exe -dedicated C:/Users/Yermak/AppData/Local/Temp/ded_boot.cfg -hogdir C:/Users/Yermak/Projects/dxx-redux/d1/hogs -nosound -notitles; echo "exit=$?"
```
Expected: console shows the version banner, `[dedicated] session: port=42425 game='boottest' ...`, `[dedicated] boot OK ...`, `exit=0`. **No window ever opens.** The cfg file is deleted afterward.

Normal mode regression: run without `-dedicated` — game window opens to the main menu as before.

- [ ] **Step 9: Commit**

```bash
git add include/args.h misc/args.c main/dedicated.c main/dedicated.h main/inferno.c main/game.c main/songs.c main/CMakeLists.txt arch/sdl/init.c arch/sdl/gr.c 2d/ arch/ogl/
git commit -m "gameserver: -dedicated headless boot mode and video chokepoint guards

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: Menu-less hosting + dedicated main loop (joinable server!)

After this task a dedicated instance hosts a real netgame that an **unmodified** client can join via JOIN GAME MANUALLY. The start sequence replicates exactly what the host menus do (traced to `net_udp.c`/`gameseq.c` line level), and lives inside `net_udp.c` where the statics are visible.

**Files:**
- Modify: `main/net_udp.c` (new `net_udp_dedicated_start_game()`; dedicated branch in `net_udp_wait_for_requests` at line ~5295; headless-safe error paths at lines ~4805 and ~4688)
- Modify: `main/net_udp.h` (prototype)
- Modify: `main/dedicated.c` (real `dedicated_main`, loop, `dedicated_connected_players`)

**Interfaces:**
- Consumes: Task 2's `Dedicated_server`/`Dedicated_cfg`; existing internals of `net_udp.c` (`net_udp_init`, `net_udp_add_player`, `UDP_Seq`, `udp_open_socket`, `net_udp_set_game_mode`, `generate_token`, `netgame_set_defaults`, `net_udp_process_game_info`); `load_mission_by_name` (`main/mission.c:734`; the builtin mission's filename is `""` — `main/mission.h:31`); `StartNewLevel` (`main/gameseq.c:1288`); `new_player_config()` (`main/playsave.c:64`); `calc_frame_time`/`calc_game_time`/`GameProcessFrame` (`main/game.c:381/408/1112`).
- Produces: `int net_udp_dedicated_start_game(void)` — returns 1 with the game at `NETSTAT_PLAYING`, sockets bound, observer-host registered; the running dedicated loop; full `dedicated_connected_players()`.

- [ ] **Step 1: `net_udp_dedicated_start_game()` in `main/net_udp.c`**

Add near `net_udp_start_game()` (line ~5169), with `#include "dedicated.h"` in the include block:

```c
// Menu-less host start for dedicated mode. Replicates the state set by
// net_udp_setup_game (params menu), net_udp_start_game and the
// unchecked-host path of net_udp_select_players (net_udp.c:5145-5150),
// with zero players and the host as pure observer. Returns 1 on success.
int net_udp_dedicated_start_game(void)
{
	net_udp_init(); // WSAStartup, memset(Netgame), UDP_Seq from Players[0].callsign, multi_new_game(), tokens

	if (Dedicated_cfg.blob_path[0])
	{
		// creator-supplied config: a UPID_GAME_INFO-format packet
		FILE *bf = fopen(Dedicated_cfg.blob_path, "rb");
		ubyte blob[UPID_GAME_INFO_SIZE];
		int blen;
		struct _sockaddr zero_addr;

		if (!bf) {
			con_printf(CON_URGENT, "[dedicated] cannot open blob %s\n", Dedicated_cfg.blob_path);
			return 0;
		}
		blen = (int)fread(blob, 1, sizeof(blob), bf);
		fclose(bf);
		remove(Dedicated_cfg.blob_path);
		if (blen < 32 || blob[0] != UPID_GAME_INFO) {
			con_printf(CON_URGENT, "[dedicated] malformed session blob\n");
			return 0;
		}
		memset(&zero_addr, 0, sizeof(zero_addr));
		if (!net_udp_process_game_info(blob, blen, zero_addr, 0, 0)) {
			con_printf(CON_URGENT, "[dedicated] session blob rejected\n");
			return 0;
		}
		if (Netgame.protocol.udp.program_iver[0] != DXX_VERSION_MAJORi ||
		    Netgame.protocol.udp.program_iver[1] != DXX_VERSION_MINORi ||
		    Netgame.protocol.udp.program_iver[2] != DXX_VERSION_MICROi) {
			con_printf(CON_URGENT, "[dedicated] creator game version mismatch\n");
			return 0;
		}
	}
	else
	{
		netgame_set_defaults();
		Netgame.gamemode = (ubyte)Dedicated_cfg.mode;
		Netgame.levelnum = Dedicated_cfg.level;
		Netgame.max_numplayers = Dedicated_cfg.maxplayers;
		memset(Netgame.game_name, 0, sizeof(Netgame.game_name));
		strncpy(Netgame.game_name, Dedicated_cfg.game_name, NETGAME_NAME_LEN);
		memset(Netgame.mission_name, 0, sizeof(Netgame.mission_name));
		strncpy(Netgame.mission_name, Dedicated_cfg.mission, 8);
	}

	// dedicated sessions are always open and tracker-less
	Netgame.RefusePlayers = 0;
	Netgame.game_flags &= ~NETGAME_FLAG_CLOSED;
#ifdef USE_TRACKER
	Netgame.Tracker = 0;
#endif

	if (!load_mission_by_name(Netgame.mission_name))
	{
		con_printf(CON_URGENT, "[dedicated] mission '%s' not found on server\n", Netgame.mission_name);
		return 0;
	}
	// re-derive the names from what actually loaded (same as net_udp_setup_game:4515-4516)
	strcpy(Netgame.mission_name, Current_mission_filename);
	strcpy(Netgame.mission_title, Current_mission_longname);
	if (Netgame.levelnum < 1 || Netgame.levelnum > Last_level)
	{
		con_printf(CON_URGENT, "[dedicated] level %d out of range (1..%d)\n", Netgame.levelnum, Last_level);
		return 0;
	}

	change_playernum_to(0);

	// socket + identity setup, mirroring net_udp_start_game (net_udp.c:5173-5206).
	// No broadcast socket: on a server the broker (or another session) owns the
	// default port, and discovery is the broker's job.
	snprintf(UDP_MyPort, sizeof(UDP_MyPort), "%d", Dedicated_cfg.port);
	if (udp_open_socket(0, Dedicated_cfg.port) != 0)
	{
		con_printf(CON_URGENT, "[dedicated] cannot bind UDP port %d\n", Dedicated_cfg.port);
		return 0;
	}
	memset(&GBcast, '\0', sizeof(struct _sockaddr));
	udp_dns_filladdr(UDP_BCAST_ADDR, UDP_PORT_DEFAULT, &GBcast);
	d_srand( (fix)timer_query() );
	Netgame.protocol.udp.GameID = d_rand();
	N_players = 0;
	Endlevel_sequence = Control_center_destroyed = 0;
	Netgame.game_status = NETSTAT_STARTING;
	Netgame.numplayers = 0;
	Netgame.numobservers = 0;
	net_udp_set_game_mode(Netgame.gamemode, 0);
	Netgame.players[0].protocol.udp.isyou = 1;
	Network_status = NETSTAT_STARTING;
	netgame_token = generate_token();

	// select-players equivalent: register the host slot (net_udp.c:5028),
	// then the unchecked-host observer path (net_udp.c:5145-5150)
	net_udp_add_player(&UDP_Seq);
	Netgame.host_is_obs = 1;
	Host_is_obs = 1;
	Game_mode |= GM_OBSERVER;
	Current_obs_player = 0;

	// loads the level, creates the 8 network player objects, runs the host
	// sync (dedicated branch of net_udp_wait_for_requests + net_udp_send_sync),
	// ghosts the observer-host object, and lands in NETSTAT_PLAYING
	StartNewLevel(Netgame.levelnum);

	if (Network_status != NETSTAT_PLAYING)
	{
		con_printf(CON_URGENT, "[dedicated] level start failed (status %d)\n", Network_status);
		return 0;
	}
	return 1;
}
```

Add to `main/net_udp.h`:

```c
int net_udp_dedicated_start_game(void);
```

Notes for the implementer:
- All the identifiers used are file-local statics or globals already visible inside `net_udp.c` (`UDP_Seq`, `UDP_MyPort`, `GBcast`, `netgame_token`, `generate_token`, `player_tokens`) — that is why this function lives here.
- `Current_mission_filename` / `Current_mission_longname`: copy the exact two assignment lines used at `net_udp.c:4515-4516` — if 4516 uses a different source variable, mirror it.
- `net_udp_process_game_info` sets `Netgame.players[]` from the blob (stale creator-side data) — harmless: `net_udp_init` ran `multi_new_game()` (all `Players[].connected = CONNECT_DISCONNECTED`) and `net_udp_add_player` + `N_players=0` reset the live state; `Netgame.players[]` beyond slot 0 is rewritten by joins.
- `GM_OBSERVER` must be OR'd **after** `net_udp_set_game_mode` (which assigns `Game_mode` fresh).

- [ ] **Step 2: dedicated branch in `net_udp_wait_for_requests` (net_udp.c:5295)**

The host reaches this via `StartNewLevel → StartNewLevelSub → multi_level_sync → net_udp_level_sync` on **every** level (initial and transitions). It currently opens a blocking `newmenu_do` (line 5310). After the existing `Network_status = NETSTAT_WAITING; net_udp_flush();` lines add:

```c
	Players[Player_num].connected = CONNECT_PLAYING;

	if (Dedicated_server)
	{
		// No menu: wait (max 45 s) until every still-connected player has
		// re-requested the new level (net_udp_process_request flips them to
		// CONNECT_PLAYING); net_udp_timeout_check dumps silent ones.
		// With zero players connected this returns immediately.
		fix64 deadline = timer_query() + F1_0 * 45;
		for (;;) {
			int i, waiting = 0;
			timer_update();
			timer_delay2(20);
			net_udp_listen();
			net_udp_timeout_check(timer_query());
			for (i = 1; i < N_players; i++)
				if (Players[i].connected && Players[i].connected != CONNECT_PLAYING)
					waiting++;
			if (!waiting)
				return 0;
			if (timer_query() > deadline) {
				con_printf(CON_NORMAL, "[dedicated] starting level without %d slow player(s)\n", waiting);
				return 0;
			}
		}
	}
```
(The original code below already contains its own `Players[Player_num].connected = CONNECT_PLAYING;` — leave it; running it twice in normal mode is harmless, or move the original line above the branch and delete the duplicate.)

- [ ] **Step 3: headless-safe sync error paths**

`main/net_udp.c:4805` (`net_udp_send_sync`, "Not enough start positions" messagebox): wrap:

```c
		if (Dedicated_server)
		{
			con_printf(CON_URGENT, "[dedicated] level has %d start positions, need %d — aborting session\n", NumNetPlayerPositions, Netgame.max_numplayers);
			Dedicated_exit_requested = 1;
		}
		else
			nm_messagebox(... existing call unchanged ...);
```
`main/net_udp.c:4688` (`net_udp_read_sync_packet` checksum mismatch — client path, but cheap to make safe): same pattern, `con_printf(CON_URGENT, ...)` instead of the box, keeping the surrounding disconnect logic unchanged.

- [ ] **Step 4: real `dedicated_main()` + loop in `main/dedicated.c`**

Replace the Task 2 stub bodies of `dedicated_main` and `dedicated_connected_players` (delete the bogus extern line):

```c
#include "args.h"
#include "playsave.h"
#include "config.h"
#include "multi.h"
#include "net_udp.h"
#include "player.h"
#include "game.h"
#include "inferno.h"

fix64 Ded_started_at; /* lifecycle timers build on this in Task 4 */

int dedicated_connected_players(void)
{
	int i, n = 0;
	for (i = (Netgame.host_is_obs ? 1 : 0); i < N_players; i++)
		if (Players[i].connected)
			n++;
	return n + Netgame.numobservers;
}

void dedicated_main(void)
{
	signal(SIGINT, ded_signal);
	signal(SIGTERM, ded_signal);

	con_printf(CON_NORMAL, "[dedicated] starting session '%s' on UDP port %d\n",
	           Dedicated_cfg.game_name, Dedicated_cfg.port);

	strcpy(Players[0].callsign, "SERVER");
	new_player_config();                 /* sane PlayerCfg defaults */
	if (PlayerCfg.maxFps > 60 || PlayerCfg.maxFps < 10)
		PlayerCfg.maxFps = 60;           /* server sim rate; packets max 40/s */
	GameCfg.VSync = 0;                   /* enables calc_frame_time's sleep */

	if (!net_udp_dedicated_start_game())
		exit(1);

	con_printf(CON_NORMAL, "[dedicated] hosting '%s' (%s, level %d, mode %d), waiting for players\n",
	           Netgame.game_name, Netgame.mission_title, Netgame.levelnum, Netgame.gamemode);

	Ded_started_at = timer_query();
	calc_frame_time();                   /* prime FrameTime; first delta is garbage */

	while (!dedicated_should_exit())
	{
		calc_frame_time();               /* includes the maxFps sleep (game.c:381) */
		calc_game_time();
		GameProcessFrame();              /* simulation + multi_do_frame -> net_udp_do_frame(0,1) */
	}

	con_printf(CON_NORMAL, "[dedicated] closing session\n");
	if (dedicated_connected_players() > 0)
		multi_leave_game();              /* tells clients the game ended */
	net_udp_close();
	exit(0);
}
```
If `calc_frame_time`, `calc_game_time`, or `GameProcessFrame` are not declared in `main/game.h`, add their prototypes there (`void calc_frame_time(void); void calc_game_time(void); void GameProcessFrame(void);`) rather than local externs.

- [ ] **Step 5: Build**

```bash
PATH=/c/Programs/msys64/mingw64/bin:$PATH cmake --build build -j
```
Expected: exits 0.

- [ ] **Step 6: Verify — a real client joins the headless server**

Terminal A (server):
```bash
./build/main/d1x-redux.exe -dedicated C:/Users/Yermak/AppData/Local/Temp/ded_boot.cfg -hogdir C:/Users/Yermak/Projects/dxx-redux/d1/hogs -nosound -notitles
```
(re-create `ded_boot.cfg` from Task 2 first — it deletes itself on read). Expected log: `starting session`, `hosting 'boottest' (Descent: First Strike, level 1, mode 0), waiting for players`, then it idles (low CPU — Task Manager should show a few % at most; if it busy-spins, `GameArg.SysUseNiceFPS` is off — pass no `-nonicefps` and re-check).

Terminal B (client):
```bash
./build/main/d1x-redux.exe -hogdir C:/Users/Yermak/Projects/dxx-redux/d1/hogs -notitles -pilot cli1 -udp_myport 42460
```
Multiplayer → JOIN GAME MANUALLY → `localhost` port `42425`. Expected: game-info screen shows `boottest`, mission Descent: First Strike, 0/8 players → join → you spawn and fly. HUD scoreboard should NOT show a "SERVER" player row (host_is_obs handling). Shoot walls, grab powerups.

Terminal C (second client, `-pilot cli2 -udp_myport 42461`): join the same way mid-game. Expected: both clients see and can shoot each other.

Quit both clients (server stays up — lifecycle exits arrive in Task 4). Ctrl+C the server. Expected: `closing session`, exit 0.

- [ ] **Step 7: Commit**

```bash
git add main/net_udp.c main/net_udp.h main/dedicated.c main/dedicated.h main/game.h
git commit -m "gameserver: menu-less dedicated hosting and headless main loop

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Dedicated end-of-level, lifecycle timers, and shutdown

Makes a dedicated session survive level transitions and terminate itself per spec. Includes two **unconditional engine bug fixes** (they affect human observer-hosts today, found by code trace): the observer-host slot is not skipped in `multi_obs_check_all_escaped()` or in the kmatrix "still playing" scan, so an observer-host game can hang at end-of-level.

Background (traced): end-of-level on an observer host = `multi_obs_check_all_escaped()` (`main/multi.c:2350`, fires when no player is `CONNECT_PLAYING`) → `PlayerFinishedLevel(0)` (`main/gameseq.c:862`) → `multi_endlevel_score()` (`main/multi.c:761`) → `kmatrix_view()` window (`main/kmatrix.c:354`) which pumps `multi_do_protocol_frame(0,1)`, waits until every connected player is `CONNECT_END_MENU`/`CONNECT_DIED_IN_MINE`, then holds `KMATRIX_VIEW_SEC` (= 7 s, `main/kmatrix.c:57`) → back in `AdvanceLevel()` (`main/gameseq.c:945`) → `Next_level_num = Current_level_num + 1` (`gameseq.c:983`) → `StartNewLevel()` → `multi_level_sync()` handshake. Anarchy at `Last_level` = game over (no wrap!); co-op final level = ending briefing + score glitz + back to menu.

**Files:**
- Modify: `main/multi.c:2350-2360` (`multi_obs_check_all_escaped` — host_is_obs skip)
- Modify: `main/kmatrix.c:298-339` (`kmatrix_handler` playing-scan — host_is_obs skip)
- Modify: `main/multi.c:761-810` (`multi_endlevel_score` — dedicated branch replaces `kmatrix_view` window)
- Modify: `main/gameseq.c:862-1010` (`PlayerFinishedLevel`/`AdvanceLevel` — dedicated anarchy wrap + co-op-final session end + skip briefing/glitz UI)
- Modify: `main/net_udp.c:5295-5330` (`net_udp_wait_for_requests` — dedicated no-menu wait loop)
- Modify: `main/net_udp.c:4805`, `main/net_udp.c:4688` (error `nm_messagebox` → `con_printf` + fail in dedicated mode)
- Modify: `main/dedicated.c`, `main/dedicated.h` (lifecycle timers, shutdown)

**Interfaces:**
- Consumes: `Dedicated_server`, `dedicated_frame()` loop and `Dedicated_cfg` struct from Tasks 2-3 (`main/dedicated.h`); `KMATRIX_VIEW_SEC`; `multi_do_protocol_frame(int, int)`.
- Produces: `void dedicated_endlevel_wait(void)` (called from `multi_endlevel_score`); `int dedicated_should_exit(void)` + `void dedicated_note_player_activity(void)` (lifecycle, called from the dedicated loop and join/leave paths); dedicated games cycle anarchy levels and exit after co-op completion.

- [ ] **Step 1: Bug fix — observer-host slot in `multi_obs_check_all_escaped` (multi.c:2350)**

```c
void multi_obs_check_all_escaped() {
	for(int i = (Netgame.host_is_obs ? 1 : 0); i < MAX_PLAYERS; i++) {
		if(Players[i].connected == CONNECT_PLAYING) {
			return;
		}
	}

	PlayerFinishedLevel(0);
}
```
(The only change: loop start `0` → `(Netgame.host_is_obs ? 1 : 0)`, matching the idiom at `multi.c:834`, `multi.c:4364`.)

- [ ] **Step 2: Bug fix — kmatrix playing-scan (kmatrix.c:~307)**

In `kmatrix_handler`'s `EVENT_WINDOW_DRAW` branch, the loop that sets `km->playing = 1` for any connected player not yet at the score screen: skip the observer host the same way:

```c
			for (i = (Netgame.host_is_obs ? 1 : 0); i < MAX_PLAYERS; i++)
```
(keep the existing `OBSERVER_PLAYER_ID` condition inside the loop unchanged).

- [ ] **Step 3: Behavior check for the two fixes (human observer-host)**

Host a normal LAN game as observer-host (Terminal A: HOST GAME → in the player-select screen **uncheck yourself**, needs one client from Terminal B checked; use the Task 1 two-terminal setup). Client B destroys the reactor and escapes (or dies). Expected: host's kmatrix screen closes ~7 s after B finishes and both advance to level 2. Before this fix the host would hang on the score screen. (If arranging a full reactor run is slow, `bigrat.msn` is a tiny mission — any of the three add-on missions in the hogdir works.)

- [ ] **Step 4: Dedicated endlevel wait (no kmatrix window)**

`main/multi.c` `multi_endlevel_score()` (line 761): the function sets per-player state then calls `kmatrix_view(...)` at line ~789. Guard the window:

```c
	if (Dedicated_server)
		dedicated_endlevel_wait();
	else
		kmatrix_view(Game_mode & GM_NETWORK);
```

Add to `main/dedicated.c`:

```c
/* Headless stand-in for the kmatrix window: keep the netcode pumped while
 * clients sit on their score screens, wait until no real player is still
 * CONNECT_PLAYING, then hold the score-view delay so clients can read it. */
void dedicated_endlevel_wait(void)
{
	fix64 end_time = -1;
	int i, playing;

	con_printf(CON_NORMAL, "[dedicated] level ended, waiting for players + %ds score view\n", KMATRIX_VIEW_SEC);

	for (;;) {
		timer_update();
		timer_delay2(20);
		multi_do_protocol_frame(0, 1);

		playing = 0;
		for (i = (Netgame.host_is_obs ? 1 : 0); i < MAX_PLAYERS; i++) {
			if (Netgame.max_numobservers > 0 && i == OBSERVER_PLAYER_ID)
				continue;
			if (Players[i].connected && Players[i].connected != CONNECT_END_MENU
			    && Players[i].connected != CONNECT_DIED_IN_MINE)
				playing = 1;
		}
		if (!playing)
			Countdown_seconds_left = -1;
		if (end_time == -1 && Countdown_seconds_left < 0 && !playing)
			end_time = timer_query() + (KMATRIX_VIEW_SEC * F1_0);
		if (end_time != -1 && timer_query() >= end_time) {
			multi_send_endlevel_packet();
			Netgame.numobservers = 0;
			return;
		}
	}
}
```

Add `#define KMATRIX_VIEW_SEC 7` visibility: `KMATRIX_VIEW_SEC` is defined in `main/kmatrix.c:57` — move the define to `main/kmatrix.h` so `dedicated.c` can use it (delete the one in kmatrix.c). Includes needed in dedicated.c: `kmatrix.h`, `multi.h`, `timer.h`, `cntrlcen.h` (for `Countdown_seconds_left`), `player.h`, `console.h`.

- [ ] **Step 5: Anarchy level wrap + co-op session end (gameseq.c)**

In `PlayerFinishedLevel()` (`main/gameseq.c:862`), the `Current_level_num == Last_level` anarchy branch (lines ~898-905) ends the game. Add before that check, so a dedicated anarchy game never reaches it:

```c
	if (Dedicated_server && (Game_mode & GM_MULTI) && !(Game_mode & GM_MULTI_COOP)
	    && Current_level_num == Last_level)
	{
		// dedicated anarchy cycles the mission instead of ending the game
		multi_endlevel_score();          // dedicated_endlevel_wait under the hood
		if (Game_mode & GM_NETWORK)
		{
			int secret = 0;
			multi_endlevel(&secret);     // NETSTAT_ENDLEVEL + endlevel packet
		}
		StartNewLevel(1);
		return;
	}
```

In `AdvanceLevel()` (`main/gameseq.c:945`), the game-over path for co-op final level runs `do_end_briefing_screens(...)` (line ~977). Guard UI in dedicated mode:

```c
	if (Current_level_num == Last_level)
	{
		if (Dedicated_server)
		{
			con_printf(CON_NORMAL, "[dedicated] mission complete, closing session\n");
			Dedicated_exit_requested = 1;
			return 1;
		}
		do_end_briefing_screens(Ending_text_filename);
		return 1;
	}
```

And in `PlayerFinishedLevel()`, the `was_multi`/game-over tail that calls `scores_maybe_add_player` + `window_close(Game_wind)` (lines ~922-931): guard with `if (Dedicated_server) return;` before the window ops (the dedicated loop sees `Dedicated_exit_requested` and exits cleanly). Similarly `DoEndLevelScoreGlitz(0)` call at line ~910 for co-op final: skip when `Dedicated_server`.

(`Dedicated_exit_requested` is an `int` in `dedicated.c`, declared in `dedicated.h` — Task 2 creates it.)

(The dedicated branch of `net_udp_wait_for_requests` and the headless-safe sync error paths were added in Task 3 — they are prerequisites of the very first level start. This task covers what happens at the END of a level.)

- [ ] **Step 6: Lifecycle timers in `dedicated.c`**

Replace Task 3's stub `dedicated_should_exit()` (which only checked `Dedicated_exit_requested`) with the full lifecycle version, using `Ded_started_at` set by `dedicated_main` in Task 3:

```c
static fix64 Ded_empty_since;   /* 0 = not currently empty */
static int Ded_ever_had_player;

/* called once per dedicated frame */
int dedicated_should_exit(void)
{
	int players = dedicated_connected_players();
	fix64 now = timer_query();

	if (Dedicated_exit_requested)
		return 1;
	if (players > 0) {
		if (!Ded_ever_had_player)
			con_printf(CON_NORMAL, "[dedicated] first player joined\n");
		Ded_ever_had_player = 1;
		Ded_empty_since = 0;
		return 0;
	}
	if (!Ded_ever_had_player) {
		if (now > Ded_started_at + i2f(Dedicated_cfg.timeout_empty_start)) {
			con_printf(CON_NORMAL, "[dedicated] nobody joined within %ds, closing\n", Dedicated_cfg.timeout_empty_start);
			return 1;
		}
		return 0;
	}
	if (!Ded_empty_since) {
		Ded_empty_since = now;
		con_printf(CON_NORMAL, "[dedicated] empty, closing in %ds unless someone joins\n", Dedicated_cfg.timeout_empty);
		return 0;
	}
	if (now > Ded_empty_since + i2f(Dedicated_cfg.timeout_empty)) {
		con_printf(CON_NORMAL, "[dedicated] still empty, closing session\n");
		return 1;
	}
	return 0;
}
```
(Range check: timers are `fix64`; `i2f(300)` = 19,660,800 — no overflow. `Ded_started_at` is the non-static `fix64` from Task 3.)

- [ ] **Step 7: Build + full lifecycle verification**

```bash
PATH=/c/Programs/msys64/mingw64/bin:$PATH cmake --build build -j
```

Write a session cfg `C:/Users/Yermak/AppData/Local/Temp/ded_test.cfg`:
```
port=42425
game_name=lifecycle
level=1
timeout_empty_start=60
timeout_empty=20
```
Run:
```bash
./build/main/d1x-redux.exe -dedicated C:/Users/Yermak/AppData/Local/Temp/ded_test.cfg -hogdir C:/Users/Yermak/Projects/dxx-redux/d1/hogs -nosound -notitles
```
1. Do not join. Expected: after ~60 s the process logs the empty-start timeout and exits 0.
2. Restart it; join with a client (`-udp_myport 42460`, JOIN GAME MANUALLY → localhost:42425), play; destroy the reactor and die/escape. Expected: client sees score screen; ~7 s after finishing, the level advances to 2 on both sides (server log shows the endlevel wait + new level load).
3. Play to the point of leaving: quit the client. Expected: server logs empty countdown and exits ~20 s later.

- [ ] **Step 8: Commit**

```bash
git add main/multi.c main/kmatrix.c main/kmatrix.h main/gameseq.c main/dedicated.c main/dedicated.h
git commit -m "gameserver: dedicated endlevel flow, lifecycle timers, obs-host endlevel fixes

Fixes two pre-existing observer-host bugs: multi_obs_check_all_escaped and
the kmatrix playing-scan did not skip the host_is_obs slot, hanging
end-of-level for observer hosts.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: Broker binary `d1x-gameserver`

A standalone UDP broker: accepts `GSP_CREATE_REQ`/`GSP_LIST_REQ`, spawns/reaps dedicated children, polls them with the engine's existing lite game-info request. **No engine headers** — `gsp.h` mirrors the few constants it needs; Task 6 adds compile-time equality checks on the engine side.

**Files:**
- Create: `gameserver/gsp.h`
- Create: `gameserver/spawn.h`, `gameserver/spawn.c`
- Create: `gameserver/gameserver.c`
- Create: `gameserver/CMakeLists.txt`
- Modify: `CMakeLists.txt` (root `d1/CMakeLists.txt`, after line 101 `add_subdirectory(texmap)`)

**Interfaces:**
- Consumes: the dedicated engine mode from Tasks 2-4: `d1x-redux -dedicated <cfgfile> -hogdir <dir>` where `<cfgfile>` is a `key=value` text file supporting keys `port`, `blob`, `game_name`, `mission`, `level`, `mode`, `timeout_empty_start`, `timeout_empty` (blob-less = defaults); the child binds UDP `port` and answers `UPID_GAME_INFO_LITE_REQ`.
- Produces: `d1x-gameserver` binary; `gameserver/gsp.h` with the GSP wire constants that Task 6's client code includes.

- [ ] **Step 1: Write `gameserver/gsp.h`**

```c
/* GSP — Game Server Protocol. Spoken ONLY on the broker's control port.
 * The in-game wire protocol (net_udp.c UPIDs) is untouched.
 *
 * This header is shared by the standalone broker (no engine headers!) and
 * the engine client (main/net_udp.c), which compile-time-asserts the
 * mirrored GSP_* constants against their engine counterparts.
 *
 * All integers little-endian, packed (no struct casts on the wire).
 * Layouts:
 *  GSP_CREATE_REQ: u8 op, u8 gsp_ver, u16 multi_proto_ver, u16 blob_len,
 *                  u8 blob[blob_len]   (blob = UPID_GAME_INFO-format
 *                  serialization from net_udp_pack_game_info())
 *  GSP_CREATE_ACK: u8 op, u8 gsp_ver, u8 result, u16 game_port
 *  GSP_LIST_REQ:   u8 op, u8 gsp_ver
 *  GSP_LIST_ACK:   u8 op, u8 gsp_ver, u8 count, count x {
 *                  u16 port, char game_name[GSP_GAME_NAME_LEN],
 *                  char mission_name[GSP_MISSION_NAME_LEN], s32 levelnum,
 *                  u8 gamemode, u8 numconnected, u8 max_numplayers,
 *                  u8 game_status }
 */
#ifndef GSP_H
#define GSP_H

#define GSP_PROTO_VERSION 1

#define GSP_CREATE_REQ 40
#define GSP_CREATE_ACK 41
#define GSP_LIST_REQ   42
#define GSP_LIST_ACK   43

/* GSP_CREATE_ACK result codes */
#define GSP_OK            0
#define GSP_ERR_FULL      1
#define GSP_ERR_SPAWN     2
#define GSP_ERR_RATELIMIT 3
#define GSP_ERR_BADREQ    4

/* Mirrored engine constants — equality asserted in main/net_udp.c */
#define GSP_GAME_NAME_LEN     16  /* NETGAME_NAME_LEN+1 */
#define GSP_MISSION_NAME_LEN   9
#define GSP_UPID_LITE_REQ      4  /* UPID_GAME_INFO_LITE_REQ */
#define GSP_UPID_LITE          5  /* UPID_GAME_INFO_LITE */
#define GSP_UPID_LITE_REQ_SIZE 11 /* UPID_GAME_INFO_LITE_REQ_SIZE */
#define GSP_REQ_ID "D1XR"         /* UDP_REQ_ID */

#define GSP_PORT_DEFAULT      42424
#define GSP_GAME_PORT_BASE    42425
#define GSP_MAX_SESSIONS_CAP  16   /* hard cap; --max-sessions may be lower */
#define GSP_LIST_ENTRY_SIZE   (2 + GSP_GAME_NAME_LEN + GSP_MISSION_NAME_LEN + 4 + 4)
#define GSP_MAX_PACKET        (3 + GSP_MAX_SESSIONS_CAP * GSP_LIST_ENTRY_SIZE)

/* Lite game-info reply layout offsets (engine wire format, see
 * net_udp_pack_game_info lite branch): */
#define GSP_LITE_OFF_GAMENAME   11  /* after u8 upid + 3x u16 version + u32 GameID */
#define GSP_LITE_OFF_MISSNAME   (11 + 16 + 26)          /* game_name[16], mission_title[26] */
#define GSP_LITE_OFF_LEVELNUM   (GSP_LITE_OFF_MISSNAME + 9)
#define GSP_LITE_OFF_GAMEMODE   (GSP_LITE_OFF_LEVELNUM + 4)
#define GSP_LITE_OFF_STATUS     (GSP_LITE_OFF_GAMEMODE + 3) /* +RefusePlayers,difficulty */
#define GSP_LITE_OFF_NUMCONN    (GSP_LITE_OFF_STATUS + 1)
#define GSP_LITE_OFF_MAXPLAYERS (GSP_LITE_OFF_NUMCONN + 1)
#define GSP_LITE_SIZE           (GSP_LITE_OFF_MAXPLAYERS + 2) /* +game_flags = 73 */

#endif
```

- [ ] **Step 2: Write `gameserver/spawn.h` and `gameserver/spawn.c`**

`gameserver/spawn.h`:

```c
/* Minimal cross-platform child-process shim for the broker. */
#ifndef GS_SPAWN_H
#define GS_SPAWN_H

typedef struct gs_proc {
#ifdef _WIN32
	void *handle;               /* HANDLE, NULL when slot free */
	unsigned long pid;
#else
	int pid;                    /* 0 when slot free */
#endif
} gs_proc;

/* argv is NULL-terminated, argv[0] = executable path.
 * Returns 0 on success. */
int gs_spawn(gs_proc *out, char *const argv[]);
int gs_proc_running(gs_proc *p);   /* 1 running, 0 exited/free */
void gs_proc_kill(gs_proc *p);     /* forceful terminate */
void gs_proc_close(gs_proc *p);    /* reap zombie / close handle, mark free */

#endif
```

`gameserver/spawn.c`:

```c
#include <stdio.h>
#include <string.h>
#include "spawn.h"

#ifdef _WIN32
#include <windows.h>

int gs_spawn(gs_proc *out, char *const argv[])
{
	char cmdline[2048] = "";
	int i;
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;

	for (i = 0; argv[i]; i++) {
		if (i) strncat(cmdline, " ", sizeof(cmdline) - strlen(cmdline) - 1);
		strncat(cmdline, "\"", sizeof(cmdline) - strlen(cmdline) - 1);
		strncat(cmdline, argv[i], sizeof(cmdline) - strlen(cmdline) - 1);
		strncat(cmdline, "\"", sizeof(cmdline) - strlen(cmdline) - 1);
	}
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	memset(&pi, 0, sizeof(pi));
	if (!CreateProcessA(argv[0], cmdline, NULL, NULL, FALSE,
	                    CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi))
		return -1;
	CloseHandle(pi.hThread);
	out->handle = pi.hProcess;
	out->pid = pi.dwProcessId;
	return 0;
}

int gs_proc_running(gs_proc *p)
{
	DWORD code;
	if (!p->handle)
		return 0;
	if (!GetExitCodeProcess(p->handle, &code))
		return 0;
	return code == STILL_ACTIVE;
}

void gs_proc_kill(gs_proc *p)
{
	if (p->handle)
		TerminateProcess(p->handle, 1);
}

void gs_proc_close(gs_proc *p)
{
	if (p->handle)
		CloseHandle(p->handle);
	p->handle = NULL;
	p->pid = 0;
}

#else /* POSIX */
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

int gs_spawn(gs_proc *out, char *const argv[])
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execv(argv[0], argv);
		_exit(127);
	}
	out->pid = pid;
	return 0;
}

int gs_proc_running(gs_proc *p)
{
	int status;
	pid_t r;
	if (p->pid <= 0)
		return 0;
	r = waitpid(p->pid, &status, WNOHANG);
	if (r == 0)
		return 1;
	p->pid = -p->pid; /* remember reaped, gs_proc_close finishes cleanup */
	return 0;
}

void gs_proc_kill(gs_proc *p)
{
	if (p->pid > 0)
		kill(p->pid, SIGTERM);
}

void gs_proc_close(gs_proc *p)
{
	int status;
	if (p->pid > 0)
		waitpid(p->pid, &status, 0);
	p->pid = 0;
}
#endif
```

- [ ] **Step 3: Write `gameserver/gameserver.c`**

```c
/* d1x-gameserver — session broker for dedicated d1x-redux games.
 *
 * One UDP socket. Clients send GSP_CREATE_REQ / GSP_LIST_REQ (see gsp.h).
 * For each session the broker spawns `d1x-redux -dedicated <cfg>` on the
 * next free game port and polls it with the engine's lite game-info
 * request; the first reply marks the session ready and triggers the
 * GSP_CREATE_ACK to the creator. Exited children are reaped and their
 * port slot reused. The broker never parses game rules and never joins
 * the game protocol beyond the lite info request.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define close_socket closesocket
typedef int socklen_t_;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#define close_socket close
#define SOCKET int
#define INVALID_SOCKET (-1)
typedef socklen_t socklen_t_;
#endif

#include "gsp.h"
#include "spawn.h"

#define GS_READY_TIMEOUT   10   /* s: child must answer lite poll */
#define GS_POLL_INTERVAL    5   /* s: lite poll cadence */
#define GS_RATE_WINDOW      5   /* s: min gap between creates per IP */
#define GS_RATE_SLOTS       8

typedef enum { SLOT_FREE = 0, SLOT_SPAWNING, SLOT_READY } slot_state;

typedef struct session {
	slot_state state;
	unsigned short port;
	gs_proc proc;
	time_t created_at;
	time_t last_poll;
	struct sockaddr_in creator;      /* who gets the CREATE_ACK */
	/* cached lite info */
	char game_name[GSP_GAME_NAME_LEN];
	char mission_name[GSP_MISSION_NAME_LEN];
	int levelnum;
	unsigned char gamemode, numconnected, max_numplayers, game_status;
	char cfg_path[512];
	char blob_path[512];
} session;

static session Sessions[GSP_MAX_SESSIONS_CAP];
static SOCKET Sock = INVALID_SOCKET;
static volatile int Running = 1;

/* config (flags override env override defaults) */
static int Cfg_port = GSP_PORT_DEFAULT;
static int Cfg_game_port_base = GSP_GAME_PORT_BASE;
static int Cfg_max_sessions = 8;
static const char *Cfg_engine = NULL;   /* required */
static const char *Cfg_hogdir = NULL;   /* required */
static const char *Cfg_tmpdir = NULL;
static int Cfg_timeout_empty_start = 300;
static int Cfg_timeout_empty = 60;
static int Cfg_test_create = 0;

static struct { unsigned long ip; time_t at; } Rate[GS_RATE_SLOTS];

static void on_signal(int sig) { (void)sig; Running = 0; }

static const char *arg_or_env(int argc, char **argv, const char *flag,
                              const char *env, const char *dflt)
{
	int i;
	const char *v;
	for (i = 1; i < argc - 1; i++)
		if (!strcmp(argv[i], flag))
			return argv[i + 1];
	v = getenv(env);
	return v ? v : dflt;
}

static int flag_present(int argc, char **argv, const char *flag)
{
	int i;
	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], flag))
			return 1;
	return 0;
}

static void put_u16(unsigned char *p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static unsigned get_u16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static int get_s32(const unsigned char *p)
{ return (int)((unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24)); }

static void send_create_ack(struct sockaddr_in *to, unsigned char result, unsigned short port)
{
	unsigned char buf[5];
	buf[0] = GSP_CREATE_ACK;
	buf[1] = GSP_PROTO_VERSION;
	buf[2] = result;
	put_u16(buf + 3, port);
	sendto(Sock, (char *)buf, sizeof(buf), 0, (struct sockaddr *)to, sizeof(*to));
}

static session *find_by_port(unsigned short port)
{
	int i;
	for (i = 0; i < Cfg_max_sessions; i++)
		if (Sessions[i].state != SLOT_FREE && Sessions[i].port == port)
			return &Sessions[i];
	return NULL;
}

static void free_session(session *s, int kill_proc)
{
	if (kill_proc && gs_proc_running(&s->proc))
		gs_proc_kill(&s->proc);
	gs_proc_close(&s->proc);
	if (s->cfg_path[0]) remove(s->cfg_path);
	if (s->blob_path[0]) remove(s->blob_path);
	memset(s, 0, sizeof(*s));
}

static int rate_limited(struct sockaddr_in *from)
{
	unsigned long ip = from->sin_addr.s_addr;
	time_t now = time(NULL);
	int i, oldest = 0;
	for (i = 0; i < GS_RATE_SLOTS; i++) {
		if (Rate[i].ip == ip && now - Rate[i].at < GS_RATE_WINDOW)
			return 1;
		if (Rate[i].at < Rate[oldest].at)
			oldest = i;
	}
	Rate[oldest].ip = ip;
	Rate[oldest].at = now;
	return 0;
}

/* Spawn a child for slot s. blob/blob_len may be NULL/0 (defaults game). */
static int spawn_session(session *s, const unsigned char *blob, unsigned blob_len)
{
	FILE *f;
	char port_buf[16];
	char *argv[10];
	int n = 0;

	snprintf(s->cfg_path, sizeof(s->cfg_path), "%s/gsp_%u.cfg", Cfg_tmpdir, (unsigned)s->port);
	s->blob_path[0] = 0;

	if (blob && blob_len) {
		snprintf(s->blob_path, sizeof(s->blob_path), "%s/gsp_%u.bin", Cfg_tmpdir, (unsigned)s->port);
		f = fopen(s->blob_path, "wb");
		if (!f)
			return -1;
		fwrite(blob, 1, blob_len, f);
		fclose(f);
	}

	f = fopen(s->cfg_path, "w");
	if (!f)
		return -1;
	fprintf(f, "port=%u\n", (unsigned)s->port);
	if (s->blob_path[0])
		fprintf(f, "blob=%s\n", s->blob_path);
	fprintf(f, "timeout_empty_start=%d\n", Cfg_timeout_empty_start);
	fprintf(f, "timeout_empty=%d\n", Cfg_timeout_empty);
	fclose(f);

	snprintf(port_buf, sizeof(port_buf), "%u", (unsigned)s->port);
	argv[n++] = (char *)Cfg_engine;
	argv[n++] = "-dedicated";
	argv[n++] = s->cfg_path;
	argv[n++] = "-hogdir";
	argv[n++] = (char *)Cfg_hogdir;
	argv[n++] = "-nosound";
	argv[n++] = "-notitles";
	argv[n] = NULL;
	(void)port_buf;

	if (gs_spawn(&s->proc, argv) != 0)
		return -1;
	return 0;
}

static void handle_create(struct sockaddr_in *from, const unsigned char *data, int len)
{
	int i;
	session *s = NULL;
	unsigned blob_len;

	if (len < 6 || data[1] != GSP_PROTO_VERSION) {
		send_create_ack(from, GSP_ERR_BADREQ, 0);
		return;
	}
	blob_len = get_u16(data + 4);
	if (6 + (int)blob_len != len) {
		send_create_ack(from, GSP_ERR_BADREQ, 0);
		return;
	}
	if (rate_limited(from)) {
		send_create_ack(from, GSP_ERR_RATELIMIT, 0);
		return;
	}
	for (i = 0; i < Cfg_max_sessions; i++)
		if (Sessions[i].state == SLOT_FREE) { s = &Sessions[i]; break; }
	if (!s) {
		send_create_ack(from, GSP_ERR_FULL, 0);
		return;
	}
	memset(s, 0, sizeof(*s));
	s->port = (unsigned short)(Cfg_game_port_base + (int)(s - Sessions));
	s->state = SLOT_SPAWNING;
	s->created_at = time(NULL);
	s->creator = *from;
	if (spawn_session(s, blob_len ? data + 6 : NULL, blob_len) != 0) {
		printf("[gs] spawn failed for port %u\n", (unsigned)s->port);
		send_create_ack(from, GSP_ERR_SPAWN, 0);
		free_session(s, 1);
		return;
	}
	printf("[gs] spawning session on port %u (pid %lu) for %s\n",
	       (unsigned)s->port, (unsigned long)s->proc.pid, inet_ntoa(from->sin_addr));
}

static void handle_list(struct sockaddr_in *from, const unsigned char *data, int len)
{
	unsigned char buf[GSP_MAX_PACKET];
	int n = 3, count = 0, i;

	if (len < 2 || data[1] != GSP_PROTO_VERSION)
		return;
	for (i = 0; i < Cfg_max_sessions; i++) {
		session *s = &Sessions[i];
		if (s->state != SLOT_READY)
			continue;
		put_u16(buf + n, s->port);                            n += 2;
		memcpy(buf + n, s->game_name, GSP_GAME_NAME_LEN);     n += GSP_GAME_NAME_LEN;
		memcpy(buf + n, s->mission_name, GSP_MISSION_NAME_LEN); n += GSP_MISSION_NAME_LEN;
		buf[n] = s->levelnum & 0xff; buf[n+1] = (s->levelnum >> 8) & 0xff;
		buf[n+2] = (s->levelnum >> 16) & 0xff; buf[n+3] = (s->levelnum >> 24) & 0xff; n += 4;
		buf[n++] = s->gamemode;
		buf[n++] = s->numconnected;
		buf[n++] = s->max_numplayers;
		buf[n++] = s->game_status;
		count++;
	}
	buf[0] = GSP_LIST_ACK;
	buf[1] = GSP_PROTO_VERSION;
	buf[2] = (unsigned char)count;
	sendto(Sock, (char *)buf, n, 0, (struct sockaddr *)from, sizeof(*from));
}

/* Child replied to our lite poll: cache its state. */
static void handle_lite_info(struct sockaddr_in *from, const unsigned char *data, int len)
{
	session *s;
	if (len < GSP_LITE_SIZE)
		return;
	s = find_by_port(ntohs(from->sin_port));
	if (!s)
		return;
	memcpy(s->game_name, data + GSP_LITE_OFF_GAMENAME, GSP_GAME_NAME_LEN);
	s->game_name[GSP_GAME_NAME_LEN - 1] = 0;
	memcpy(s->mission_name, data + GSP_LITE_OFF_MISSNAME, GSP_MISSION_NAME_LEN);
	s->mission_name[GSP_MISSION_NAME_LEN - 1] = 0;
	s->levelnum = get_s32(data + GSP_LITE_OFF_LEVELNUM);
	s->gamemode = data[GSP_LITE_OFF_GAMEMODE];
	s->game_status = data[GSP_LITE_OFF_STATUS];
	s->numconnected = data[GSP_LITE_OFF_NUMCONN];
	s->max_numplayers = data[GSP_LITE_OFF_MAXPLAYERS];
	if (s->state == SLOT_SPAWNING) {
		s->state = SLOT_READY;
		printf("[gs] session on port %u ready: '%s' (%s L%d)\n",
		       (unsigned)s->port, s->game_name, s->mission_name, s->levelnum);
		send_create_ack(&s->creator, GSP_OK, s->port);
	}
}

static void poll_children(void)
{
	/* engine version numbers injected by CMake, same values the engine
	 * itself is compiled with (net_udp_check_game_info_request compares
	 * them exactly) */
	unsigned char req[GSP_UPID_LITE_REQ_SIZE];
	struct sockaddr_in to;
	time_t now = time(NULL);
	int i;

	req[0] = GSP_UPID_LITE_REQ;
	memcpy(req + 1, GSP_REQ_ID, 4);
	put_u16(req + 5, DXX_VERSION_MAJORi);
	put_u16(req + 7, DXX_VERSION_MINORi);
	put_u16(req + 9, DXX_VERSION_MICROi);

	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = inet_addr("127.0.0.1");

	for (i = 0; i < Cfg_max_sessions; i++) {
		session *s = &Sessions[i];
		if (s->state == SLOT_FREE)
			continue;

		if (!gs_proc_running(&s->proc)) {
			printf("[gs] session on port %u exited\n", (unsigned)s->port);
			if (s->state == SLOT_SPAWNING)
				send_create_ack(&s->creator, GSP_ERR_SPAWN, 0);
			free_session(s, 0);
			continue;
		}
		if (s->state == SLOT_SPAWNING && now - s->created_at > GS_READY_TIMEOUT) {
			printf("[gs] session on port %u never became ready, killing\n", (unsigned)s->port);
			send_create_ack(&s->creator, GSP_ERR_SPAWN, 0);
			free_session(s, 1);
			continue;
		}
		if (now - s->last_poll >= (s->state == SLOT_SPAWNING ? 1 : GS_POLL_INTERVAL)) {
			s->last_poll = now;
			to.sin_port = htons(s->port);
			sendto(Sock, (char *)req, sizeof(req), 0, (struct sockaddr *)&to, sizeof(to));
		}
	}
}

int main(int argc, char **argv)
{
	struct sockaddr_in bindaddr;
	fd_set rfds;
	struct timeval tv;

	Cfg_port = atoi(arg_or_env(argc, argv, "--port", "GS_PORT", "42424"));
	Cfg_game_port_base = atoi(arg_or_env(argc, argv, "--game-port-base", "GS_GAME_PORT_BASE", "42425"));
	Cfg_max_sessions = atoi(arg_or_env(argc, argv, "--max-sessions", "GS_MAX_SESSIONS", "8"));
	Cfg_engine = arg_or_env(argc, argv, "--engine", "GS_ENGINE", NULL);
	Cfg_hogdir = arg_or_env(argc, argv, "--hogdir", "GS_HOGDIR", NULL);
	Cfg_timeout_empty_start = atoi(arg_or_env(argc, argv, "--timeout-empty-start", "GS_TIMEOUT_EMPTY_START", "300"));
	Cfg_timeout_empty = atoi(arg_or_env(argc, argv, "--timeout-empty", "GS_TIMEOUT_EMPTY", "60"));
	Cfg_test_create = flag_present(argc, argv, "--test-create");
#ifdef _WIN32
	Cfg_tmpdir = arg_or_env(argc, argv, "--tmpdir", "TEMP", ".");
#else
	Cfg_tmpdir = arg_or_env(argc, argv, "--tmpdir", "GS_TMPDIR", "/tmp");
#endif

	if (Cfg_max_sessions < 1 || Cfg_max_sessions > GSP_MAX_SESSIONS_CAP)
		Cfg_max_sessions = 8;
	if (!Cfg_engine || !Cfg_hogdir) {
		fprintf(stderr,
		        "usage: d1x-gameserver --engine <path-to-d1x-redux> --hogdir <data-dir>\n"
		        "  [--port 42424] [--game-port-base 42425] [--max-sessions 8]\n"
		        "  [--timeout-empty-start 300] [--timeout-empty 60] [--tmpdir <dir>] [--test-create]\n"
		        "  (env: GS_PORT, GS_GAME_PORT_BASE, GS_MAX_SESSIONS, GS_ENGINE, GS_HOGDIR,\n"
		        "   GS_TIMEOUT_EMPTY_START, GS_TIMEOUT_EMPTY)\n");
		return 2;
	}

#ifdef _WIN32
	{
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			fprintf(stderr, "[gs] WSAStartup failed\n");
			return 1;
		}
	}
#endif
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	Sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (Sock == INVALID_SOCKET) {
		fprintf(stderr, "[gs] socket() failed\n");
		return 1;
	}
	memset(&bindaddr, 0, sizeof(bindaddr));
	bindaddr.sin_family = AF_INET;
	bindaddr.sin_addr.s_addr = INADDR_ANY;
	bindaddr.sin_port = htons((unsigned short)Cfg_port);
	if (bind(Sock, (struct sockaddr *)&bindaddr, sizeof(bindaddr)) != 0) {
		fprintf(stderr, "[gs] cannot bind UDP port %d\n", Cfg_port);
		return 1;
	}

	printf("[gs] d1x-gameserver listening on UDP %d; engine=%s hogdir=%s max-sessions=%d game-ports=%d..%d\n",
	       Cfg_port, Cfg_engine, Cfg_hogdir, Cfg_max_sessions,
	       Cfg_game_port_base, Cfg_game_port_base + Cfg_max_sessions - 1);

	if (Cfg_test_create) {
		session *s = &Sessions[0];
		memset(s, 0, sizeof(*s));
		s->port = (unsigned short)Cfg_game_port_base;
		s->state = SLOT_SPAWNING;
		s->created_at = time(NULL);
		s->creator = bindaddr; /* ACK goes nowhere useful; fine for smoke test */
		if (spawn_session(s, NULL, 0) != 0) {
			printf("[gs] --test-create spawn failed\n");
			free_session(s, 1);
		} else {
			printf("[gs] --test-create: spawned defaults session on port %u\n", (unsigned)s->port);
		}
	}

	while (Running) {
		FD_ZERO(&rfds);
		FD_SET(Sock, &rfds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		if (select((int)Sock + 1, &rfds, NULL, NULL, &tv) > 0 && FD_ISSET(Sock, &rfds)) {
			unsigned char data[2048];
			struct sockaddr_in from;
			socklen_t_ fromlen = sizeof(from);
			int len = recvfrom(Sock, (char *)data, sizeof(data), 0,
			                   (struct sockaddr *)&from, &fromlen);
			if (len >= 2) {
				switch (data[0]) {
				case GSP_CREATE_REQ: handle_create(&from, data, len); break;
				case GSP_LIST_REQ:   handle_list(&from, data, len);   break;
				case GSP_UPID_LITE:  handle_lite_info(&from, data, len); break;
				default: break; /* ignore unknown */
				}
			}
		}
		poll_children();
	}

	printf("[gs] shutting down, terminating sessions\n");
	{
		int i;
		for (i = 0; i < Cfg_max_sessions; i++)
			if (Sessions[i].state != SLOT_FREE)
				free_session(&Sessions[i], 1);
	}
	close_socket(Sock);
#ifdef _WIN32
	WSACleanup();
#endif
	return 0;
}
```

- [ ] **Step 4: Write `gameserver/CMakeLists.txt` and hook into the root**

`gameserver/CMakeLists.txt`:

```cmake
add_executable(d1x-gameserver gameserver.c spawn.c)
target_compile_definitions(d1x-gameserver PRIVATE
    DXX_VERSION_MAJORi=${PROJECT_VERSION_MAJOR}
    DXX_VERSION_MINORi=${PROJECT_VERSION_MINOR}
    DXX_VERSION_MICROi=${PROJECT_VERSION_PATCH}
)
if(WIN32)
    target_link_libraries(d1x-gameserver ws2_32)
endif()
```

Note: `main/CMakeLists.txt:75` uses the same `${PROJECT_VERSION_*}` variables, so broker and engine agree on the version triple that `net_udp_check_game_info_request()` compares. `${PROJECT_VERSION_PATCH}` may be empty for a two-part version — check how `main/CMakeLists.txt:75-77` handles MICRO and copy exactly what it does.

In root `d1/CMakeLists.txt`, after line 101 (`add_subdirectory(texmap)`):

```cmake
add_subdirectory(gameserver)
```

(No `add_dependencies` — the broker shares no libs with the game.)

- [ ] **Step 5: Build**

```bash
PATH=/c/Programs/msys64/mingw64/bin:$PATH cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
PATH=/c/Programs/msys64/mingw64/bin:$PATH cmake --build build -j
```
Expected: exits 0, produces `build/gameserver/d1x-gameserver.exe`.

- [ ] **Step 6: Broker smoke test (no client needed)**

```bash
./build/gameserver/d1x-gameserver.exe --engine "$(pwd)/build/main/d1x-redux.exe" --hogdir C:/Users/Yermak/Projects/dxx-redux/d1/hogs --test-create
```
Expected within ~10 s:
```
[gs] d1x-gameserver listening on UDP 42424; ...
[gs] --test-create: spawned defaults session on port 42425
[gs] session on port 42425 ready: 'Dedicated' ( L1)
```
(The builtin Descent mission's filename is the empty string — `D1_MISSION_FILENAME` in `main/mission.h:31` — so the mission field prints empty; add-on missions print their stem.)
Then verify LIST from PowerShell (second terminal):

```powershell
$u = New-Object System.Net.Sockets.UdpClient
$u.Connect("127.0.0.1", 42424)
[void]$u.Send([byte[]](42,1), 2)   # GSP_LIST_REQ v1
$ep = New-Object System.Net.IPEndPoint ([System.Net.IPAddress]::Any, 0)
$r = $u.Receive([ref]$ep)
"opcode=$($r[0]) count=$($r[2]) port=$($r[3] + 256*$r[4])"
```
Expected output: `opcode=43 count=1 port=42425`.

Stop the broker with Ctrl+C — expected: `[gs] shutting down, terminating sessions` and the child process exits (check Task Manager / `tasklist | grep d1x-redux` shows none).

- [ ] **Step 7: Commit**

```bash
git add gameserver/ CMakeLists.txt
git commit -m "gameserver: add d1x-gameserver session broker

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: Client — GSP requests + "GAME SERVER" menu

Adds the player-facing side: a persisted server address, a Game Server submenu with Browse/Create, GSP request/response handling on the client's existing UDP socket, and auto-join of created games via the existing direct-join machinery.

**Files:**
- Create: `main/gameserver_menus.c`, `main/gameserver_menus.h`
- Modify: `main/net_udp.c` (GSP response dispatch in `net_udp_process_packet` at line ~3375; compile-time `gsp.h` sync checks; make `net_udp_game_connect` + `direct_join` usable from the new file)
- Modify: `main/menu.c:2589-2599` (menu entry), `main/menu.h` (constant)
- Modify: `main/config.c`, `main/config.h` (GameserverAddr persistence)
- Modify: `main/CMakeLists.txt` (add `gameserver_menus.c` to the source list)

**Interfaces:**
- Consumes: `gameserver/gsp.h` constants (Task 5); `net_udp_pack_game_info()` (Task 1); existing `direct_join` struct, `net_udp_game_connect(direct_join *dj)`, `udp_open_socket`, `udp_dns_filladdr`, `net_udp_listen`, `multi_new_game`, `change_playernum_to` (all in `net_udp.c`).
- Produces: `void do_gameserver_menu(void);` (called from `menu.c`); globals `Gameserver_create_mode` (checked in `net_udp_game_param_handler`) and `int net_udp_gameserver_create(void)` (called from the same handler).

- [ ] **Step 1: `direct_join` visibility**

`direct_join` is defined inside `net_udp.c` (search `typedef struct direct_join`, around line 1030). Move the typedef to `main/net_udp.h` (below the `UDP_mdata_recv` struct) so `gameserver_menus.c` can use it, and add these prototypes to `net_udp.h`:

```c
int net_udp_game_connect(direct_join *dj);
int udp_open_socket(int socknum, int port);
int udp_dns_filladdr(char *host, int port, struct _sockaddr *sAddr);
void net_udp_init();
void net_udp_close();
void net_udp_listen();
```
(Remove any now-duplicate `static`/local declarations in `net_udp.c` accordingly; several of these already have non-static definitions — only add missing prototypes.)

- [ ] **Step 2: GSP state + dispatch in `net_udp.c`**

Near the top of `net_udp.c` (after the `#include "gsp.h"` you add to the include block — path via the `main/CMakeLists.txt` include dirs, add `../gameserver` to `target_include_directories` there):

```c
#include "gsp.h"

#include "gameserver_menus.h"   /* mailbox types + externs */

/* compile-time sync between standalone gsp.h and engine headers */
#if GSP_GAME_NAME_LEN != (NETGAME_NAME_LEN+1)
#error gsp.h GSP_GAME_NAME_LEN out of sync with NETGAME_NAME_LEN
#endif
#if GSP_MISSION_NAME_LEN != 9
#error gsp.h GSP_MISSION_NAME_LEN out of sync
#endif
#if GSP_UPID_LITE_REQ != UPID_GAME_INFO_LITE_REQ || GSP_UPID_LITE != UPID_GAME_INFO_LITE
#error gsp.h UPID mirror out of sync
#endif
#if GSP_UPID_LITE_REQ_SIZE != UPID_GAME_INFO_LITE_REQ_SIZE
#error gsp.h UPID_GAME_INFO_LITE_REQ_SIZE mirror out of sync
#endif
```

(The mailbox globals `GSP_create_result` / `GSP_list_result` / `GSP_awaiting` are **defined** in `gameserver_menus.c` — add these three definitions at the top of that file in Step 5, right after `Gameserver_create_mode`:)

```c
gsp_create_result GSP_create_result;
gsp_list_result GSP_list_result;
int GSP_awaiting; /* 0 none, 1 awaiting create ack, 2 awaiting list ack */
```

In `net_udp_process_packet()` (net_udp.c:3375), add cases before the `default`:

```c
		case GSP_CREATE_ACK:
			if (GSP_awaiting == 1 && length >= 5) {
				GSP_create_result.result = data[2];
				GSP_create_result.port = GET_INTEL_SHORT(&data[3]);
				GSP_create_result.valid = 1;
			}
			break;
		case GSP_LIST_ACK:
			if (GSP_awaiting == 2 && length >= 3) {
				int i, off = 3;
				int count = data[2];
				if (count > GSP_MAX_SESSIONS_CAP)
					count = GSP_MAX_SESSIONS_CAP;
				if (length < 3 + count * GSP_LIST_ENTRY_SIZE)
					break;
				for (i = 0; i < count; i++) {
					gsp_list_entry *e = &GSP_list_result.entries[i];
					e->port = GET_INTEL_SHORT(&data[off]); off += 2;
					memcpy(e->game_name, &data[off], GSP_GAME_NAME_LEN); off += GSP_GAME_NAME_LEN;
					e->game_name[GSP_GAME_NAME_LEN-1] = 0;
					memcpy(e->mission_name, &data[off], GSP_MISSION_NAME_LEN); off += GSP_MISSION_NAME_LEN;
					e->mission_name[GSP_MISSION_NAME_LEN-1] = 0;
					e->levelnum = GET_INTEL_INT(&data[off]); off += 4;
					e->gamemode = data[off++];
					e->numconnected = data[off++];
					e->max_numplayers = data[off++];
					e->game_status = data[off++];
				}
				GSP_list_result.count = count;
				GSP_list_result.valid = 1;
			}
			break;
```

The mailbox types live in `main/gameserver_menus.h`:

```c
#ifndef GAMESERVER_MENUS_H
#define GAMESERVER_MENUS_H

#include "gsp.h"

typedef struct gsp_list_entry {
	unsigned short port;
	char game_name[GSP_GAME_NAME_LEN];
	char mission_name[GSP_MISSION_NAME_LEN];
	int levelnum;
	unsigned char gamemode, numconnected, max_numplayers, game_status;
} gsp_list_entry;

typedef struct gsp_list_result {
	int valid;
	int count;
	gsp_list_entry entries[GSP_MAX_SESSIONS_CAP];
} gsp_list_result;

typedef struct gsp_create_result {
	int valid;
	unsigned char result;
	unsigned short port;
} gsp_create_result;

extern gsp_create_result GSP_create_result;
extern gsp_list_result GSP_list_result;
extern int GSP_awaiting;
extern int Gameserver_create_mode;

void do_gameserver_menu(void);
int net_udp_gameserver_create(void);

#endif
```

- [ ] **Step 3: config persistence**

`main/config.h`: inside `struct Cfg` add:

```c
	char GameserverAddr[128];
	int GameserverPort;
```

`main/config.c`: in `ReadConfigFile()` defaults block (after line ~120):

```c
	memset(GameCfg.GameserverAddr, 0, sizeof(GameCfg.GameserverAddr));
	GameCfg.GameserverPort = 42424;
```

In the key parsing loop, following the existing pattern (`strncpy` + strip `\n` like `CMLevelMusicPath` at lines ~161-165):

```c
		else if (!strcmp(line, GameserverAddrStr))
		{
			strncpy(GameCfg.GameserverAddr, value, sizeof(GameCfg.GameserverAddr) - 1);
			p = strchr(GameCfg.GameserverAddr, '\n');
			if (p) *p = 0;
		}
		else if (!strcmp(line, GameserverPortStr))
			GameCfg.GameserverPort = strtol(value, NULL, 10);
```

with the key-name constants next to the existing ones (top of config.c):

```c
static char *GameserverAddrStr = "GameserverAddr";
static char *GameserverPortStr = "GameserverPort";
```

and in `WriteConfigFile()`, following the existing `PHYSFSX_printf` pattern:

```c
	PHYSFSX_printf(infile, "%s=%s\n", GameserverAddrStr, GameCfg.GameserverAddr);
	PHYSFSX_printf(infile, "%s=%d\n", GameserverPortStr, GameCfg.GameserverPort);
```

- [ ] **Step 4: menu entry**

`main/menu.h`: add `MENU_GAMESERVER` to the menu-id enum/defines (find `MENU_JOIN_MANUAL_UDP_NETGAME` and add a new id after the last used value).

`main/menu.c:2596` (after the JOIN GAME MANUALLY line):

```c
	m[num_options].type=NM_TYPE_MENU; m[num_options].text="GAME SERVER"; menu_choice[num_options]=MENU_GAMESERVER; num_options++;
```

In the same file's menu dispatch (`do_option`, the `switch` containing `case MENU_MULTIPLAYER:` at line ~595), add:

```c
		case MENU_GAMESERVER:
			do_gameserver_menu();
			break;
```

with `#include "gameserver_menus.h"` at the top of `menu.c`.

- [ ] **Step 5: `main/gameserver_menus.c`**

```c
/* Client-side Game Server UI: browse and create sessions on a
 * d1x-gameserver broker, then join via the normal direct-join flow. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gameserver_menus.h"
#include "net_udp.h"
#include "multi.h"
#include "newmenu.h"
#include "config.h"
#include "text.h"
#include "timer.h"
#include "u_mem.h"
#include "dxxerror.h"
#include "gamefont.h"
#include "byteswap.h"
#include "args.h"

int Gameserver_create_mode = 0;

static struct _sockaddr GSP_server_addr;
static char GSP_portbuf[6];
static char GSP_myportbuf[6];

/* ---- transport helpers ---- */

static int gsp_open_and_resolve(void)
{
	if ((atoi(GSP_myportbuf)) <= 1024 || (atoi(GSP_myportbuf)) > 65535)
		snprintf(GSP_myportbuf, sizeof(GSP_myportbuf), "%d", UDP_PORT_DEFAULT);

	if (udp_open_socket(0, atoi(GSP_myportbuf)) != 0)
		return 0;
	if (udp_dns_filladdr(GameCfg.GameserverAddr, atoi(GSP_portbuf), &GSP_server_addr) < 0)
		return 0;
	return 1;
}

static void gsp_send_list_req(void)
{
	ubyte buf[2];
	buf[0] = GSP_LIST_REQ;
	buf[1] = GSP_PROTO_VERSION;
	dxx_sendto(UDP_Socket[0], buf, sizeof(buf), 0, (struct sockaddr *)&GSP_server_addr, sizeof(struct _sockaddr));
}

static void gsp_send_create_req(ubyte *blob, int blob_len)
{
	ubyte buf[6 + UPID_GAME_INFO_SIZE];
	buf[0] = GSP_CREATE_REQ;
	buf[1] = GSP_PROTO_VERSION;
	PUT_INTEL_SHORT(buf + 2, MULTI_PROTO_VERSION);
	PUT_INTEL_SHORT(buf + 4, blob_len);
	memcpy(buf + 6, blob, blob_len);
	dxx_sendto(UDP_Socket[0], buf, 6 + blob_len, 0, (struct sockaddr *)&GSP_server_addr, sizeof(struct _sockaddr));
}

/* ---- join helper: hand a server session to the normal join flow ---- */

static void gsp_join_session(unsigned short port)
{
	direct_join *dj;
	char portbuf[6];
	newmenu_item m[1];

	MALLOC(dj, direct_join, 1);
	if (!dj)
		return;
	memset(dj, 0, sizeof(*dj));

	snprintf(dj->addrbuf, sizeof(dj->addrbuf), "%s", GameCfg.GameserverAddr);
	snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)port);
	snprintf(dj->portbuf, sizeof(dj->portbuf), "%s", portbuf);

	if (udp_dns_filladdr(dj->addrbuf, atoi(dj->portbuf), &dj->host_addr) < 0) {
		d_free(dj);
		return;
	}

	multi_new_game();
	net_udp_reset_connection_statuses();
	N_players = 0;
	change_playernum_to(1);
	dj->start_time = timer_query();
	dj->last_time = 0;
	memcpy((struct _sockaddr *)&Netgame.players[0].protocol.udp.addr,
	       (struct _sockaddr *)&dj->host_addr, sizeof(struct _sockaddr));
	dj->connecting = 1;

	m[0].type = NM_TYPE_TEXT; m[0].text = "Connecting to session...";
	newmenu_do1(NULL, "GAME SERVER", 1, m,
	            (int (*)(newmenu *, d_event *, void *))gsp_join_poll, dj, 0);
}

static int gsp_join_poll(newmenu *menu, d_event *event, direct_join *dj)
{
	menu = menu;
	switch (event->type)
	{
		case EVENT_IDLE:
			if (dj->connecting) {
				if (net_udp_game_connect(dj))
					return -2; /* joined; close this window */
				if (!dj->connecting)
					return -2; /* gave up (error box already shown) */
			}
			break;
		case EVENT_WINDOW_CLOSE:
			if (!Game_wind)
				net_udp_close();
			d_free(dj);
			break;
		default:
			break;
	}
	return 0;
}
```

(Note: define `gsp_join_poll` above `gsp_join_session` in the real file — shown out of order here for readability.)

```c
/* ---- browse ---- */

typedef struct gsp_browse_state {
	fix64 start_time;
	fix64 last_send;
	int done;
} gsp_browse_state;

static int gsp_browse_poll(newmenu *menu, d_event *event, gsp_browse_state *bs)
{
	menu = menu;
	if (event->type != EVENT_IDLE)
		return 0;

	timer_delay2(5);
	net_udp_listen();

	if (GSP_list_result.valid) {
		bs->done = 1;
		return -2;
	}
	if (timer_query() >= bs->start_time + F1_0 * 5) {
		nm_messagebox(TXT_ERROR, 1, TXT_OK, "No response from game server at\n%s:%s", GameCfg.GameserverAddr, GSP_portbuf);
		return -2;
	}
	if (timer_query() >= bs->last_send + F1_0) {
		gsp_send_list_req();
		bs->last_send = timer_query();
	}
	return 0;
}

static const char *gsp_mode_name(int mode)
{
	switch (mode) {
		case 0: return "anarchy";
		case 1: return "team";
		case 2: return "robo";
		case 3: return "coop";
		case 7: return "bounty";
		default: return "?";
	}
}

static void gsp_browse(void)
{
	gsp_browse_state bs;
	newmenu_item m[1];
	int i, choice;

	memset(&GSP_list_result, 0, sizeof(GSP_list_result));
	GSP_awaiting = 2;
	bs.start_time = timer_query();
	bs.last_send = 0;
	bs.done = 0;

	m[0].type = NM_TYPE_TEXT; m[0].text = "Requesting game list...";
	newmenu_do1(NULL, "GAME SERVER", 1, m,
	            (int (*)(newmenu *, d_event *, void *))gsp_browse_poll, &bs, 0);
	GSP_awaiting = 0;

	if (!GSP_list_result.valid)
		return;
	if (GSP_list_result.count == 0) {
		nm_messagebox(NULL, 1, TXT_OK, "No games running on this server.\nUse CREATE GAME to start one.");
		return;
	}

	{
		newmenu_item items[GSP_MAX_SESSIONS_CAP];
		char labels[GSP_MAX_SESSIONS_CAP][80];
		for (i = 0; i < GSP_list_result.count; i++) {
			gsp_list_entry *e = &GSP_list_result.entries[i];
			snprintf(labels[i], sizeof(labels[i]), "%-15s %-8s %s L%d  %d/%d",
			         e->game_name, gsp_mode_name(e->gamemode), e->mission_name,
			         e->levelnum, e->numconnected, e->max_numplayers);
			items[i].type = NM_TYPE_MENU;
			items[i].text = labels[i];
		}
		choice = newmenu_do1(NULL, "SERVER GAMES", GSP_list_result.count, items, NULL, NULL, 0);
		if (choice >= 0 && choice < GSP_list_result.count)
			gsp_join_session(GSP_list_result.entries[choice].port);
	}
}

/* ---- create ---- */

typedef struct gsp_create_state {
	fix64 start_time;
	fix64 last_send;
	ubyte blob[UPID_GAME_INFO_SIZE];
	int blob_len;
} gsp_create_state;

static int gsp_create_poll(newmenu *menu, d_event *event, gsp_create_state *cs)
{
	menu = menu;
	if (event->type != EVENT_IDLE)
		return 0;

	timer_delay2(5);
	net_udp_listen();

	if (GSP_create_result.valid)
		return -2;
	if (timer_query() >= cs->start_time + F1_0 * 10) {
		nm_messagebox(TXT_ERROR, 1, TXT_OK, "No response from game server at\n%s:%s", GameCfg.GameserverAddr, GSP_portbuf);
		return -2;
	}
	if (timer_query() >= cs->last_send + F1_0) {
		gsp_send_create_req(cs->blob, cs->blob_len);
		cs->last_send = timer_query();
	}
	return 0;
}

/* Called from net_udp_game_param_handler instead of net_udp_start_game()
 * when Gameserver_create_mode is set. Returns 1 on success (menu closes). */
int net_udp_gameserver_create(void)
{
	gsp_create_state cs;
	newmenu_item m[1];
	struct _sockaddr zero_addr;

	/* server sessions are always open games */
	Netgame.RefusePlayers = 0;
	Netgame.game_flags &= ~NETGAME_FLAG_CLOSED;

	memset(&zero_addr, 0, sizeof(zero_addr));
	net_udp_update_netgame();
	cs.blob_len = net_udp_pack_game_info(cs.blob, UPID_GAME_INFO, &zero_addr, 0);
	cs.start_time = timer_query();
	cs.last_send = 0;

	memset(&GSP_create_result, 0, sizeof(GSP_create_result));
	GSP_awaiting = 1;

	m[0].type = NM_TYPE_TEXT; m[0].text = "Creating game on server...";
	newmenu_do1(NULL, "GAME SERVER", 1, m,
	            (int (*)(newmenu *, d_event *, void *))gsp_create_poll, &cs, 0);
	GSP_awaiting = 0;

	if (!GSP_create_result.valid)
		return 0;

	switch (GSP_create_result.result) {
		case GSP_OK:
			gsp_join_session(GSP_create_result.port);
			return 1;
		case GSP_ERR_FULL:
			nm_messagebox(TXT_ERROR, 1, TXT_OK, "Server is full (no free sessions).");
			return 0;
		case GSP_ERR_RATELIMIT:
			nm_messagebox(TXT_ERROR, 1, TXT_OK, "Creating games too fast.\nWait a few seconds.");
			return 0;
		case GSP_ERR_SPAWN:
			nm_messagebox(TXT_ERROR, 1, TXT_OK, "Server could not start the game.\n(Mission missing on server?)");
			return 0;
		default:
			nm_messagebox(TXT_ERROR, 1, TXT_OK, "Server rejected the request.");
			return 0;
	}
}

/* ---- top-level menu ---- */

static int gameserver_menu_handler(newmenu *menu, d_event *event, void *userdata)
{
	newmenu_item *items = newmenu_get_items(menu);
	int citem = newmenu_get_citem(menu);
	userdata = userdata;

	if (event->type != EVENT_NEWMENU_SELECTED)
		return 0;

	GameCfg.GameserverPort = atoi(GSP_portbuf);
	WriteConfigFile();

	if (citem == 4 || citem == 5) { /* BROWSE / CREATE rows, see layout below */
		if (!gsp_open_and_resolve()) {
			nm_messagebox(TXT_ERROR, 1, TXT_OK, "Cannot resolve server address:\n%s", GameCfg.GameserverAddr);
			return 1;
		}
	}

	if (citem == 4) {
		gsp_browse();
		return 1;
	}
	if (citem == 5) {
		int mission_result;
		Gameserver_create_mode = 1;
		/* mission select -> netgame params menu (same path as HOST GAME) */
		mission_result = select_mission(1, TXT_MULTI_MISSION, net_udp_setup_game);
		Gameserver_create_mode = 0;
		(void)mission_result;
		return 1;
	}
	return 0;
}

void do_gameserver_menu(void)
{
	newmenu_item m[6];
	int nitems = 0;

	net_udp_init();

	if (!GameCfg.GameserverAddr[0])
		snprintf(GameCfg.GameserverAddr, sizeof(GameCfg.GameserverAddr), "localhost");
	snprintf(GSP_portbuf, sizeof(GSP_portbuf), "%d", GameCfg.GameserverPort);
	if (GameArg.MplUdpMyPort != 0)
		snprintf(GSP_myportbuf, sizeof(GSP_myportbuf), "%d", GameArg.MplUdpMyPort);
	else
		snprintf(GSP_myportbuf, sizeof(GSP_myportbuf), "%d", UDP_PORT_DEFAULT);

	m[nitems].type = NM_TYPE_TEXT;  m[nitems].text = "SERVER ADDRESS:"; nitems++;
	m[nitems].type = NM_TYPE_INPUT; m[nitems].text = GameCfg.GameserverAddr; m[nitems].text_len = 127; nitems++;
	m[nitems].type = NM_TYPE_TEXT;  m[nitems].text = "SERVER PORT:"; nitems++;
	m[nitems].type = NM_TYPE_INPUT; m[nitems].text = GSP_portbuf; m[nitems].text_len = 5; nitems++;
	m[nitems].type = NM_TYPE_MENU;  m[nitems].text = "BROWSE GAMES"; nitems++;
	m[nitems].type = NM_TYPE_MENU;  m[nitems].text = "CREATE GAME"; nitems++;

	newmenu_do1(NULL, "GAME SERVER", nitems, m, gameserver_menu_handler, NULL, 0);
}
```

Declare `extern int select_mission(int anarchy_mode, char *message, int (*when_selected)(void));` if `mission.h` doesn't already export it (it does — check `main/mission.h`).

- [ ] **Step 6: hook create-mode into the params menu**

In `net_udp_game_param_handler` (`main/net_udp.c:4309-4310`), change:

```c
			if (citem==opt->start_game)
				return !net_udp_start_game();
```
to:
```c
			if (citem==opt->start_game)
			{
				if (Gameserver_create_mode)
					return !net_udp_gameserver_create();
				return !net_udp_start_game();
			}
```
with `#include "gameserver_menus.h"` added to `net_udp.c`'s includes.

In `net_udp_setup_game()` (`net_udp.c:4478`), the "Open/Closed/Restricted game" radio rows (lines ~4560-4564): when `Gameserver_create_mode` is set, force the Open radio on and skip adding the Closed/Restricted rows:

```c
	if (Gameserver_create_mode)
	{
		m[optnum].type = NM_TYPE_RADIO; m[optnum].text = "Open game"; m[optnum].group=1; m[optnum].value=1; optnum++;
	}
	else
	{
		/* existing three radio rows unchanged */
	}
```
(Keep the `opt->closed`/`opt->refuse` index bookkeeping consistent — set `opt->closed = opt->refuse = -1` in create mode and guard the two `menus[opt->closed]`/`menus[opt->refuse]` reads in the handler (lines ~4282-4286) with `if (opt->closed >= 0)` / `if (opt->refuse >= 0)`. Also guard line ~4165 `menus[opt->closed].value = 1` for team anarchy the same way.)

- [ ] **Step 7: add to build + compile**

`main/CMakeLists.txt`: add `gameserver_menus.c` to the source list, and `../gameserver` to `target_include_directories` (find the existing `target_include_directories` or `include_directories` block).

```bash
PATH=/c/Programs/msys64/mingw64/bin:$PATH cmake --build build -j
```
Expected: exits 0.

- [ ] **Step 8: End-to-end verification (three terminals, all local)**

Terminal 1 — broker:
```bash
./build/gameserver/d1x-gameserver.exe --engine "$(pwd)/build/main/d1x-redux.exe" --hogdir C:/Users/Yermak/Projects/dxx-redux/d1/hogs
```
Terminal 2 — creator client (own port to avoid clashing with the broker's 42424):
```bash
./build/main/d1x-redux.exe -hogdir C:/Users/Yermak/Projects/dxx-redux/d1/hogs -notitles -pilot cli1 -udp_myport 42460
```
Multiplayer → GAME SERVER → address `localhost` port `42424` → CREATE GAME → pick Descent: First Strike → set game name `testgame` → Start. Expected: "Creating game on server..." → broker logs spawn+ready → game-info screen for the new session → join → you are in the mine (alone) as a player.

Terminal 3 — second client:
```bash
./build/main/d1x-redux.exe -hogdir C:/Users/Yermak/Projects/dxx-redux/d1/hogs -notitles -pilot cli2 -udp_myport 42461
```
Multiplayer → GAME SERVER → BROWSE GAMES. Expected: one row `testgame anarchy descent L1 1/8`. Select it → join → both clients see each other, can shoot each other; broker LIST now reports `2/8` (re-browse from a menu or check broker log after next poll).

Quit both clients. Expected: within `--timeout-empty` + poll interval, broker logs `session on port 42425 exited` (child self-terminated).

- [ ] **Step 9: Config persistence check**

Reopen client 2 → Multiplayer → GAME SERVER. Expected: address/port fields pre-filled from `descent.cfg` (`GameserverAddr=localhost`).

- [ ] **Step 10: Commit**

```bash
git add main/gameserver_menus.c main/gameserver_menus.h main/net_udp.c main/net_udp.h main/menu.c main/menu.h main/config.c main/config.h main/CMakeLists.txt
git commit -m "gameserver: client Game Server menu (browse/create/join via broker)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: Co-op & robot modes validation (+ robot-rotation guard)

All modes are in scope per the spec. Robot modes exercise `multibot.c` with an observer host; this task adds the one known guard and then plays through the risk areas, fixing fallout.

**Files:**
- Modify: `main/multibot.c:76` (`MULTI_ROBOT_PRIORITY` zero-guard)
- Possibly small fixes discovered during validation (document each in the commit message)

**Interfaces:**
- Consumes: everything from Tasks 1-6.
- Produces: validated co-op/robo-anarchy behavior; no new API.

- [ ] **Step 1: Guard the robot-control rotation**

`main/multibot.c:76`:
```c
#define MULTI_ROBOT_PRIORITY(objnum, pnum) ((objnum + pnum) % (N_players - (Netgame.host_is_obs ? 1 : 0)))
```
divides by zero when a dedicated (host_is_obs) session is empty (`N_players == 1`). Change to:

```c
#define MULTI_ROBOT_NUM_CONTROLLERS ((N_players - (Netgame.host_is_obs ? 1 : 0)) > 0 ? (N_players - (Netgame.host_is_obs ? 1 : 0)) : 1)
#define MULTI_ROBOT_PRIORITY(objnum, pnum) ((objnum + pnum) % MULTI_ROBOT_NUM_CONTROLLERS)
```

Build:
```bash
PATH=/c/Programs/msys64/mingw64/bin:$PATH cmake --build build -j
```
Expected: exits 0.

- [ ] **Step 2: Robo-anarchy smoke test**

Broker + one client (Task 6 setup). CREATE GAME → mode "Robo-anarchy" → mission Descent: First Strike. Expected: create succeeds, robots are present and attack the joined player, robot kills count, no server crash while the session is briefly empty before the creator joins (the empty window exercises the Step 1 guard).

- [ ] **Step 3: Co-op smoke test (two clients)**

CREATE GAME → mode "Cooperative" (max players auto-caps at 4 — the existing menu logic at `net_udp.c:4170-4191` does this). Join with both clients. Verify:
1. Robots sync between the two clients (shoot the same robot, HP is consistent; robot kills attributed).
2. Keys: pick up blue key — both clients see door openable (co-op shares keys).
3. Level advance: both players fly into the exit. Expected: score screen ~7 s, then level 2 loads on both clients with the server following (watch server log).
4. One player quits mid-level; the other finishes the level alone. Expected: advance still works.
5. Both quit; session self-closes after `timeout_empty`.

- [ ] **Step 4: Join-in-progress in co-op**

With one player mid-level, join the second via BROWSE GAMES. Expected: joins into the current level with correct robot states (object sync covers robots — they are `Objects[]` entries sent by `net_udp_send_objects`).

- [ ] **Step 5: Fix whatever breaks**

Likely areas (from the code trace, check them if a symptom appears):
- Robot ownership handoff when the controlling player leaves (`multi_strip_robots` / `multi_dropped_robots` paths in `multibot.c`) with the host slot excluded.
- Co-op scoring UI on the dedicated host (any `nm_messagebox`/window reachable from co-op-specific packets — grep the co-op multi handlers for UI calls if the server ever blocks).
- Boss robot logic (`ai.c` boss teleport/cloak) with zero controlling players — only relevant while empty; the session is normally empty only briefly.

Each fix: smallest change, gated by `Dedicated_server` or `host_is_obs` idiom as appropriate; re-run the affected smoke test.

- [ ] **Step 6: Commit**

```bash
git add main/multibot.c
git commit -m "gameserver: guard robot-control rotation for empty dedicated sessions

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```
(Include any Step 5 fixes in the same commit with their explanation, or separate commits if unrelated.)

---

### Task 8: Docker image, compose example, docs

**Files:**
- Create: `Dockerfile` (in `d1/`)
- Create: `docker-compose.yml` (in `d1/`)
- Create: `.dockerignore` (in `d1/`)
- Create: `gameserver/README.md`

**Interfaces:**
- Consumes: the complete server (Tasks 1-7).
- Produces: `d1x-gameserver` Docker image; documented deployment.

- [ ] **Step 1: `.dockerignore`**

```
build/
dist/
hogs/
*.exe
```

- [ ] **Step 2: `Dockerfile`**

```dockerfile
# d1x-redux dedicated game server.
# Game data (descent.hog/descent.pig, add-on missions) is NOT included --
# mount it at /data.
FROM debian:bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build libsdl1.2-dev libphysfs-dev \
    && rm -rf /var/lib/apt/lists/*
COPY . /src/d1
WORKDIR /src/d1
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DOPENGL=OFF -DOPENGLMERGE=OFF -DSDLMIXER=OFF -DPNG=OFF -DTRACKER=OFF \
    && cmake --build build -j

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsdl1.2debian libphysfs1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/d1/build/main/d1x-redux /usr/local/bin/d1x-redux
COPY --from=build /src/d1/build/gameserver/d1x-gameserver /usr/local/bin/d1x-gameserver
ENV GS_ENGINE=/usr/local/bin/d1x-redux \
    GS_HOGDIR=/data \
    GS_TMPDIR=/tmp
VOLUME /data
EXPOSE 42424/udp 42425-42440/udp
ENTRYPOINT ["/usr/local/bin/d1x-gameserver"]
```

Known risks this step validates (fix forward if they bite, they are the point of the docker build):
- SDL 1.2 / PhysFS dev packages on bookworm: `libsdl1.2-dev` is present ("compat" build); if `libphysfs-dev` (PhysFS 3.x) breaks the PhysFS 1.x-era calls in `physfsx.h`, pin the fix in `include/physfsx.h` rather than pinning an old distro.
- The `-DOPENGL=OFF` software-renderer build on Linux may have bit-rotted (`arch/x11` + `2d` sw paths). Compile errors here get fixed in-tree — they're also a gift to the d2 follow-up.

- [ ] **Step 3: `docker-compose.yml`**

```yaml
services:
  d1x-gameserver:
    build: .
    ports:
      - "42424-42440:42424-42440/udp"
    volumes:
      - ./hogs:/data:ro
    environment:
      GS_MAX_SESSIONS: "8"
      GS_TIMEOUT_EMPTY_START: "300"
      GS_TIMEOUT_EMPTY: "60"
    restart: unless-stopped
```

- [ ] **Step 4: `gameserver/README.md`**

```markdown
# d1x-gameserver — dedicated game server

Hosts multiple d1x-redux netgames on one machine. Each session is a real
d1x-redux engine running headless as a non-playing observer-host; players
connect to the server's public address (create/browse via Multiplayer →
GAME SERVER in the client), which avoids NAT/ISP restrictions on
player-hosted games.

## Running natively

    d1x-gameserver --engine <path-to-d1x-redux> --hogdir <data-dir>
        [--port 42424] [--game-port-base 42425] [--max-sessions 8]
        [--timeout-empty-start 300] [--timeout-empty 60] [--test-create]

The data dir must contain descent.hog + descent.pig (and any add-on
mission .hog/.msn files you want the server to host). Sessions close
when the last player leaves (after --timeout-empty seconds) or if nobody
joins at all (--timeout-empty-start).

## Docker

    docker build -t d1x-gameserver .
    docker run -d -p 42424-42440:42424-42440/udp -v /path/to/data:/data:ro d1x-gameserver

or `docker compose up -d` with the provided docker-compose.yml (expects
game data in ./hogs). Game data is never baked into the image.

## Firewall

Open UDP 42424 (broker) and 42425..(42425+max-sessions-1) (games).
```

- [ ] **Step 5: Verify docker build + run (requires Docker Desktop)**

From `d1/`:
```bash
docker build -t d1x-gameserver .
docker run --rm -p 42424-42440:42424-42440/udp -v "C:/Users/Yermak/Projects/dxx-redux/d1/hogs:/data:ro" d1x-gameserver --test-create
```
Expected: `[gs] d1x-gameserver listening...` then `session on port 42425 ready`. Then from Windows run a client (`-udp_myport 42460`), Multiplayer → GAME SERVER → address `localhost` → BROWSE GAMES shows the test session; join it and fly. Ctrl+C the container; expected clean shutdown log.

If Docker is not installed/running on this machine, mark this step for the user to run and verify the Dockerfile by reading it — do not silently skip: report it as unverified.

- [ ] **Step 6: Final acceptance sweep (whole feature)**

1. Full local loop again (broker + 2 clients): create anarchy via menu, browse, join both, play, kill each other (kill matrix updates, no host row visible beyond observer handling), both leave, session self-closes, port reused by a new create.
2. Parallel sessions: create a second game while the first runs; both listed; join one from each client.
3. Error paths: browse with broker down (timeout box); create 9th session (`GSP_ERR_FULL` box); kill a client process mid-game (other client sees disconnect after existing timeout).
4. Latency sanity: in a server game, the in-game ping display (existing HUD ping) on localhost should be single-digit ms — comparable to a locally hosted game; nothing in the dedicated path adds systematic delay beyond the host-relay model that already exists.
5. `d2` untouched: `cd ../d2 && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j` — expected: builds as before (run from repo root of the worktree; use the same PATH prefix).

- [ ] **Step 7: Commit**

```bash
git add Dockerfile docker-compose.yml .dockerignore gameserver/README.md
git commit -m "gameserver: Docker image, compose example, server README

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

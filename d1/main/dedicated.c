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
#include "args.h"
#include "playsave.h"
#include "config.h"
#include "multi.h"
#include "net_udp.h"
#include "player.h"
#include "game.h"
#include "inferno.h"
#include "kmatrix.h"
#include "cntrlcen.h"

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
	Dedicated_cfg.tracker = 1;

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
		else if (!strcmp(key, "tracker")) Dedicated_cfg.tracker = atoi(val);
		else if (!strcmp(key, "tracker_addr")) strncpy(Dedicated_cfg.tracker_addr, val, sizeof(Dedicated_cfg.tracker_addr) - 1);
	}
	fclose(f);
	remove(path); /* single-use, written by the broker */

	/* Feed tracker_addr into the same GameArg slots -tracker_hostaddr/-hostport
	 * fill, so udp_tracker_init() (called from net_udp_init) resolves it. Must
	 * happen here: by the time the netcode starts there is no other hook, and
	 * an absent key must leave whatever argv/the compiled default said. */
	if (Dedicated_cfg.tracker_addr[0]) {
		char *colon = strchr(Dedicated_cfg.tracker_addr, ':');
		if (colon) {
			*colon = 0;
			if (atoi(colon + 1) > 0)
				GameArg.MplTrackerPort = atoi(colon + 1);
		}
		GameArg.MplTrackerAddr = Dedicated_cfg.tracker_addr;
	}
	return 1;
}

static fix64 Ded_started_at;

static int dedicated_connected_players(void)
{
	int i, n = 0;
	for (i = (Netgame.host_is_obs ? 1 : 0); i < N_players; i++)
		if (Players[i].connected)
			n++;
	return n + Netgame.numobservers;
}

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

static fix64 Ded_empty_since;   /* 0 = not currently empty */
static int Ded_ever_had_player;

/* called once per dedicated frame */
static int dedicated_should_exit(void)
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

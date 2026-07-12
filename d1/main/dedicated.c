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

fix64 Ded_started_at; /* lifecycle timers build on this in Task 4 */

int dedicated_connected_players(void)
{
	int i, n = 0;
	for (i = (Netgame.host_is_obs ? 1 : 0); i < N_players; i++)
		if (Players[i].connected)
			n++;
	return n + Netgame.numobservers;
}

int dedicated_should_exit(void)
{
	return Dedicated_exit_requested; /* lifecycle timers arrive in Task 4 */
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

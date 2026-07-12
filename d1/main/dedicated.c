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

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
	int tracker;               /* 1 = also advertise the session on the tracker */
	char tracker_addr[128];    /* "host[:port]"; "" = keep the engine default */
} dedicated_config;

extern dedicated_config Dedicated_cfg;

int dedicated_parse_cfg(const char *path);
void dedicated_main(void);           /* runs the session; never returns */
void dedicated_endlevel_wait(void);

#endif

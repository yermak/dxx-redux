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

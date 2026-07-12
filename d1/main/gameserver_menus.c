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
#include "game.h"
#include "player.h"

int Gameserver_create_mode = 0;

gsp_create_result GSP_create_result;
gsp_list_result GSP_list_result;
int GSP_awaiting; /* 0 none, 1 awaiting create ack, 2 awaiting list ack */

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
	net_udp_gsp_sendto(buf, sizeof(buf), &GSP_server_addr);
}

static void gsp_send_create_req(ubyte *blob, int blob_len)
{
	ubyte buf[6 + UPID_GAME_INFO_SIZE];
	buf[0] = GSP_CREATE_REQ;
	buf[1] = GSP_PROTO_VERSION;
	PUT_INTEL_SHORT(buf + 2, MULTI_PROTO_VERSION);
	PUT_INTEL_SHORT(buf + 4, blob_len);
	memcpy(buf + 6, blob, blob_len);
	net_udp_gsp_sendto(buf, 6 + blob_len, &GSP_server_addr);
}

/* ---- join helper: hand a server session to the normal join flow ---- */

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
	ubyte saved_refuse;
	ubyte saved_flags;

	/* Server sessions are always open games. Force the flags only for the
	 * wire blob, then restore them: write_netgame_profile persists Netgame
	 * on setup-menu exit, and the pilot's saved HOST GAME defaults must not
	 * inherit values the create-mode UI never offered as a choice. The
	 * dedicated child forces Open on its side regardless. */
	saved_refuse = Netgame.RefusePlayers;
	saved_flags = Netgame.game_flags;
	Netgame.RefusePlayers = 0;
	Netgame.game_flags &= ~NETGAME_FLAG_CLOSED;

	memset(&zero_addr, 0, sizeof(zero_addr));
	net_udp_update_netgame();
	cs.blob_len = net_udp_pack_game_info(cs.blob, UPID_GAME_INFO, &zero_addr, 0);
	Netgame.RefusePlayers = saved_refuse;
	Netgame.game_flags = saved_flags;
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

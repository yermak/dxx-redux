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

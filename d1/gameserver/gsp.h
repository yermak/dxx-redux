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

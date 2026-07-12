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

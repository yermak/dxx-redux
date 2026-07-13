# Hosting the DXX-Redux game server on Google Cloud

## TL;DR — not Cloud Run

**Cloud Run cannot host this server.** It accepts only HTTP/gRPC/WebSockets over
TCP, on a single port, in stateless request-scoped instances that scale to zero.
This server is **UDP**, needs a **port range** (42424 broker + 42425+ per-session
game ports), and holds **long-lived stateful sessions** with child processes.
Any one of those rules Cloud Run out.

The right GCP target is **Compute Engine** — a plain VM with a public IP and a
firewall rule for the UDP ports. That is what `deploy-gcp.sh` sets up. It runs the
same Docker image you already built, and gives you a stable public address, which
is exactly what beats the NAT/ISP problem this feature was built for.

(If you specifically want managed orchestration, GKE also works — it supports UDP
`LoadBalancer` services and port ranges — but for a single game server it is far
more moving parts than a VM. Use the VM unless you already run GKE.)

## What gets created

Everything lives in your project and is removable (see Teardown):

| Resource | Purpose |
|---|---|
| Static external IPv4 (`dxx-gameserver-ip`) | stable address you hand to players |
| Firewall rule (`dxx-gameserver-udp`) | allows inbound UDP 42424–42440 |
| VM (`dxx-gameserver`, Ubuntu + Docker) | runs the `d1x-gameserver` container |

The **clean, data-less** image is shipped to the VM with `docker save | scp | docker load`
— no container registry needed. Your Descent data files are uploaded to the VM only
(`scp`), never to any Google-hosted registry.

## Prerequisites

1. **gcloud CLI** installed and authenticated: `gcloud auth login`
2. A **GCP project with billing enabled** (note the project ID).
3. **Docker** running locally with the image built:
   ```bash
   docker images | grep d1x-gameserver     # expect  d1x-gameserver:latest
   # if missing:
   cd d1 && docker build -t d1x-gameserver .
   ```
4. Your **Descent data files** (`descent.hog`, `descent.pig`, plus any add-on
   mission `.hog`/`.msn`) in one folder.

## Deploy

1. Open `deploy-gcp.sh` and edit the **CONFIG** block — at minimum `PROJECT` and
   `DATA_DIR`. Optionally set `ZONE` (pick one near your players), `MACHINE_TYPE`,
   and `SRC_RANGES` (who may connect).
2. Run it:
   ```bash
   bash d1/gameserver/deploy-gcp.sh
   ```
   First run takes a few minutes (VM boot + Docker install + ~50 MB image upload).
   It is idempotent — safe to re-run to update the server after rebuilding the image.
3. It prints the **public IP** and the exact connect steps when done.

## Connect a client

Use the **gameserver-branch** client build (it has the GAME SERVER menu):

```
d1x-redux.exe -hogdir <your data folder> -udp_myport 46000
```

Then **Multiplayer → GAME SERVER**:
- **Server address:** the public IP the script printed
- **Port:** `42424`
- **BROWSE GAMES** to join an existing session, or **CREATE GAME** to start one
  (you auto-join as the first player; it spawns on the next free game port).

Notes:
- The client reuses the address you type for the game port too, so a remote public
  IP "just works" — the server never has to advertise its own address.
- `-udp_myport` only matters when the client PC's own UDP 42424 is already in use
  (e.g. you also run the server or a second client on that PC). A value **outside
  42424–42440** is always safe; give each client on one PC a distinct one.

## Managing the server

```bash
# live logs (session spawn / ready / exit)
gcloud compute ssh dxx-gameserver --zone=<zone> --command='sudo docker logs -f gameserver'

# stop / start (stopping halts compute billing; the static IP still incurs a small charge)
gcloud compute instances stop  dxx-gameserver --zone=<zone>
gcloud compute instances start dxx-gameserver --zone=<zone>

# update to a newer build: rebuild the image locally, then re-run deploy-gcp.sh
```

## Security

- `SRC_RANGES="0.0.0.0/0"` exposes the UDP ports to the entire internet — required
  for arbitrary players to connect, but it is open. Restrict it to known player IPs
  when you can:
  ```bash
  gcloud compute firewall-rules update dxx-gameserver-udp --source-ranges=<ip>/32,<ip>/32
  ```
- There is no authentication on game creation in v1 (the spec defers auth). Anyone
  who can reach the broker can create/list/join sessions, bounded by `GS_MAX_SESSIONS`
  and the per-IP create rate limit. Keep that in mind if you leave it open.
- The container runs as root (same as the local image); it binds only the UDP game
  ports and does no disk writes outside `/tmp`.

## Cost (rough, us-central1, subject to change)

- `e2-small` running 24/7: ~US$13/month. `e2-micro` is free-tier-eligible in some
  regions but is tight for multiple sessions.
- Static external IPv4: ~US$3/month while reserved.
- Egress: a few GB/month for a small game — negligible on the free egress tier.

Stop or delete the VM when you are not using it to avoid charges.

## Teardown (removes everything this created)

```bash
gcloud compute instances     delete dxx-gameserver     --zone=<zone>   --quiet
gcloud compute firewall-rules delete dxx-gameserver-udp                --quiet
gcloud compute addresses      delete dxx-gameserver-ip  --region=<region> --quiet
```

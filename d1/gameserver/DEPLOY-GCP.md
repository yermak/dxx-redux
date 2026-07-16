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

## One-time GCP setup

Do these once. Steps 1–4 are in the browser (Google Cloud Console); steps 5–8 are
on your PC.

### 1. Google Cloud account
Go to <https://console.cloud.google.com> and sign in with a Google account. First
time: accept the free trial if offered (US$300 credit / 90 days — optional, but it
covers this comfortably).

### 2. Enable billing
Console → ☰ menu → **Billing** → create/link a billing account with a payment
method. Compute Engine needs an active billing account **even for free-tier
resources**.

### 3. Create a project
- Top bar → project dropdown → **New Project** → name it (e.g. `dxx-server`) → Create.
- Note its **Project ID** (Console → ☰ → **Cloud overview → Dashboard**). It looks
  like `dxx-server-472913` — this is **not** the display name, and it is exactly
  what goes in the script's `PROJECT=`.
- Confirm **billing is linked to this project** (Billing → the project shows "linked").

### 4. Enable the Compute Engine API
Console → ☰ → **APIs & Services → + Enable APIs and Services** → search
**Compute Engine API** → **Enable**. The first enable takes ~1 minute and creates
the project's default Compute service account. (The script also runs
`gcloud services enable compute.googleapis.com`; doing it here first avoids the wait.)

### 5. Install the Google Cloud CLI (on your PC)
Download the Windows installer: <https://cloud.google.com/sdk/docs/install> → run it,
accept defaults. Open a **new** terminal (Git Bash, or the "Google Cloud SDK Shell"
the installer adds) and verify:
```bash
gcloud --version
```

### 6. Authenticate and select the project
```bash
gcloud auth login                          # opens a browser; sign in, allow access
gcloud config set project YOUR_PROJECT_ID
gcloud config get-value project            # should echo your Project ID
```

### 7. Permissions (IAM)
- If **you** created the project, you are its **Owner** — you already have every
  permission the script needs. Skip this step.
- If someone else owns it, have them grant your account, on the project:
  **Compute Admin** (`roles/compute.admin`) and **Service Account User**
  (`roles/iam.serviceAccountUser`):
  ```bash
  gcloud projects add-iam-policy-binding YOUR_PROJECT_ID \
    --member="user:you@example.com" --role="roles/compute.admin"
  gcloud projects add-iam-policy-binding YOUR_PROJECT_ID \
    --member="user:you@example.com" --role="roles/iam.serviceAccountUser"
  ```
- The **VM's own** service account needs no extra roles: the deploy ships the image
  and data to the VM over SSH/scp, so the VM never pulls from a registry or bucket.

### 8. Local prerequisites
- **Docker Desktop running**, with the image built (`docker images | grep d1x-gameserver`
  → expect `d1x-gameserver:latest`; rebuild with `cd d1 && docker build -t d1x-gameserver .`).
- Your **Descent data files** (`descent.hog`, `descent.pig`, plus any add-on mission
  `.hog`/`.msn`) in one folder.
- **Run `deploy-gcp.sh` from this PC** (Git Bash) — **not** from Cloud Shell. The
  script uploads your *local* Docker image, which only exists on your machine.

### What the script configures for you (no manual action)
- Uses your project's **default VPC network** (auto-created with new projects — no
  network setup needed).
- Reserves a **static external IP**.
- Creates the **firewall rule** allowing inbound UDP 42424–42440 from `SRC_RANGES`.
- Creates the **VM** (Ubuntu + Docker), uploads the image + data, runs the container.

### Common first-run issues
- **"Billing must be enabled"** → finish steps 2–3 (billing linked to the project).
- **Quota error** on brand-new/free-trial accounts (e.g. `IN_USE_ADDRESSES`, CPU) →
  pick a different `ZONE`/region, or Console → **IAM & Admin → Quotas** to check/raise.
  A single `e2-small` + one static IP is within default quotas in most regions.
- **First `gcloud compute ssh` asks to create an SSH key** → press Enter (an empty
  passphrase is fine); the script waits for the key to propagate.
- **`gcloud: command not found`** right after install → open a new terminal, or use
  the "Google Cloud SDK Shell" the installer created.

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

**Restricting who can connect (firewall source).** By default `SRC_RANGES="0.0.0.0/0"`
opens the UDP ports to the entire internet. The script gives you two ways to lock it
down, both in the CONFIG block:

- **Just you** — set `RESTRICT_TO_MY_IP="yes"`. The script detects the public IP of
  the machine you run it from and allows only that address (`/32`). Play from the
  **same network** you ran it from (the server sees your home connection's public IP).
- **A specific set of players** — leave `RESTRICT_TO_MY_IP="no"` and list each
  player's **public IP** in `SRC_RANGES`, comma-separated, each as a `/32`:
  ```
  SRC_RANGES="203.0.113.7/32,198.51.100.4/32"
  ```

**Changing the allowed IPs later** (e.g. add a friend, or your home IP changed) —
edit the values above and re-run just the firewall step, no VM redeploy:
```bash
FIREWALL_ONLY=yes bash d1/gameserver/deploy-gcp.sh
```
This **replaces** the rule's full source list with your current `SRC_RANGES`, so
include every IP you want allowed. (Equivalent raw command:
`gcloud compute firewall-rules update dxx-gameserver-udp --source-ranges=<ip>/32,<ip>/32`.)

Caveats: home/consumer IPs are often **dynamic** and can change — if players suddenly
can't connect, their (or your) public IP likely changed; re-run the firewall step with
the new value. Each player is identified by the public IP their traffic arrives from,
so everyone behind a different router/NAT needs their own `/32` entry.
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

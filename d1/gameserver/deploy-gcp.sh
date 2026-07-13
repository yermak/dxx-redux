#!/usr/bin/env bash
#
# deploy-gcp.sh — host the DXX-Redux dedicated game server (d1x-gameserver) on
# Google Compute Engine: a VM with a static public IP and the required UDP
# ports open, running the Docker image you built locally.
#
# WHY COMPUTE ENGINE AND NOT CLOUD RUN
#   Cloud Run only accepts HTTP/gRPC/WebSockets over TCP, on a single port, in
#   stateless request-scoped instances that scale to zero. This server is UDP,
#   needs a PORT RANGE (42424 broker + 42425.. per-session game ports), and
#   holds long-lived stateful sessions with child processes. Cloud Run cannot
#   host it. A plain VM (this script) is the right GCP target.
#
# WHAT IT CREATES (all in your project, all removable — see teardown below)
#   * a reserved static external IPv4 address
#   * a firewall rule allowing inbound UDP 42424-42440
#   * an Ubuntu VM (Docker installed via startup-script) running the container
#
# The clean, data-less image is uploaded to the VM (docker save | scp | load);
# your Descent data files are uploaded to the VM only. Nothing copyrighted is
# pushed to any container registry.
#
# PREREQUISITES (on the machine you run this from: Git Bash / WSL / Linux / macOS)
#   1. gcloud CLI installed and authenticated:   gcloud auth login
#   2. a GCP project with billing enabled
#   3. Docker running locally with the image built:
#        docker images | grep d1x-gameserver         # expect d1x-gameserver:latest
#      (build it if missing:  cd d1 && docker build -t d1x-gameserver .)
#   4. your legally-obtained Descent data files (descent.hog, descent.pig, and
#      any add-on mission .hog/.msn) in one local folder.
#
# USAGE
#   1. Edit the CONFIG block below (at minimum: PROJECT and DATA_DIR).
#   2. bash deploy-gcp.sh
#   3. Note the public IP it prints; connect the client to <IP>:42424.

set -euo pipefail

############################## CONFIG — edit me ##############################
PROJECT="your-gcp-project-id"                 # gcloud projects list
ZONE="us-central1-a"                          # pick one near your players
MACHINE_TYPE="e2-small"                        # e2-micro = free-tier-eligible but tight; e2-small for real use
INSTANCE="dxx-gameserver"
IMAGE_LOCAL="d1x-gameserver:latest"            # the data-less image
DATA_DIR="/c/Users/Yermak/Projects/dxx-redux/d1/hogs"   # your Descent data folder
SRC_RANGES="0.0.0.0/0"                         # who may connect. 0.0.0.0/0 = the whole internet.
                                               #   Restrict to your players' IPs if you can, e.g. "203.0.113.7/32"
MAX_SESSIONS="8"                               # concurrent games (game ports 42425..42425+MAX-1)
TIMEOUT_EMPTY_START="600"                       # seconds a new session waits for its first player before closing
##############################################################################

REGION="${ZONE%-*}"
NET_TAG="dxx-gameserver"
GC="gcloud --project=$PROJECT --quiet"

echo ">> Project $PROJECT | zone $ZONE (region $REGION)"
$GC config set project "$PROJECT" >/dev/null

echo ">> Enabling Compute Engine API (idempotent)"
$GC services enable compute.googleapis.com >/dev/null

echo ">> Reserving static external IP '$INSTANCE-ip' (idempotent)"
$GC compute addresses create "$INSTANCE-ip" --region="$REGION" 2>/dev/null || true
STATIC_IP=$($GC compute addresses describe "$INSTANCE-ip" --region="$REGION" --format='value(address)')
echo "   static IP = $STATIC_IP"

echo ">> Firewall rule '$NET_TAG-udp' for UDP 42424-42440 from $SRC_RANGES (idempotent)"
$GC compute firewall-rules create "$NET_TAG-udp" \
  --direction=INGRESS --action=ALLOW \
  --rules=udp:42424-42440 \
  --source-ranges="$SRC_RANGES" \
  --target-tags="$NET_TAG" 2>/dev/null || \
  echo "   (rule exists; to change the allowed source run: gcloud compute firewall-rules update $NET_TAG-udp --source-ranges=... )"

echo ">> Creating VM '$INSTANCE' (idempotent)"
$GC compute instances create "$INSTANCE" \
  --zone="$ZONE" --machine-type="$MACHINE_TYPE" \
  --image-family=ubuntu-2204-lts --image-project=ubuntu-os-cloud \
  --address="$STATIC_IP" --tags="$NET_TAG" \
  --metadata=startup-script='#!/bin/bash
if ! command -v docker >/dev/null 2>&1; then curl -fsSL https://get.docker.com | sh; fi' \
  2>/dev/null || echo "   (instance already exists; continuing)"

echo ">> Waiting for the VM to accept SSH and finish installing Docker..."
until $GC compute ssh "$INSTANCE" --zone="$ZONE" \
        --command="command -v docker >/dev/null 2>&1" 2>/dev/null; do
  echo "   ...waiting"; sleep 10
done

echo ">> Exporting the local image and uploading it (~50 MB gzipped)"
TMP_IMG="$(mktemp -t dxximg.XXXXXX).tgz"
docker save "$IMAGE_LOCAL" | gzip > "$TMP_IMG"
$GC compute scp "$TMP_IMG" "$INSTANCE:~/dxx-image.tgz" --zone="$ZONE"
rm -f "$TMP_IMG"

echo ">> Uploading game data from $DATA_DIR"
DATA_BASENAME="$(basename "$DATA_DIR")"
$GC compute scp --recurse "$DATA_DIR" "$INSTANCE:~/" --zone="$ZONE"

echo ">> Loading image, staging data, launching the container"
$GC compute ssh "$INSTANCE" --zone="$ZONE" --command="
  set -e
  sudo docker load -i ~/dxx-image.tgz
  sudo mkdir -p /opt/dxx-data
  sudo cp ~/$DATA_BASENAME/* /opt/dxx-data/
  sudo docker rm -f gameserver 2>/dev/null || true
  sudo docker run -d --name gameserver --restart unless-stopped \
    -p 42424-42440:42424-42440/udp \
    -e GS_MAX_SESSIONS=$MAX_SESSIONS \
    -e GS_TIMEOUT_EMPTY_START=$TIMEOUT_EMPTY_START \
    -v /opt/dxx-data:/data:ro \
    $IMAGE_LOCAL
  sleep 2
  echo '--- container ---'
  sudo docker ps --filter name=gameserver --format 'RUNNING {{.Status}} | {{.Ports}}'
  echo '--- broker log ---'
  sudo docker logs gameserver 2>&1 | tail -5
"

cat <<EOF

==========================================================================
 DXX-Redux game server is live on Google Compute Engine.

   Public IP : $STATIC_IP
   Broker    : UDP $STATIC_IP:42424   (game sessions use 42425-42440)

 CONNECT A CLIENT (gameserver-branch build):
   d1x-redux.exe -hogdir <your data> -udp_myport 46000
   then  Multiplayer -> GAME SERVER
         Server address: $STATIC_IP    Port: 42424
         -> BROWSE GAMES  or  CREATE GAME
   (-udp_myport only matters if the client PC's own 42424 is busy; a value
    outside 42424-42440 is always safe. Each client on one PC needs its own.)

 MANAGE:
   logs   : gcloud compute ssh $INSTANCE --zone=$ZONE --command='sudo docker logs -f gameserver'
   stop   : gcloud compute instances stop $INSTANCE --zone=$ZONE     (stops billing for compute)
   start  : gcloud compute instances start $INSTANCE --zone=$ZONE
   update : re-run this script after rebuilding the image locally

 TEARDOWN (stop all charges):
   gcloud compute instances delete $INSTANCE --zone=$ZONE --quiet
   gcloud compute firewall-rules delete $NET_TAG-udp --quiet
   gcloud compute addresses delete $INSTANCE-ip --region=$REGION --quiet
==========================================================================
EOF

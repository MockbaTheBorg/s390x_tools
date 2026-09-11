#!/bin/sh
# Local HTTP server for guest netboot installs (e.g. ubuntu25's netboot
# prompt) to fetch an ISO from the host directly instead of the internet --
# faster, and avoids any download flakiness on the real link.
#
# Usage: ./web-up.sh [dir]
#   dir defaults to the media folder where install ISOs live.
# Serves on 0.0.0.0:8000, reachable at the QETH gateway IP (192.168.100.1)
# net-up.sh sets up NAT for, so the guest can reach it directly, no
# forwarding needed. Binds 0.0.0.0 rather than 192.168.100.1 directly since
# that address only exists once Hercules brings up the QETH TUN device.
set -e

DIR="${1:-/run/media/mockba/data/IBM}"
BIND=0.0.0.0
PORT=8000

if [ ! -d "$DIR" ]; then
    echo "web-up.sh: not a directory: $DIR" >&2
    exit 1
fi

echo "web-up.sh: serving $DIR on http://192.168.100.1:$PORT/ (Ctrl-C to stop)"
cd "$DIR"
exec python3 -m http.server "$PORT" --bind "$BIND"

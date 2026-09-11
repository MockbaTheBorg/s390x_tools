#!/bin/sh
# Builds and installs the hutape driver on the guest, persisting across
# reboots: module in /lib/modules, autoload via modules-load.d, devices
# auto-onlined via a systemd oneshot unit. Runs in PARALLEL with
# tape_34xx (0580/3490 stays on tape_34xx -- see hutape.c header).
#
# Run as root ON THE GUEST, from this directory (the whole tools/hutape/
# tree copied over, e.g. via scp -r).
set -e

if [ "$(id -u)" != 0 ]; then
	echo "install.sh: must run as root" >&2
	exit 1
fi

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$SCRIPT_DIR"

echo "==> building module"
make

KVER=$(uname -r)
MODDIR="/lib/modules/$KVER/extra"
mkdir -p "$MODDIR"
cp hutape.ko "$MODDIR/hutape.ko"
depmod -a "$KVER"

echo "==> autoload on boot"
echo hutape > /etc/modules-load.d/hutape.conf

echo "==> auto-online devices on boot"
cp hutape-online.sh /usr/local/sbin/hutape-online.sh
chmod 755 /usr/local/sbin/hutape-online.sh
cp hutape-online.service /etc/systemd/system/hutape-online.service
systemctl daemon-reload
systemctl enable hutape-online.service

echo "==> loading now (so this boot has it too, not just future ones)"
modprobe hutape || true
/usr/local/sbin/hutape-online.sh

echo "Done. /dev/htp* should now exist (root-only, 0600) and will reappear"
echo "automatically on every future boot. Verify: ls -l /dev/htp*"

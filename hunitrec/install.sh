#!/bin/sh
# Builds and installs the hunitrec driver + apps on the guest, persisting
# across reboots: module in /lib/modules, autoload via modules-load.d,
# devices auto-onlined via a systemd oneshot unit.
#
# Run as root ON THE GUEST, from this directory (the whole tools/hunitrec/
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

echo "==> building apps"
make -C apps

KVER=$(uname -r)
MODDIR="/lib/modules/$KVER/extra"
mkdir -p "$MODDIR"
cp hunitrec.ko "$MODDIR/hunitrec.ko"
depmod -a "$KVER"

echo "==> autoload on boot"
echo hunitrec > /etc/modules-load.d/hunitrec.conf

echo "==> auto-online devices on boot"
cp hunitrec-online.sh /usr/local/sbin/hunitrec-online.sh
chmod 755 /usr/local/sbin/hunitrec-online.sh
cp hunitrec-online.service /etc/systemd/system/hunitrec-online.service
systemctl daemon-reload
systemctl enable hunitrec-online.service

echo "==> installing apps"
cp apps/hprint apps/hpunch apps/hread /usr/local/bin/

echo "==> loading now (so this boot has it too, not just future ones)"
modprobe hunitrec || true
/usr/local/sbin/hunitrec-online.sh

echo "Done. /dev/hprt*, /dev/hrdr*, /dev/hpch* should now exist and will"
echo "reappear automatically on every future boot. Verify: ls /dev/hprt* /dev/hrdr* /dev/hpch*"

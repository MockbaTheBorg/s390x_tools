#!/bin/sh
# Bring every device the hunitrec driver actually claimed online. Safe
# to re-run.
#
# Deliberately NOT a hardcoded devnum list: my config's printers/
# reader/punch happen to be at 0009/000E/000F/000C/000D, but another
# Hercules config could put them anywhere, have more or fewer of them,
# or omit some entirely. hunitrec.c's ccw_device_id match table (CU
# type 0x2821 for the printer, CU 0x3505 for reader/punch) already
# decides, at the kernel level, which devices are "ours" regardless of
# devnum -- /sys/bus/ccw/drivers/hunitrec/ lists exactly those, so this
# script just onlines whatever's actually there.
for d in /sys/bus/ccw/drivers/hunitrec/0.0.*; do
	[ -e "$d" ] || continue
	chccwdev -e "$(basename "$d")" 2>/dev/null || true
done

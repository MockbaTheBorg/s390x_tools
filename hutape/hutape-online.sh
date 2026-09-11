#!/bin/sh
# Bring every device the hutape driver actually claimed online. Safe to
# re-run.
#
# Deliberately NOT a hardcoded devnum list, for two reasons:
#
# 1. Portability: my config's tapes happen to be at 0560/0580/0590,
#    but another Hercules config could use different devnums, have more
#    tape drives, or fewer. hutape.c's ccw_device_id match table (CU
#    types 0x3480/0x3490/0x3590) already decides, at the kernel level,
#    which devices are "ours" regardless of devnum --
#    /sys/bus/ccw/drivers/hutape/ lists exactly those.
#
# 2. The 0x3490 conditional: hutape's match table includes CU 0x3490 as
#    a fallback for when tape_34xx isn't loaded/bound yet at boot (a
#    real, observed outcome -- see docs/hutape-driver.md's load-order
#    note). If tape_34xx wins a given 3490 first, that device never
#    shows up under /sys/bus/ccw/drivers/hutape/ at all -- so iterating
#    this directory is automatically also the "only online a 3490 if
#    hutape actually has it" check, with no devnum-specific logic
#    needed.
for d in /sys/bus/ccw/drivers/hutape/0.0.*; do
	[ -e "$d" ] || continue
	chccwdev -e "$(basename "$d")" 2>/dev/null || true
done

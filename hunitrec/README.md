# hunitrec

Native CCW driver + apps for the Hercules-emulated 1403 printer / 3505
reader / 3525 punch on the `ubuntu26` guest (standalone, no z/VM/`vmur`).

**Devnums below (0009/000C/000D/000E/000F) are my `ubuntu26.rc`'s
assignment, not universal** — another Hercules config can put these
devices anywhere, have more or fewer of them, or omit some entirely.
The driver doesn't care: it matches by CU type, not devnum, so it binds
whatever's actually configured. `hunitrec-online.sh` reflects this too
— it discovers devices via `/sys/bus/ccw/drivers/hunitrec/` rather than
a hardcoded list. Use the "Which /dev/hprtN is which Hercules device?"
section below to find the real mapping on any given guest.

## Contents

- `hunitrec.c`, `Makefile` — the kernel module (printer/reader/punch CCW driver)
- `apps/` — `hprint`, `hpunch`, `hread` userspace tools + their Makefile
- `hunitrec-online.sh`, `hunitrec-online.service` — boot-time auto-online
  of the 5 devices (installed to `/usr/local/sbin` and
  `/etc/systemd/system` by `install.sh`)
- `install.sh` — builds everything and installs it persistently (module
  in `/lib/modules`, autoload via `/etc/modules-load.d`, auto-online via
  systemd)

## Reimplement from scratch

On the host, copy this whole directory to the guest, then on the host:

```
scp -r tools/hunitrec ubuntu@192.168.100.2:/tmp/hunitrec
ssh ubuntu@192.168.100.2
sudo /tmp/hunitrec/install.sh
```

That builds the module (needs `linux-headers-$(uname -r)` installed —
already present on this guest) and apps, installs everything, and brings
the devices up immediately. Future reboots pick it up automatically via
`/etc/modules-load.d/hunitrec.conf` + `hunitrec-online.service`.

**Known limitation**: the module is installed for the *current* kernel
version only (`/lib/modules/$(uname -r)/extra`) — a kernel upgrade needs a
rebuild + re-run of `install.sh` (no DKMS wired up).

## Manual use (without installing)

Loading the module and bringing devices online is root-only (kernel
module + ccw device state); once that's done, **using** the resulting
`/dev/hprt*`/`/dev/hrdr*`/`/dev/hpch*` nodes needs no `sudo` at all —
the driver registers them `0666` (`hu->misc.mode` in `hunitrec.c`), so
any regular user on the box can read/write them directly.

```
make && make -C apps
sudo insmod hunitrec.ko
sudo ./hunitrec-online.sh     # or: chccwdev -e 0.0.<devnum> per device
./apps/hprint /dev/hprt2 file.txt
./apps/hpunch /dev/hpch0 file.txt
./apps/hread  /dev/hrdr0 out.txt
```

Once installed (`install.sh`), the apps are on `$PATH` at `/usr/local/bin`,
so the examples below drop the `./apps/` prefix. Examples use `/dev/hprt2`
(0.0.000e this session), `/dev/hpch0` (0.0.000d), `/dev/hrdr0` (0.0.000c) —
check the mapping below first, since the `hprtN` number can shift between
boots.

## Which /dev/hprtN is which Hercules device?

Each `/dev/hprt*`/`/dev/hrdr*`/`/dev/hpch*` node's `misc.parent` points at
its underlying ccw device, so sysfs has the answer:

```
for m in /sys/class/misc/h{prt,rdr,pch}*; do
  n=$(basename "$m"); d=$(readlink -f "$m/device")
  echo "/dev/$n -> ${d##*/}"
done
```
```
/dev/hprt0 -> 0.0.0009
/dev/hprt1 -> 0.0.000e
/dev/hprt2 -> 0.0.000f
/dev/hrdr0 -> 0.0.000c
/dev/hpch0 -> 0.0.000d
```
(Also in `dmesg | grep hunitrec` at load time, but that scrolls out of the
ring buffer — sysfs is the queryable source of truth.)

**The numbering is not guaranteed stable across boots** — it's assigned in
whatever order the ccw bus probes the devices, and that order has already
been observed to differ between two boots of this same guest (`000e` was
`hprt2` once, `hprt1` another time). Always re-check the mapping above
after a reboot rather than trusting a previously-documented `hprtN` number.

## Silent by default

All three apps print nothing on success — only real errors (bad device
path, write/read failure) always print, with or without `-v`. Pass
`-v`/`--verbose` for a one-line summary (`hprint`/`hpunch`: "N line(s)
sent to ..."; `hread`: "waiting..." / "N card(s) read from ..."). Good for
scripting (silent = clean exit code only) vs. interactive use (`-v` to see
what happened).

## Examples (all tested, no `sudo` needed for any of these)

Print a file to printer:
```
hprint /dev/hprt2 report.txt
```

Pipe a command's output to a printer:
```
uname -a | hprint /dev/hprt2
```

Punch a file — same shape as printing, different device/node:
```
hpunch /dev/hpch0 deck.txt
```
Pipe a command's output to the punch:
```
date | hpunch /dev/hpch0
```

Read a file from the reader (stops cleanly at end-of-deck), with a
summary line since `-v` is given:
```
hread -v /dev/hrdr0 cards.txt
```

Same, but start `hread` *before* submitting the deck (`-b`/`--blocking`
waits/polls for the first card instead of exiting immediately with "0
cards"; once a card arrives it reads normally and stops at that deck's
end):
```
hread -b -v /dev/hrdr0 cards.txt
```

Pipe the reader's output into a Linux command (no outfile arg -> stdout;
silent by default so nothing but the cards themselves hits the pipe even
without `-v`):
```
hread /dev/hrdr0 | tr a-z A-Z
```

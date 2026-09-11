# hutape

Native CCW driver for the Hercules-emulated 3480 (0560) and 3590 (0590)
tape drives on the `ubuntu26` guest, running in **parallel** with the
in-tree `tape_34xx` driver — it does not replace or unbind `tape_34xx`
from the 3490 (0580). Same shape as `tools/hunitrec/`.

**Devnums below (0560/0580/0590) are my `ubuntu26.rc`'s assignment, not
universal** — another config can use different devnums, and have more
or fewer tape drives of each type (or none). The driver matches by CU
type (0x3480/0x3490/0x3590), not devnum, so it binds whatever's
actually configured; `hutape-online.sh` discovers devices via
`/sys/bus/ccw/drivers/hutape/` rather than a hardcoded list. Use the
"Which /dev/htpN is which Hercules device?" section below to find the
real mapping on any given guest.

## Contents

- `hutape.c`, `Makefile` — the kernel module
- `hutape-online.sh`, `hutape-online.service` — boot-time auto-online of
  3480/3590 (installed to `/usr/local/sbin` and `/etc/systemd/system` by
  `install.sh`)
- `install.sh` — builds and installs everything persistently (module in
  `/lib/modules`, autoload via `/etc/modules-load.d`, auto-online via
  systemd)

No bespoke userspace apps here (unlike `hunitrec`'s `hprint`/`hpunch`/
`hread`) — standard `dd` (bulk I/O) and `mt` (positioning, via the
in-kernel `MTIOCTOP` ioctl the driver implements) already do the job.

## Reimplement from scratch

On the host, copy this whole directory to the guest, then on the host:

```
scp -r tools/hutape ubuntu@192.168.100.2:/tmp/hutape
ssh ubuntu@192.168.100.2
sudo /tmp/hutape/install.sh
```

Needs `linux-headers-$(uname -r)` (already present on this guest). Brings
3480/3590 up immediately; future reboots pick it up automatically via
`/etc/modules-load.d/hutape.conf` + `hutape-online.service`. **3490 is
deliberately never touched** — not built into the device match logic's
online script, and `tape_34xx` (loaded earlier) already owns it, so
loading `hutape` doesn't disturb it.

**Known limitation**: module installed for the *current* kernel version
only (`/lib/modules/$(uname -r)/extra`) — a kernel upgrade needs a
rebuild + re-run of `install.sh` (no DKMS, matches `hunitrec`).

## Manual use (without installing)

Loading the module and bringing devices online is root-only; the
resulting `/dev/htp*` nodes are **also** root-only (`0600`), unlike
`hunitrec`'s `0666` — a tape write/rewind/erase is destructive to the
mounted volume, so this one stays root-only by default.

```
make
sudo insmod hutape.ko
sudo ./hutape-online.sh     # or: chccwdev -e 0.0.0560 / 0.0.0590
```

## Which /dev/htpN is which Hercules device?

Same pattern as `hunitrec` — `misc.parent` links to the ccw device:

```
for m in /sys/class/misc/htp*; do
  n=$(basename "$m"); d=$(readlink -f "$m/device")
  echo "/dev/$n -> ${d##*/}"
done
```
```
/dev/htp0 -> 0.0.0560
/dev/htp1 -> 0.0.0590
```
Not guaranteed stable across reboots (probe-order dependent, same
caveat as `hunitrec`) — re-check after a reboot.

## Using it: `dd` for data, `mt` for positioning

One `read()`/`write()` call = one tape block (variable length, up to
65535 bytes — the CCW count field is 16-bit). `dd`'s `bs=` is exactly "bytes per read(2)/write(2) call," so
**always set `bs=65536` (or your known max block size) — a smaller `bs`
on read will silently truncate a larger block**, and on write just caps
what you can put in one block.

Positioning uses the standard `MTIOCTOP` ioctl (`linux/mtio.h`), so the
stock `mt` command works unmodified. Implemented ops: `rewind`, `offline`
(rewind+unload), `weof`/`weofi` (write tapemark), `fsf`/`bsf` (space
file), `fsr`/`bsr` (space record/block), `erase` (erase gap — NOT a
full-tape erase, see caveat below), `nop`/`reset` (no-op). Anything else
(`status` included — no `MTIOCGET` support) returns `ENOTTY`.

### Examples (all tested)

Write one file (one block) to the 3480, then rewind and read it back:
```
mt -f /dev/htp0 rewind
dd if=myfile.txt of=/dev/htp0 bs=65536
mt -f /dev/htp0 weof
mt -f /dev/htp0 rewind
dd if=/dev/htp0 of=readback.txt bs=65536
```
Same shape for the 3590 — just `/dev/htp1` instead of `/dev/htp0`
(check the devnum mapping above, it can shift).

Write two files on one tape, separated by a tapemark:
```
mt -f /dev/htp0 rewind
dd if=file1.txt of=/dev/htp0 bs=65536
mt -f /dev/htp0 weof
dd if=file2.txt of=/dev/htp0 bs=65536
mt -f /dev/htp0 weof
```

**Reading multiple files — the important nuance**: reading through a
tapemark (a `read()` call that returns 0 bytes) automatically advances
position into the *next* file at the driver/tape level, exactly like
real tape hardware. Whether a given *program* actually reaches that
point depends on whether it keeps calling `read()` until it gets 0
bytes: `dd` without `count=` does (see below); `tar` does **not** — it
stops at its own end-of-archive marker instead, so `tar` always needs
an explicit `fsf` between archives even after reading one (see the
`tar` examples further down). For `dd`:
```
mt -f /dev/htp0 rewind
dd if=/dev/htp0 of=file1-out.txt bs=65536   # reads file 1, auto-stops+advances at its tapemark
dd if=/dev/htp0 of=file2-out.txt bs=65536   # already positioned at file 2 -- just read it
```
Use `fsf`/`bsf` only to **skip** a file you do *not* want to read:
```
mt -f /dev/htp0 rewind
mt -f /dev/htp0 fsf 1        # skip file 1 entirely, without reading it
dd if=/dev/htp0 of=file2-out.txt bs=65536
```
Calling `fsf` again on a position you already reached by reading
through its tapemark double-skips a file (confirmed while testing this
driver — `HHC00204E ... end of file (uninitialized tape)` on the
following read means you've walked off the end of what's actually
written; re-`devinit` the tape file in Hercules or back up with `bsf`).

Erase gap (approximation — erases a short gap at the current position,
not the whole tape):
```
mt -f /dev/htp0 erase
```

### Backup examples: `tar` over the tape (tested on both 3480 and 3590)

`tar` writing straight to a tape device is the standard Unix pattern —
works unmodified against `/dev/htp*`, no `bs=` needed (tar picks its own
block size, 10240 bytes by default, well under the 65535 cap).

Save one file, then a whole folder, to the same tape (two tar archives,
separated by a tapemark — same multi-file pattern as the raw `dd`
example above):
```
mt -f /dev/htp0 rewind
tar cvf /dev/htp0 hello.txt
mt -f /dev/htp0 weof
tar cvf /dev/htp0 -C /path/to myfolder
mt -f /dev/htp0 weof
```

**Important difference from the raw `dd` case above**: `tar` (both
`tvf` listing and `xvf` extracting) stops reading at its *own*
end-of-archive marker (two all-zero 512-byte blocks), not at the
underlying tape's physical tapemark — unlike `dd`, which reads until it
gets our driver's real 0-byte/tapemark return. So `tar` does **not**
auto-advance into the next archive the way a raw `dd` read does; an
explicit `mt fsf 1` is needed before every archive after the first,
even one you just read. (Confirmed by hitting `tar: This does not look
like a tar archive` when skipping the `fsf`.)

List contents without extracting:
```
mt -f /dev/htp0 rewind
tar tvf /dev/htp0                 # lists the file archive
mt -f /dev/htp0 fsf 1             # required -- tar tvf did NOT reach the tapemark
tar tvf /dev/htp0                 # lists the folder archive
```

Restore both (rewind first; `fsf` required again between the two,
same reason as listing):
```
mt -f /dev/htp0 rewind
tar xvf /dev/htp0 -C /restore/here    # file archive
mt -f /dev/htp0 fsf 1
tar xvf /dev/htp0 -C /restore/here    # folder archive
```

All four (write, list, extract, both file and folder) confirmed end to
end on 0560 and separately on 0580 — files came back byte-identical
both times.

### Does a tape we write have a label?

**No.** Checked with Hercules's own `tapemap` utility against the raw
`.aws` file after a `tar cvf /dev/htp0 hello.txt`:
```
HHC02721I File No. 1: Blocks=1, Bytes=10240, Block size min=10240, max=10240, avg=10240
HHC02704I End of tape
```
One block, tar's own default block size, immediately followed by the
tapemark. No VOL1/HDR1/HDR2/EOF1/EOF2 label records — those are small
(80-byte) fixed records a real dataset-label-aware writer inserts around
the data; nothing in this stack writes them. **This driver has no
opinion on labeling at all** — it's a byte-block passthrough (`write()`
= one CCW = one block), so labeling is entirely up to whatever
userspace program writes to the device. `tar`/`dd`/`cpio` writing
straight to a raw tape device never write labels, on real Unix tape
drives (`/dev/st0`) or here — this tape is structurally identical to
one any of those tools would produce against real hardware. A
*labeled* tape (IBM standard label, what z/OS's own dataset utilities
write) would need a separate label-writing layer on top — out of scope
here, and not something `tar`/`mt` do on any platform.

## Not implemented

- `MTIOCGET` (tape status query) — `mt status` will fail with `ENOTTY`.
  Position/error state has to be inferred from command exit codes and
  Hercules's own `devlist`/console output instead.
- Anything beyond the CCWs listed above (no locate-block, no path-group,
  no buffered-log, etc.) — this covers ordinary sequential read/write/
  position use, not the full 3590 feature set.

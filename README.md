# s390x_tools

Native Linux drivers and host-side helper scripts for running Ubuntu on
s390x under Hercules emulation, giving the guest direct access to
Hercules-emulated printer, card reader/punch, and tape devices without
going through z/VM or `vmur`.

## Contents

- `hunitrec/` — CCW driver + userspace apps (`hprint`, `hpunch`, `hread`)
  for the emulated 1403 printer / 3505 reader / 3525 punch. See
  `hunitrec/README.md`.
- `hutape/` — CCW driver for the emulated 3480/3590 tape drives, running
  alongside the in-tree `tape_34xx` driver. Standard `dd`/`mt`/`tar` work
  against the resulting `/dev/htp*` nodes. See `hutape/README.md`.
- `net-up.sh` — host-side NAT/forwarding setup for the guest's QETH
  network device. Run once per host boot.
- `web-up.sh` — local HTTP server for guest netboot installs to fetch an
  ISO from the host instead of the internet.

## License

MIT — see `LICENSE`.

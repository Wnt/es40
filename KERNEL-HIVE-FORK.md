# Kernel Hive fork

This is a patch-carrying fork of
[ES40-Emu/es40](https://github.com/ES40-Emu/es40), published in support of
[Kernel Hive](https://github.com/Wnt/osgallery), a browser-streamed museum of
emulated and virtualized operating systems. One of its exhibits is Windows
2000 for Alpha AXP running on an emulated AlphaServer ES40, and getting that
exhibit working — and fast enough to feel responsive over a browser stream —
requires a growing set of local changes to es40 itself.

Unlike the museum's MAME and QEMU forks, the patches here live directly on
[`main`](../../tree/main), based on upstream `main` at `a9bda96`. The commit
log is the authoritative list; so far:

- `SDL: mouse.absolute mode for absolute host pointer streams (VNC/XTEST)` —
  a `gui { mouse.absolute }` config option for hosts that feed the emulator
  absolute pointer coordinates (VNC into a virtual display, XTEST injection).
  SDL's relative-mouse capture re-centers the hidden pointer after every
  event, which turns an absolute injection stream into runaway
  center-relative deltas; this mode derives PS/2 deltas by differencing
  successive window positions instead. Default off; physical-pointer
  behaviour unchanged.
- `Disk: lock-free empty check for the media-mailbox poll` — the emulation
  loop polled every disk's operator-media-change mailbox through two mutexes
  and a heap-allocating `std::deque` per drive per iteration, just to find it
  empty. An atomic empty-flag makes the poll a single load; measured −25% to
  the Windows 2000 kernel-splash boot checkpoint.

Each patch is its own commit with a descriptive subject and body, so any of
them can be read, reviewed, or cherry-picked independently — see the commit
log for what changed and why.

Changes that were tried, measured, and not adopted are preserved on
experimental branches rather than silently dropped, with the measurement
result recorded in the commit message:

- [`tlb-hint-experimental`](../../tree/tlb-hint-experimental) — a verified
  per-page hint cache in front of the translation-buffer linear scan.
  Measured null: with `-O3` inlining the scan is no longer a measurable
  cost, so the hint had nothing to save.

This fork carries forward upstream es40's existing license terms unchanged;
see [COPYING](COPYING).

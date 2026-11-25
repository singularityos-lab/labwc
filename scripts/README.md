These scripts are intended to be run from the project top-level directory
like this: `scripts/foo.sh`

- `scripts/check`: wrapper to check all files in `src/` and `include/`

- `scripts/checkpatch.pl`: Quick hack on the Linux kernel [checkpatch.pl]
  to lint C files written according to the labwc coding style. Run like
  this: `./checkpatch.pl --no-tree --terse --strict --file <file>`

[checkpatch.pl]: https://raw.githubusercontent.com/torvalds/linux/4ce9f970457899defdf68e26e0502c7245002eb3/scripts/checkpatch.pl

- `scripts/frame-stats.py`: summarise a frame timing log written by a
  compositor started with `LABWC_FRAME_LOG=<path>`. Run like this:
  `scripts/frame-stats.py frames.log [markers.txt]`. The optional markers
  file holds `<name>-start <ns>` and `<name>-end <ns>` lines in
  `CLOCK_MONOTONIC` nanoseconds and splits the report per window. It prints
  render time, the interval between presented frames and the number of
  missed refresh cycles.

- `scripts/wl-fuzz.py`: protocol fuzzer. It reads the protocol XML files
  given on the command line, binds the globals the compositor announces,
  sets up a mapped toplevel and a layer surface, then sends requests with
  well-typed but hostile arguments until the compositor drops the client,
  over and over. Run it against a compositor built with
  `-Db_sanitize=address,undefined`:
  `scripts/wl-fuzz.py <socket> <compositor-pid> <seconds> <seed> <xml-dir>...`.
  It exits non-zero and prints the last requests of the connection when the
  compositor dies; the same seed replays the same run.

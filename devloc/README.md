# devloc

Locate the physical device and offset backing a file (or a raw device
sector) by triggering I/O and observing which leaf device actually
serves it.

devloc is an empirical "oracle": it does not parse dm tables and does
no mapping arithmetic.  For each probed block it issues a single
direct-I/O read through the top-level block device (the one holding
the filesystem), watches `block:block_rq_issue` tracepoint events on
the devices below, and reports which device and sector received the
I/O.  It reports observations, flags ambiguity instead of guessing,
and can verify a result by comparing the bytes read from the top
device with the bytes at the candidate location.

It is intended both as a diagnostic and as a verifier for tools that
compute file-to-disk mappings statically (FIEMAP + dm table
arithmetic): any static mapper can be validated by diffing its output
against `devloc --json`.

## Usage

```
devloc [options] FILE [OFFSET [LENGTH]]
devloc [options] --device DEV --sector N
```

Offsets and lengths accept K/M/G/T suffixes (binary) and `s` for
512-byte sectors.  LENGTH defaults to one block.  Run as root
(tracepoint access).

```
Input scope:
  --device DEV --sector N   probe a raw device/sector directly
  --block-size SIZE         probe granularity (default 4KiB)
  --sample N                probe N evenly spaced blocks in the range
  --probe-all               probe every block (no sampling)
  --repeat N                repeat each probe N times (path/mirror spread)

Triggering:
  (default)                 read-only: one O_DIRECT read per probe
  --nonce                   write a unique nonce, observe, restore
                            (needs --force if the device holds a live fs)
  --no-restore              leave the nonce in place (test/scratch only)
  --passthrough             observe events only, trigger nothing

Observation:
  --leaves LIST             comma list of maj:min or device paths to
                            match events against (default: auto-detect
                            via sysfs slaves tree)
  --timeout MSECS           per-probe event wait (default 2000)
  --events                  dump raw observed events
  --quiet-check             abort if leaf devices show background I/O

Output:
  --json                    machine-readable output (offsets in bytes)
  --summary                 per-device histogram of probed blocks
  --units b|s|k|m|g         display units (default bytes)
  --force-writeback         pass FIEMAP_FLAG_SYNC (flush delalloc)
  --no-verify-content       skip content comparison
  --strict                  abort at the first ambiguous probe
  -v                        be verbose
```

### Exit status

```
0  every probe resolved to exactly one device+offset
1  usage or I/O error
2  one or more probes ambiguous or unobserved ("couldn't tell")
3  one or more probes unmappable (hole, delalloc)
```

## Examples

XFS on a linear LV (`dmsetup table`: `0 1048576 linear 8:0 2560`):

```
# devloc /mnt/demo/data.img 8M
top:      /mnt/demo/data.img on /mnt/demo (xfs) dev 252:1 devlocvg-demo
range:    file offset 8388608 + 4096 (1 block)
leaves:   sda (8:0)

OFFSET         FS SECTOR   DEVICE             DEV OFFSET     LEN      STATUS
8388608        16576       sda                9797632        4096     verified
```

(fs sector 16576 + linear start 2560 = 19136 sectors = 9797632 bytes.)

When the leaf is a partition, the offset is reported relative to the
partition with the disk-relative offset in a note:

```
OFFSET         FS SECTOR   DEVICE             DEV OFFSET     LEN      STATUS
8388608        16576       sdb1               9797632        4096     verified
    (sdb1 is a partition of sdb, start 2048 sectors; disk-relative offset 9808128 on sdb)
```

Nonce mode (writes a unique block, finds where it landed, restores the
original — finds all mirror copies, labels parity legs):

```
# devloc --nonce --force /mnt/raid5/f 4M
OFFSET         FS SECTOR   DEVICE             DEV OFFSET     LEN      STATUS
4194304        8384        sdd                7634944        4096     parity (no nonce)
                           sdb                7634944        4096     copy (nonce verified, not serving reads)
    (nonce (restored))
```

Nonce mode also performs a **read probe** before the write: on a
healthy array only a data location serves reads of the block, so it is
identified by behavior rather than content.  This matters when parity
content equals the data — which really happens: in an all-zero stripe,
`parity = data XOR 0 = data`, so the nonce is byte-identical at the
parity leg:

```
# devloc --nonce --force /mnt/raid5/zeros 16M   (all-zero file)
OFFSET         FS SECTOR   DEVICE             DEV OFFSET     LEN      STATUS
16781312       32968       sdc                11112448       4096     copy (nonce verified, not serving reads)
                           sdb                11112448       4096     data (serves reads, nonce verified)
    (read probe: locations serving reads are data copies; others holding the nonce
     are mirror copies or parity equal to the data by coincidence)
    (nonce (restored))
```

Here both legs hold the nonce, but only sdb serves reads — it is the
data location; sdc is parity whose content coincidentally matches.
Mirror legs that do not serve this particular read (md often pins
raid1 reads to one leg) are reported as copies, and the data location
is always among the read-serving rows.

`--summary` shows which disks a file's blocks live on (useful for
stripes):

```
# devloc --summary --sample 64 /mnt/str/file 0 64M
DEVICE          BLOCKS   %     FILE RANGE
sda             33       51.6  0 .. 63M
sdb             31       48.4  0 .. 63M
```

JSON output (offsets always in bytes; both node-relative and
disk-relative offsets are reported):

```json
{ "file_offset": 51200000, "fs_offset": 51200000, "len": 4096,
  "extent_flags": "", "observed": true, "ambiguous": false,
  "read_probed": true,
  "observations": [ { "device": "sda", "major": 8, "minor": 0,
    "offset": 52510720, "disk": "sda", "disk_offset": 52510720,
    "verified": true, "served": 1, "in_leaf_set": true,
    "serves_reads": true } ] }
```

## How it works

1. **File to device offset**: FIEMAP translates the file range to
   physical offsets on the block device holding the filesystem (the
   "top" device, found via `st_dev` and mountinfo).
2. **Leaf discovery**: walk `/sys/dev/block/<dev>/slaves` recursively
   from the top device.  This uses topology knowledge only, never
   mapping knowledge.  A device with no slaves is a leaf.
3. **Probe**: drain the tracepoint ring buffers, issue one `O_DIRECT`
   read of the block through the top device, read the events, and
   match them against the leaf set by size and direction.  Distinct
   candidate locations are deduplicated; more than one plausible
   candidate is reported as AMBIGUOUS (with `--verify-content`, the
   default, exactly one content-matching candidate resolves the probe
   and the others are reported as excluded background).
4. **Content check**: the bytes read from the top device are compared
   with the bytes at each candidate location.  This works for linear,
   striped, mirror/raid data legs, and cache (writeback hits); it is
   expected to differ below crypt or raid4/5/6 parity, which the
   output notes.

### Event conventions (verified on 6.19)

`block_rq_issue` records carry:

- `dev` = the devt of `rq->q->disk`, i.e. always the **whole disk**,
  never a partition;
- `sector` = **whole-disk-relative**: bios that target a partition are
  adjusted by `blk_partition_remap()` (which adds `bd_start_sect`)
  before the request reaches the disk queue.  dm does not set
  `BIO_REMAPPED` for its clones, so this applies to dm-mapped I/O as
  well as direct partition access.

devloc therefore stores candidates with disk-relative sectors and
reads the whole disk for content checks, deriving partition-relative
offsets for display.  Tracepoint field offsets are parsed at runtime
from the tracefs `format` file, so field additions/reorderings across
kernels are handled.

The tracepoint is consumed with `perf_event_open()` +
`PERF_SAMPLE_RAW|TIME` and per-CPU mmap ring buffers.  (Note: plain
`read()` on such an fd returns the event *counter*, not samples —
samples are only visible through the mmap ring.)

## Caveats

- **Page cache**: probes use `O_DIRECT` on the top device so a cached
  page never suppresses the I/O.  Normal file reads after writing may
  produce no I/O at all.
- **Multipath** is request-based: no `block_rq_issue` event fires on
  the mpath dm device (only on the path disks, which are leaves, so
  mapping still works).  Path selection can vary between probes; use
  `--repeat` to see the spread.
- **dm-crypt**: reads verify content on the crypt device's parent is
  not expected to match (the leaf holds ciphertext).  The location is
  still correct; `--no-verify-content` silences the mismatch note.
  `--nonce` cannot see through crypt (the plaintext nonce never
  appears below the crypt layer).
- **Thin provisioning**: reading an unmapped block returns zeros
  without reaching a data device — reported as NO-OBSERVATION.
- **dm-cache**: a read is served from wherever it lives at that
  moment (cache device or origin).  The observation is the ground
  truth for that instant.
- **Partitions**: when several partitions of the same disk are leaves
  (e.g. two PVs on partitions of one disk), an event is attributed
  only to the partition whose sector range contains it; the others are
  not reported as candidates.
- **md raid**: raid writes can sit in the stripe cache briefly;
  `--nonce` collects write events for the full `--timeout` window
  instead of stopping at the first.  raid1 reads are typically all
  served from one leg for this access pattern (md read_balance), so
  use `--nonce` to enumerate mirror copies rather than `--repeat`.
  The nonce read probe runs before the write and evicts the stripe
  cache with unrelated reads first (a just-written stripe would
  otherwise be served from cache without disk I/O); if the read probe
  still observes nothing, locations fall back to content-only
  labeling.  Note that in an all-zero stripe, parity content equals
  data content, so nonce-verified non-serving locations may be either
  mirror copies or coincidental parity — the read-serving rows are
  the data locations.
- **Loop devices**: a loop leaf's I/O often lands on whatever disk
  backs the loop file; such events carry a device outside the leaf
  set and are reported as `not-in-leaf-set` rather than dropped.
- **delalloc**: extents that are not yet on disk are reported as
  unmappable; `--force-writeback` (FIEMAP_FLAG_SYNC) flushes them
  first.
- **Ambiguity is honest**: on a busy system, unrelated I/O of the
  same size and direction on a leaf device can look like the probe.
  Content verification resolves almost all of it; anything else is
  reported as AMBIGUOUS, never silently guessed.  `--nonce` is immune
  to background I/O.
- `--nonce` writes to the device under test.  It saves and restores
  the original block (and verifies the restore), but a crash between
  write and restore leaves the nonce on disk.  `--force` is required
  when the device holds a live filesystem.  Intended for test
  setups.

## Building and testing

```
make            # builds ./devloc, no dependencies beyond libc
```

`test-devloc.sh` builds LVM stacks with known tables (linear over a
disk, linear over a partition, striped) and verifies devloc's
observations against arithmetic computed from `dmsetup table` and
`filefrag`, plus nonce, hole, and JSON checks:

```
# in a test VM with spare disks sda sdb sdc:
DEVLOC=/root/devloc bash test-devloc.sh /dev/sda /dev/sdb /dev/sdc
```

`test-devloc-raid.sh` does the same for every lvm raid type, checking
devloc's observations against the md/dm-raid layout math computed from
the dm tables:

| type | layout verified |
|---|---|
| raid0 | stripe math over the `raid raid0` target |
| raid1 | every leg at rimage_start + fs_sector; nonce finds all legs |
| raid4 | dedicated parity on the first leg (`ALGORITHM_PARITY_0`), data legs in order |
| raid5 | left-symmetric (`raid5_ls`): parity rotates, data follows parity |
| raid6 | row/within arithmetic with content and nonce discrimination (raid6 needs 5 devices; a 5th PV is split off the 4th disk) |
| raid10 | near layout: copies on adjacent legs `(k*2, k*2+1)` |

Each type is checked for: read location at two offsets against the
computed candidates, content verification, nonce write path (data copy
found at the right location, parity legs identified, original block
restored), and the row/within stripe invariant on every observed
location.  For raid4/5/6 an additional zero-region test verifies the
read-probe role identification: in an all-zero stripe parity content
equals the data, and the tool must still identify exactly one
read-serving location matching the computed data candidate.

```
# in a test VM with four spare disks:
DEVLOC=/root/devloc bash test-devloc-raid.sh /dev/sda /dev/sdb /dev/sdc /dev/sdd
```

Both test scripts wrap lvm commands with
`--config 'global { use_lvmlockd = 0 }'` because the test host's lvm
build has experimental global-protect/PR locking that makes pv/vg
commands fail intermittently on local test VGs.

Verified on Fedora 44, kernel 6.19, LVM 2.03.43: all 16 + 57 tests
pass (device mode, file mode, partition leaf, stripe arithmetic,
raid0/1/4/5/6/10 layout math incl. exact sector matches, nonce +
restore, mirror-copy and parity-leg identification, zero-region
data-vs-parity discrimination, hole, JSON).

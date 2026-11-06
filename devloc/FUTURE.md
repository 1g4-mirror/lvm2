# devloc — future features

Features that have been discussed or designed but are **not yet
implemented** in devloc (the file locator).  Everything currently
implemented is documented in README.md; this file is the roadmap of
what is missing.

## Static mapping (the other half of the original design)

devloc is only the empirical oracle.  The original design called for a
companion static mapper, none of which exists yet:

- **Static mapper**: compute file offset → device + offset from FIEMAP
  plus dm table arithmetic (linear, striped, raid layouts), without
  triggering I/O.  Would answer for offsets that have not been probed
  and for all mirror legs / multipath paths at once.
- **`--verify` mode**: diff a static mapper's computed locations
  against devloc's observed JSON — the oracle's whole reason for
  existing.  The JSON output was designed for this but nothing
  consumes it yet.
- **Full chain display**: devloc deliberately reports only the two
  ends of the stack (top device and leaf).  Displaying the
  intermediate chain requires either static table arithmetic or the
  `block:block_bio_remap` tracepoint collector (see below).
- **LVM-native integration**: query `lvs -a --reportformat json`
  (seg_pe_ranges, dataoffset, stripe_size) for the LV→PV half and
  implement only the dm-generic descent — discussed, not built.
- **dm-integrity layout arithmetic** (journal/bitmap tag placement)
  and **dm-crypt offset arithmetic** for a static mapper.

## Stacks that are located but not verified or enumerated

- **dm-crypt**: locations are correct (I/O observation is unaffected)
  but content verification always differs below crypt; never tested.
  `--nonce` cannot see through crypt (the plaintext nonce never
  appears below the crypt layer).
- **Multipath**: request-based; only the path that served a probe is
  observed.  Missing: path enumeration for a LUN, a `--repeat` spread
  view, and reporting all paths as legitimate locations.
- **raid1/raid10 in read mode**: only the leg serving the read is
  reported (md often pins reads to one leg).  Nonce mode enumerates
  copies; a read-mode leaf-scan or write-scan mode does not exist.
- **dm-cache**: the served location (cache device or origin) is
  reported; enumerating both copies (cache_dump-style) is not
  supported.
- **dm-snapshot**: COW exception store awareness — never handled or
  tested.
- **thin provisioning**: unmapped blocks report NO-OBSERVATION, but
  the cause is not distinguished (unmapped vs all-cached vs no I/O
  reached a leaf); thin metadata parsing (thin_ls-style) to map or
  explain unmapped probes is missing.
- **Loop devices**: the not-in-leaf-set fallback reports events on
  the disk backing the loop file, but the loop → backing file →
  underlying disk chain is not resolved into a true location with
  offset arithmetic.
- **md (non-dm) stacks** and **partitions of partitions** (nvme
  multipath, CONFIG_NVME_MULTIPATH): leaf discovery should handle
  them but neither is tested.

## Observation engine

- **Bio-based device fallback**: brd ramdisks and other bio-based
  devices emit no `block_rq_issue` events (no request queue), which
  forces the `LVM_TEST_PREFER_BRD=0` workaround in the integrity
  tests.  A fallback to `block:block_bio_queue` /
  `block_bio_complete` tracepoints for bio-based leaves would remove
  the workaround.
- **`block:block_bio_remap` collector**: the per-hop event method
  (dm's `dm_submit_bio_remap` emits old_dev/old_sector → dev/sector
  per layer, including crypt's deferred submissions).  Would enable
  chain reconstruction; devloc currently uses only leaf observation.
- **PERF_RECORD_LOST handling**: lost records are silently skipped;
  overflow is not reported and ring sizes are not adaptive.
- **Busy-system candidate scoring**: content check plus a warning is
  the only discrimination; no comm/timestamp/size scoring for
  candidates that cannot be content-verified (crypt, parity).
- **`--progress`** display for large probe sets.

## Output and interface

- **`--summary` interleaving detection**: the design called for a
  "interleaved; striped device?" hint when sampled blocks alternate
  between devices; the current summary prints only per-device ranges.
- **`--count N`**: probe N consecutive sectors in `--device` mode
  (was in the original usage synopsis, dropped during prototyping).
- **NO-OBSERVATION reason codes**: a machine-readable distinction of
  why a probe observed nothing (JSON currently has only observed:
  false).
- **Exit code granularity**: "location found but not verified" and
  "verified" both exit 0; no separate code for unverified-only
  results.

## Safety

- **`--nonce` crash recovery**: a crash between the nonce write and
  the restore leaves the nonce on disk.  A journal file allowing
  recovery (or a verify-on-start of a previous run's nonce) is not
  implemented.

## Testing gaps

- **Degraded arrays**: corrupting/reading through a rebuilding or
  failed leg (lvconvert --repair, write-mostly) — the labeling
  semantics ("served by" rather than "data") are designed but the
  behavior is untested.
- **Stack coverage**: no test suites for crypt, multipath, dm-cache,
  snapshot, or non-lvm md stacks.
- **Older kernels**: sector conventions were verified on 6.19
  (blk_partition_remap + dm not setting BIO_REMAPPED); pre-BIO_REMAPPED
  kernels are untested.
- **integrity-caching.sh**: the devloc conversion is validated
  end-to-end standalone, but the full test is blocked by a
  pre-existing lvm crash (vgcreate with 6 PVs aborts in that build);
  it needs a re-run once that is fixed.
- **Cluster framework runs**: the converted integrity tests have only
  been run with the ndev-vanilla flavour via lib/runner, not through
  the lvmtest cluster machinery.

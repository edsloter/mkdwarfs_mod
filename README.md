# DwarFS — Global Deduplication Edition

> Original upstream project and full reference documentation:
> [mhx/dwarfs](https://github.com/mhx/dwarfs)
> (`README`, build instructions, usage, and release binaries).

A fork of [mhx/dwarfs](https://github.com/mhx/dwarfs) (upstream `main` as of
2026-11-05; upstream release at the time: v0.15.8) adding
**global byte-level (chunk-level) deduplication** before compression, plus
all-in-one build scripts with full static binary support.

---

## What's New

### Global deduplication (`--dedup-global`)

Stock DwarFS deduplicates at the file level — two files must be byte-identical
to share a chunk.  This fork scans a second time with a content-defined
chunker (CDC) and hashes each chunk with XXH3-128.  Duplicate chunks anywhere
in the image are stored once.

```
Original:  74.63 MiB  (4 files, each containing a 16 MiB blob)
DwarFS:    26.63 MiB  (-64.3%)
                        ▲
                        │
    ┌───────────────────┴───────────────────┐
    │  16 MiB blob appears 4 times         │
    │  → stored once, referenced 4 times   │
    │  savings: 48 MiB (64.3%)             │
    └───────────────────────────────────────┘
```

### Parallel hashing + deterministic resolution
Global deduplication now parallelizes the content-defined chunk hashing
instead of hashing chunk by chunk on a single thread:
- **Parallel hashing:** fragments are collected into batches (roughly one
  batch per `--num-segmenter-workers` fragments) which are hashed
  concurrently by a dedicated hash worker pool. The pool defaults to one
  thread per CPU core of the machine and can be tuned with
  `--num-segmenter-workers` (or `--num-workers`).
- **Deterministic resolution:** after hashing, the de-duplication index
  is updated serially, in the original chunk order, so builds remain
  reproducible. No temporary files or extra file system reads are needed.
- **Multi-pass:** when using `--min`/`--max`, the coarser-to-finer CDC
  passes are still processed sequentially, as finer passes depend on the
  results of coarser ones.


### Static binaries

Both build scripts support fully static builds:

| Platform | Flag | Description |
|----------|------|-------------|
| Linux | `--static` | Statically links all libraries; binary runs on any x86-64 Linux with glibc ≥ 2.27 |
| Windows | `-Static` | Static CRT (`/MT`); no runtime DLLs beyond Windows kernel DLLs |

### Build scripts

| Script | Platform | Description |
|--------|----------|-------------|
| `build_mkdwarfs_mod.sh` | Linux (WSL) | All-in-one: deps → cmake → build → smoke test |
| `build_mkdwarfs_mod.ps1` | Windows | All-in-one: vcpkg → cmake → build → smoke test |

#### Linux (`build_mkdwarfs_mod.sh`)

```bash
# Dynamic build (default)
./build_mkdwarfs_mod.sh

# Static build
./build_mkdwarfs_mod.sh --static

# Build with tests
./build_mkdwarfs_mod.sh --with-tests

# All options
./build_mkdwarfs_mod.sh \
    --build-dir ./mybuild \
    --static \
    --with-tests \
    --skip-smoke-test \
    --skip-deps
```

#### Windows (`build_mkdwarfs_mod.ps1`)

```powershell
# Dynamic build (default)
powershell -ExecutionPolicy Bypass -File .\build_mkdwarfs_mod.ps1

# Static build
powershell -ExecutionPolicy Bypass -File .\build_mkdwarfs_mod.ps1 -Static

# All options
powershell -ExecutionPolicy Bypass -File .\build_mkdwarfs_mod.ps1 `
    -BuildDir D:\builds\dwarfs-mod `
    -VcpkgRoot D:\tools\vcpkg `
    -Static `
    -WithTests `
    -NoFuseDriver
```

### `--dedup-global` syntax

Global deduplication is **off by default**.  When omitted, mkdwarfs behaves
identically to stock DwarFS.

```
mkdwarfs -i INPUT -o OUTPUT --dedup-global [VALUE]
```

| Value | Meaning |
|-------|---------|
| *(flag omitted)* | **Disabled** (default) — stock DwarFS behavior |
| `N` | Chunk size target in bytes (must be power of two) |
| `Nb` | Explicit bytes (e.g. `1b`, `512b`) — useful to avoid ambiguity |
| `Nk`, `Nm`, `Ng` | kibibytes, mebibytes, gibibytes |
| `off` or `0` | Explicitly disable (same as omitting the flag) |

The implicit default when the flag is given without a value is 4 KiB.

### `--file-dedup` flag

When `--dedup-global` is set, file-level deduplication is **disabled by default**
because the global chunk-level dedup catches all duplicates anyway (including
whole-file duplicates) without the overhead of full-file hashing.

Use `--file-dedup` to explicitly re-enable file-level dedup when you want both:

```
# global dedup only (file dedup OFF — fastest when files are unique)
mkdwarfs -i in -o out.dwarfs -C zstd --dedup-global

# global dedup + file dedup (both ON)
mkdwarfs -i in -o out.dwarfs -C zstd --dedup-global --file-dedup

# stock behavior (file dedup ON, no global dedup)
mkdwarfs -i in -o out.dwarfs -C zstd
```

When `--file-dedup` is used without `--dedup-global`, it has no effect (file
dedup is already on by default).

Examples:
```
# stock DwarFS behavior (no global dedup)
mkdwarfs -i in -o out.dwarfs -C zstd

# global dedup with 4 KiB chunks (default when flag is given)
mkdwarfs -i in -o out.dwarfs -C zstd --dedup-global

# global dedup with 64 KiB chunks
mkdwarfs -i in -o out.dwarfs -C zstd --dedup-global 64k

# global dedup with 1 MiB chunks
mkdwarfs -i in -o out.dwarfs -C zstd --dedup-global 1m
```

### Multi-pass hierarchical CDC (`--min`/`--max`)

When `--dedup-global` is combined with `--min` and `--max`, the segmenter
processes data hierarchically: first at the coarsest level (max), then
re-chunks unmatched regions at each finer level (max/2, max/4, ..., min)
in powers of two.  Each level only processes byte ranges that were not
matched at coarser levels.

This finds duplicates at large granularities first, saving both CPU and
memory for data that deduplicates at the coarser levels.

```
mkdwarfs -i in -o out.dwarfs -C zstd --dedup-global --min 4k --max 128m
```

This runs 8 CDC passes: 128m → 64m → 32m → 16m → 8m → 4m → 2m → 1m → 512k →
256k → 128k → 64k → 32k → 16k → 8k → 4k.  Unmatched regions from each pass
are excluded from finer passes.

| Flag | Description |
|------|-------------|
| `--dedup-global` | Enables global dedup (required); value is the finest level (default 4k) |
| `--min` | Override finest CDC level (default: same as `--dedup-global` value) |
| `--max` | Set coarsest CDC level; enables multi-pass when > min |

**How it works:**

1. Level 0 (128m): CDC splits data into ~128m chunks; XXH3-128 checked against index
2. Matched chunks → emit dedup reference (saved)
3. Unmatched regions → passed to level 1 (64m)
4. Repeat until min level reached
5. Remaining unmatched chunks stored in blocks

Levels are processed sequentially for determinism.  Files within each level
are processed in streaming order (one extent at a time, no full-file buffering).

### Overhead

Global dedup trades build-time resources for smaller images.

**Hash index (RAM during build):**

The in-memory index stores one entry per unique chunk.  Each entry holds a
16-byte XXH3-128 key + 12-byte location + ~12 bytes of hash-map overhead
(~40 bytes total per unique chunk).

| Input size | Chunk size | Unique chunks | Hash index RAM |
|-----------|-----------|---------------|---------------|
| 1 GiB | 4 KiB | ~262k | ~10 MiB |
| 10 GiB | 4 KiB | ~2.6M | ~100 MiB |
| 100 GiB | 4 KiB | ~26M | ~1 GiB |
| 1 GiB | 1 MiB | ~1k | ~40 KiB |
| 10 GiB | 1 MiB | ~10k | ~400 KiB |
| 100 GiB | 1 MiB | ~100k | ~4 MiB |

Smaller chunk sizes find more duplicates but use proportionally more RAM.
Chunk sizes below 4 KiB are rarely beneficial — the hash index grows
linearly while dedup gains plateau once chunks are smaller than the
repeated patterns in the data.

**Build time:** XXH3-128 hashes at ~50 GB/s; the CDC gear-hash pass adds a
second scan over each block.  Expect roughly 10–30% more wall-clock time
compared to stock mkdwarfs, dominated by I/O rather than CPU.

**Image metadata:** Each deduplicated chunk adds a small reference in the
shared files table (~8 bytes per reference).  This is negligible compared to
the dedup savings.

**Disk spillover (`--max-index-memory`):**

When the in-memory hash index exceeds the configured limit, entries are
automatically spilled to sorted temporary files on disk.  Lookups fall
through from RAM to disk via binary search (O(log N) per spill file),
trading speed for unlimited index capacity.

```
# limit index to 8 GiB RAM; excess spills to disk
mkdwarfs -i in -o out.dwarfs -C zstd --dedup-global --max-index-memory 8g

# unlimited (default — current behavior)
mkdwarfs -i in -o out.dwarfs -C zstd --dedup-global
```

Spill files are created in the system temp directory and cleaned up
automatically when mkdwarfs exits.  The final image is identical — disk
spillover is purely a build-time optimization.

---

## Benefits vs. Stock mkdwarfs

| | Stock mkdwarfs | This fork (`--dedup-global`) |
|---|---|---|
| **Chunk-level dedup** | No | Yes (opt-in via flag) |
| **File-level dedup** | Always on | Off by default with `--dedup-global` (re-enable with `--file-dedup`) |
| **Compression ratio** | Good | Better for workloads with repeated data across files |
| **Build-time RAM** | Baseline | +hash index (~40 bytes per unique chunk; see overhead table) |
| **Build time** | Baseline | +10–30% (second CDC + XXH3 pass, I/O-bound); file dedup skipped by default saves full-file hashing |
| **Image metadata** | Baseline | +~8 bytes per deduplicated chunk reference |
| **Image format** | — | Fully compatible (no format changes) |

### When global dedup helps

- Container images with shared base layers
- Source trees with vendored/copied files
- Backup archives with incremental snapshots
- Any dataset with byte-level repetition across files

### When it doesn't help

- Files that are already unique (random data, large media)
- Very small datasets (< 16 MiB)
- Compression alone already captures the redundancy

---

## Compatibility

Images created with `--dedup-global` are **fully compatible** with stock
DwarFS tools.  The format is unchanged — no new metadata, no new block types.

| Test | Result |
|------|--------|
| Stock `dwarfsck` reads `--dedup-global` image | Pass |
| Stock `dwarfsextract` extracts byte-identical files | Pass |
| Stock `dwarfs` FUSE-mounts with correct content | Pass |
| Cross-platform: Linux image → Windows extract | Pass |
| Cross-platform: Windows image → Linux extract | Pass |
| Official v0.15.7 binaries vs our v0.15.5 fork | Pass |

---

## What Was Modified

Only the **writer/make** portion of DwarFS was modified.  The reader,
mounter, extractor, and on-disk format are **unchanged**.

### New files

| File | Purpose |
|------|---------|
| `include/dwarfs/writer/internal/cdc_chunker.h` | CDC chunker with gear-hash table, two-phase normalized masks |
| `include/dwarfs/writer/internal/global_dedup_index.h` | Content-addressed chunk index with disk spillover (phmap flat_hash_map, XXH3-128, sorted temp files) |
| `include/dwarfs/writer/internal/global_dedup_segmenter.h` | Segmenter interface for global dedup mode |
| `src/writer/internal/global_dedup_segmenter.cpp` | Implementation: single-pass and multi-pass CDC → hash → index → deferred write |
| `test/cdc_chunker_test.cpp` | 6 unit tests for CDC chunker |
| `test/tool_mkdwarfs_main_global_dedup_test.cpp` | 6 integration tests |
| `build_mkdwarfs_mod.sh` | Linux all-in-one build script |
| `build_mkdwarfs_mod.ps1` | Windows all-in-one build script |

### Modified files

| File | Change |
|------|--------|
| `tools/src/mkdwarfs_main.cpp` | `--dedup-global`, `--file-dedup`, `--min`, `--max`, `--max-index-memory` flag parsing |
| `tools/src/dwarfs_main.cpp` | Cherry-picked upstream fix (gh #375): don't canonicalize mountpoint on Windows |
| `tools/src/dwarfsck_main.cpp` | `vm.count()` → `vm.count()` (boost DLL compat) |
| `tools/src/dwarfsextract_main.cpp` | Same boost DLL compat fix |
| `src/writer/scanner.cpp` | Route to global dedup segmenter when enabled |
| `src/writer/segmenter.cpp` | Thread-ordering support for category streams |
| `src/writer/segmenter_factory.cpp` | Factory selection for global dedup mode |
| `src/writer/console_writer.cpp` | Display global dedup stats |
| `include/dwarfs/writer/segmenter.h` | Thread-segmented category interface + multi-pass config |
| `include/dwarfs/writer/segmenter_factory.h` | Factory options for dedup params (min, max, index memory) |
| `include/dwarfs/writer/internal/progress.h` | Global dedup progress counters |
| `src/writer/internal/provisional_entry.cpp` | Cross-platform path separator fix (MSVC) |
| `cmake/libdwarfs.cmake` | Added global_dedup_segmenter.cpp + PkgConfig::XXHASH |
| `CMakeLists.txt` | `STATIC_BUILD_DO_NOT_USE` made Windows-aware (MSVC guard) |
| `doc/mkdwarfs.md` | Documentation for `--dedup-global` flag |

---

## License

New source files are © 2026 Edward Sloter and licensed under
[AGPLv3](../LICENSES/AGPL-3.0.txt). Modified original files retain the
license of the original. See the [`LICENSES/`](../LICENSES) folder for the
full license texts of all dependencies.

# #1158 — durable EnvIO bounds: listing pages, listing bounds and positioned reads

## Current delivery scope

The 2026-11-02 reassessment below supersedes the earlier implementation and
validation descriptions in this record. The branch now retains only directory
paging and positioned byte reads. The original unbounded `list` and read tool
are preserved for compatibility; `list_files` and the tool's proposed byte
parameters were removed from the current tree. Earlier descriptions and test
counts document the preserved historical implementation, not the current tree.
The ticket remains open for tree-listing and read-tool work.

## Scope

Aligns the durable environment's file IO with the frozen Linux EnvIO surfaces
that `[#1089](1089-local-environment.md)` left as an implementation gap, under
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md)
and [#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044), with
[#1158](https://github.com/lanshengzhi/cpp-coding-harness/issues/1158) as its
ticket. Named baseline: `pi-v1.3.0` /
`71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.

`#1089` delivered `Environment::read(path)` (the whole file) and
`Environment::list(path)` (every entry of one directory, no bound, no filter).
Frozen pi exposes instead:

- `BinaryReader.read(offset, length)` — up to `length` bytes at `offset`, fewer
  only at end of file, and an empty result for an offset at or past the end
  (`packages/durable/src/env/index.ts:96-101`).
- `DirReader.next(maxEntries)` — pages of one directory that continue where the
  previous call stopped and report `done`
  (`packages/durable/src/env/index.ts:161-168`).
- `FileSystem.listFiles` — a whole-tree walk with `maxEntries` (default 50,000),
  `maxBytes` (default 4 MiB, at most 15.9 MiB), `hidden`, `exclude`,
  `timeoutMs`, returning `{entries, truncated}`
  (`packages/durable/src/env/index.ts:177-208`), implemented by the pi-env
  daemon's `ignore`-based walk (`packages/env/daemon/src/list.rs`).

The durable coding-tools `read` registration read the whole file and had no
positional parameters. Whole-row verification, capability promotion and the
consolidated ledger rebuild stay with
[#1180](https://github.com/lanshengzhi/cpp-coding-harness/issues/1180); the
serial merger registers `audit-1158` itself.

## Frozen evidence

The candidate family `fixtures/pi-v1.3.0/audit-1158/` was captured from
`/tmp/cch-1080-pi-frozen` at `71023aec…` (verified `HEAD`, clean tracked
sources), Linux x86-64 / glibc 2.44 / Node v26.13.0 / tsx 4.22.1, on
2026-11-01T20:17:52.388Z:

| Artifact | Scenarios | Bytes | SHA-256 |
|---|---:|---:|---|
| `listing.json` | 4 | 7363 | `75752b17a33a3436f4ba154420ef78f01d68b68f7cff62a523ab722f1e3f5fda` |
| `positioned-read.json` | 3 | 3953 | `35f57cf0eb7ffea5a072542bb785eeac052b5967d8994a40177d60d1f7c1da81` |

`manifest.json` SHA-256: `160004b4e57b3e39ca5d280d3d6e587e428f704836eaf14f836652b4971f08dd`;
capture generator SHA-256: `7326835625ce2415643627c37bd7d80674b3f353439f51714af81a134065bd34`.

Scenarios:

1. **Pages continue where the previous page stopped** — `DirReader.next(2)` over
   `a`, `b`, `c` returns two pages of 2 and 1 entries, `done` false then true;
   the union of the pages is the directory.
2. **Depth first in name order** — `listFiles` over `b.txt`, `a/z.txt`,
   `a/b/c.txt`, `.hidden/x`, `A.txt` and an empty directory returns
   `.hidden`, `.hidden/x`, `A.txt`, `a`, `a/b`, `a/b/c.txt`, `a/z.txt`,
   `b.txt`, `empty`; `hidden:false` drops the dot entries.
3. **Bounds and `truncated`** — on `a`, `b`, `c`: `maxEntries:2` →
   `[a, b]`, truncated; `maxEntries:3` → all three, not truncated;
   `maxEntries:0` → `[]`, truncated; `maxBytes:6` → `[a, b]`, truncated;
   `maxBytes:5` → `[a]`, truncated (each entry costs its UTF-8 path plus two).
4. **Exclusion rules** — `exclude:["node_modules"]` skips that subtree, `.git` is
   never listed, `hidden:false` drops dot entries, and `out/`/`sub/` survive.
5. **Positioned reads** — `0123456789abcdef` read at `(0,4)`, `(4,4)`, `(10,8)`,
   `(4,8192)`, `(10,0)` returns `0123`, `4567`, `abcdef`, `456789abcdef`, ``.
6. **At or past end of file** — offsets 16, 17 and 4096, and an empty file at
   offsets 0 and 1, return zero bytes with no error.
7. **Across internal read chunks** — a 3 MiB pattern read at `1048569` for 64
   bytes and at `2 MiB` for 1 MiB returns the exact requested bytes, recorded as
   length, SHA-256 and head/tail (the `BinaryReader` reads in 1 MiB chunks).

`NodeExecutionEnv` implements the first, second and last groups directly.
`listFiles` is the optional daemon-backed member, so the capture starts the
checkout's own daemon and builds it from the frozen sources with
`cargo build --locked` when the binary is missing; `gitignore` is disabled on
every call because that result depends on the host's git configuration. The
command recorded in the manifest is
`PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1158/capture-1158.mts`;
the daemon binary is a prerequisite, built by the script on first use.
`python3 scripts/evidence/audit_evidence.py check --manifest
fixtures/pi-v1.3.0/audit-1158/manifest.json` accepts the unregistered bundle.

## Delivered behavior

`<cch/durable/Environment.hpp>` replaces the unbounded `list` with the frozen
two listing surfaces and adds the positional read:

- `open_directory(path)` returns a `DirectoryReader`; `next(max_entries)` yields
  `ListingPage{entries, done}`. The reader holds the directory open, so a page
  continues the same scan; a page is never larger than `max_entries`, and `done`
  may arrive with the last entries or with an empty page. A zero
  `max_entries` is a `Validation` error (frozen `DirReader.next`). An entry that
  vanishes before its metadata is read is skipped instead of failing the page.
- `list_files(path, ListOptions)` returns
  `FileListing{entries, truncated}`. `ListOptions` carries `max_entries`
  (50,000), `max_bytes` (4 MiB, clamped to the frozen 15.9 MiB payload
  ceiling), `hidden` (dot names included by default), `excluded_names` (skipped
  with their subtree) and `timeout` (10 s). The walk is depth first, parents
  before their entries, siblings in byte order; entries cost their UTF-8
  relative path plus two bytes; the first entry beyond a bound stops the listing
  and sets `truncated`. `.git` is never listed, symbolic links are listed but not
  followed, and entries that cannot be read are skipped.
- `read(path, offset, length)` returns up to `length` bytes at `offset` with
  `pread`, so bytes before `offset` are never transferred, fewer bytes only mean
  end of file, and an offset at or past the end returns an empty result rather
  than an error. The whole-file `read(path)` is unchanged for `edit`.

The durable `read` tool declares optional `offset` and `length` byte parameters
(frozen `readFile` semantics) and routes that call through the positional read;
without them it keeps the whole-file read and image detection. A range boundary
inside a UTF-8 sequence becomes U+FFFD so the tool still returns valid text.
Validation refuses negative, fractional or imprecise (above 2^53) byte counts.

Memory is bounded by the listing's own bound rather than the directory or file
size: `smallest_names` keeps only the smallest `remaining + 1` names of one
directory while the directory streams past, so a level never holds more names
than the listing may still emit, and the positional read allocates only its
64 KiB transfer buffer plus the requested range.

## Validation

Measured on the default strict Debug preset, base `origin/main` `7e2ccc288`.

- Focused Validation: `ctest --preset vcpkg -L issue1158` selected **7** and
  passed **7/7** in 0.29 s.
- Owning module: `ctest --preset vcpkg -L durable -j1` selected **334**, passed
  all, **9 skipped** (the frozen-pi continuation prerequisites, unrelated to this
  slice).
- Architecture-sensitive selection: `ctest --preset vcpkg -L architecture -j1`
  selected **43**, passed **43/43**. The Owner Interface header gained the
  reader class and the listing/read shapes; the build-phase Parity Architecture
  Gate passed on every build.
- Full Validation: `TERM_PROGRAM=herdr ctest --preset vcpkg -j1` selected
  **4178**, passed **4165**, skipped **11**, failed **2**. The skips are the nine
  frozen-pi durable continuation cases, the realtime-clock adjustment case and
  the opt-in Native TUI differential capture. The two failures are the
  `cch_pi_v130_consolidation_check`/`_boundaries` pair, and they fail identically
  on a pristine `git archive origin/main` tree: the #1175 registration commit
  `7e2ccc28` changed `fixtures/pi-v1.3.0/baselines.json` after the `1090`
  delivery's revalidation, so `consolidate_audits.py check` reports
  `1090-durable-images: delivery digest mismatch`. The PM owns that repair on
  main; this slice changes no pinned delivery input. Three runner-sensitive cases
  failed once under the serial run and pass in isolation: the #990 TUI late-reply
  timing case, the #1096 codemode close/admission case and the documented #617
  `[quarantine]` projection cost bound (2.53 ms against the 2 ms Debug bound).
  No bound, timeout or quarantine policy changed here.
- Discriminating case for "neither path may load an unbounded amount": with the
  positional read replaced by a whole-file read and `list_files` ignoring
  `max_entries`, `-L issue1158` fails — the 512 MiB sparse read raises the
  process `VmHWM` to 583,376,896 bytes against a 64 MiB allowance, the bounded
  listing returns 3000 entries instead of 100, and `truncated` stays false. With
  the delivered code the same case passes: the sparse read raises `VmHWM` by
  less than 64 MiB, the listing stops at 100 with `truncated`, and a 3000-entry
  directory pages in 31 pages of at most 100 entries.
- Frozen comparison: four of the seven focused cases read the committed frozen
  artifact (`FrozenScenario`) rather than C++-invented values; the page case
  compares page sizes, `done` marks and the union of the page names, and the
  listing cases compare every entry path and kind plus `truncated`.
- `scripts/format-check.sh origin/main` and `git diff --check`: clean.

## 2026-11-02 reassessment after merging main

This ticket remains open. The owner resolved the issue wording against frozen
`71023aec`: `BinaryReader.read(offset, length)` uses zero-based bytes and returns
empty at or past EOF; the durable `read` tool instead uses one-based line
`offset` and `limit`, reports an offset beyond the line count as an error, and
applies the frozen default output limits. The issue's “same treatment” cannot
mean identical parameter units on both surfaces. The prior proposed tool byte `offset`/`length` interface and its focused test
were removed. The restored tool's no-parameter path still reads the whole file. Frozen reference:
`packages/durable/src/env/index.ts` `BinaryReader` and
`packages/durable/src/tools/read.ts` `readSchema`, `readText`, `readHead`.

The environment interface and focused tests cover paged directory continuation,
entry/byte limits, hidden and explicit excluded names, positional bytes, EOF,
and large inputs. They do not cover the frozen default gitignore behavior:
work-tree detection, parent and nested `.gitignore`, `.git/info/exclude`, global
excludes, rule precedence, and `gitignore:false`. The capture explicitly disables
that behavior. Frozen `packages/env/daemon/src/list.rs` uses the Rust `ignore`
crate; the application-private `GitIgnoreMatcher` implements a different npm
ignore subset and is not an established durable Owner seam.

The removed tree-listing implementation has further gaps before acceptance:
`smallest_names` scans a whole directory before checking the deadline; metadata
failures are skipped only after retaining the bounded prefix, and unsupported
file types were classified as files. Frozen listing skips unsupported
kinds and checks its timeout as it walks. Frozen tool comparison still needs
line selection, past-end error, default byte/line limits, BOM/UTF-8, diagnostics,
and bounded memory, including the no-parameter path. The removed tool test asserted byte parameters and is not evidence for
line semantics.

A possible split is environment pagination and positional reads first, followed
by listing ignore semantics and bounded line-oriented tool behavior. The first
slice excludes the incompatible tool API and tree-listing implementation; the
latter's edge cases must be resolved in its follow-up. The positioned operation
opens a path for each call: it does not retain the frozen `BinaryReader`'s file
identity across rename or replacement between calls. It returns a Validation
error when the requested nonempty range reaches beyond `off_t`'s addressable
range; the committed EOF comparison does not establish behavior across the full
`uintmax_t` domain. Follow-up is needed if the selected public contract requires
an open reader's identity or EOF behavior beyond the addressable range; the
existing evidence does not establish either property. Shared delivery receipt
revalidation and serial-merger registration of `audit-1158` remain prerequisites
for any delivery that changes their pinned inputs or registry.

### Exact-full-page observation

The independent [exact-page probe](1158-exact-page-probe.mts), run on a clean
`71023aec1546d6d677a65669c5665c0927ce0dd0` checkout, created four files and
called `next(2)` three times. It observed `(entry count, done)` of
`(2, false), (2, false), (0, true)`. The observed names were `c,d` then `a,b`;
filesystem order is not part of the assertion. Frozen
`packages/durable/src/env/node.ts` `NodeDirReader.next` sets `done` only after
`dir.read()` returns `null` and stops as soon as the page fills. The C++
large-directory test exercises this exact multiple (3000 entries, 100 per page)
and requires 31 pages. Reproduce the read-only observation with:

```sh
PI_CHECKOUT=/tmp/pi-1113-frozen TSX_TSCONFIG_PATH=/tmp/pi-1113-frozen/tsconfig.json \
  NODE_OPTIONS=--conditions=source /tmp/pi-1113-frozen/node_modules/.bin/tsx \
  docs/research/pi-v1.3.0/1158-exact-page-probe.mts
```

This probe is separate from the candidate audit family and changes no baseline
registration.

### Current-tree validation

On merged revision `b9b7c1fb2`, the `issue1158` selection passed 4/4. The
`durable` selection ran 353 cases: 343 passed, 9 skipped (frozen-pi continuation
prerequisites), and 1 failed. The failure, `generation publishes the attempt
before the model stream and usage with the final answer`, observed
`atomic_answer_commits == 0` instead of 1; a separate rerun failed identically.
The corresponding test and Harness/Session/Workflow implementation are unchanged
relative to the merged main. A pre-existing main-checkout binary also failed the
same case, but its exact revision is unverified. The cause and possible indirect
relationship to this branch remain unresolved.

The 1090/1101/1104 receipt selection passed 36/36 and the 1184 selection passed
10/10; their post-merge JUnit artifacts and reviewed changed-input digests replace
neither prior artifact. Consolidation build and check passed, as did all 42
`ConsolidationAuditTest.py` cases. Candidate `audit-1158` passed its manifest
check; registry verification covered 56 registered bundles, 116 artifacts and
654 scenarios. The serial architecture selection passed 43/43. An earlier run
concurrent with a build was interrupted after `PARITY-6002` reported changing
Ninja dependency evidence; it did not complete. Registration remains with the
serial merger. No unfiltered CTest suite was run, at the user's request.

## Capability status

No capability row is claimed and no ledger membership changes. This slice closes
the EnvIO bounds gap recorded against `#1089`; whole-row verification and the
`audit-1158` registration remain with #1180.

## Remaining boundary

- `list_files` does not implement frozen `gitignore` handling (`.gitignore`
  files, `.git/info/exclude`, the global excludes file, work-tree detection).
  The `ignore`-crate semantics behind frozen `listFiles` are not ported; the
  capture disables `gitignore` so its observations stay host-independent. Only
  `hidden`, `excluded_names` and the unconditional `.git` exclusion are claimed.
- Frozen `DirReader` pages one directory; it has no byte bound and no exclusion
  filter, so those live in `list_files`, exactly as in the frozen surfaces.
- The durable `read` tool's positional parameters are bytes, not the frozen
  `packages/durable/src/tools/read.ts` line-oriented `offset`/`limit` slice with
  its truncation diagnostics. Line selection, BOM-stripped line counting and the
  "Showing lines …" diagnostics remain unimplemented.
- Frozen `listFiles` reports `{path, kind}`; `FileListEntry` also carries `size`,
  which the frozen shape leaves to `fileInfo`.
- Sibling ordering in `list_files` keeps frozen byte order by retaining a
  bounded prefix per directory; a directory larger than the remaining entry
  bound is therefore truncated at the bound rather than fully sorted first
  (frozen sorts the whole level, which is unbounded in that one directory).

# Source-File Layout Cleanup — Plan

**Status: COMPLETE (2026-05-07).** All three passes landed in
commits 74b6e0b (Pass A1), f3108f1 (Pass A2), 1465ec7 (Pass B),
3188d1b + 188d370 + 9b576e6 + 761ffc1 (Pass C), and 526f580
(style-guide refinement). 4a6e57e cleaned up review NITs.

Captured 2026-05-07 after a project-wide style review surfaced
~50 files with violations of the canonical "Source File Layout"
order documented in `docs/AXL-Coding-Style.md`. The review was
triggered by the user spotting a mid-file `#include` plus
scattered globals/typedefs/`static`s in several modules.

## Background

`docs/AXL-Coding-Style.md` §"Source File Layout" prescribes a
strict top-to-bottom order for `.c` files:

  1. SPDX/copyright + `@file` block
  2. Includes
  3. `AXL_LOG_DOMAIN(…)`
  4. Macros
  5. Types
  6. File-scope variables
  7. Forward declarations of static functions (when needed)
  8. Function implementations grouped under banner comments

The codebase has drifted in two directions:

- **Section-co-located declarations** — typedefs, macros, and
  static vars are sometimes declared in the body, just before
  the helper section that uses them, under a banner comment.
  Locally readable, but contradicts the canonical order and makes
  it harder to see all of a file's types/globals at a glance.
- **`extern` of project-internal symbols** — repeated extern
  declarations of `axl_*` and EDK2 (`gImageHandle`) globals
  inside function bodies, instead of including the header that
  already declares them or hoisting one block to the top.

User's framing: "If we need middle-of-file includes/sections,
that hints to me that it should have probably been done in a
separate file." The cleanup plan honours that — for the heavy
offenders, the answer is to split, not to hoist into one giant
file with everything at the top.

## Pass A — high-severity, mechanical (no behavior change)

Headline finding: **5 mid-file `#include` directives and 4
project-internal `extern`s** that should be replaced by including
the right header. Plus **11 repeated `extern EFI_HANDLE
gImageHandle;`** sites across 4 files that should consolidate.

### Mid-file `#include`s

- `include/axl/axl-mem.h:265` — `#include <axl/axl-mem-impl.h>` ~240
  lines below the top includes block.
- `include/axl/axl-mem.h:27` — `#include <axl/axl-macros.h>` placed
  *inside* `extern "C" {` (should be above it).
- `include/axl/axl-str.h:1158` — `#include <stdarg.h>` mid-file.
- `src/net/axl-mbedtls-platform.c:142` — `#include
  <mbedtls/platform_time.h>` between functions.
- `src/net/axl-tls.c:50-61` — second include block in an `#else`
  branch, after stub function impls. Possibly defensible (gated
  on AXL_TLS) but worth re-evaluating.

### `extern` of project-internal symbols

- `src/net/axl-mbedtls-platform.c:148` — `extern uint64_t
  axl_time_get_ms(void);` inside a function. Replace with
  `#include <axl/axl-time.h>`.
- `src/util/axl-driver.c:468` — `extern void axl_stream_init(void);`
  inside a function. Replace with `#include <axl/axl-stream.h>`.
- `src/util/axl-sys.c:70` — `extern const EFI_GUID
  *axl_protocol_lookup_guid(...);` inside a function. Symbol is
  defined in `src/util/axl-protocol.c`; needs an internal header
  (e.g. `src/util/axl-protocol-internal.h`) to share between the
  two files without leaking to public API.
- `tools/timetest.c:21` — `extern uint64_t
  axl_backend_get_monotonic_us(void);` workaround for not exposing
  the backend header to tools. Proper fix: expose a public
  microsecond-monotonic time API or use an existing one
  (`axl_time_get_ms` then convert).

### `gImageHandle` and friends — hoist or include

Files repeatedly redeclaring EDK2 globals inside functions:

- `src/util/axl-driver.c` — 7 sites (155, 346, 374, 431, 461,
  537, 746). Consolidate to one top-of-file `extern` block, or
  include the UEFI header that declares it.
- `src/util/axl-diag.c:40`
- `src/runtime/axl-runtime.c:107`
- `src/crt0/axl-crt0-minimal.c:69`

Pass-A scope estimate: ~15 files, one commit, zero semantic
risk. This alone clears every "high-severity" finding the review
agents flagged.

## Pass B — mechanical scatter cleanup (small files)

Files with 1-3 small scatter findings — typedefs, macros, or
file-scope `static`s introduced between functions instead of in
the typed/macros/vars section near the top. One commit per
module is fine; risk remains low (no semantic change).

Highlights from the review (full list lives in the agent reports
referenced from the session conversation):

- `src/data/axl-str.c` — late typedef (`ScanLen` at 2151) plus
  scattered macros and base64 tables.
- `src/util/axl-driver.c` — 6 scatter findings beyond the
  `gImageHandle` block (already covered in Pass A).
- `src/util/axl-sys.c` — 5 scatter findings (3 typedefs at
  242/295/330; macro at 208; static at 339).
- `src/util/axl-image-verify.c` — DER `#define` cluster at
  154-162; OID statics at 167, 170.
- `src/log/axl-log.c` — macros at 430-431; static at 597.
- `src/fs/axl-fs.c` — `struct AxlDir` at 278; macro at 391.
- `src/usb/axl-usb.c` — static cursors at 116-118; macro at
  134; enum at 666.
- `src/spd/axl-spd.c` — file-scope statics split across the
  file (line 31 vs 186-187).
- `src/data/axl-json-build.c`, `src/data/axl-sidecar.c`,
  `src/data/axl-digest-sha256.c`, `src/data/axl-json-print.c`,
  `src/event/axl-wait.c`, `src/net/axl-url.c`,
  `src/crt0/axl-reloc.c` — one or two scatters each.
- `src/ipmi/axl-ipmi.c:379` — single typedef.
- `tools/{cat,find,grep,i2c,ipmi,mkrd,netinfo}.c` — one or two
  scatters each, mostly typedefs between functions.

Pass-B scope estimate: ~25 files, several small commits.

## Pass C — architectural calls on the heavy offenders

Files where scatter is symptomatic of "this file does too much"
rather than "the author forgot to hoist":

- **`src/pci/axl-pci.c`** (~1700 lines, ≥10 scattered declaration
  blocks). Natural splits: capability walk, bridge/topology
  enumeration, the typed-reader / config-space accessors. Likely
  becomes `axl-pci.c` + `axl-pci-cap.c` + `axl-pci-bridge.c` +
  `axl-pci-walk.c`.
- **`src/backend/native/axl-backend-native.c`** (~1300 lines, 7
  scattered blocks). Internal `EventCloseRing`, MP-services
  context, shell-protocol probes, simple-text-input-ex caching
  — each is its own concern. Splits would mirror those.
- **`src/acpi/axl-acpi.c`** — FADT + MCFG + MADT typedef clusters
  point at three sub-modules: a generic table walker, a typed
  FADT reader, a typed MCFG reader (MADT could live with the
  walker).
- **`src/smbios/axl-smbios.c`** — `SmbType0..SmbType5` typedefs
  in one block at 335-403 are the SMBIOS tables 0-5. The split
  question is whether to break per-type or keep the table cluster
  but hoist it. The cluster itself is fine; just hoist.

For each of these, open a separate discussion: split vs hoist.
Don't reflexively reach for "split" — `axl-smbios.c` reads as a
"hoist the type cluster to the top" candidate, not a split.

Pass-C scope estimate: 4-6 files touched, 2-4 new files
introduced, several commits. Each split is reviewable on its own
merits and should not be batched.

## After C — style-guide refinement

Update `docs/AXL-Coding-Style.md` §"Source File Layout" to add a
sentence (verbatim is fine):

> If a section under a banner comment accumulates its own
> typedefs, macros, and file-scope statics, that's a signal to
> split it into its own file rather than declare them inline in
> the body. Hoisting to the top of a 1500-line file makes the
> declarations harder to find than declaring them inline; the
> right answer is usually a smaller file.

This locks the user's framing into the guide so future agents
don't re-discover the same gray area.

## Testing strategy

All three passes are structurally cosmetic — no semantic change,
no behavior risk. Failure modes are typos and broken builds, both
caught by:

  - `make tests` on both arches
  - `./test/integration/test-axl.sh` (unit ratchet)
  - `./test/integration/test-http.sh` (HTTP integration)
  - `./test/integration/test-tools.sh` (tools smoke)

Run all three after every commit. Bump `.last-pass-count` only
on net additions (these passes shouldn't change the count).

## Out of scope for this plan

- Public API changes.
- Behavioral changes.
- Doc updates outside the style-guide refinement above.
- Any work on consumer projects (axl-webfs, etc.) that depends
  on these splits — Pass C may break header paths that consumers
  import; if so, surface the migration cost before splitting.

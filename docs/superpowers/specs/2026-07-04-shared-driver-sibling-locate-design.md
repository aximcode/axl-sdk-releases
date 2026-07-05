# Design: sibling-only shared-driver locate + default search reorder (v2.7.1)

**Date:** 2026-07-04
**Status:** approved (designed with the user in-session)
**Scope:** additive public API + one default-behavior change, in `src/util/` +
`include/axl/` + `include/axl.h` + tests/docs. Ships in the pending `##
Unreleased` → **v2.7.1** section alongside the stdio-bridge liveness fix.

**Semver note:** new public API is normally a MINOR bump; the user has chosen to
ship it as the v2.7.1 PATCH deliberately. Recorded, not an oversight.

Origin: request from the delldiags/axl-utils `do.efi`/`doDriver.efi` consumer
(`local/docs/handoff-shared-driver-sibling-locate.md`).

## 1. Problem

A thin, **version-pinned** launcher (`do.efi`) must load its resident driver
(`doDriver.efi`) from **its own directory only** and **hard-fail** if the
matching driver isn't staged beside it. The two halves share a cross-image
vtable ABI, so loading a wrong-version driver is a *silent-corruption* hazard,
not a graceful failure.

The turnkey resolver `axl_shared_driver_locate()` does not preserve that policy.
Its cold path (`driver_build_candidates`, `src/util/axl-driver.c:1103`) searches,
in order:

1. `/drivers/<arch>/<name>` on the image's volume  ← **before** the sibling
2. `<image_dir>/<name>` (the sibling)
3. `/drivers/<name>` at volume root
3.5. `<name>` at volume root
4. `/drivers/<arch>/<name>` on **every other** mounted volume

Two defects:
- **(A, affects everyone)** a stale `/drivers/<arch>/<name>` is *preferred* over
  the driver co-staged beside the launcher — surprising for any consumer.
- **(B, the pinning need)** even sibling-first, the multi-path search still
  *falls back* to other locations, so it can never express "hard-fail if not
  beside me."

## 2. Changes

### 2.1 Reorder the default search — sibling first (fixes A, all consumers)

In `driver_build_candidates`, emit the sibling candidate (`<image_dir>/<name>`)
**before** `/drivers/<arch>/<name>`-on-image-volume. New order:

1. `<image_dir>/<name>` (sibling)  ← moved to front
2. `/drivers/<arch>/<name>` on the image's volume
3. `/drivers/<name>` at volume root
3.5. `<name>` at volume root
4. `/drivers/<arch>/<name>` on every other volume

A co-located driver is the most specific intent; it should win. This does **not**
give hard-fail (the search still falls back), so 2.2 is still needed for strict
pinning. **Behavior change** — verify no in-tree consumer relies on
`/drivers/<arch>/` winning over a sibling (grep + the existing driver tests must
stay green).

### 2.2 New API: `axl_shared_driver_locate_sibling` (fixes B)

```c
/* <axl/axl-shared-driver.h> */
/**
 * @brief Locate a shared-driver vtable, SIBLING-ONLY (version-pinned).
 *
 * Warm resident short-circuit; else cold-load @p driver_filename from the
 * LAUNCHER's OWN directory only (@ref axl_driver_load_sibling) and start it —
 * no /drivers, no volume-root, no cross-volume search. Hard-fails
 * (AXL_NOT_FOUND) if the driver isn't staged beside the launcher. For
 * version-pinned launchers that must pair with the exact driver co-staged with
 * them. Thin by construction (no embedded-blob arg). Installs the stdio bridge
 * like the other locate* variants.
 *
 * NOTE: pinning governs only the COLD path. Once ANY driver of this identity is
 * resident, the warm short-circuit returns it regardless of version — the first
 * cold load pins for the boot. (Same model as do.efi today.)
 *
 * @return AXL_OK; AXL_NOT_FOUND if not staged beside the launcher (rc from
 *   axl_driver_load_sibling, returned verbatim); AXL_INVALID on a non-bare
 *   @p driver_filename; AXL_ERR on start / post-load resolve failure.
 */
int axl_shared_driver_locate_sibling(
    const char  *name,
    const char  *driver_filename,
    void       **out_iface);
```

**Impl** (in `src/util/axl-shared-driver.c`, mirroring
`axl_shared_driver_locate_with_image_info`): validate args + `*out_iface = NULL`;
derive GUID via `axl_shared_driver_guid`; **warm** `axl_protocol_find_guid` → if
resolved, skip to bridge install + return AXL_OK; **cold**
`axl_driver_load_sibling(driver_filename, &h)` — **return its rc verbatim** on
failure (preserves AXL_NOT_FOUND / AXL_INVALID) — then `axl_driver_start(h)`
(unload `h` on start failure); re-resolve via `axl_protocol_find_guid`;
`axl_backend_stdio_bridge_install()`; return AXL_OK. Does NOT go through the
multi-path `_axl_driver_ensure_*` seam.

### 2.3 Granular rc across the whole `locate*` family

Today `axl_shared_driver_locate` / `_with_load_options` / `_with_image_info`
collapse every cold-path failure to `AXL_ERR` (`axl-shared-driver.c:217,228`).
Change them to **propagate the underlying rc**: `AXL_NOT_FOUND` when the driver
can't be resolved from any candidate or the embedded blob, `AXL_ERR` for other
errors (bad args, post-load resolve mismatch). Requires
`_axl_driver_ensure_with_embedded_info` to return `AXL_NOT_FOUND` (not `AXL_ERR`)
on its "nothing loaded" path; locate* then returns that rc instead of forcing
`AXL_ERR`. Keeps the existing `axl_warning` diagnostics.

Rationale: consistency with 2.2, and consumers (do.efi) want to distinguish "not
staged / not found" from other failures. Callers checking `!= AXL_OK` are
unaffected; this is the family's highest regression-surface change → the
integration pass must confirm no in-tree caller switches on `== AXL_ERR`.

### 2.4 `axl_shared_driver_run_sibling` + `AXL_SHARED_DRIVER_LAUNCHER_SIBLING`

Mirror the existing `AXL_SHARED_DRIVER_LAUNCHER` → `axl_shared_driver_run`
composition: add a `run_sibling` helper (locate-sibling + dispatch + not-found
exit-status), and make the macro a one-liner over it.

```c
/* <axl/axl-shared-driver.h> — sibling-only sibling of axl_shared_driver_run */
int axl_shared_driver_run_sibling(
    const char  *name,
    const char  *driver_filename,
    int          argc,
    char       **argv);
/* Impl: axl_shared_driver_locate_sibling → on failure axl_set_exit_status(
   AXL_EFI_NOT_FOUND) + return 1 (match axl_shared_driver_run's shape) →
   axl_shared_driver_dispatch(vt, argc, argv). */

/* include/axl.h — symmetric with AXL_SHARED_DRIVER_LAUNCHER / _THIN */
#define AXL_SHARED_DRIVER_LAUNCHER_SIBLING(name_str, driver_filename)          \
  int main(int argc, char **argv) {                                            \
    return axl_shared_driver_run_sibling(                                      \
        (name_str), (driver_filename), argc, argv);                           \
  }
```

A turnkey `int main` for strict-pinned launchers that don't need `do.efi`'s
`-u`/`--reload` escape hatches. `do.efi` itself keeps its custom `main` (it has
those hatches) but collapses its `find_resident` + sibling-load block into the
single `axl_shared_driver_locate_sibling` call. Match the exact error/exit shape
of `axl_shared_driver_run` (verify against `axl-shared-driver.c`).

## 3. Tests (bucket A — new public API, STRICT test-first)

A single QEMU fixture (launcher + trivial resident driver) staged three ways to
prove the policies diverge exactly at the sibling-vs-`/drivers` boundary. Use
`test_add_efi src dest` to place artifacts at explicit image paths.

| Staging | `axl_shared_driver_locate_sibling` | `axl_shared_driver_locate` (multi-path) |
|---|---|---|
| driver **beside** launcher | AXL_OK, resolves | AXL_OK, resolves |
| driver **only** at `/drivers/<arch>/` (not beside) | **AXL_NOT_FOUND** (hard-fail) | AXL_OK, resolves |
| driver at **both** (sibling + `/drivers/<arch>/`) | resolves the sibling | resolves the **sibling** (proves 2.1 reorder) |

Exact-value assertions on the rc (`AXL_NOT_FOUND` vs `AXL_OK`) and, for the
"both" case, on *which* driver answered (stage two drivers that report distinct
identities so the winner is observable). Confirm RED first where applicable
(e.g. the reorder assertion fails against current `driver_build_candidates`
order). Both arches, ratchet-exempt integration fixture.

Must stay green (both arches): existing `test-driver-*`, `test-sd-ergo`,
`test-stdio-bridge-*`, `test-io-redirect` fixtures — especially anything that
loads a driver, to catch a reorder regression.

## 4. Gates / docs / release

- Test-first (bucket A): header + docstring first (contract already designed
  here), failing tests, RED, implement, GREEN, refactor, integration review
  before commit.
- Both arches (`make ARCH=x64`/`aa64`), 0 warnings; `scripts/lint.sh` clean.
- Docs (same change): header docstrings (with the warm-path caveat); `make
  check-docs`; add `.. doxygenfile::`? — no new header (adds to existing
  `axl-shared-driver.h`, already rendered). Update
  `docs/AXL-Shared-Driver-Recipe.md` (the sibling variant + the reorder note +
  the pinning use case) and `CHANGELOG.md` `## Unreleased` → `### Added`
  (`axl_shared_driver_locate_sibling` + the macro) and `### Changed` (default
  search now prefers the sibling). Build Sphinx (0 warnings).
- Ships in **v2.7.1** with the liveness fix; **user tag approval required**
  (`feedback_release_approval_gate`).

## 5. File-level change list

- `src/util/axl-driver.c`: reorder the two candidate appends in
  `driver_build_candidates` (sibling first); make
  `_axl_driver_ensure_with_embedded_info` return `AXL_NOT_FOUND` on its
  nothing-loaded path.
- `src/util/axl-shared-driver.c`: add `axl_shared_driver_locate_sibling` and
  `axl_shared_driver_run_sibling`; propagate granular rc in the multi-path
  locate wrappers.
- `include/axl/axl-shared-driver.h`: declare `axl_shared_driver_locate_sibling`
  + `axl_shared_driver_run_sibling` (+ docstrings); note the granular-rc
  contract on the family.
- `include/axl.h`: add `AXL_SHARED_DRIVER_LAUNCHER_SIBLING`.
- `test/integration/`: new fixture (launcher + driver(s)) + `test-*-qemu.sh`
  covering the divergence matrix.
- `docs/AXL-Shared-Driver-Recipe.md`, `CHANGELOG.md`.

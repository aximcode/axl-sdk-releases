# Remove the `AXL_TLS` build flag — always build with mbedTLS

> **Status: IMPLEMENTED 2026-08-21 (T1-T4 all landed).** Owner of the
> measurement that the flag bought nothing, and of the argument that the
> property it was really protecting was already delivered — and already tested
> — by something else. §10 carries the measurements as TAKEN, not intended.

## 1. Summary

`AXL_TLS` selects whether mbedTLS is compiled into `libaxl.a`. Remove it: build
with mbedTLS unconditionally.

The justification is a single measurement (§2). The flag exists to spare
consumers the size of TLS they do not use, and it does not do that — because
`--gc-sections` plus the `axl_tls_init()` ops indirection already do, for both
settings, identically. What the flag adds is a second build configuration, and
this tree has paid for that configuration repeatedly (§3).

Immediate motivation: the Ed25519 design (`2026-08-21-ed25519-design.md` §3)
chose to implement SHA-512 inside `AxlDigest` **specifically to avoid depending
on the TLS toggle**, duplicating a compress function mbedTLS already compiles.
Remove the toggle and that reasoning evaporates — AxlDigest can wrap mbedTLS's
SHA-512, one implementation instead of two, and an accelerated kernel installed
through `MBEDTLS_SHA512_PROCESS_ALT` accelerates the whole TLS stack rather than
one signature algorithm. That spec is revised after this lands (§9).

## 2. The measurement

Built both ways, x64, same tree:

| | non-TLS build | `AXL_TLS=1` build |
|---|---|---|
| `hello.efi` | 47,616 bytes | **47,616 bytes** |
| mbedtls symbols in `hello.so` | — | **0** |
| binaries present in both prefixes | 54 | 127 |
| **byte-identical** among the common set | **50 of 54** | |

The four that differ are `AxlTestCrypto`, `AxlTestJose` and `AxlTestNet` — each
of which genuinely compiles TLS-gated code — plus `AxlTestSsh`, whose two copies
were built at different points in the measuring session and whose difference was
therefore **not attributable to the flag**.

These were working numbers, taken from trees built at different moments. **§10
supersedes them** with three clean builds from an empty tree, and reaches a
stronger conclusion: `hello.efi` is byte-identical, by sha256, across the
pre-change non-TLS build, the pre-change TLS build, and HEAD.

**The property the flag was protecting is already guaranteed and already
asserted.** `test/integration/test-tls-strippable.sh` exists to prove that
"building libaxl with `AXL_TLS=1` must NOT pull mbedTLS (~280 KB) into a
consumer that links the HTTP client but only ever speaks plain `http://`". The
mechanism is an ops indirection (`src/net/axl-http-client-tls.h`) populated by
`axl_tls_init()`, so a consumer that never calls it lets `--gc-sections` drop
the TLS module and mbedTLS with it. The flag is a third thing guarding a
property that already has a mechanism and a test.

## 3. What the flag has cost

Each of these is recorded in the tree, not hypothesised:

- **`PREFIX` grew a third input.** `scripts/build-prefix.sh` exists because
  "PREFIX is a function of three inputs and grew a fourth: ARCH always, BUILD
  since the RELEASE tree split, and AXL_TLS now that a TLS build gets its own
  tree". Its docstring records that when the TLS suffix appeared, **"98 [hand-
  written prefixes] across 66 scripts silently pointed at a tree nothing had
  built"**.
- **Toggling WIPES objects.** The Makefile's AXL_TLS state-change detection
  deletes `.o`, both archives and every `.efi`. CLAUDE.md records that
  `run-integration.sh` forces `AXL_TLS=1` on every test *because* that wipe
  "fires constantly and — run concurrently — clobbers other tests' artifacts
  mid-run".
- **CI carries ordering constraints purely to dodge it** — `release.yml`:
  "ORDER MATTERS: unit tests BEFORE the AXL_TLS=1 build, not after."
- **Two ratchet baselines, which drift.** `.last-pass-count` and
  `.last-pass-count.tls`. Observed 2026-08-21: the TLS baseline was **104
  assertions stale**, because TLS runs happen less often.
- **Half the crypto/JOSE surface does not run by default.** The default config
  skips **16 groups**; `AXL_TLS=1` skips **5** and runs **161 more assertions**.
- **Whole integration tests exist only as workarounds.**
  `test-jose-qemu.sh` (12 `AXL_TLS` references) and `test-pk-verify-qemu.sh`
  (10) each rebuild their binary with `AXL_TLS=1` into a segregated prefix
  *because* "the default unit suite builds without AXL_TLS".
  `test-https-client-qemu.sh` and `test-http-async-qemu.sh` carry smaller
  versions of the same workaround.
- **`install.sh` once silently defeated the split** (fixed in the v4.1.0 work).

## 4. Scope

407 occurrences of `AXL_TLS` across ~88 files, but the semantic surface is much
smaller than that count suggests:

| area | occurrences | nature |
|---|---:|---|
| `test/` | 166 | skip branches, rebuild workarounds, balancers |
| docs + `*.md` | 127 | **mostly history** — handoffs and ROADMAP-Archive, which are NOT rewritten |
| `src/` + `include/` | 67 | of which the real guards are **33 `AXL_HAVE_TLS`**; the rest are comments and the unrelated `AXL_TLS_ERR`/`_OK`/`_WANT_MORE` enum names |
| `Makefile` | 29 | the toggle, the PREFIX suffix, the state-change wipe |
| `scripts/` | 16 | `build-prefix.sh`, `sdk-prefix.sh`, `install.sh`, `build-packages.sh`, `lint.sh` |
| `.github/workflows/` | 13 | matrix entries and ordering comments |
| `tools/` + `sdk/` | 8 | consumer-facing guards |

**Historical documents are left alone.** A handoff that says "built with
`AXL_TLS=1`" was true when written; rewriting history to match the present is
how a record stops being one. Only current docs — ROADMAP, README, CLAUDE.md,
RELEASING, the sphinx module pages — are updated.

## 5. mbedTLS stays a submodule

Decided deliberately, after first recommending the opposite for a wrong reason.

The tree has exactly one submodule (mbedTLS, verified clean at upstream tag
`v3.6.3`) and five tracked vendored copies: libvterm (22 files), lzma (14),
freetype (5), sdefl (3), stb (2). Vendoring mbedTLS's `library/` + `include/`
would add roughly 200 files and 8 MB — about 30x the largest existing tracked
dependency, permanently in history.

**The split is principled, not accidental, and consistency argues for keeping
it.** mbedTLS is the only dependency consumed *wholesale and unmodified*, which
is exactly what a submodule is good at. The other five are curated subsets
carrying local patches — libvterm has eight, freetype has a memory-corruption
fix, lzma has an AXL-authored `errno.h`. You cannot submodule "five files of
FreeType with a typo fix".

**The one real regression, engineered rather than left to chance.** Today a
clone without `--recursive` still builds, with TLS off. Afterwards it cannot.
The Makefile must detect an empty `deps/mbedtls` and fail with a single line
naming `git submodule update --init --recursive` — not with a hundred
missing-header errors. CI already checks out `submodules: recursive`.

## 6. What is deleted

| thing | after |
|---|---|
| the `AXL_TLS` toggle and the 33 `AXL_HAVE_TLS` **guards** | gone; the code is unconditional |
| the `AXL_HAVE_TLS` **macro itself** | **KEPT, and now always defined.** It appears in public headers, so a consumer may well write `#ifdef AXL_HAVE_TLS` in their own code. Undefining it would silently take the false branch and disable *their* TLS path — a silent break for no gain. Its meaning ("TLS is available here") stays accurate; it is simply always true now |
| the `PREFIX` TLS suffix | `PREFIX` returns to `(ARCH, BUILD)` |
| the **`AXL_TLS` input to** the state-change object wipe | gone — but **the wipe itself STAYS**. It covers three inputs (CLAUDE.md): the `AXL_TLS` toggle, a `CFLAGS`/`CXXFLAGS`/`INCLUDES` hash, and `CC`/`CXX`. Only the first disappears; deleting the mechanism would drop the guard against a flag or compiler change, which is the reason it was built. `TLS_STATE` is renamed to reflect what it still signs |
| CI's TLS ordering constraints | gone |
| `.last-pass-count.tls` | one baseline |
| the rebuild workarounds in four integration tests | those binaries run in the normal suite |
| unit-side `#ifdef AXL_HAVE_TLS` skip branches **and their SKIP balancers** | gone together, keeping the counts equal as CLAUDE.md requires |

## 7. What must not regress

`test-tls-strippable.sh` becomes the load-bearing guarantee, so it is
**strengthened before anything is removed** (§8, T1): in addition to the
plain-HTTP consumer it already checks, it must assert that a plain `hello.efi`
carries **zero** mbedtls symbols. That is the exact invariant the whole change
rests on, and it is currently proven only for one consumer shape.

Per CLAUDE.md, the strengthened assertion gets a sabotage proving it can fail
before its silence is trusted.

## 8. Phasing

Four steps, each green before the next.

| phase | deliverable | done when |
|---|---|---|
| **T1** | strengthen `test-tls-strippable.sh` | it asserts zero mbedtls symbols in a plain `hello.efi`, and a sabotage proves the assertion can fail — **before** anything is removed |
| **T2** | drop the flag from code and build system | 33 guards gone, `PREFIX` back to two inputs, wipe deleted, submodule precondition added with a readable error |
| **T3** | collapse the test and CI workarounds | four wrapper tests simplified, one ratchet baseline, CI ordering constraints removed, balancers retired with their branches |
| **T4** | measure and record | §10's numbers, including a clean-build re-confirmation that produced images are unchanged |

**T1 precedes T2 deliberately**: the gate that makes this safe must exist, and
be shown capable of failing, before the thing it guards is removed. Doing it in
the other order means the removal is verified by a test written after the fact
to agree with it.

## 9. Consequences for other work

- **The Ed25519 spec's §3 is revised** once this lands: SHA-512 becomes a thin
  wrapper over mbedTLS's rather than a fourth `AxlDigest` implementation, its
  E1 phase shrinks accordingly, and the aa64 `FEAT_SHA512` kernel installs
  through `MBEDTLS_SHA512_PROCESS_ALT` where it accelerates TLS as well.
- **AxlSsh is unaffected in shape** but benefits: it needs X25519 and AES-GCM
  from mbedTLS regardless, so it was always going to link TLS.

## 10. Measurements — TAKEN 2026-08-21

Three clean builds from an empty tree, x64, 8 cores, same machine: the
pre-change revision both ways, and HEAD.

| | pre, non-TLS | pre, `AXL_TLS=1` | **post (HEAD)** |
|---|---:|---:|---:|
| clean build, `-j8` | 2.7 s | 3.2 s | **3.7 s** |
| objects compiled | 277 | 328 | **328** |
| `libaxl.a` | 17,114,632 B / 266 | 20,396,374 B / 317 | **20,353,390 B / 317** |
| `hello.efi` | 47,616 B | 47,616 B | **47,616 B** |

**`hello.efi` is BYTE-IDENTICAL across all three**, same sha256
(`c7988646e5d54dbb…`) — which is the strongest form of the claim this change
rests on, and stronger than §2's original evidence. It is not merely that the
flag made no difference; removing it made no difference either.

**The honest cost, to someone who was building without TLS:** +1.0 s on a clean
build (2.7 -> 3.7) and **+3.24 MB** of `libaxl.a` (17.11 -> 20.35 MB). The
archive is unstripped; produced images are the row above.

**Post is 43 KB SMALLER than the pre-change TLS build** (20,396,374 ->
20,353,390) at the same member count, from deleting 232 lines of stub branches
and dropping `-Wno-redundant-decls`.

**Suite assertion count: 10,591 -> 10,752 (+161), one baseline**, both arches.
That is the TLS-gated assertions now running unconditionally, and it lands on
the old TLS number as predicted — a drop would have meant a skip branch removed
without its balancer.

## 11. Non-goals

- **Not vendoring mbedTLS in-tree** (§5).
- **Not rewriting historical documents** (§4).
- **Not removing any mbedTLS module** from `MBEDTLS_SOURCES`. Trimming unused
  modules is a separate, measurable question; conflating it with this change
  would make "the images are identical" untestable.
- **Not touching `axl_tls_*` public API.** This is a build-configuration
  change; no consumer's source changes.
- **Not adding a licence-staging parity gate.** Warranted (the two package
  paths drifted twice), but unrelated to this change and tracked separately.

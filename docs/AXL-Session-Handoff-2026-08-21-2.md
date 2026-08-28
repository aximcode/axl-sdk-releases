# Handoff — 2026-08-21 (session 2): a default made safe, SSH started, a flag deleted

> Self-contained. Everything below was measured or checked in-session; where a
> claim is inherited rather than verified, it says so.

## 0. START HERE — state, and what is actually open

**Tree:** HEAD `57b47805`, VERSION **4.3.1**, on `main`, tracked files clean,
**15 commits UNPUSHED**. `verify.sh` **ALL GREEN**; integration **173/0 X64**
and **73/0 AARCH64** uncached; unit **10752** both arches (one baseline).

Untracked and NOT mine: `SCRATCH.txt` and six older `docs/AXL-*.md` — they were
there before this session and are Mike's.

**Nothing is half-finished.** Three tracks completed; one is blocked on a
dependency that now has its own spec.

| open | where | note |
|---|---|---|
| **Revise the Ed25519 spec §3, then implement it** | `docs/superpowers/specs/2026-08-21-ed25519-design.md` | §3 chose to write SHA-512 into AxlDigest *specifically to avoid the TLS toggle*. **That toggle is now gone**, so §3 should become "wrap mbedTLS's SHA-512" and E1 shrinks. Do this BEFORE implementing |
| **AxlSsh P1 Task 6** | `docs/superpowers/plans/2026-08-21-axl-ssh-p1-transport.md` | Tasks 1-5 done and green. **Task 6 is BLOCKED on Ed25519** (§3 below) |
| Licence-staging parity gate | — | The two package paths drifted **twice** (DejaVu, then NOTICE). `check-flag-parity` is the precedent. Deliberately not built |
| Keep shipping the EDK2 driver binaries? | `third_party/edk2/` | A product call, not technical. Mike asked; unanswered |
| CMake port | ROADMAP | PROPOSED, not started (unchanged) |

**The dependency chain that emerged, in order:**
`AXL_TLS removal (DONE)` → `revise Ed25519 spec` → `implement Ed25519` →
`AxlSsh Task 6` → P1 complete.

---

## 1. Track (b): the shell launcher is ON by default and heals itself

Commit `f57dcba0`. The previous handoff listed this as blocked: *"needs the
firmware that hung, which no longer exists here."*

**That framing was the blocker, not the firmware.** It assumed safety meant
*fixing the fault*, which needs a reproducer. It doesn't — it means the default
**checking its own work**. The oracle already existed and was already emitted on
every affected run: the staged `startup.nsh` echoes `___AXL_APP_OUTPUT_BEGIN___`
**before** the app line, so a missing sentinel can only mean the Shell never ran
`startup.nsh`.

`run-qemu.sh` now records a negative memo against (arch, firmware, launcher) and
re-runs itself without the launcher. **Only failures are recorded** — working
firmware needs no entry and pays nothing, and a stale or unwritable memo can
only cost speed, never correctness. Borrowed from glibc's syscall fallbacks.

**Two premises in ROADMAP were wrong, both measured:**

- *"the risk is a consumer's… `run-qemu.sh` ships to them in host-tools"* —
  host-tools ships **scripts only**: no `axl-shell-launcher.efi`, no Makefile,
  no `libaxl.a`. `find_shell_launcher` returns 1 there (proven with a control).
  Packaged consumers never had the launcher.
- **The failure space has two shapes and only one needed a retry.** A launcher
  that *returns* is already self-healed by firmware — BdsDxe falls through to
  `Boot0002 "EFI Internal Shell"`, which runs `startup.nsh` anyway (measured:
  exit 0, sentinel present). Only a *hang* strands the guest.

**The fault is now reproducible without the firmware**, which is what actually
unblocked it: `AXL_SHELL_LAUNCHER_BIN` stages any PE as the launcher, and
`kbprobe` (blocks forever in `axl_console_read_key(UINT64_MAX)`) reproduces
"loads BOOTX64.EFI, never reaches the Shell" exactly.

`AXL_SHELL_LAUNCHER=1` still means "always, ignore the memo, no fallback" — that
is why `run-integration.sh` and `test-shell-launcher-qemu.sh` keep it.
`stage_boot_shell` takes a policy argument defaulting to `off`, so only
`run-qemu.sh` (which owns the serial log) opts into `auto`.

---

## 2. AxlSsh P1: Tasks 1-5 done, 94 assertions

`2d2cea7a` codec · `0e978806` version exchange · `3e58042c` packet protocol ·
`6c2427a6` KEXINIT · `4c005264` KDF.

**The plan was materially wrong in places and needed correcting as I went.** A
census over every `axl_*`/`AXL_*` token in it found **12 symbols that do not
exist**:

| plan said | tree has |
|---|---|
| `AxlStrBuf`, `axl_strbuf_*` | `AxlString`, `axl_string_new/append_len/len/data/free` |
| `axl_digest_*`, `AXL_DIGEST_SHA256` | `axl_checksum_*`, `AXL_CHECKSUM_SHA256` |
| `AXL_INCOMPLETE` | did not exist — **added to `AxlStatus` as -11** (Mike's call) |

It also named **two** registration points for a new test binary; there are
**three**. `BUILD_TEST` only defines the rule — without adding the name to
`TESTS`, nothing depends on the `.efi`, so it is never built and the test
silently never runs. No gate catches that.

**Defects fixed while implementing, each with a test that fails without it:**

- `axl_ssh_get_string` NULL-guarded the *read* of `off` then wrote through it
  unconditionally.
- `axl_ssh_get_u32` evaluated `len - *off` (size_t underflow) before testing
  `*off > len` — correct only by `||` short-circuit.
- **The ident parser treated an unterminated over-long line as "send more",
  forever.** The 255-byte cap only applied after a CRLF was found. Also nothing
  bounded *total* preamble.
- KEXINIT **never checked compression**, so a peer offering only `zlib` was
  accepted; and it validated only the four slots it read, so a message that
  simply stopped there was "well formed".
- **The KDF test was not a known-answer test.** It asserted determinism — which
  a *wrong* derivation satisfies perfectly. Vectors are now computed
  independently (python hashlib) and pinned. And **two blocks cannot test the
  chaining**: at block two, "the whole output so far" and "the previous block"
  are the same 32 bytes. A 96-byte vector with a pinned K3 separates them —
  sabotage caught that the 64-byte one did not.

**Sabotage found two of the plan's own bounds were untested**: adding the RFC
rules made its hostile vectors get rejected *earlier*, so disabling the size cap
changed nothing observable. The vectors are now chosen so exactly one rule can
reject each.

`axl_ssh_kdf`'s contract states that **`k` must already be mpint-encoded** —
the RFC hashes K as an mpint, and passing a raw X25519 secret would produce
self-consistent keys that reject every real client.

---

## 3. THE BLOCKER: Ed25519, and why mbedTLS cannot provide it

**`axl-crypto.h:86` says `AXL_PK_ED25519` "requires PSA crypto, which AXL does
not enable". That is wrong, and it sent this session down a false path.**

Verified against `deps/mbedtls` at 3.6.3:

| checked | result |
|---|---|
| `PSA_ALG_ED25519PH` | present in `include/psa/crypto_values.h` as a **constant only** |
| any `library/*.c` implementing EdDSA | **none** — sole hit is `ssl_debug_helpers_generated.c`, a debug string table |
| `MBEDTLS_ECP_TYPE_*` | `SHORT_WEIERSTRASS` and `MONTGOMERY` only — **no twisted Edwards** |

`MBEDTLS_ECP_DP_CURVE25519_ENABLED` is the **Montgomery** form for X25519 ECDH.
Ed25519 needs the Edwards form of the same curve. Enabling PSA gets nothing.

**So it is a vendoring project.** Spec:
`docs/superpowers/specs/2026-08-21-ed25519-design.md`. Decisions already made
with Mike: general `AxlCrypto` capability; **ref10** vendored in-tree; opt-in at
link time via a weak symbol + `-u` (the ~30 KB base table must not land in every
resident driver, and `--gc-sections` cannot remove it because `axl_pk_key_new`
is an `if/else-if` dispatcher every consumer calls); a **two-axis seam**
(provider × backend) so Ed448 / Ed25519ph / ctx / X25519 are later additions;
and **both** accelerated kernels in v1 (aa64 `FEAT_SHA512`, x64 ADX/BMI2 via
fiat-crypto's formally verified C).

**§6a of that spec records a real gate hole**: `check-no-avx.py` already handles
BMI2 (`mulx` et al are in `VEX_GPR_MNEMONICS`), but **`ADCX`/`ADOX` are
legacy-encoded, not VEX, so the gate cannot see them at all**. Extending it is
in scope, not deferred.

**Also wrong in the AxlSsh spec §6**, and recorded: OpenSSH's
`chacha20-poly1305@openssh.com` is a **different construction** from
`AXL_AEAD_CHACHA20_POLY1305` (raw ChaCha20, two-key split, separately encrypted
length). `aes256-gcm@openssh.com` — its own documented fallback — is supported
and is an OpenSSH 9.9p1 default.

**§3 of the Ed25519 spec now needs revising** before implementation: it chose to
implement SHA-512 inside AxlDigest *to avoid depending on the TLS toggle*. That
toggle no longer exists.

---

## 4. `AXL_TLS` is gone — mbedTLS compiles unconditionally

Spec (status IMPLEMENTED, §10 has measurements):
`docs/superpowers/specs/2026-08-21-axl-tls-flag-removal-design.md`.
Commits `772eb641` (T1) · `efba3a29` (T2) · `7102e24e` (T3) · `7cf32198` (T4) ·
`57b47805` (doc sync).

**The flag never did what it existed for.** Three clean builds from an empty
tree:

| | pre non-TLS | pre `AXL_TLS=1` | post |
|---|---:|---:|---:|
| clean build `-j8` | 2.7 s | 3.2 s | 3.7 s |
| `libaxl.a` | 17,114,632 / 266 | 20,396,374 / 317 | 20,353,390 / 317 |
| `hello.efi` | 47,616 | 47,616 | 47,616 |

**`hello.efi` is byte-identical by sha256 across all three.** Not merely that
the flag made no difference — removing it made none either.

What it cost is on the record: `PREFIX` grew a third input (and 98 hand-composed
paths across 66 scripts silently pointed at a tree nothing had built when the
suffix appeared); toggling **wiped every object**, which is why the suite forced
one value and CI carried "ORDER MATTERS" comments; two ratchet baselines drifted
(the `.tls` one was **104 assertions stale**); four integration tests existed
wholly or partly as rebuild workarounds. **+161 unit assertions now run by
default.**

**Things to know about the new state:**

- `PREFIX` is `(ARCH, BUILD)` — no `-tls`. `.last-pass-count.tls` retired; ONE
  baseline at **10752**.
- **The object wipe STAYS.** My own spec said it was "gone"; that was wrong and
  is corrected there — it signs `CFLAGS`/`CXXFLAGS`/`INCLUDES` and `CC`/`CXX`
  too, and deleting it would drop the guard those exist for. `TLS_STATE` →
  `BUILD_STATE`.
- **`AXL_HAVE_TLS` is still defined, always** — it reaches public headers, so
  undefining it would silently disable a *consumer's* own TLS branch.
- **mbedTLS is a REQUIRED submodule**; the Makefile fails with the fix-it
  command, and `make clean` still works without it (verified).
- Two assertions were **inverted rather than deleted**
  (`test-install-concurrent.sh`, `test-test-cache.sh`): a stray `AXL_TLS` must
  not re-split the tree or bust the cache key.

**Three latent defects surfaced** — see §6.

---

## 5. Licence and attribution: four obligations were unmet

Commits `c8dde13b`, `0f9eccd8`. Found by auditing what `libaxl.a` actually links
against what the packages carry, and **verified by building the package and
inspecting it**.

- **`NOTICE` was never in the SDK package.** Apache-2.0 §4(d). The host-tools
  tarball shipped it; the `.deb`/`.rpm` did not — so the file stating the other
  obligations was the one missing.
- **libvterm** (MIT, statically linked as AxlVterm) had **no THIRD_PARTY.md
  entry and no licence in the packages**, and carries **eight local AXL
  patches**, none disclosed. MIT offers no public-domain election, so this was
  an obligation.
- **FreeType**: THIRD_PARTY.md claimed *"No source modifications were made"* —
  **false**. `ftgrays.c` carries an AXL fix for an upstream typo
  (`max_ex - min_ey` for `max_ex - min_ex`) that corrupts memory rasterizing
  rotated text. Its licence was absent too, despite the FTL's credit clause.
- **LZMA** had no entry, and its AXL-authored `errno.h` shim was undisclosed.
- The two package paths **had drifted**: `release.yml` staged DejaVu,
  `build-packages.sh` did not.

Separately (`0f9eccd8`): two docs claimed *"every vendored dependency is
permissive — … lexbor Apache"*. **`deps/lexbor` is not a dependency at all** —
an untracked local checkout from a throwaway spike, referenced by nothing. And
the claim was disprovable: we already ship GPL iPXE, correctly, as **mere
aggregation** (GPL-2.0 §3). The real argument against wolfSSH is **linking, not
shipping**, which is stronger.

**Answers to two questions Mike asked:**
- The EDK2 binaries **are** in release artifacts — in the **tools tarballs**
  (`drivers/<arch>/`), not the `.deb`/`.rpm`. There is no EDK2 *source* in the
  tree; the rule is intact. They are runtime fallbacks for firmware missing
  optional UEFI modules.
- SoftBMC's shape: `AXL_SERVICE_DRIVER` in "Model A" (two images sharing one
  descriptor; `driver_tick_ms = 50`), and for the console it uses the
  **take-over device** (`axl_console_device_install` / `_inject_key`), *not* the
  mirror — deliberately. **That is the path an SSH session channel should take
  in AxlSsh P3/P5.** `uefi-ssh/` is an empty directory, not prior art.

---

## 6. Traps this session paid for

- **Code behind a disabled `#ifdef` is UNGATED code.** Making TLS unconditional
  surfaced three latent defects at once, all reported as new: mbedTLS headers
  needed `-isystem` not `-I`; `-DMBEDTLS_CONFIG_FILE='<...>'` trips
  `bugprone-macro-parentheses` as a **command-line macro no header filter can
  suppress**; and `check-libc-overlap` found `time` STRONG in both `libaxl.a`
  and newlib (ours must win — mbedTLS uses it for **certificate validity**).
  `lint.sh` had been green for years *because it never compiled that code*.
- **"Empty is not an answer" — hit again.** `dpkg-deb` does not exist on this
  box; with `2>/dev/null` its "command not found" read as "the package contains
  no licences at all". I nearly reported a false alarm. Redone with `ar` and a
  checked exit status.
- **A piped exit code, twice.** `sabotage.sh … | tail -6` made a detected
  sabotage report "NOT detected"; and `clang-tidy … | tail` reported exit 0.
- **A grep that matched its own noise.** `grep -ic "error|warning"` on a build
  log returned 2 — matching `error.o`, mbedTLS's `error.c`.
- **`sabotage.sh` restores the SOURCE, not the artifacts.** An ad-hoc re-check
  after a sabotage read the *sabotaged* `hello.so`.
- **A spec can be wrong in ways only implementation reveals.** I corrected my
  own AXL_TLS spec twice mid-implementation (the wipe; `AXL_HAVE_TLS`).
- **My own recommendation reversed twice on lexbor.** I argued for vendoring
  mbedTLS in-tree because "lexbor is already 53 MB in-tree". It isn't tracked at
  all. Checking Mike's question is what exposed it.

## 7. Suggested first move next session

Revise `docs/superpowers/specs/2026-08-21-ed25519-design.md` §3 (SHA-512 wraps
mbedTLS's rather than duplicating it; E1 shrinks accordingly; the aa64
`FEAT_SHA512` kernel installs through `MBEDTLS_SHA512_PROCESS_ALT` where it
accelerates TLS too), get Mike's nod, then write the E1 plan.

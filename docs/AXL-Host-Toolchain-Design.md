# The host compiler builds x64 — `AXL_TOOLCHAIN=auto` and the freestanding link

> **Status: SHIPPED 2026-09-03.** Designed and built the same day. The
> feasibility spike is DONE and GREEN (§2); §4 down is what shipped.
>
> Owner of the facts about **which compiler builds a consumer's UEFI image,
> and how one is chosen**. `AXL-Distribution-Design.md` owns the `axl`
> manager and links here for the toolchain verbs; `AXL-Libc-Substrate-Design.md`
> owns what AXL implements itself and why, and this document is the one
> exception to its "newlib is on every link" rule — see §5.3.

---

## 1. The problem

A C-only consumer on x64 must install a **239 MB** bare-metal toolchain to
build anything, and the only thing that toolchain is genuinely required for is
**C++**. The 239 MB splits as 191 MB compiler+binutils / 21 MB C++ runtime /
3.4 MB newlib, so splitting the C++ runtime out saves nothing: the compiler is
the bulk.

The consumers this matters to, measured across every AXL consumer on the
development box (`#include <axl` over all four source roots):

| consumer | C files | C++ files | effect |
|---|---|---|---|
| `axl-utils` | all | 0 | the driver: builds on Ubuntu 24.04 WSL2 with the team's own gcc |
| `uefi-ipmitool` | 24 | 0 | benefits |
| `axl-webfs` | 13 | 0 | benefits |
| AGT | 1 | 185 | keeps the bare-metal toolchain, unaffected (§3) |

Three of four consumers are C-only and pay 239 MB for a C++ runtime they
never invoke.

## 2. What the spike proved

Reproduction, measurements and the three wrong answers reached first are in
`AXL-Session-Handoff-2026-09-03.md` §4. The load-bearing results:

- **Ubuntu 24.04's gcc 13.3 built and booted** `hello`, `cache-demo` and
  `http-server` in a container where `/opt` was EMPTY and `x86_64-elf-gcc`
  existed nowhere. Control in that same container: the default build failed
  with `x64 C compiler not found at /opt/x86_64-elf-gcc-14.3.0-axl3`.
- **`AXL_TOOLCHAIN=cross` needed no `axl-cc` change** to do it.
- **Host binutils emits `pei-x86-64`** on Ubuntu and AlmaLinux. Re-verified on
  the AlmaLinux development box: `objdump --info` lists `pei-x86-64` three
  times and `pei-aarch64-little` zero times.
- 0 AVX and 0 `%fs` references in the produced image, so
  `-mstack-protector-guard=global` holds under host gcc.
- Same image size as the cross build (46592 B for `cache-demo`).

**aa64 is not in scope and never will be.** ARM's toolchain is what supplies
`pei-aarch64-little`; the host's binutils does not have it. aa64 keeps the
bare-metal toolchain, always.

## 3. The decision: `auto`, not a default flip

x64 resolves the compiler by **what is installed**, rather than defaulting
unconditionally to either one.

An unconditional flip to the host compiler was considered and rejected: AGT is
185 C++ files, and a hard flip breaks its x64 build at the next upgrade until
someone runs an opt-in verb. `auto` gives the C-only consumer the entire
benefit — a fresh machine installs no toolchain at all — while a machine that
already has a toolchain behaves exactly as it does today.

**This is therefore not a breaking change** — no build that worked before
stops working. All four cases:

| consumer state | today | after |
|---|---|---|
| C-only, no toolchain | hard error | **builds** (new capability) |
| C-only, toolchain installed | builds with bare-metal | identical |
| C++, toolchain installed | builds | identical |
| C++, no toolchain | hard error | a better error naming the remedy (§6.3) |

Nobody's working build changes.

Row 1 still owes one caveat, and it is why an "Upgrading" entry exists after
all (`CHANGELOG.md`, Upgrading item 6). A machine in that row was never a
*working* build, but the hard error it got could be doing a job on purpose: a
relocated toolchain launched without `AXL_X64_GCC` exported, or a CI image
whose `/opt` was wiped, used to fail loudly with `x64 C compiler not found at
/opt/...`, and a machine-level "always bare-metal" policy could be relying on
exactly that failure as its enforcement. Under `auto` the same machine now
builds quietly with the host compiler instead — a loud failure became a quiet
success with a different compiler, not a breaking change by the table's own
definition, but a real change for that policy. The remedy is one line: set
`AXL_TOOLCHAIN=axl` to keep the old hard-fail-if-missing guarantee.

### 3.1 No persisted pin, and why

An earlier shape gave `axl` a `toolchain use host|axl` verb writing a
per-install-root pin file. Under `auto` that machinery does not earn its keep:
**the toolchain's presence IS the selection**, and presence is already managed
by an `axl` verb (`axl toolchain install`). A pin would add a new file format,
a new precedence rung, an interaction with `axl use` / `prune` / `uninstall`,
and a reason for `axl-cc` to learn the manager's install-root layout — to
express something two existing mechanisms already express.

The one case a pin would have bought is "always bare-metal, fail loudly if
absent" as a machine-level reproducibility guard. That is covered by making
**explicit `AXL_TOOLCHAIN=axl` a hard failure** when the toolchain is missing
(§4.1) — CI sets it in its environment, which is where a reproducibility
constraint belongs.

What `auto` does require is that presence be reversible **both ways** by the
manager, so §6.2 adds `axl toolchain uninstall <arch>`. Spending the effort
there instead of on a pin is the trade.

## 4. Resolution

`AXL_TOOLCHAIN` gains two spellings. It stays an environment variable and
stays the same variable the Makefile takes — a variant one build path
understands and another ignores is the drift `make check-flag-parity` exists
to catch.

| value | meaning |
|---|---|
| `auto` | **x64 default.** Bare-metal x64 gcc present → `axl`; absent → `host`. |
| `axl` | Force bare-metal. **Hard-fails when absent** — the reproducibility guard. Remains the aa64 default. |
| `host` | Force the host compiler. **Refused by name on aa64** (§4.2). |
| `cross` | Unchanged: the caller supplies locators, conf defaults not consulted. |

### 4.1 `auto` probes the compiler, never the directory

Presence is `[[ -n "$GCC_BIN" ]] && { [[ -x "$GCC_BIN" ]] || command -v
"$GCC_BIN" >/dev/null; }` — the same rule `axl-cc`'s own compiler validation
applies, and for the reason `axl toolchain list` documents: a partial extract
leaves the directory in place and nothing in it, which is precisely the state
worth distinguishing.

The `command -v` half is not decoration. A locator may hold a BARE NAME on
`PATH` (`AXL_X64_GCC=x86_64-elf-gcc`) rather than an absolute path; `-x` alone
fails on that and would route a consumer with a perfectly good toolchain to the
host compiler **silently**, which is the one outcome `auto` must never produce
without saying so.

`auto` falling back to `host` is **silent by design** on a successful build;
`axl-cc --verbose` and `axl toolchain list` (§6.1) are where the resolved
variant is reported. What must never be silent is the *guard*: an explicit
`AXL_TOOLCHAIN=axl` with no toolchain installed keeps today's
"compiler not found at `<path>`" failure verbatim. Without that, `auto` could
mask a genuinely broken bare-metal install by quietly building with host gcc,
and §7.4 asserts it does not.

**`AXL_X64_GCC` is honoured under explicit `host` and discarded under the
`auto → host` fall-back — deliberately, not an oversight.** Under explicit
`AXL_TOOLCHAIN=host`, `GCC_BIN` still resolves as
`${AXL_X64_GCC:-${AXL_X64_GCC_DEFAULT:-}}`, so a caller who names a compiler
gets exactly that one. Under `auto`, `AXL_X64_GCC` is what the fall-back is
**reacting to**: it is the locator `auto` just proved absent or unusable (§4.1
above), so re-consulting it here would hand the host path the exact compiler
it could not find — a silent no-op fall-back rather than a working one. The
fall-back therefore resolves `GCC_BIN` fresh, from `AXL_X64_HOST_GCC` or
`command -v gcc`, and ignores `AXL_X64_GCC` entirely for that one build. A
caller who wants their own compiler under the fall-back path names it via
`AXL_X64_HOST_GCC`, which is the host variant's own override and is honoured
identically whether `host` was explicit or resolved.

**`AXL_X64_BINUTILS_PREFIX` (`CROSS` in the code) is discarded the same way,
and unlike `AXL_X64_GCC` it has no fall-back-side override to redirect to.**
Explicit `AXL_TOOLCHAIN=host` still resolves it as
`${AXL_X64_BINUTILS_PREFIX:-${AXL_X64_BINUTILS_PREFIX_DEFAULT:-}}`, honouring
an export exactly as the C compiler locator does. The `auto → host` fall-back
instead forces `CROSS=""` unconditionally — unprefixed host `ld`/`objcopy`/`ar`
— the same way it forces `GXX_BIN=""` (C++ stays refused either way), and
there is no `AXL_X64_HOST_BINUTILS_PREFIX` twin for a caller to redirect it to
instead. So all three locators — `AXL_X64_GCC`, `AXL_X64_GXX` and
`AXL_X64_BINUTILS_PREFIX` — are discarded under the fall-back, not reproduced:
the resolved `GCC_BIN`/`GXX_BIN`/`CROSS` triple matches what explicit `host`
produces only when none of the three is exported. Export any of them and
explicit `host` honours it while the fall-back does not.

### 4.2 aa64 rejects `host`

`AXL_TOOLCHAIN=host` on aa64 fails by name, saying that the host's binutils
does not emit `pei-aarch64-little` and that ARM's toolchain supplies it. A
reader who has seen x64 work will otherwise assume `host` works for both; the
error is where that assumption is cheapest to correct.

## 5. What `host` mode compiles and links against

### 5.1 Host mode is strictly *freestanding*

`gcc -print-file-name=include` ships most of the C standard's **freestanding**
header set — `stddef.h`, `stdarg.h`, `stdint.h`, `stdbool.h`, `float.h`,
`iso646.h`, `stdalign.h`, `stdatomic.h`, `stdnoreturn.h` — and no `string.h`,
`stdio.h` or `stdlib.h`.

**`limits.h` is the exception, and it is NOT available.** Measured, not
assumed: gcc's own `limits.h` reaches the platform's through `#include_next`
(via `syslimits.h`), and `-nostdinc` removes the directory that would answer
it. So the usable set is nine headers, not ten. Nothing in this tree includes
`limits.h`, which is why no build breaks — but the diagnostic in §6.3 must name
it alongside the hosted headers, because a consumer who reaches for it gets the
same confusing "No such file or directory" for a header the C standard calls
freestanding.

**So under `host`, the C standard library is absent, not shimmed.** That is
the single most important fact about this mode and every diagnostic should
reflect it. AXL's own API is unaffected: `libaxl.a` calls `axl_memcpy`, not
`memcpy`.

The blast radius on this tree is **zero**: every hosted-libc include under
`sdk/examples/` and `tools/` is in `fwtool.c`, behind `#ifdef AXL_HOSTED` —
the host-side build of that tool, not the UEFI one. Across every external
consumer, exactly one file (`agt/tools/axedit-sample.c`) includes a hosted
libc header, and AGT keeps the bare-metal toolchain regardless.

Compile flags under `host` are those of `axl` plus:

```
-nostdinc -isystem $(HOST_GCC -print-file-name=include)
```

Nothing else changes — freestanding, `-fno-builtin`, `-mno-red-zone`,
`-fshort-wchar`, `-fPIC`, and the stack protector with `guard=global` all
apply identically, which is what the spike's 0-AVX / 0-`%fs` result confirms.

### 5.2 The hermeticity exemption is REPLACED, not extended

`axl-cc`'s host-header check exempts `$_herm_tc_root`, the compiler's
grandparent (`<root>/bin/<cc>`), so that a toolchain living under `/usr/local`
is not reported as a host reach.

For `/usr/bin/gcc` that grandparent is **`/usr`**. Carrying the existing rule
into host mode would whitelist the entire host tree and silently disable the
check — the exact failure its own comment warns about for `$SDK_DIR`, and
worse than the false positive it was written to fix.

Under `host`, the exemption is therefore **exactly** the one directory
`$(HOST_GCC -print-file-name=include)`. Those headers are the *compiler's*
and ABI-neutral; a libc's are not. `--allow-host-paths` is far too blunt to
serve here — it also permits real `/usr/include`, which is the thing this mode
must keep refusing.

### 5.3 The link: no libc, plus a weak leaf archive

The `_liblist` lookup drops from `(libc.a libm.a libgcc.a)` to **`(libgcc.a)`**.
The hard error when `-print-file-name` cannot find a C library is correct
under `axl` and wrong under `host`, where there is deliberately none. Removing
that requirement also deletes the spike's `-B`/fake-`libc.a` scaffolding
outright: nothing needs to impersonate a C library any more.

`libaxl.a`'s **vendored** code (libvterm, lzma, mbedTLS) calls the plain C leaf
names, and `--gc-sections` drops those references only when the vendored code
is unused. `http-server` is the shipped source that exposes this:
without a provider it fails with `undefined reference to memcmp memcpy memmove
memset strchr strlen`.

The provider is **`lib/axl/<arch>/libaxl-standin.a`**: `src/data/axl-str-compat.c`
and `src/mem/axl-intrinsics.c`, restored from `6ec731d3^`. 195 lines, 4,396
bytes compiled, eleven functions — `memcpy`, `memset`, `memmove`, `memcmp`,
`memchr`, `strlen`, `strcmp`, `strncmp`, `strchr`, `strstr`, `strncpy` — each
`__attribute__((weak))` and each forwarding to its `axl_*` equivalent.

Three properties make this the right answer rather than a newlib subset:

- **It is not a consumer-facing libc.** It completes the link for vendored
  code. A consumer calling `strlen()` still has no `<string.h>` to declare it
  (§5.1), and that is intended.
- **It is built by the SDK's own bare-metal toolchain and shipped prebuilt**,
  exactly as `libaxl.a` and the crt0 objects are. Same target ABI; the spike
  proved a host-gcc link consumes those prebuilt artifacts. No host
  compilation step, and the archive is identical on every consumer machine.
- **It is a separate archive**, linked only under `host`, inside the existing
  `--start-group`. The default build stays byte-identical and
  `check-libc-overlap`'s invariant is untouched (§7.1).

`6ec731d3` deleted these files with the rationale *"libc.a is on every link
now, so a second provider would only compete on scan order."* **That rationale
does not apply where newlib is absent** — under `host` there is no competitor.
The need never went away; newlib took the role, and this mode takes it back.

`axl-cxxrt-alloc.o` and `axl-cxxrt-stubs.o` are **not** linked under `host`.
They exist to serve newlib — `sbrk` for its dlmalloc, and the
`write`/`read`/`open`/`close`/`lseek`/`fstat` porting layer its reentrant
stdio calls back into. With no newlib nothing references them, and linking
them anyway would imply to the next reader that `host` mode has one.

## 6. The manager

### 6.1 `axl toolchain list` reports the resolved variant and its reason

It already probes each arch's compiler and prints the resolved root, so this
is an extra line per arch rather than new machinery. It must show:

- which variant is **active** and **why** — `auto → host: no bare-metal
  toolchain installed`, `auto → axl`, or `pinned by $AXL_TOOLCHAIN`;
- that **C++ is unavailable** under `host`, with the verb that fixes it.

A reader looking for "which compiler will build my code" has exactly one place
to look.

### 6.2 `axl toolchain uninstall <arch>` — new

`auto` makes the toolchain's presence load-bearing, so `axl` must be able to
remove it as well as install it. Today the only way back is `rm -rf` by hand.

It removes the root the manifest names for `<arch>`, and **only** when that
root carries a `.axl-receipt` with `AXL_RECEIPT_KIND=toolchain` and a matching
`AXL_RECEIPT_ARCH` — the §21 ownership guard, applied exactly as `axl prune`
applies it, with the same re-mark remedy (`axl-install-toolchain <arch>`) when
a root predates receipts. An unmarked root is refused, not removed: a
toolchain we did not install is outside the policy.

### 6.3 Diagnostics name the remedy, not just the refusal

- **C++ under `host`** refuses, naming why (`dbd7d296`: libsupc++'s exception
  globals and the stack canary are read through `%fs`, glibc's TLS block,
  which UEFI never sets up) and how to
  proceed: `axl toolchain install x64`, after which `auto` selects it with no
  further action.
- **A missing hosted header** is the failure a `host`-mode user is most likely
  to hit and the least likely to diagnose: `fatal error: string.h: No such
  file or directory` does not say "this mode has no C library". When
  compilation fails and the compiler's own stderr names a known hosted header,
  **append** — never replace — a note stating §5.1's fact and both ways out
  (use AXL's API, or install the bare-metal toolchain).

## 7. Verification

### 7.1 `check-libc-overlap` learns the new archive by its invariant

The gate checks names that both `libaxl.a` and newlib define where AXL's is
**strong**. `libaxl-standin.a` is a deliberate second provider that never
coexists with newlib, so the safe thing to check is not an exclusion but the
property that makes it safe: **every symbol it defines is weak.** An
exclusion would stop the gate seeing a regression; the assertion cannot.

### 7.2 Every example and every tool, under the variant

The spike's second wrong answer — "no libc is needed at all" — was reached by
building four simple sources, all of which pass because `--gc-sections` drops
the vendored code that references the plain names. The gate against repeating
it is coverage, not a comment: **build every `sdk/examples/` source and every
`tools/` source under `host`.** `http-server` is non-negotiable.

### 7.3 A booted image, and a matrix of containers over which compiler exists

- A new `test/integration/test-host-toolchain-qemu.sh` builds under `host` and
  boots the result on OVMF.
- Four arms of `test-consumer-install.sh`, under podman, matrixed on which
  compiler is actually on the machine:

  | arm | toolchain mounted | host gcc installed | proves |
  |---|---|---|---|
  | `debian(dash)` / `fedora(bash)` | yes | no | `axl` builds on a machine with nothing else |
  | `ubuntu(host gcc)` | no | yes | `auto -> host`, with the toolchain genuinely ABSENT — `/opt` is checked empty before and after install, because "unused" and "absent" are different claims and only a container proves the second |
  | `ubuntu(both)` | yes | yes | `auto -> axl`, with BOTH compilers present |

  `ubuntu(both)` closes a gap the other three cannot: none of them has both
  compilers to choose *wrong* between. If `auto`'s presence probe ever
  regressed to reporting "absent" unconditionally, `debian`/`fedora` would
  fail LOUDLY for lack of any compiler (a different, safe outcome), and
  `ubuntu(host gcc)` would keep resolving to `host` correctly, since there is
  genuinely nothing else there to resolve to — neither run would say
  anything is wrong. The one `auto -> axl` assertion that existed before
  `ubuntu(both)` (§7.4) runs on the development box, which is not a fresh
  install: a long-lived `stage/`, a populated PATH and an existing manager
  root are exactly what the container harness exists to rule out.
  `ubuntu(both)` is the only detector for the dangerous direction: probe
  says absent, host gcc is on PATH, the build SUCCEEDS anyway — silently,
  with the wrong compiler, no diagnostic.

  Both compilers also produce a byte-identical-size `hello.efi` (37376 bytes,
  measured), so existence, non-emptiness and PE32+ validity discriminate
  nothing between them. `ubuntu(both)` instead asserts *who* built the
  image: `axl-cc --verbose` echoes every compile/link/objcopy command with
  the invoked binary's full path, so the arm greps those lines for the
  mounted toolchain's absolute `x86_64-elf-*` paths. And it asserts the
  resolution on its *mechanism*, not its result: the verbose banner must
  read `axl (auto)`, not the bare word — a pinned `AXL_TOOLCHAIN=axl` would
  satisfy a bare match too, which is the same hole this arm exists to close.

### 7.4 Control assertions — prove each check can fail

Per the tree's standing rule that a detector's silence is worth nothing until
it has been shown it can fail:

- With the bare-metal toolchain hidden, **`AXL_TOOLCHAIN=axl` must fail by
  name.** This is what proves `auto` is not masking a broken bare-metal path.
- With the toolchain present, `auto` must resolve to `axl` — otherwise the
  probe is reporting absence unconditionally and every consumer silently moved
  to host gcc.
- `AXL_TOOLCHAIN=host` on aa64 must fail (§4.2).
- A `host` build of a source that includes `<string.h>` must fail *and* carry
  the §6.3 note.

Each control must not pollute what it proves — clear any state the control
creates before asserting on it.

### 7.5 Verification traps found while proving §7.1–§7.4

Building and testing this feature surfaced a run of distinct ways a check
reported success while testing nothing — each cost real debugging time before
it was understood, and none is exotic; each is a standard shell footgun (word
splitting, `set -e`/`pipefail` interaction, `$?` lifetime, tty detection, an
unbounded `sed`/`grep` range) or a standard filesystem-permissions fact
(sticky/writable-parent semantics). Three, about how `scripts/sabotage.sh`
itself is invoked, are recorded in that script's own header comment, since
that is where a future user of the tool will actually hit them. The rest are
recorded here, against this feature's own test suite:

- **An assertion pre-satisfied by the text it is meant to replace.** A test
  asserted the diagnostic contained `\bhost\b` — and passed before any of
  this feature's code existed, because the OLD "not a toolchain variant"
  error echoes the user's own typo back, and the typo under test happened to
  be `host`. A test whose expected string already appears in the failure it
  is meant to replace is green from the moment it is written. Fix: also
  assert the message is NOT the old typo-echo text.

- **`set -e` aborting the whole test file instead of failing one
  assertion.** A refused-variant case that legitimately exits non-zero,
  captured as `out=$(cmd)`, aborts the entire script under `set -e` rather
  than letting the harness record one failure — silently skipping every
  assertion after it. Wrap such captures `|| true` and check the status
  explicitly.

- **The same trap in a diagnostic path, not a test.** Capturing a host
  compiler's stderr through a `grep | sort | head` pipeline, under
  `set -e -o pipefail`, aborted the whole SCRIPT (not just one check)
  whenever the underlying compile failed for a reason other than the one
  being diagnosed — invisible in the output because the coincidental exit
  code looked plausible. Found with `bash -x`; fixed the same way, `|| true`
  on the pipeline.

- **A control that compares REDIRECTED output cannot see a property that
  only exists on a tty.** Capturing gcc's stderr to a file for a "no other
  compile flags changed" diff makes that stderr a non-tty, which silently
  turns OFF `-fdiagnostics-color=auto` for every C compile — colored
  diagnostics were lost globally, on every arch and variant, and a
  byte-for-byte diff of the already-redirected output could not see it,
  because both sides of the diff were blind to it in the same way. The
  property only exists when a real terminal is attached; a control checking
  for it has to attach one, not infer it from redirected bytes.

- **An unscoped `grep`/`sed` matching the WRONG section.** `grep -q 'builds
  with: axl'` against a two-arch report matched aa64's constant text even
  when x64 was the arch under test and had actually failed. A `sed -n
  '/^  x64/,$p'` block-extractor runs to end-of-file, which happens to
  include a trailing footer line able to independently satisfy the same
  assertion. Both read as "passing" while checking nothing about the arch or
  section actually named. Fix: bound the extraction on BOTH ends (the next
  blank line, not `$`), and prove the bound is load-bearing by constructing
  the exact fixture — the missing footer, the other arch's text — that would
  have made the old, unbounded form pass for the wrong reason.

- **A double-quoted empty variable matching nothing, silently.** A
  post-sabotage restore check compared a variable inside double quotes
  against an expected value; the variable was unset in the failure path
  being tested, so the comparison degenerated to matching an empty string
  and reported a valid restore regardless of whether one had actually
  happened. The restore itself was fine — the check meant to prove it was
  not proving anything.

- **`$?` clobbered by the command reporting it.** `cmd; echo "rc=$?"; exit
  $?` reads as "run cmd, print its status, then exit with it", but the
  second `$?` belongs to `echo`, not `cmd` — `echo` succeeded, so `exit $?`
  exits 0 regardless of what `cmd` did. Capture once: `cmd; rc=$?; echo
  "rc=$rc"; exit "$rc"`.

- **A stale `BUILD=RELEASE` tree shadowing a rebuilt `BUILD=DEBUG` one.** An
  earlier `install.sh --arch x64` run in the same working tree had already
  built and staged a `BUILD=RELEASE` `libaxl.a`. A later sabotage against the
  DEBUG tree produced a correctly-sabotaged debug archive, but `axl-cc`'s
  candidate-resolution ladder (staged prefix → `BUILD=RELEASE` tree →
  default build) found the untouched RELEASE archive FIRST and linked
  against it — a clean-looking pass over a tree the sabotage never touched.
  The fix generalizes past this feature: resolve the EXACT prefix a test
  will read from (`make -s ARCH=x64 BUILD=RELEASE print-prefix`) and delete
  anything stale there before trusting a rebuild.

- **A survival assertion satisfied by a PARTIAL delete.** `/opt` is
  `root:root 0755` on a normal Linux install, while the toolchain root
  `axl-install-toolchain` creates inside it is user-owned. A plain `rm -rf
  <root>` run as that user therefore deletes every file and subdirectory it
  owns, then fails to unlink the now-empty directory itself because the
  PARENT is not writable by that user — exiting non-zero with the directory
  entry still present. Both of the obvious safety checks ("the command
  reported failure" and "the directory still exists") stay TRUE while the
  compiler underneath is completely gone. The fix asserts the thing that
  actually matters — the compiler binary is still present AND executable —
  never that a directory entry survives, and adds a parent-writability check
  before attempting the removal at all (§6.2).

The pattern across all of these: a detector's silence is worth nothing until
something has been made to show it can fail, and a control that passed
without ever being shown a real defect is not evidence of anything.

## 8. Explicitly unchanged

- **The SDK's own build.** `libaxl.a` is still built and shipped by the
  bare-metal toolchain. Only what the **consumer** compiles with changes;
  the two are not the same thing and the spike depended on them differing.

  **But the Makefile and `install.sh` must still ACCEPT the spelling.** Once
  `auto` is the x64 default, a consumer will reasonably export
  `AXL_TOOLCHAIN=auto`, and a variant the driver treats as the default while
  the build system rejects outright is the three-paths-disagree failure
  `make check-flag-parity` exists for. So both map **`auto` → `axl`** (the
  SDK's own build stays bare-metal, honouring this section) and reject
  **`host`** by name, saying the SDK builds itself bare-metal and that `host`
  governs only what a consumer compiles.
- **aa64, entirely.**
- **`libaxl.a`'s bytes**, on every existing configuration.
- **`packaging/install.sh` stays POSIX `sh`.**

## 9. Out of scope

- C++ under the host compiler. The host compiler is glibc-targeted and
  `dbd7d296` records why that breaks under UEFI; making it work is a separate
  project, not a flag.
- A per-install-root pin file (§3.1).
- Any host-compiler support for aa64 (§2).

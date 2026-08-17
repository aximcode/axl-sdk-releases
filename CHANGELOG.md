# Changelog

All notable changes to the AXL SDK are documented here. This project
follows [Semantic Versioning](https://semver.org/).

## 4.1.0 — 2026-08-16
### Added

- **`axl::unique_handle<T>` — unique ownership of an AXL C handle, for C++.**
  `AXL_AUTOPTR` stays exactly as it is for C; what it cannot be is a class
  member, moved, or returned from a factory, so the long-lived cases fell back
  to a raw pointer and a hand-written destructor. This is the same ownership
  idea where the language can express all three.

  ```cpp
  #include <axl/axl-handle.hpp>
  #include <axl/axl-loop.h>          // AND the header declaring your type

  axl::unique_handle<AxlLoop> loop{axl_loop_new()};
  axl_loop_run(loop.get());          // the C API, unchanged
  ```

  **Include both headers.** `axl-handle.hpp` pulls in `<memory>` and
  `<axl/axl-macros.h>` and nothing else; the trait for a type is emitted where
  that *type* is declared. Making `unique_handle<T>` always resolve would mean
  dragging every subsystem header in, and a consumer who tried that shape
  measured its public-header include weight growing ~13x. Missing it is a
  compile error (`'AxlVterm' was not declared in this scope`), not a silent
  failure.

  **Opt-in per type, because owning some of them is a bug.** The deleter
  resolves through `axl::handle_traits<T>`, emitted by the existing
  `AXL_DEFINE_AUTOPTR_CLEANUP` — so ~61 types get a handle in the same line
  that gives C its cleanup attribute, and it cannot drift from the destroy
  function it names. Two types state their reason via `AXL_DEFINE_NO_HANDLE`
  instead of leaving a bare `incomplete type`: `AxlSurface` (a borrowed node in
  a tree `axl_compositor_free` destroys whole) and `AxlJsonReader` (a
  caller-owned value struct). Gated by `make check-handle-exclusions`.

  `sizeof(axl::unique_handle<T>) == sizeof(T *)`, the deleter is stateless, and
  the destroy call inlines to what a hand-written destructor would emit. A
  consumer migrating every `AXL_AUTOPTR` in its tree measured +628 B (+0.10%)
  on its largest image.

- **`AXL_TOOLCHAIN` selects which toolchain supplies the compiler and
  binutils.** `axl` (default, what `axl-install-toolchain` installs) or `cross`
  (one you supply, named by `AXL_<ARCH>_GCC` / `_GXX` / `_BINUTILS_PREFIX`).
  Under `cross` the defaults are not consulted, so a locator left unset is
  refused **by name** rather than silently falling back. Honoured by the
  Makefile, `axl-cc` and `install.sh` alike.

  A variant beside the locators is the convention everywhere this is solved —
  Zephyr's `ZEPHYR_TOOLCHAIN_VARIANT`, EDK2's `TOOL_CHAIN_TAG`, the kernel's
  `LLVM=1` — and AXL had only the locators. See **Removed** below for what
  that cost.

- **`scripts/sdk-prefix.sh` + `AXL_SDK_PREFIX`** answer "where is the staged
  SDK", as `scripts/build-prefix.sh` already answers "where is the build
  directory". Two things lived under `out/` and both were called "the prefix";
  they are now separately addressable. The default is unchanged (`out`), so
  nothing moves unless asked.

### Removed

- **The README's macOS / native-Windows build instructions.** They told you to
  install a Homebrew `x86_64-unknown-linux-gnu` cross and run
  `make CROSS=<prefix>-`. That could not work, for two independent reasons,
  both measured: `CROSS=` has selected **binutils only** since C moved to the
  bare-metal cross (the compiler comes from `AXL_<ARCH>_GCC`), and a
  glibc-targeted cross cannot build the tree at all now that `include/compat/`
  is gone — `deps/` needs genuine newlib headers and dies on the first
  `<stdlib.h>`.

  Building AXL needs a **bare-metal** cross (`x86_64-elf` /
  `aarch64-none-elf`), and those are published for Linux x86-64 hosts only.
  Use WSL on Windows; on macOS, a container or VM. If you have a bare-metal
  cross for your host, point `AXL_TOOLCHAIN=cross` at it.

### Fixed

- **`test-jose-cc-qemu.sh` could report a library defect when staging had
  failed.** It ran `install.sh … | tail -1`, and a pipeline's exit status is
  the last command's — so a failed install reported success, the test carried
  on against a stale staged prefix, and died ten lines later on an undefined
  reference. It now checks the status and says which of the two happened.

## 4.0.0 — 2026-08-15
### Breaking

- **`axl-c++ --hosted` and the CMake `HOSTED` keyword are removed.** Both now
  fail with a message naming the removal. C++ is compiled hosted
  unconditionally, so `std::vector`, `std::string`, `std::map` and
  `std::unordered_map` work with no flag at all — the flag only ever switched
  off a freestanding C++ mode that no longer exists, and removing it from a
  build produces byte-identical output.

  ```console
  # before
  $ axl-c++ --hosted containers.cpp -o app.efi
  # after
  $ axl-c++ containers.cpp -o app.efi
  ```

  ```cmake
  # before
  axl_add_app(myapp myapp.cpp HOSTED)
  # after
  axl_add_app(myapp myapp.cpp)
  ```

  C sources are unaffected and still compile `-ffreestanding`; a mixed C/C++
  image links exactly as before.

  **What the containers cost, measured** (`-Os`, both arches, every image
  booted rather than merely linked). A C++ image that uses no container is the
  same size as the C one to the byte, so the cost below is the containers and
  nothing else:

  | added to a hello image | x64 | aa64 |
  |---|---|---|
  | `<vector>` + `<algorithm>` | +1,782 | +1,962 |
  | `<string>`, real use (`+=` / `append` / `insert`) | +1,863 | +2,115 |
  | `<map>` keyed by `std::string` | +5,415 | +6,395 |
  | `<unordered_map>` | +5,871 | +5,261 |
  | **all four in one image** | **+14,245** | **+14,597** |

  The four together cost less than the sum because they share the string
  machinery. Reach for them.

- **`axl-cc` rejects host headers and host libraries.** A UEFI image cannot
  use them — they describe another libc, another ABI, and an OS that will not
  be there — and the failure mode is a struct that disagrees at runtime rather
  than a link error that names the cause.

  Two checks, because neither covers the other. The **flags** are inspected
  before anything compiles, which is the only point that can see
  `-Wl,-L/usr/lib` or a host `.a` handed in positionally. A separate **`-M`
  pass** after each compile reads the compiler's own record of every file it
  opened, which catches an absolute `#include "/usr/..."` naming no flag at
  all. `-M` and not `-MM`: `-MM` omits system headers by definition, so a leak
  through `-isystem /usr/include` is invisible to it — measured on one
  translation unit at 0 hits against 19. The pass is isolated rather than
  bolted onto the real compile, so it cannot perturb a consumer's own
  dependency flags; it costs ~14 ms against a ~27 ms compile.

  ```sh
  axl-cc --allow-host-paths myapp.c -o myapp.efi
  ```

  `--allow-host-paths` is the opt-out, off by default, for the same reason
  `--allow-uefi` is: when a consumer genuinely needs it, that intent belongs on
  the build line where a reviewer sees it. The SDK's own headers and the
  toolchain's are never affected, wherever they are installed.

- **`include/compat/` is gone, and C compiles with the bare-metal cross on both
  arches.** The seven hand-written libc shims existed only because x64
  compiled with the host's gcc and aa64 with a glibc-targeted Linux cross.
  Both arches now use a bare-metal cross whose newlib supplies the genuine
  headers, so the shims stand in front of nothing.

  AXL's own code never used them — across `src/` and `include/` it includes
  only `stddef`, `stdint`, `stdbool` and `stdarg` — so `compat/` was always
  there for third-party sources, and `deps/` gets the real headers now.
  Consumers who let `axl-cc` or the CMake package own the include path need no
  change, and `install.sh` deletes a `compat/` left behind by an older install
  into the same prefix. A build that named `<sdk>/include/compat` itself must
  drop it.

  There is **no host fallback anywhere**: a missing cross is an error naming
  the installer, in the Makefile and in `axl-cc` alike. Falling back would
  defeat the point and re-create the bug the build-state signature exists to
  catch, where a suite run silently measured the host toolchain.

- **`axl-cc --depfile` is removed.** Pass gcc's own `-MD -MP -MF <path>`
  instead; `axl-cc` forwards them like any other compile flag.

  It existed to post-process the dependency file so every path was absolute,
  because a *relative* source makes gcc emit compile-cwd-relative
  prerequisites that CMake's `DEPFILE` resolved against the wrong directory.
  Pass an absolute source — which the generated CMake package now does — and
  gcc's output is already absolute.

  **This also fixes a staleness bug.** `--depfile` used `-MMD` internally,
  which omits `-isystem` headers by definition; the SDK arrives that way, so
  it tracked no SDK header at all and editing one did not rebuild a
  consumer's object. `-MD` lists them.

### Added

- **Real `try` / `catch` under UEFI, on both arches.** `axl-c++ -fexceptions`
  now produces a working exceptions image: throw, catch by type, nested throw,
  rethrow preserving the exception object, and a global constructor that throws
  and catches **before `main`**.

  ```console
  $ axl-c++ -fexceptions app.cpp -o app.efi
  ```

  **There is no `--exceptions` flag, deliberately.** `-fexceptions` is a real
  gcc flag the caller already has to pass for landing pads to be emitted, so
  `axl-cc` detects that — on the command line, or via an input object
  referencing `__gxx_personality_v0`, so the staged `-c`-then-link flow works
  too. A second AXL-specific spelling would only be a way for the two to get
  out of step. Same shape as the existing `-frtti` detection, sharing its
  single `nm -u` pass.

  The flag selects a linker script that `KEEP`s `.eh_frame` and links the
  toolchain's `libstdc++` / `libsupc++` / `libc` / `libm` / `libgcc` in place
  of `libaxl-cxx.a` — that archive exists to supply what a firmware image
  otherwise lacks, and libstdc++ defines all of it properly, with real throws
  instead of halts. **A C image pays nothing**: the `KEEP` is what would cost
  it +16.8% for tables it can never use, so it lives in a separate script, and
  frame registration hangs off a weak reference that a pure-C link never
  resolves.

  Exceptions are **off by default and cost real bytes when on** — an image
  using all four containers goes 61 KB to 279 KB on x64 (69 KB to 274 KB on
  aa64). Roughly half of that is libstdc++'s verbose terminate handler
  dragging in the C++ demangler and newlib's `stdio`, not the unwinder, which
  is ~22 KB. Budget for it before switching a whole codebase over.

  Callbacks that AXL invokes from its own C frames must still not throw, and
  `AXL_CB_NOEXCEPT` (3.2.0) makes that a compile error rather than a
  convention — AXL's C frames carry no landing pads, so an exception unwinding
  through one runs no cleanup at all.

- **The x86_64-elf toolchain is a published release artifact.** A bare-metal
  x64 cross is now mandatory and no upstream ships one, which left every
  machine facing a ~40-minute source build. `axl-install-toolchain x64` is a
  55 MB download-and-verify, with the source build as fallback — the shape
  aa64 has always had. A checksum mismatch is fatal rather than a silent
  fallback to building, because it means the manifest and the published
  artifact disagree.

- **`axl-cc` refuses an image whose global constructors would never run.**
  AXL walks `.init_array` only, and GCC's `x86_64-*-elf` target defaults to
  emitting constructors into the legacy `.ctors` — so a toolchain built
  without `--enable-initfini-array` produces an image that links clean while
  every global constructor, including the 26 objects' worth inside libstdc++,
  silently does not run. One `nm` over the `.so` already produced, 6 ms per
  link; `make check-ctors` covers what the Makefile builds. Both fail closed,
  and the diagnostic names the toolchain rather than guessing at a cause.

### Changed

- **x64 C++ compiles with AXL's own `x86_64-elf-g++`**, not the host's. The
  SDK now takes no compiler, assembler or linker from the distro on either
  arch, and the `.deb`/`.rpm` depend only on `curl` and `xz-utils` (to fetch
  the toolchains). Install with `axl-install-toolchain all`.
- **A staged C++ build no longer needs a flag to link.** `axl-c++ -c a.cpp`
  followed by `axl-c++ a.o -o app.efi` previously failed on an undefined
  `operator delete`; the C++ runtime archive is now selected from the objects.
- **The generated CMake package calls `axl-cc` instead of reimplementing it.**
  `axl-config.cmake` carried its own compile line, `ld`, `objcopy` and
  `pe-set-debug` — about 200 lines mirroring the script, now 110 that delegate.
  Three build paths become two.

  This fixes two consumer-visible defects. `axl_add_app` produces a custom
  target, so `target_compile_options()` errored with "non-compilable target
  type" — the package had **no way to pass a compile flag at all**, which made
  `-fexceptions` not merely unwired but unreachable. And the previous
  implementation tracked no header dependencies (its `DEPENDS` named only the
  source), so editing an SDK or project header rebuilt nothing.

  ```cmake
  axl_add_app(myapp myapp.cpp OPTIONS -fexceptions -O2)
  ```

### Fixed

- **A packaged install could not compile the example its own post-install
  message recommends.** `axl-cc`'s host-path check grepped the `-M` output for
  `^/usr/` without excluding the translation unit itself, and
  `build-packages.sh` stages the examples to `/usr/share/doc/axl-sdk/examples/`
  — so a source that merely *lived* under `/usr` was reported as the host
  header it had supposedly opened, naming itself in the error. Every packaged
  install failed on the first command the package tells the user to run.
- **`time()` was declared in a shape no genuine `<time.h>` accepts.** It was
  defined as `time(long long *)`, matching only the retired `compat` shim;
  newlib declares `time_t time(time_t *)` with `time_t` as `long`, so any real
  header rejected it outright. The widths already agreed — only the spelling
  was wrong.
- **`sdefl` / `sinfl` referenced an `assert()` nothing resolves.** The 15
  assertions had been dead for the file's whole life because `compat`'s
  `assert.h` defined `assert` as `((void)0)`; against a real `<assert.h>` they
  become `__assert_func` references. `NDEBUG` keeps them dead, preserving
  today's behaviour exactly — switching assertions *on* in a shipped
  compression path is a separate decision, since a firing assert in firmware is
  a panic rather than a message.
- **Three Doxygen errors that failed the v3.2.0 Docs run.**

### Build

Contributor-facing; none of this changes a consumer's build.

- **An `AXL_TLS=1` build gets its own output tree** (`out/native-<arch>-tls`).
  `AXL_TLS` is in the build-state signature, so toggling it wipes the objects,
  both archives and every `.efi` under the prefix — and the toggle is constant
  in practice, since `test-axl.sh` builds TLS-off while `run-integration.sh`
  exports `AXL_TLS=1`. Sharing one prefix rebuilt ~300 objects on every
  alternation and made running the two concurrently corrupt both.

  **Ask `scripts/build-prefix.sh` for the path rather than composing it.** A
  hand-written `out/native-$arch` is the rule frozen at one input, and PREFIX
  is now a function of three. `common-test.sh` offers memoized
  `test_build_prefix` / `test_build_dir` wrappers; the script exists as a
  script because 51 of the callers source nothing at all.
- **`clang-tidy` skips translation units that cannot have changed**, keyed on
  the TU and every header in its `.d` list by content, plus a salt covering
  the tidy version, `.clang-tidy`, the check set and the whole compile
  database. `scripts/lint.sh` 71s to 32s warm. `LINT_NO_CACHE=1` forces a full
  run, and a TU with no dependency record is always linted — unknown means
  unsafe. Nothing is recorded unless the batch passed.
- **`scripts/build-docs.sh` builds HTML and man concurrently** and gives Sphinx
  the cores: 115s to 39s cold, 8s warm. `-W` still fails the build under `-j`,
  which was verified rather than assumed.
- **`cut-release.sh` refuses to date a `### Breaking` section into a non-major
  bump** (`scripts/check-release-semver.sh`, `--allow-breaking` to override).
  It reads only the section being dated, so it does not fire on every past
  release. This is the check that made the present release a major.

### Documentation

- **`axl_pci_get_class_code` does not precheck function presence — the header
  now says so.** It is the only one of the three standard-header accessors
  without that precheck: an absent function's config space reads all-ones, so
  it returns `AXL_OK` with `0xFFFFFF`, which looks like an answer. The
  sentinel is 24 bits and **not** `0xFFFFFFFF`, and `0xFFFFFF` is also a
  legitimate reading for a present but class-less function — so it means "no
  usable class", not "nothing there". Behaviour is unchanged and the asymmetry
  is pinned by test; making it consistent is an API change that wants
  coordinating with a consumer bump.

### Legal

- **The SDK now distributes GPL binaries**, so the corresponding source for
  all three toolchain components is attached to the same release, per GPLv3
  6(d). The recipe is in git, builds unmodified upstream sources with no
  patches, and the release notes carry a three-year written offer. **A
  consumer's own compiled output is unaffected** — that is the GCC Runtime
  Library Exception.

## 3.2.3 — 2026-08-14

### Changed

- **The library stops warning about what it already returns.** 213 of 358
  `axl_warning` calls under `src/` sat immediately before a `return AXL_ERR`
  / `NULL` / `-1` / `false` / `AXL_NOT_FOUND`, telling the caller a second
  time something the status already said — and asserting a severity only the
  caller has the context to judge. They are now `axl_debug`, still visible
  under `-v`. The census moves from 138/358/1/169 (error/warning/info/debug)
  to 138/145/1/382: warnings are now rare enough to mean something.

  Nothing was swept blind. A warning stays when the caller **cannot** see the
  event in the return value, and those were left alone: 53 in `void` helpers,
  ~67 where the block carries on so the eventual return does not describe the
  warned event (a skipped config field, a clamped worker count, a torn-down
  connection on a socket that keeps accepting), and the rest judged by hand.

  **Fifteen were restored on review**, in three groups.

  *The caller never finds out.* "The block returns a failure" is not the same
  as "someone learns of it": a status can be checked one frame up and
  discarded the frame above that. `axl_attempt_begin()`'s callers all drop its
  `bool` — including the idiom its own header documents — so a breadcrumb that
  failed to persist takes next-boot driver quarantine with it, silently.
  `webdav`'s `ensure_headers()` is checked by an `insert_header()` returning
  `void`, so a response ships successfully minus `DAV` / `Allow` /
  `Content-Range`. Same shape in `axl-console-device.c` (a refused pointer
  eviction) and `axl-9p-server.c` (a server bug that reaps a live connection
  while the listener keeps accepting).

  *The status is not merely unheard but wrong.* A 9P cross-directory rename
  that copied and then failed to unlink returns `AXL_ERR` — "the rename
  failed" — when the file now exists at **both** paths. And the descriptor
  builders in `axl-net-opts.c` / `axl-config.c` return a `size_t` count where
  `0` is equally the legitimate "nothing requested", so an under-sized array
  is indistinguishable from an empty one and a tool silently ships with no
  `--nic` / `--port` options.

  *The level was a documented promise.* `axl-cpu.h` and
  `axl-console-device.h` state in prose that these paths warn — the first so a
  consumer reads it as "monitoring unavailable on this firmware" rather than
  going silently un-monitored. A documented level is part of the public
  contract, so a checkable return does not make it a duplicate.

- **`make check-log-levels` now gates warnings too.** It flags an
  `axl_warning` whose block then returns a failure, and takes the existing
  `/* log-level: */` marker as the opt-out. Requiring a marker on every
  legitimate warning would have meant ~129 of them and taught authors to
  paste one unread; gating the provable duplicate does not.

  Seventeen calls carry the marker: the fifteen restored above, plus
  `axl-service.c` reporting a service that declined to start — the framework
  carries on, so the run continues with it simply absent and nothing returns
  "one was supposed to be here" — and `axl-http-ws.c` reporting an oversized
  frame **dropped** on a connection that stays up, which is silent data loss
  the peer cannot see. Tests assert some of these, but that follows from them
  mattering and is not the reason for the level.

  **The gate's own failure-sentinel list was wrong** and is fixed here: it
  counted a bare `return 0` and every `EFI_*` including `EFI_SUCCESS` as an
  error return, so it flagged warnings on **success** paths — the inverse of
  the rule, and the reason several of the restorations above had been demoted
  in the first place. `AXL_OK` is `0`, so a bare `0` is usually success and is
  no longer treated as a sentinel at all. The gate now errs toward missing a
  duplicate rather than toward flagging a success path: an unflagged duplicate
  is noise, a wrongly-flagged success path is lost signal.

## 3.2.2 — 2026-08-14

### Fixed

- **`axl_info` no longer announces success.** 3.2.1 demoted the eight sites a
  consumer had *observed*. It adopted that release, deleted its domain-pinning
  workaround, re-measured a healthy run at `AXL_LOG_INFO` — and still saw six
  lines:

  ```
  6x  [INFO]  ipmi: SMBIOS Type 38: KCS @ 0xca2/0xca3 (modifier=0x01)
  ```

  That is a *discovery success*. The list had been assembled from output rather
  than from a census, so it could only ever contain what someone had already
  watched scroll past. `src/ipmi/axl-ipmi.c` even contradicted itself inside one
  function, exactly as the backends had: `:331` was already `axl_debug` for the
  equivalent step ("Type 38 absent; trying default KCS") while `:244`, `:257`
  and `:275` were `axl_info`.

  This release censuses instead. All 42 `axl_info` calls under `src/` were
  classified; **41 are now `axl_debug`** — every "transport ready",
  "installed", "listening", "network ready", "loaded", "AP workers running",
  "no IP4Config2", "certificate loaded". A caller learns each of those from a
  non-NULL handle or an `AXL_OK`.

  **This matters more on real hardware than in emulation.** The consumer's QEMU
  gates only reach the KCS path; on a PowerEdge it is `ipmi-dell.c` or
  `ipmi-edkii.c` that fires, and on ARM64 Grace it is `ipmi-ssif.c` — the
  shipped SSIF path. None of those are reachable by any downstream test, so
  they are fixed here despite nobody having reported them.

- **One `axl_info` survives, deliberately.** `axl_mem_dump_leaks()` returns
  `void` and exists *to* report: the line is its return value, not commentary
  on an operation that already returned a status. The QEMU harness's leak gate
  greps for that exact string to prove a binary reported at all, so demoting it
  would not have quietened the tree — it would have blinded the gate. Verified
  by sabotage: demoting it fails the whole run.

### Added

- **`make check-log-levels`** — every `axl_info` under `src/` must carry a
  `/* log-level: <why> */` marker. The rule had been written down twice and
  broken twice; a keyword scan for `ready|installed|listening` would pass the
  next phrasing nobody thought of, so INFO is now a level you opt into and
  defend. Wired into `LINT_GATES`, so `verify.sh` runs it.

- **The rule itself, in `docs/AXL-Coding-Style.md`** ("Log Levels in Library
  Code"). The style guide said where to put `AXL_LOG_DOMAIN` and never said
  when to use which level, which is how one event acquired two verdicts. It
  records the discriminator — *can the caller observe this any other way?* —
  the `void`-helper exception that keeps four `axl-driver.c` warnings, and the
  distinction between reporting and announcing.

- **`run-qemu.sh --setvar NAME=VALUE`** — `set`s a shell variable before the
  app runs. Two integration suites asserted on library INFO lines as a proxy
  for "the operation happened" (`console device installed/uninstalled`,
  `reload reclaimed old image`), so quietening the library broke them. They now
  pass `--setvar AXL_LOG_LEVEL=debug` (or `set` it in their own startup script)
  and assert on the same lines at debug — the library is quiet by default and
  the run that wants detail asks for it, which is the right way round. No
  assertion was weakened to accommodate the change.

## 3.2.1 — 2026-08-14

### Fixed

- **The library announced at `warning`/`info` what it had already reported to
  its caller through the return value.** A consumer that moved its default
  level to `AXL_LOG_INFO` — under the ordinary rule that a real failure should
  be visible without `-v`, and nothing should be logged on the green path —
  saw nine AXL lines on a fully passing 209-assertion run. Its only recourse
  was to pin AXL's domains to `ERROR` from the outside, which a consumer
  should never have to do.

  Eight sites drop to `debug`. Only the caller knows whether a missing file is
  a fault or an expected probe, and the status it is already checking says the
  same thing the log line did:

  | Site | Was |
  |---|---|
  | `axl_file_get_contents` open failure (`src/fs/axl-fs.c`) | `warning` |
  | `axl_file_set_contents` open failure (`src/fs/axl-fs.c`) | `warning` |
  | `axl_fopen` open failure (`src/stream/axl-stream-file.c`) | `warning` |
  | `axl_driver_load` read failure (`src/util/axl-driver.c`) | `warning` |
  | "No IPMI transport available" (`src/ipmi/axl-ipmi.c`) | `warning` |
  | "IPMI KCS transport ready" (`src/ipmi/axl-ipmi-kcs.c`) | `info` |
  | "driver ensure: loaded …" (`src/util/axl-driver.c`) | `info` |
  | "loaded driver: …" (`src/util/axl-driver.c`) | `info` |

  The last three are success lines: a machine with no BMC is a configuration,
  not a fault, and a driver that loaded is the outcome the caller asked for.
  The two `driver` ones cost a line per driver on a healthy boot for any
  consumer at INFO that loads through those paths.

  The split verdict was already visible inside the library: `"open failed"`
  was `debug` in all three backends and `warning` in the two wrappers one
  layer above them, and `axl-driver.c` contradicted itself inside a single
  function — a `LoadImage` failure at `debug`, the read failure ten lines
  later at `warning`.

  **No API or ABI change**, and nothing is lost: every demoted line is still
  there at `-v`. Warnings that are a `void`-returning helper's *only* channel
  are deliberately untouched — four in `axl-driver.c`'s image-identity path
  stay at `warning`, because the caller has no return value to inspect.

## 3.2.0 — 2026-08-12

### Breaking

- **Every public callback typedef is now `noexcept` in C++.** 171 of them
  across the public headers carry `AXL_CB_NOEXCEPT`, which expands to
  `noexcept` in C++ and to nothing in C. **C consumers are unaffected.** A C++
  consumer passing a callback must add `noexcept` to its definition:

  ```cpp
  // before
  static void on_frame(void *user, uint64_t ms) { ... }
  // after
  static void on_frame(void *user, uint64_t ms) noexcept { ... }
  ```

  Inline lambdas need it too, and in practice that is the commoner idiom:

  ```cpp
  auto exit_cb = [](void *data) noexcept -> bool { ... };
  ```

  Since C++17 `noexcept` is part of a function's type, so a throwing callback
  no longer converts and the compiler reports it at the call site.

  **Scale, from migrating a real consumer:** the AGT toolkit (206 C++ files)
  needed `noexcept` on 45 files -- free functions, static member trampolines,
  two `AxlConsoleOps`/`AxlArgsNode` tables, and 18 inline lambdas. Mechanical,
  but not one-line: build with `make -k` to collect every site at once rather
  than one compile at a time.

  **Why:** AXL invokes consumer callbacks from its own C frames, and those
  frames carry no landing pads, so an exception unwinding through them runs
  **no cleanup at all** — measured, a leaked `RaiseTPL` climbed 0 -> 16, and
  returning to the shell above `TPL_APPLICATION` wedges the machine at every
  raised level, silently on x64. That is a hung box rather than a bad exit
  code, which is why this is a compile error instead of a convention.

  **If you want to throw**, catch at your own boundary and return a status —
  measured clean (TPL restored, error propagated, nothing unwound through a
  libaxl frame). This is the same trampoline pattern glibmm uses for GTK
  signal handlers, and for the same reason: GLib is likewise built without
  `-fexceptions` (verified: `libglib-2.0` has `.eh_frame` but no
  `.gcc_except_table`), so its `g_autoptr` cleanups would not run either.
  glibmm hides the trampoline in generated code; AXL has no generated wrapper
  layer, so the boundary is enforced by the type system instead.

  See `docs/AXL-Cxx-Design.md` §6b.

- **Applications can no longer include `<uefi/...>`.** The generated UEFI
  headers now refuse to compile unless `AXL_ALLOW_UEFI` is defined, which
  makes "the public API is EFI-free" a property of the build rather than of
  discipline. Previously `uefi/` shipped inside the SDK include directory and
  `axl-cc` put that on `-isystem`, so any application could reach the whole
  EDK2 surface by typing an `#include`.

  Nothing changes for drivers: `axl-cc --type driver` and `--type runtime`
  (CMake `axl_add_driver`) grant it automatically, because producing or
  interposing on a protocol means implementing that protocol's own types.

  An application that genuinely needs raw firmware access opts in explicitly:

  ```sh
  axl-cc --allow-uefi myapp.c -o myapp.efi
  ```
  ```cmake
  axl_add_app(myapp myapp.c ALLOW_UEFI)
  ```

  The intent is that the opt-in is visible on the build line instead of buried
  in a source file. Prefer an `axl_*` API; a gap worth an escape hatch is worth
  reporting.


- **`axl_json_parse` and `axl_json_load_file` take the dialect as a
  parameter**, and the `_flags` twins beside them are gone:

  ```c
  bool axl_json_parse(const char *json, size_t len,
                      AxlJsonFlags flags, AxlJsonReader *r);
  bool axl_json_load_file(const char *path, AxlJsonFlags flags,
                          AxlJsonReader *r, void **out_buf, size_t *out_len);
  ```

  The flags word sits before the out-param, matching
  `axl_json_parse_source()` and `axl_json_scanner_init()`.

  **Migrating is not "add a `0`".** `AXL_JSON_STRICT` *is* `0`, but the old
  no-flags entry points deliberately defaulted to **`AXL_JSON_RELAXED`** —
  so inserting a zero compiles clean and silently switches the call from
  the JSON5 superset to RFC 8259, changing which documents parse *and*
  which ill-formed bytes are repaired (RELAXED names `AXL_JSON_UTF8_RAW`;
  STRICT does not). Existing `axl_json_parse(doc, len, &r)` becomes
  `axl_json_parse(doc, len, AXL_JSON_RELAXED, &r)` to preserve behaviour
  exactly. An existing `axl_json_parse_flags(doc, len, f, &r)` is a pure
  rename.

  The arity change is the migration mechanism: every call site is a compile
  error until it states the dialect it had been getting.

- **Renamed**, with no behavioural change:
  `axl_json_indent_flags` → **`axl_json_indent`**,
  `axl_json_depth_flags` → **`axl_json_depth`**,
  `axl_json_type_of` → **`axl_json_get_type`**.

- **Removed `axl_json_root_array_begin`** — use
  `axl_json_value_array_begin(r, iter)`, which is the same call under a name
  that does not lie. Both read the reader's own value, and a sub-reader
  handed back by `axl_json_array_next()` has no "root", which is exactly
  where the old name got used.

- **Removed `axl_json_extract_string`** — use `axl_json_parse` +
  `axl_json_get_string`, which is what it did.

- **Removed the container-scoped writer overrides**
  `axl_json_obj_begin_flags` / `axl_json_arr_begin_flags`, and the
  `AXL_JSON_SCOPED_MASK` that described what they accepted.
  `axl_json_obj_begin(w)` / `axl_json_arr_begin(w)` are unchanged and still
  take one argument. There is no replacement and none is planned; see
  `docs/AXL-JSON-Design.md` decision 41 for why a merged
  `axl_json_obj_begin(w, flags)` cannot work (the override *replaced*
  per-value formatting, so `0` was a meaningful value and cannot also mean
  "no override").

- **`AxlJsonWriter` shrinks by ~2 KB** and its layout changes. The
  `saved_flags[AXL_JSON_WRITER_MAX_DEPTH]` array existed only to restore a
  scoped override, so it went with the feature. The struct is caller-placed,
  usually on the stack, and this is a firmware SDK — recompile consumers.

### Added

**C++ is a first-class surface, and the SDK links no `libstdc++.a`.**

- `axl::cout` / `axl::cin` / `axl::cerr`, and `axl::string` — standalone with
  SSO, not a skin over another string.
- **The standard containers run under UEFI.** `axl-c++ --hosted` makes
  `std::vector` / `string` / `map` / `unordered_map`, `std::list` and
  `std::shared_ptr` work on both arches. The advice that came out of measuring
  it: do not write containers, use these.
- `libaxl-cxx.a` supplies what the toolchain's libstdc++ needs from us —
  every `operator new` / `delete` form (nothrow and aligned included), the five
  `std::__throw_*`, `ceil`, and AXL's own `_Prime_rehash_policy`. Eleven
  functions were the whole gap.
- **Exceptions work on both arches**, with LLVM libunwind vendored and built
  AVX-free, and RTTI enabled. The answer turned out to be the toolchain:
  `scripts/install-toolchain` builds an `x86_64-elf` bare-metal compiler so x64
  matches aa64.
- `axl-c++` ships in the packages unconditionally — it is a dependency-free
  `exec axl-cc -x c++` wrapper, and without `libaxl-cxx.a` it names the install
  step that is missing rather than failing obscurely.

**JSON: one engine, four faces** (the redesign, P1–P15 — see
`docs/AXL-JSON-Design.md`).

- **One 64-bit `AxlJsonFlags` space** shared by reader and writer, with each
  JSON5 sub-flag honoured individually rather than as a bundle.
- **One parser for every dialect** — jsmn is gone, and a conformance gate keeps
  the single engine honest.
- **A pull scanner** (`axl_json_scanner_*`) over the same grammar, scanning
  from a source through a refill window, so a document larger than memory is a
  loop rather than a special case.
- **Source and sink**: eight I/O entry points built from two vtables.
- **Structured parse errors** — code and position, plus
  `axl_json_error_format()` to render them.
- Reader: `REJECT_DUPLICATES`; `UTF8_STRICT` / `UTF8_REPAIR` / `UTF8_RAW`;
  NaN/Infinity; a lossless way to read any number; `axl_json_get_double`;
  object iteration that makes a discovered key usable; the string decoder is
  public.
- Writer: `COMPACT`, `ESCAPE_SLASH`, `EMBED`, `SORT_KEYS` (ordered by the
  DECODED key), `ENSURE_ASCII` with surrogate pairs; the depth cap moves 32 →
  256; every writer path guarantees well-formed UTF-8 out.
- `axl_http_response_get_json()` — the strict-by-default response parse
  (network JSON is strict in both directions; decision 40).

**Streams take consumer-supplied backends.** `axl_stream_open_custom()` plus
the `axl_stream_ctx()` accessor complete the contract, and the built-in
backends are built through the same public constructor rather than beside it.

**Numbers and strings, correctly rounded.** `axl_str_to_double` /
`axl_str_to_float` (Clinger fast path with an exact decimal fallback),
`axl_double_to_str`, `axl_u64_to_str` / `axl_s64_to_str`, `%f` / `%e` / `%g` in
`axl_sscanf`, `axl_utf8_encode` to complete the codepoint pair, and the IEEE
nan/inf predicate surface in AxlMath.

**Elsewhere**

- AxlArray moves closer to GArray: `sized_new`, `steal`, element destructors.
- RTC wake alarm, and the raw firmware memory map.
- Unscoped UEFI variable inspection, and nvstore gets its walk back.
- A serial log sink, on one shared line builder; serial `open` is now
  EXCLUSIVE and a port answers for its own handle.
- Opacity inherits down the surface tree (gfx E11).

### Changed

- **AxlTcp queues outbound sends, and never refuses one for capacity.**
  `axl_tcp_send_async` used to reject a second send while one was in flight, so
  every caller had to invent its own serialisation — three did, and the four
  submit sites that instead read the refusal as fatal reset healthy connections
  under load. It now accepts the send and transmits it when its turn comes,
  FIFO. `AXL_BUSY` is gone from its return set: `AXL_OK` means accepted
  (submitted or queued, and its callback will fire exactly once), `AXL_ERR`
  means rejected outright (closed socket, bad argument, allocation failure) and
  no callback follows. A transport that refuses the bytes now reports that the
  same way a transport that drops them does — through the callback. Ported from
  EDK2's socket layer; see `docs/AXL-Tcp-Queue-Design.md`.

- **A send callback is deferred to the event loop, never called inline.** It
  does not run inside `axl_tcp_send_async`, nor inside another send's
  completion — it is queued for the loop's next iteration, which is how EDK2
  signals its own tokens (`gBS->SignalEvent`). So a callback may do whatever it
  likes to the socket, `axl_tcp_close` included, without tripping over a
  transport operation still in progress underneath it. Transmission is not
  delayed: the next queued send goes to the firmware immediately, and only the
  notification waits for the tick.

- **`axl_tcp_close` retires every pending send** — the one on the wire and
  every one queued behind it — firing each callback with `AXL_CANCELLED` before
  it returns, so an accepted send always gets exactly one callback and a
  borrowed buffer is always released. Closing an already-closed socket is a
  no-op, and sends or receives started on a closed socket are refused rather
  than armed against firmware state that is already released.

- **`axl_tcp_send` (the synchronous wrapper) declines to queue.** Behind
  another caller's send it returns `AXL_ERR` immediately instead of waiting its
  turn: a sync call's contract is "finished when I return", and it cannot
  honour that behind a send whose progress a different loop drives. Use
  `axl_tcp_send_async` when a socket has more than one writer.

- The blend path resolves its SIMD tier once per blend rather than once per
  scanline, and the SIMD dispatch check is memoised while the BSP is provably
  alone.

- `AxlHashTable` indexes by mask instead of a runtime modulo.

- The tree builds as `-std=gnu2x` / C++23 — the newest standards the cross
  compiler accepts (`gnu23` is the same language mode under a spelling gcc 13
  rejects).

- `axl_json_reader_error()`, `axl_json_source_init_mem()` and
  `axl_json_parse_source()` docstrings now reference `axl_json_parse()`
  where they referenced the removed twin. No behaviour change.

### Fixed

- **~1 request in 3 was dropped under concurrent TLS handshakes.** A server
  that had its own handshake flight still in flight refused the response send,
  and four submit sites in `axl-http-response.c` read that retryable status as
  fatal and reset a healthy connection. The client saw `curl` error 56.
  Measured at ~50% failure of a 24-connection gate; 0 of 6 runs after the fix.
- **A WebSocket frame's TLS ciphertext leaked on every teardown-mid-send.**
  `axl_tcp_close` cancelled the transport token without firing the callback
  that frees the encrypted copy, and this layer keeps exactly one frame in
  flight, so a TLS frame at teardown was always the one that leaked.
- `axl_file_delete()` CREATED the path it was asked to remove, when that path
  did not exist.
- **No C++ global constructor had ever run.** Nothing references an
  `.init_array` entry, so `--gc-sections` collected the section — silently, for
  as long as the C++ layer existed. The linker scripts `KEEP()` it now, and a
  ctor fixture in the suite fails if that regresses.
- A latent AArch64 relocation-table split (`DT_RELA` across two sections, which
  the crt0 walked past the end of); `-frtti` was the first workload to trigger
  it. Gated by `make check-reloc-coverage`.
- JSON string accessors did not decode `\uXXXX` — shipped corruption. Also
  fixed: JSON5 `\0` / `\x00` smuggling an interior NUL, `\xNN` treated as a raw
  byte rather than ES5's code unit, a `\<CR><LF>` line continuation counted as
  two terminators, truncation leaving half a UTF-8 sequence, and every
  non-ASCII byte dropped on platforms where `char` is signed.
- `axl_dtoa` read past `kPow10`; `axl_sscanf`'s field-width accumulator could
  overflow.
- Leaks: `axl_getenv` treated an owned string as borrowed, `AXL_LOG_LEVEL` was
  never freed, `axl_stream_init` published statics it did not reset, two
  wrapper sources were left open, and a JOSE test discarded an owned key.
- The RTC was re-entered from a notify function; an ordinary serial
  double-close is harmless now.
- Four AP worker-pool defects, against real MP Services semantics rather than
  assumed ones.
- `axl_tls` collapsed a retryable send refusal into a fatal handshake error.

- **`sdk/examples/memfs.c` did not compile.** It still named
  `AxlFsProviderInfo`, a type renamed to `AxlFsEntry` some time ago; the
  example was reachable by no build rule and no test, so nothing noticed.
  It also left `AxlFsEntry.alloc_size` uninitialised, which the fix closes
  by zeroing the whole versioned struct the way the in-tree providers do.
- **`make check-tautology` wiped the build tree.** It was missing from the
  Makefile's `NONCLEAN_GOALS`, so running it after an `AXL_TLS=1` build read
  `TLS_STATE=off`, saw a toggle, and deleted `$(BUILDDIR)/*.o` plus
  `libaxl.a`. In `verify.sh` that make job runs *concurrently* with both
  arch builds, so it deleted objects out from under them mid-build.

### Build

- **A teardown memory leak now fails the QEMU run.** `test_check_leaks` greps
  every test binary's serial log for the leak report and requires each binary
  to print a verdict at all — a gate that cannot see is worse than none. It
  found three library leaks on the day it landed.
- **`scripts/sabotage.sh`** — snapshot, apply, run, restore, and verify the
  file is byte-identical again. Restoring by hand had produced two wrong
  answers in this tree: a `sed -i.bak` restore gives the source the backup's
  older mtime so `make` skips the rebuild, and a sed that matches nothing
  leaves the suite green in a way that reads as "no test covers this".
- **New gates**, each catching something that had already happened:
  `check-dep-tracking` (`CXXFLAGS` lacked `-MD -MP` for the whole life of the
  C++ layer, so a header edit rebuilt nothing), `check-no-avx`,
  `check-flag-parity`, `check-reloc-coverage`, `check-bss-clear`,
  `check-cxx-entry`, `check-json-dialect`, `check-uefi-scope`, `check-nul`,
  `check-test-registered`.
- **The rebuild signature covers flags AND the compiler**, not just the
  `AXL_TLS` toggle. Editing `CFLAGS_BASE` used to rebuild nothing, which
  produced four wrong readings while the stack protector was added; naming a
  different `CXX` used to reuse host-g++ objects, so a suite run "with the
  bare-metal toolchain" silently measured the host one.
- The stack protector is ON (`-fstack-protector-strong
  -mstack-protector-guard=global`, the second half load-bearing on x64), with
  `test-stack-guard-qemu.sh` failing if it silently stops applying.
- `scripts/lint.sh` runs clang `-Wall -Wextra` over every TU (12 warnings gcc
  never emitted, one of them a real `-Wformat` defect), clang-tidy over `src/`,
  and `bugprone-*` over `test/unit/` and `tools/`.
- `scripts/verify.sh` runs the whole gate set concurrently and prints one
  table; the lint-gate list has one definition (`LINT_GATES`) instead of two
  that drifted.
- `run-qemu.sh` types when the GUEST is ready rather than when a wall clock
  says so, gates capture on the guest too, and no longer leaks its state dir or
  its guest on `--background`.
- CI repairs: `-std=gnu2x` (gcc 13 rejects `gnu23`), `safe.directory` for the
  container lint job over a uid-1001 checkout, a wall-clock budget of the
  runner's own, and `install.sh --cpp` requiring a toolchain only for the arch
  being built — the integration job had never passed that step.

- **New gate `make check-examples`** — compiles every `sdk/examples/*.c`
  and `*.cpp` against the current public headers. 20 of the 51 examples were
  reachable by no build rule and no test, yet
  `scripts/build-packages.sh` copies them verbatim into the `.deb` and
  `.rpm`, so the first person to compile one was a consumer. Compile-only
  (~2.5 s); the link and runtime stay covered by the Makefile rules and
  `test/integration/test-axl-cc-*.sh`. Fails if it finds fewer than 20
  sources, so a broken glob cannot report clean forever.
- **The lint-gate list has one definition.** `LINT_GATES` in the Makefile is
  the source of truth; `verify.sh` reads it back via
  `make -s print-lint-gates` instead of keeping a second copy. The two had
  drifted, which is how `check-tautology` came to be a build-wiping gate,
  and `check-cxx-entry` came to be a gate `verify.sh` never ran. Both are
  fixed by construction, and `verify.sh` now refuses to run if the list
  comes back near-empty.

## 3.1.0 — 2026-07-27

### Added

- **`axl_gfx_buffer_fill_rect`** (`<axl/axl-gfx-surface.h>`) — a raw,
  non-compositing rect fill on an off-screen buffer. Every *drawing*
  primitive composites source-over onto a destination treated as opaque,
  forcing the result's alpha to `0xFF`, so `axl_gfx_fill_rect` cannot lay
  down a **translucent** value — a see-through veil written through it stops
  being see-through. The only raw writer, `axl_gfx_buffer_clear`, is
  whole-buffer. Consumers were hand-rolling a row loop over
  `axl_gfx_buffer_pixels`. Like the rest of the `axl_gfx_buffer_*` family it
  honors **no ambient graphics state** (not the clip stack, not the blend
  mode, not the gamma flag) and behaves the same whether or not the buffer is
  the current draw target; intersect with `axl_gfx_get_clip` yourself if you
  want clipping. `axl_gfx_buffer_clear` is now its full-extent case.
- **`AxlServiceDeploy.embedded_only`** + **`axl_driver_ensure_embedded_only`**
  — load the embedded driver blob directly, skipping the disk search. The
  embedded-side mirror of `axl_driver_ensure_from_path`, for a service that
  ships its driver in-image and must not bind a stale copy found on disk.
- **`run-qemu.sh --no-gpu`** (alias `--headless`) — start the guest with no
  graphics adapter, so the firmware exposes no GOP and `axl_gfx_available()`
  is false. A logic-only test then renders nothing at all, which is the fix
  for a compute-bound test binary tripping the CPU gate by way of an
  incidental full-screen render.
- **`run-qemu.sh --workload idle|compute` + `--max-duration SECS`** — declare
  the SHAPE of a run so the right gate applies. On a 1-vCPU KVM guest topping
  out ~1.05 cores, a compute-bound binary holds the CPU threshold from boot to
  exit, so the spike sampler is really measuring run *length* while reporting
  a spike. `compute` turns the sustain check off and **requires** a wall-clock
  budget instead — it can never mean "no gate" — failing with its own exit
  code (9) and a message that says duration.

### Changed

- **A full-screen backdrop blur is ~3.7x faster** (1280x800, radius 12:
  20.9 ms -> 5.6 ms, x64 under KVM). Three bit-exact changes, all validated
  against the independent tent oracle in `gfx-simd-selftest.c`: the per-pixel
  round-and-divide was **four int64 divisions per output pixel** — `div =
  (radius+1)²` is a runtime value the compiler cannot strength-reduce, and it
  was ~60% of the blur — now an exact Granlund-Montgomery multiply-shift
  reciprocal; the second axis pass no longer strides down columns (one useful
  pixel per cache line) but runs as a tiled transpose plus another unit-stride
  row pass; and the row pass is SIMD-dispatched (SSE4.1 / NEON) over the four
  BGRA channels. Note for anyone extending it: a running sum is *serial* along
  the pass axis, so neighbouring pixels cannot be vectorised and AVX2 buys
  nothing without restructuring to run several rows in lockstep.
- **`axl_gfx_buffer_blur` is radius-independent** — O(w*h) per pass instead of
  O(w*h*r), by factoring the tent kernel into two running-sum box passes. The
  intermediate is carried at full precision, so the output is byte-identical
  to the previous kernel. A large-radius veil now costs the same per pixel as
  a small one.
- **A change under a backdrop-blur veil re-blurs only its blur halo, not the
  whole veil** (compositor E10). A blur is a bounded neighbourhood read: a
  backdrop change moves the frost only within `radius`, and computing that
  exactly needs raw backdrop only within `2*radius`. The present recomposites
  that halo, writes back only the part the blur got exact, and restores the
  collateral it painted over but did not change — so the frame is
  byte-identical to a full re-blur. Measured: a 12x18 caret under a
  full-screen 1280x800 veil went from **31.6 ms to 0.07 ms per present**. The
  plan is declined, falling back to the whole-veil repaint, wherever it cannot
  be proven equivalent: veils overlapping each other, a veil whose blit is
  split by an opaque surface in front of it, more than 8 veils, an
  OOM-degraded region, or any allocation failure.

  > **Consumer note:** an opaque surface stacked in front of a veil (a modal
  > dialog's card) splits the veil's blit and declines the fast path. Leaving
  > that card non-opaque is measured at 116 µs/present versus 15614 µs with
  > `axl_surface_set_opaque(true)`. The cost of doing so is one extra clipped
  > blit of an already-composited surface.

### Fixed

- **A driver whose `DriverEntry` returned an error corrupted the DXE pool and
  hung the machine** — a silent 100%-CPU spin on RELEASE, an `ASSERT
  [DxeCore] Pool.c` on DEBUG. For a buffer-loaded image AXL synthesizes a
  device path and tracks it in a private per-handle record; EDK2's
  `CoreStartImage` auto-unloads an image whose entry point failed, freeing
  `Image->Info.FilePath` — the same block — so cleanup freed it a second time.
  Cleanup is now liveness-aware: it probes `EFI_LOADED_IMAGE_PROTOCOL`, which
  the firmware's auto-unload reliably uninstalls, and skips only the block the
  firmware already freed (it does **not** free the synthesized device path,
  which the firmware leaves alone — skipping that would leak it and orphan the
  handle on every failed start).
- **`axl_driver_unload` no longer reports success for a handle that is not an
  image.** "No `LOADED_IMAGE` on this handle" is ambiguous — it means both
  "the firmware already auto-unloaded it" and "this was never an image" — and
  returning `AXL_OK` for the second claims an unload that never happened. The
  two are now distinguished by status: a destroyed handle answers
  `EFI_INVALID_PARAMETER` (nothing to do), a live non-image handle answers
  `EFI_UNSUPPORTED` (let `UnloadImage` run and surface the real failure).

### Build

- **Each `BUILD` gets its own output tree.** Objects are flag-dependent and
  make cannot tell that a `.o` was compiled with different `CFLAGS` — the
  cache key is the `.c` timestamp alone — so a `BUILD=RELEASE` compile left
  objects newer than their sources and the next default `make` reused them
  with the wrong flags. The symptom was a *phantom* test failure (`debug:
  alloc fill 0xDA`, a DEBUG-only poison test running against a RELEASE
  library) that `make tests` could not clear; only `make clean` did. `DEBUG`
  keeps the historical `out/native-<arch>`, so nothing that names it changes;
  anything else gets `out/native-<arch>-<build>` and the two coexist. New:
  `make print-prefix` reports a configuration's directory (so callers stop
  hardcoding it) and `make clean-all` wipes every tree.
- **CI no longer re-runs on a release tag** — it was validated on `main`
  before the cut, so the tag re-ran an identical ~38-minute matrix for an
  identical result. Release tags run the publish workflow (and Docs on a
  major); dispatch CI by hand on `main` before tagging.
- Internal dogfooding gate: `axl_malloc` is the default allocator throughout
  the library, and the remaining raw firmware-pool calls carry an explicit
  `axl-pool-direct` marker so new ones cannot slip in unnoticed.

## 3.0.0 — 2026-07-25

> **Consumers must fully REBUILD against this release, not relink.** Two of
> the changes below are invisible to the linker. A translation unit still
> compiled against the old `int _axl_service_driver_init(...)` prototype
> keeps truncating the firmware status with no diagnostic — the symbol name
> is unchanged, so the link succeeds and the S0 bug survives in that object
> file. Likewise, a TU compiled against the old `AxlServiceDeploy` allocates
> a struct one pointer short, so the new library reads `driver_path` past
> its end. Delete stale objects (and any staged copy of the SDK headers)
> before rebuilding.

### Added

- **`AxlConsoleVtEnc`** (`<axl/axl-console-vt-enc.h>`) — the ops→VT
  encoder, lifted out of `axl-console-mirror` and made public. It is the
  **remote** counterpart to `axl_console_term_ops` (the local, cell-grid
  consumer): bind it to any producer and get the UTF-8 + ANSI/VT byte
  stream an xterm-class terminal wants, plus
  `axl_console_vt_enc_snapshot` for late-join repaint. Previously this
  code was private to the mirror, which hard-wires it to the **tap**
  producer — so a take-over console (`axl_console_device_install`) had
  no supported path to a remote terminal at all, the only public
  `AxlConsoleOps` consumer being `axl-console-term`, which rasterizes
  rather than serializes. `AxlConsoleMirror` is now literally tap +
  encoder; its emitted bytes are unchanged (the op bodies were moved
  verbatim and the existing mirror tests are the byte-identity net).
- **`axl_console_device_get_size`** — read the device's resolved geometry,
  mirroring `axl_console_tap_get_size`. Needed by a `passthrough_local`
  consumer in particular: passthrough forces geometry to physical, so the
  resolved size is not something the caller passed in, and a consumer
  that assumed 80x25 would size its screen model wrong.
- **`AxlConsoleTee`** (`<axl/axl-console-tee.h>`) — fan one producer's
  `AxlConsoleOps` out to several consumers, so "render locally **and**
  mirror remotely" becomes expressible (a producer binds exactly one
  vtable). It is substrate rather than three lines in each consumer
  because `scrollrect` and `set_term_prop` return **negotiation**, not
  status: split across consumers the answers can disagree, and the naive
  forwarder corrupts a grid silently. The tee answers accepted only when
  every consumer accepted, and asks all of them (no short-circuit, so the
  result cannot depend on add order). Declining is the safe direction
  because it is recoverable — the producer redraws the rect as ordinary
  damage, repairing consumers that scrolled and those that did not —
  whereas a false "accepted" emits no damage and leaves the declining
  consumer permanently wrong.
- **`AxlConsoleDeviceConfig.passthrough_local`** — keep the firmware
  consoles in the ConSplitter fan-out instead of evicting them, so
  GraphicsConsole carries on painting the local display while the
  consumer still receives every op. The default (evict) is right when the
  consumer OWNS the framebuffer — it renders the grid itself, and a
  co-painting GraphicsConsole would fight it — and wrong when the
  consumer only OBSERVES, e.g. mirroring the console to a remote viewer,
  where evicting blanks the local monitor and freezes anything sampling
  the GOP. Requires physical geometry (`cols`/`rows` must both be 0):
  two consoles painting one screen must agree on the grid, and an
  explicit size is refused rather than half-honoured. Consequence:
  geometry is pinned, so `axl_console_device_set_size` cannot honour a
  far-end resize. Verified in DEBUG OVMF by a new `passthrough` scenario
  in `test-console-device-qemu.sh`, the exact inverse of the take-over
  scenario's clean-region check.
- **`axl_driver_ensure_from_path`** (`<axl/axl-driver.h>`) — the pinned
  sibling of `axl_driver_ensure_with_embedded`: short-circuit on an
  already-registered protocol, otherwise load, start and verify
  **exactly one named file**. No four-path search, no embedded
  fallback. `override_name` did not cover this — it substitutes a
  *name* into the same search, so it cannot separate two files that
  share a name.
- **`AxlServiceDeploy.driver_path`** — the AxlService-level form of the
  same thing. With it set, `axl_service_start_embedded` loads exactly
  that file, so a stale `drivers/<arch>/<driver_name>` from an older
  install cannot shadow the image the launcher just staged. (It really
  can: a launcher at the volume root has no usable image directory, so
  its own sibling is only search candidate #4 while `drivers/<arch>/`
  is #2 — reproduced under OVMF/X64 by
  `test/integration/test-service-pin-path-qemu.sh`.) `driver_name`,
  `driver_blob` and `driver_blob_len` all feed the default resolution
  only, so none is read — or required — when a path is pinned.

- **`AxlFileView`'s consistency model is now documented as close-to-open**
  — the same guarantee NFS gives by default. A freshly opened view sees
  the file's current contents, unconditionally and against any writer;
  re-opening is how a caller gets fresh data. Whether a view that is
  ALREADY OPEN notices a write is explicitly **best effort**, and callers
  must not build on it. This is a documentation change: it names a model
  the type always had, in place of the per-read coherence the header
  previously implied.
- **Best-effort coherence for an already-open view.** Every AXL write path
  now records that it touched a file, and a view compares one integer per
  access — re-stat'ing and dropping its cached pages only when the file it
  reads actually moved. A read that follows no write costs a load and a
  compare, not a firmware round trip. Covers `axl_file_set_contents`,
  `axl_file_write_atomic`, `axl_file_truncate`, `axl_file_delete`,
  `axl_file_rename`, `axl_file_move`, `axl_dir_mkdir`/`_rmdir`,
  `AxlFileWriter`, every `axl_fwrite`/`axl_pwrite` on a file stream, and
  the file log handler, for every `AxlFileView` consumer rather than just
  the 9P server. Previously such a view reported the old length over the
  old bytes indefinitely, with no error and nothing to check. It does NOT
  fire for a writer in another PE image, a non-AXL writer, or the Shell —
  hence best effort, and hence close-to-open as the guarantee.
- `axl_file_view_refresh()` — run that best-effort check on demand. It
  does not reveal a write the view could not otherwise see; its value is
  the return, `AXL_ERR` if the file was deleted or renamed away, which is
  the only way to tell that apart from an ordinary empty read (a vanished
  file reports size 0).
- `axl_file_view_set_pinned()` — turn the best-effort half off for one
  view, for a consumer that has already committed to a length or to byte
  offsets. Freezes the length the view **reports**; it is not a snapshot
  of the bytes, and the header is explicit about why (`AxlPageCache` may
  evict a resident page at any time, and UEFI has no file-snapshot
  primitive).

- **`cut` and `tr` tools** — POSIX `cut(1)` / `tr(1)` ports. The UEFI
  Shell ships neither (nor `grep`/`sed`/`cat`), so a pipeline like
  `tool | cut -f2 | tr a-z A-Z` had no building blocks. Both read stdin as
  UCS-2 (`axl_stdin_text`), so they compose with the Shell's `|` pipes;
  GNU-parity tested against the host tools.
- **`lsproto` tool** — list the live UEFI protocols on a handle by their
  canonical spec name (`EFI_RAM_DISK_PROTOCOL`, not the Shell's short
  `RamDisk`), via a generated GUID→name table and an upgraded
  `axl_protocol_guid_name`.
- **`profile-qemu.sh` + `gdb-sample.py`** — a sampling profiler for AXL
  apps under QEMU (via the `--gdb` stub): answers "QEMU is pegged at
  100% — WHERE?" with `file:line`, and reports a host-CPU spin/idle
  verdict. Shipped in the host-tools package.
- **Shell-free file access for BDS boot-option apps** — the path resolver
  and `axl_driver_load_sibling` now work with no `EFI_SHELL_PROTOCOL` at
  all (device path built from the running image's own
  `LoadedImage->DeviceHandle`), so an app launched directly as a boot
  option — not from a shell — can still find files beside it.
- **Co-painting console reshape** — a `passthrough_local` consumer can now
  reshape through the physical text-mode list (`AxlConsoleOps::resize`),
  so a co-painting consumer and GraphicsConsole stay in sync on geometry
  rather than desyncing on a half-switched mode.
- **Console output coalescing** — an `AxlConsoleVtBuf` buffering VT sink
  plus `axl_console_vt_enc_flush()` and a `coalesce` flag, with snapshot
  REP/ECH run-merging. A full-screen repaint that previously emitted a
  per-cell flood of WebSocket frames now coalesces to a handful; a
  consumer sets `coalesce=true` and flushes once per loop tick.
- **`axl-shell-launcher`** (test harness, **opt-in**) — a tiny chainloader
  that starts the EDK2 Shell with LoadOptions `-delay 0`, skipping its
  5-second startup countdown (~5 s of `gBS->Stall` busy-wait per guest boot
  across the QEMU suite). Enabled by `AXL_SHELL_LAUNCHER=1`; the default
  boots the Shell directly (robust everywhere — the launcher chainloads
  whatever Shell is staged, and a Shell that mismatches the firmware hangs
  when started, so it is gated until validated for a given environment).

### Breaking

This release lands a public-API consistency audit (145 headers) that
normalizes naming, return types, and parameter order across the surface.
Most changes are source-compatible for callers that test `== AXL_OK`, but
several require edits on rebuild — the highest-risk being the parameter-order
changes, which can compile silently. See `docs/AXL-API-Consistency-Audit.md`.

- **Header rename: `<axl/axl-port.h>` → `<axl/axl-io-port.h>`.** Update the
  include; the old path is gone.
- **Parameter-order normalization — some reorders compile silently.**
  Out-params moved last; the reader/fill/task callback argument order was
  canonicalized; and `axl_shared_driver_unpublish` now mirrors
  `axl_shared_driver_publish`. Where the reordered arguments share a type
  the compiler will NOT flag a stale call site — audit calls to these by
  hand.
- **`int` → `AxlStatus` return types on multi-outcome functions** — the
  HTTP server/client, ramdisk, SPD, `axl_net_ensure_drivers`, and the TLS
  path now return a typed `AxlStatus`. Tests against `AXL_OK` are
  unchanged; code that stored the result in an `int` or compared it to a
  literal `0`/`1` should move to the enum.
- **`AxlFsStatus` renumbered to the negative-value convention** — compare
  against the named constants, not the old numeric values.
- **Constructors return the object, not `int` + out-param.** `axl_vterm_new`
  and `axl_console_screen_new` now return `T*` (NULL on failure); the
  `_new`/`_free` constructor/destructor naming is applied consistently.
- **`axl_queue_deinit` split into `axl_queue_deinit` / `_deinit_full`** to
  fix a stack-queue teardown footgun — pick the variant matching how the
  queue was allocated.
- **`AXL_WARN_UNUSED` added to security / must-check functions** — a
  discarded return now warns, and a `-Werror` consumer build fails until
  the result is checked.
- **Enum-flag and const/type hygiene** — several flag params are now typed
  enums and several pointer params gained `const`; a mismatched caller gets
  a compile error.

- **`axl_file_view_size` lost its `const`** — it is now
  `size_t axl_file_view_size(AxlFileView *v)`. A caller holding a
  `const AxlFileView *` gets a hard compile error, C++ consumers included.
  The length is a property of the file, not of the view, and the call may
  now perform a firmware stat plus a stream close/reopen when the file has
  been written — so it can no longer be `const` and is no longer a
  struct-field read. Note `axl_file_view_stats` deliberately keeps its
  `const`: it reads counters and does not sync.
- **`axl_file_view_page`'s borrowed pointer has a shorter life.** It was
  documented as valid until the next call "that may evict (any read/page
  call)"; `axl_file_view_size` and `axl_file_view_refresh` now run the
  coherence check too, and that drops every frame the view holds. So
  `p = axl_file_view_page(v, …); sz = axl_file_view_size(v); use(p);` was
  legal before this release and is a use-after-invalidate now. The pointer
  is valid only until the next call on the view, whichever it is.
- **9P `Tfsync` on a bound-but-never-opened fid now answers
  `Rlerror(EBADF)`** (was `Rfsync`). `fsync(2)` has no meaning without an
  open file description, and `Tread`, `Treaddir` and `Twrite` already
  refused the same fid. Wire-visible to a client that fsyncs a fid it
  walked to but never opened.

- **`AxlWebDavOps.write_close` is now `int (*)(void *ctx, bool aborted)`**
  (was `void`). The final flush is where a streaming PUT becomes durable
  and nothing earlier can report it, so the slot has to return a status:
  `AXL_ERR` on the clean-EOF call makes PUT answer **500** instead of 201.
  Implementors of the vtable must update their signature — a hand-written
  backend outside this repo is the case that breaks, and one already did.
  The return is ignored on the abort call. `write_open` and `write_close`
  must be wired as a pair; a NULL close with a non-NULL open reports 201
  for every upload that carried a body.

### Changed

- **`axl_service_reload` / `axl_service_reload_buffer` distinguish a
  load failure from a start failure** and return `AxlStatus` instead of
  `int`. Per the reload contract a load failure tears nothing down (the
  service is still serving) while a start failure leaves the service
  down — but both used to report `AXL_ERR`, so a caller had to
  conservatively roll back and cold-reset even when the box was
  healthy. `AXL_ERR` now means, and only means, "this service is DOWN":
  `AXL_INVALID` (caller misuse), `AXL_NOT_FOUND` (replacement could not
  be loaded) and `AXL_NO_RESOURCES` (serialize / handoff-event /
  LoadOptions failure) all mean the service is untouched and still
  serving. Source-compatible for any caller that tests `!= AXL_OK`; the
  only observable change is which negative value those pre-teardown
  failures report.

- **QEMU harness de-duplicated** — `run-qemu.sh` and the integration
  harness (`common-test.sh`) now share `qemu_strip_kvm`,
  `qemu_stage_disk`, `cpu_policy_init`, and the Shell-staging helper from
  `scripts/axl-common.sh`, so the two can no longer drift. `run-qemu.sh`'s
  long-dead 1.5-core CPU-spike check is replaced by a working
  0.5-core + warm-up + TCG-carve-out policy (opt out with `--no-cpu-warn`).

### Fixed

- **Tool text output now pipes.** `axl_stdout` (and `axl_print` /
  `axl_printf` / `axl_write(axl_stdout, …)`) wrote only `gST->ConOut`,
  which the UEFI shell swaps for a `>` / `>a` redirect but NOT for a `|`
  pipe — so `tool | other` printed the producer's output to the SCREEN
  and the downstream stage received nothing. `axl_stdout` now detects a
  non-interactive stdout (the same `GetFileSize` probe `axl_stdin`
  already uses) and writes its transcoded UCS-2 to the
  `EFI_SHELL_PARAMETERS_PROTOCOL.StdOut` handle, so `tool | other`,
  `tool | other | third`, and the ASCII operators `>a` / `|a` all carry a
  tool's text output. The interactive console keeps the ConOut path so
  the console subsystem (tap / mirror / device) still observes output.
  `axl_stdout_raw` is now only for BINARY payloads, not for piping text.
  Covered by the new `test-tool-redirect-pipe-qemu.sh` (all tools ×
  `>` / `>a` / `|` / `|a`, plus the stdin-consumer filters).

- **`_axl_service_driver_init` no longer truncates the firmware status.**
  It was declared `int` while returning `EFI_INVALID_PARAMETER` /
  `EFI_OUT_OF_RESOURCES` / `EFI_ABORTED`, so the 64-bit status lost
  `EFI_ERROR_BIT` on the way out: `EFI_ABORTED` (`0x8000000000000015`)
  reached `AXL_SERVICE_DRIVER`'s `DriverEntry` as `0x15`, which
  `EFI_ERROR()` reads as **success**. A service whose `setup` returned
  `AXL_ERR` was therefore reported to the firmware as started —
  `StartImage` succeeded, `axl_driver_start` returned `AXL_OK`, and
  `axl_service_reload` declared a healthy hot-swap for a service that
  never attached (the old image had already released its ports). The
  declaration and definition now carry `AxlEfiStatus`, and
  `AXL_SERVICE_DRIVER`'s widening cast is gone. Source-compatible for
  consumers — the macro is used exactly as before.
- **`axl_app_image_path()` honours its documented NULL for synthetic load
  contexts.** `<axl/axl-app.h>` has always promised NULL for "synthetic
  load contexts that bypass the usual file-load path", but a
  buffer-loaded driver got a decode of the `MemoryMapped(...)/FilePath`
  device path AXL synthesizes after such a load (so the aarch64 shell can
  render the handle) — a volume-less `"\<name>"` naming a file the image
  was never loaded from, and, when the loader supplied no name, naming
  the *launcher* instead. Anything writing to or loading from `<self>`
  acted on the wrong path. The accessor now returns a path only when the
  image really came from a file (a FilePath the firmware can resolve
  against a source volume).
- **`axl_driver_get_image_path()` gets the same treatment**, for the same
  condition and the same reason: it read `LoadedImage->FilePath` directly
  and decoded the synthesized node too. This cannot move the driver search
  order — `driver_build_candidates` only calls it inside a branch that has
  already matched `DeviceHandle` against an enumerated volume, which
  implies the gate the fix adds.
- **Sidecar discovery for buffer-loaded drivers works again.** The
  ParentHandle fallback `<axl/axl-app.h>`'s internals documented had been
  dead since AXL started synthesizing a device path for buffer loads: the
  synthetic FilePath ended the walk before it reached the launcher, so
  `axl_resolve_data_file` from inside an embedded driver found nothing.
  The anchor is now derived separately from the image's own path and
  walks past images with no file of their own.
- **`axl_service_reload` verifies the replacement actually attached.**
  `StartImage` succeeding is not proof; and the plain `LocateProtocol`
  re-check `axl_driver_ensure_with_embedded` uses cannot work here
  because the *old* image still publishes the service GUID. The reload
  now counts the handles publishing that GUID before and after the
  start and treats "no increase" as a start failure (unloading the
  replacement). A failed enumeration disarms the check rather than
  failing a reload that actually succeeded.

- **Write paths reported success for bytes that never reached the volume.**
  Closing a file cannot report a failure (`EFI_FILE_PROTOCOL.Close` is
  specified to return only `EFI_SUCCESS`), so every path that relied on
  close to flush was silently lossy on a full volume, write-protected
  media or a device error. `axl_fflush` on a file stream was itself a
  no-op that always failed. Fixed across `axl_fflush`,
  `axl_file_set_contents`, `axl_file_write_atomic` (which was promoting a
  temp over a good file on that false success), `axl_file_move` (which
  deleted the source), `axl_file_truncate`, `axl_file_writer_close`,
  `axl_piece_tree_save`, `axl_log_flush`, the 9P server's fid teardown
  (`Tclunk` now answers `Rlerror(EIO)`), the WebDAV PUT handler, and the
  `tar` / `sed` / `netload` / `fetch` tools.
- `axl_file_write_atomic` deleted its temp file when the promote failed,
  even when its own delete-then-rename fallback had already removed the
  target — losing the data from both places. The temp is now kept
  whenever the target did not survive.
- **A graceful `EFI_TCP4.Close()` could wedge the driver loop.** At raised
  TPL, with un-flushed TX, the firmware's graceful close spins forever
  inside the driver pump. The close is now promoted to an abortive RST at
  raised TPL, so a console reshape / teardown no longer hangs the server
  (root-caused by reproducing it live plus a QEMU-monitor stack walk).
- **`rsod-decode.py` reconstructs a backtrace when the firmware printed no
  frames** — it walks the frame-pointer chain (`--fp-unwind` forces it),
  cross-validated byte-identical against a real Dell AArch64 trace.
- **`run-qemu.sh --serial-socket` no longer drops early output.** The
  chardev now uses `wait=on`, so the guest blocks until the caller's
  serial reader attaches; previously a fast app could print its
  ready-marker before the reader connected and the caller would wait
  forever (a latent race the removed Shell countdown had been masking).
- **The pure-lint gates no longer trip the `AXL_TLS` state-change wipe.** A
  bare `make check-ascii` (etc.) after an `AXL_TLS=1` build used to read
  `TLS_STATE=off` and wipe the TLS tree, forcing a full rebuild; the
  lint-only gates are now excluded from the wipe like `clean`/`help`.
- **`find_shell_efi` stages a Shell that matches the firmware in use.** Since
  v2.9.0 (`ab2b9762`) it preferred a distro/system-package `Shell.efi` over
  extracting one from the active firmware. A packaged Shell that doesn't match
  the firmware (e.g. EL10's `/usr/share/edk2/ovmf/Shell.efi` against the
  qemu-10.0.0 OVMF) starts but **hangs before its banner**, so every
  ambient-Shell QEMU boot hung (directly, or via the opt-in launcher; only
  `--boot-target`, which uses the firmware's own internal Shell, was spared).
  It now prefers extracting the firmware's own Shell (native fwtool → python →
  uefiextract) and falls back to the distro package only when extraction is
  impossible.


## 2.9.0 — 2026-07-14

A large release centered on a new **console subsystem**: a producer-agnostic
console contract with three producers (firmware-tap, take-over device, VT-stream
parser) and two consumers (a remote VT encoder and a local on-screen terminal),
plus a server-side screen model for late-join repaint. Adds the `fbcon` graphical
terminal and `kbtune` keyboard tuner, an input debounce/spacing layer, and a
dogfooding lint gate. No breaking API changes.

### Added

- **`AxlConsoleOps` — a producer-agnostic console contract** (`<axl/axl-console-ops.h>`).
  A structured op vtable (`clear_screen`, `set_cursor`, `output_text`, `set_pen`,
  `erase`, `moverect`, `scrollrect`, `set_term_prop`, …) that any console
  *producer* reports and any *consumer* renders — so one consumer binds three
  different producers unchanged. Coordinates are 0-based, rects half-open; a
  whole-pen snapshot (`set_pen`) and one extensible `set_term_prop` channel carry
  rendition, cursor, alt-screen, and reverse-video state.
- **`AxlVterm` — a VT/xterm byte-stream parser** (`<axl/axl-vterm.h>`), the second
  producer, over a **vendored libvterm** (MIT, Layer 2 only; `screen.c` vendored
  but never compiled). Coalesces libvterm's positioned glyphs into cursor-relative
  runs and accumulates its incremental pen into a snapshot; restores SCOSC/SCORC
  (`CSI s`/`CSI u`), adds `clear_scrollback` (`CSI 3J`), and bounds CSI arg count
  (untrusted-input hardening). `axl_vterm_char_width` is the single wcwidth
  authority shared by producer and consumer.
- **`AxlConsoleTap` — the firmware-console producer** (`<axl/axl-console-tap.h>`),
  the swap-strategy surgery split out of the mirror: wraps
  `EFI_SIMPLE_TEXT_OUTPUT`, owns the `SIMPLE_TEXT_OUTPUT_MODE` the guest reads back
  when `passthrough_local` is off, tracks the alternate screen (explicit or an
  `auto_alt_screen` heuristic), and runs a key-injection ring. New
  `axl_console_tap_get_size` reports the resolved (configured-or-physical)
  geometry. Sanitizes control chars in `output_text` (escape-injection fix) and
  folds `Ctrl+letter` on the Simple read like EDK2's ConSplitter.
- **`AxlConsoleDevice` — the take-over producer** (`<axl/axl-console-device.h>`):
  installs itself as the console (output-only), takes over the pointer so guest
  apps run mouse-free, and relays remote input into the firmware key path with a
  `key_filter` peek hook.
- **`AxlConsoleTerm` — a local on-screen terminal** (`<axl/axl-console-term.h>`):
  binds `AxlConsoleOps` straight into a cell grid drawn on the GOP (or an
  offscreen `AxlGfxBuffer`), with a scrollback ring, mouse selection + clipboard
  copy, reflow (`set_font`/`resize`/`set_bounds`/`set_palette`), per-cell damage
  rendering, and an optional software mouse-cursor overlay.
- **`AxlConsoleScreen` — a server-side screen model + snapshot serializer**
  (`<axl/axl-console-screen.h>`): fed a VT stream, it maintains primary + alternate
  cell grids and serializes the current screen as one self-contained, coalesced VT
  repaint (`axl_console_screen_snapshot`) so a mid-session client repaints instead
  of replaying a raw byte tail. **`axl_console_mirror_snapshot`** exposes the same
  late-join repaint on the mirror, which composes an internal screen fed from its
  own emitted stream.
- **`AxlConsoleMirror` enhancements**: alt-screen control (explicit + auto) and
  `input_capture`; owns `SIMPLE_TEXT_OUTPUT_MODE` when `passthrough_local` is off;
  a golden-output test pins the emitted VT stream.
- **`fbcon`** — a graphical terminal that takes over the UEFI shell on the GOP
  (embeds the take-over driver as a runnable launcher; `Ctrl+\` restart, mouse
  cursor sprite, `-d`/`-g` input-gate knobs).
- **`kbtune`** — a keyboard-bounce tuner (GOP UI), plus an **input debounce +
  min-gap delivery gate** in `<axl/axl-input.h>` (`axl_input_set_key_debounce`,
  `AxlKeyGate` / `axl_input_key_gate_ready_at` / `_mark`) and
  `axl_input_attach_mouse_ifaces`.
- **`check-dogfood`** (`make check-dogfood`, in CI) — a per-file ratchet that keeps
  library UEFI protocol / boot-service calls routed through the backend +
  `axl_efi_call` seam, failing only on a new raw `proto->Method()` call.
- **`axl_vsnprintf`** (`<axl/axl-str.h>`) — the `va_list` sibling of `axl_snprintf`.
- **Console read-key unification**: `axl_console_read_key` runs on the Ex read path
  and reports `AxlKey.modifiers`; `run-qemu.sh --boot-target` stages an app as
  `\EFI\BOOT\BOOTx64.EFI`, and its CPU monitor now runs under `--screenshot` and
  fails the run on a CPU spike.

### Fixed

- **auto_alt_screen** latched the alternate screen at boot and never left it; it
  now enters on a backward cursor jump after a clear and leaves on a newline.
- **console-device / fbcon**: guest `Ctrl+C` froze the display + mouse (render
  loop quit); the read loop faulted (`#GP`) on real hardware after a keyboard
  interface was evicted (now re-resolved each pass); a use-after-free on uninstall;
  a ConSplitter mode assert and a ConsoleLogger dead-loop on a non-80x25 take-over.
- **backend**: re-locate `EFI_SHELL_PROTOCOL` on each call to fix an
  exit-under-resident-loop use-after-free.
- **console**: a zero-length selection (a click with no drag) now selects nothing
  instead of leaving a cell inverted; non-blocking `axl_console_read_key` reads
  directly instead of via `CheckEvent`; the loop drains keypress sources in the
  non-blocking dispatch path.
- **docs**: fixed a Sphinx/Breathe build crash (a `###` heading in a doc comment)
  that would have failed the Pages deploy, plus several broken `@ref` targets.

### Changed

- C++ `AXL_APP` / `AXL_DRIVER` now emit **unmangled** firmware entry points
  (`_AxlEntry` / `DriverEntry`), guarded by a `check-cxx-entry` gate.
- `axl_map_refresh` no longer echoes the `map -r` listing on either shell.

### Dependencies

- Vendored **libvterm** (MIT, neovim fork), Layer 2 only.

## 2.8.8 — 2026-07-08

### Added

- **Universal `-b` / `--page` page-break, delegated to the shell.** Every tool
  built on `axl_args_run` now accepts the UEFI shell's page-break convention
  (and a `--page` alias) — it is never an "unknown flag." Paging is a shell
  service, not an SDK-side pager: AXL tools write to the console (`gST->ConOut`)
  which the shell wraps, so the new `axl_console_set_page_break()`
  (`<axl/axl-console.h>`) simply flips `EFI_SHELL_PROTOCOL.EnablePageBreak`. It
  is suppressed inside a script (gated on `BatchIsActive`) so a `startup.nsh`
  never hangs on a keystroke, and cleared on every tool-exit path so paging
  can't leak into the next command. A tool that declares its own `-b` (or a
  `page` flag) keeps it — each spelling defers independently.
- **`axl_argv_drop()`** (`<axl/axl-args.h>`) — remove an element from `argv`
  in place (shift down, NUL-terminate, decrement `argc`); NULL- and
  bounds-safe. The argv-surgery primitive for a tool that pre-strips its own
  flags before `axl_args_run`.

### Fixed

- **Consumers can `#include` the standard C headers (`<string.h>`, `<stdlib.h>`,
  …) on every arch.** The SDK's `compat/` freestanding shims — the same ones the
  library builds against — are now staged under `include/axl-sdk/compat/` and
  put on the include search path for every consumer entry point (`axl-cc` C and
  C++, the CMake package, and the pkg-config `Cflags`). Previously a consumer
  that included a hosted-libc header failed to build for AArch64 (the cross-gcc
  ships only freestanding headers) and on x86-64 only "worked" by silently
  borrowing host glibc from `/usr/include` — which a freestanding UEFI SDK must
  never do. Consumers using only `<axl.h>` / `<axl/*.h>` were unaffected.
- **CLI tools report POSIX exit codes instead of "Aborted."** An unarmed
  non-zero `main()` return now maps to a small code `1..255` (readable as
  `%lasterror%`), no longer collapsing every failure to `EFI_ABORTED` (0x15).
  Convention: `0` success, `1` negative result (no match / usage error), `2`
  trouble (couldn't open a file).
- **CLI tools explain *why* they failed.** `grep` / `cat` / `hexdump` / `sed` /
  `find` now print `tool: cannot open 'path'` to stderr and exit 2 on a file
  they can't open (previously: `grep` silently returned 0, `cat` a stray code,
  `hexdump` wrote to stdout). `find <missing>` errors with a message instead of
  echoing the bogus name.
- **`mkrd` no longer clobbers `%path%`** — the map refresh snapshots and restores
  it around the shell's `map -r`.
- **`dmidecode`** groups records by ascending SMBIOS type (stable).

### Changed

- **`tar` accepts `-f`** (GNU/BSD style), and `axl_args` now supports getopt
  short-flag bundling in default mode (`-cf a.tar`, `grep -ic pat f`).

## 2.8.7 — 2026-07-08

### Added

- **File layer resolves `fsN:` and cwd-relative paths on the old EFI 1.x
  shell.** That shell publishes no `EFI_SHELL_PROTOCOL`, so the whole
  path-based file/directory API (`axl_fopen`, `axl_file_info`,
  `axl_file_get_contents` / `axl_file_set_contents`, `axl_file_delete` /
  `axl_file_rename`, `axl_dir_mkdir` / `axl_dir_rmdir` / `axl_dir_open`, and
  `axl_get_current_dir` / `axl_getenv`) previously failed there — it could not
  open any `fsN:`-qualified or relative path. The backend now resolves paths
  itself through `SHELL_ENVIRONMENT` (`GetMap` + `CurDir`) and
  `EFI_FILE_PROTOCOL`, reaching exact behavioral parity with the modern shell.
  No API change; consumers that already worked on the modern shell now work on
  the old one unchanged.
- **`axl_setenv` / `axl_unsetenv` work on the old EFI 1.x shell.** That shell's
  `SHELL_ENVIRONMENT` has no programmatic `SetEnv`, so both previously failed
  there (even from a plain shell app). They now drive the shell's own `set`
  command through the Execute service — the mechanism `mkrd` already uses for
  `map -r` — so a consumer that reads a value into a shell variable (e.g. a
  `-f<file> var` idiom) works on the old shell as it does on the modern one,
  including from a resident driver. Old-shell-only limits (the `set` command
  line can't carry them unmangled): the value cannot contain `"`, `^`, `%`, or a
  newline, and the name must be a bare identifier (`[A-Za-z0-9_]`); such a call
  returns `AXL_ERR` rather than setting a corrupted value. Values with spaces
  are fine.

### Changed

- **`axl_dir_mkdir` is now idempotent for an existing directory.** It succeeds
  when the path already exists *as a directory* (so `mkdir -p` and
  copy-into-existing flows need no pre-check) and fails only when a
  *non-directory* already occupies the path. Previously the UEFI FAT
  create-directory primitive reported success in both cases — including the
  silent-conflict case where a file was in the way. The docstring's old
  "AXL_ERR if it already exists" wording is corrected to describe the
  idempotent contract.

### Fixed

- **`axl_file_set_contents` truncates the target.** It documents "creates or
  overwrites", but it opened the file with `CREATE` and wrote from offset 0
  without shrinking, so rewriting an existing file with a *shorter* buffer left
  the previous tail behind (`"hi"` over `"LONG-DATA"` produced `"hiNG-DATA"`).
  It now truncates to exactly the written length. Shell-independent — the bug
  was equally present on the modern shell.
- **`cut-release.sh --dry-run` no longer strands its version bump.** The
  version/CHANGELOG bump is now reverted on ANY early exit (via a trap), not
  only the happy path, so a `--dry-run` piped into `head` (SIGPIPE) or any other
  interruption can't leave the working tree bumped.

## 2.8.6 — 2026-07-07

### Added

- **`axl_shell_kind()`** (`<axl/axl-shell.h>`) — reports which command shell is
  hosting the running image: `AXL_SHELL_KIND_UEFI` (the modern EDK2
  `EFI_SHELL_PROTOCOL`), `AXL_SHELL_KIND_EFI_1X` (the older EFI 1.x shell, which
  publishes `SHELL_ENVIRONMENT` / `SHELL_INTERFACE` instead), or
  `AXL_SHELL_KIND_NONE`. It is the single branch point for behavior that differs
  between the two shells.
- **`axl_volume_alias_to_fsn()`** (`<axl/axl-fs.h>`) — add a named alias for an
  already-mapped volume by driving the shell's own `map <alias> <fsN>:` command.
  This is the map path for shells with no programmatic `SetMap` (the old EFI 1.x
  shell); because it aliases an existing `fsN` it inherits a resolvable device
  path where `SetMap`-by-device-path can't.

### Fixed

- **`mkrd -d` refreshes the shell map on the old shell.** Destroying a RAM disk
  now drives `map -r` (via the shell's Execute service) after unregistering the
  device, so the removed disk's `fsN` no longer lingers as a dangling entry
  ("Invalid file system mapping on fsN"). Symmetric with create's map refresh.

- **Shared-driver sibling-locate works with no `EFI_SHELL_PROTOCOL`.**
  `axl_driver_load_sibling()` (and `axl_shared_driver_locate_sibling()`) now
  resolve the version-pinned sibling driver from the launcher's own
  `LoadedImage` device path — its directory and volume — instead of the shell's
  device-path-to-map lookup. This makes the "thin launcher + resident driver"
  pattern work on the older EFI 1.x shell and under BDS, where the shell map is
  unavailable, in addition to the modern shell. The sibling-only / version-pinning
  contract is unchanged (the driver must still be staged beside the launcher);
  the shell `path` search is retained as a fallback for a path-searched launch
  whose firmware left the launcher a bare command name.

### Changed

- **`mkrd` maps the RAM disk on the old shell with no manual `map -r`.** On the
  older EFI 1.x shell there is no programmatic `SetMap`, but the shell exposes an
  Execute service — so `mkrd <label>` now drives the shell's own `map -r` (the
  way the legacy `mkramdisk` did) during create, leaving the disk immediately
  usable as an `fsN` with no manual step, and **exits 0**. It previously returned
  a non-zero status that surfaced as `EFI_ABORTED` (aborting a `mkrd X; …; X:`
  script) and required the user to run `map -r` by hand. When run at the
  interactive prompt it also aliases the label (`map <label> <fsN>:`), so both
  `fsN:` and `<label>:` work; under a `startup.nsh` (the shell's
  backward-compatible mode, where the device-path aliases the alias needs aren't
  generated) it cleanly maps `fsN:` only. The modern-shell behavior (`SetMap` of
  an `FS<n>` primary + label alias) is unchanged.
- **`mkrd -l` resolves the mapping on the old shell too.** Its MAPPING column
  shows the disk's `fsN` — reverse-looked-up through the EFI 1.x
  `SHELL_ENVIRONMENT.GetMap` — instead of `(unmapped)`, and the LABEL/ALIAS
  column shows the label with its colon (`FOOBAR:`) once the alias is set. It
  renders in the old shell's own lowercase (`fs1:`) while the modern shell keeps
  its uppercase `FS1:`, so the listing stays consistent with whichever shell is
  in use.

## 2.8.5 — 2026-07-07

### Added

- **Every tool reports the SDK release version.** `<tool> --version` (and
  `-V`) prints `<tool> <version>` and exits; the version is also shown in
  `<tool> -h` help and stamped into the `AXL_DIAG` diagnostic dump. The stamp
  is applied once at the `AXL_TOOL_MAIN` layer from the single-source
  `AXL_VERSION_STRING`, so it covers every tool uniformly — those using the
  `axl_args_run` parser and the custom-parser ones (storage tools, `sed`, the
  `axl` busybox multiplexer) alike — and always matches the release version.
- **`axl_version()`** (`<axl/axl-version.h>`) — runtime accessor returning the
  linked library's SDK version string, so consumer apps and tools can report
  the exact SDK build they link against.

### Changed

- **`dmidecode`: `-V`/`--version` now report the tool version** (matching every
  other tool and the real `dmidecode`). The previous `-V` behavior — printing
  the SMBIOS specification version — moved to `--smbios-version`.

## 2.8.4 — 2026-07-07

### Fixed

- **SSIF IPMI reaches the BMC on multi-bus ARM64 servers (Nvidia Grace).** The
  SSIF opener bound the first I2C master and a single fixed slave address
  (`raw >> 1`), so on servers that publish several I2C masters — the BMC on only
  one — every BMC write NAKed ("SSIF write failed after 5 retries"). It now
  probes every SMBus/I2C controller, tries each SMBIOS slave-address
  interpretation (`as-is` / `>>1` / `<<1`), and claims the (controller, address)
  pair that answers IPMI Get Device ID (a write ACK alone isn't enough — a full
  write+read is required). The probe write fails fast; the read stays patient
  (~3.8 s) for a slow BMC.

### Changed

- **SSIF multi-part writes are disabled on `EFI_I2C_MASTER_PROTOCOL`
  transports.** The Nvidia Grace UEFI I2C driver hangs on multi-part SSIF
  writes; a request larger than 32 B over an I2C master is now refused with a
  clear error rather than wedging the bus. IPMI requests are almost always a few
  bytes, so this is safe for the common path; multi-part reads (FRU/SDR) are
  unaffected, and HC transports keep multi-part writes.

## 2.8.3 — 2026-07-07

### Changed

- **`mkrd <label>` now maps a RAM disk so a bare `map` lists it immediately as a
  native volume** — `FS<n>: Alias(s):<LABEL>:` — with no `map -r` (behavior
  change). It makes two `SetMap` calls on the disk: the next-free `FS<n>` first
  (the primary name `map` displays, since the shell shows the first-inserted name
  as primary), then the volume `<LABEL>` (the `Alias(s):` column). Both `FS<n>:`
  and `<LABEL>:` are usable paths. A plain `SetMap` of just the label (the prior
  behavior) was *hidden* from `map` — a bare label matches none of the shell's
  `FS#`/`BLK#`/`HD*`/`CD*`/`F*` display patterns.
- **`mkrd` drops the `-a`/`--alias` option and the `%<label>%` env var**
  (behavior change) — the label *is* the alias now, so neither is needed. The
  label is set as the alias only when it is a clean map token; a reserved
  `fs<digits>` label, an over-long one, one with characters outside
  `[A-Za-z0-9_-]`, or one already mapped to another volume (which is never
  clobbered) is skipped, and the disk is reachable as `FS<n>:` only, with a note.
- **`mkrd -l` columns are now `MAPPING` / `LABEL/ALIAS` / `SIZE` / `FSTYPE`** —
  `MAPPING` is the disk's `fsN` handle (or `(unmapped)`); `LABEL/ALIAS` is the
  volume label, shown with a trailing `:` only when it is currently a live shell
  alias (a `map -r` that dropped it shows the label plain); `SIZE` is `<n>MB`;
  `FSTYPE` is `FAT16` / `FAT32`.
- **`mkrd -d <label>` unmaps every shell name pointing at the destroyed disk**
  (the `fsN` primary and the label alias), by re-reading the device's own map
  name — so it removes only names that belong to that device, never another
  volume's same-spelled entry.

## 2.8.2 — 2026-07-07

### Fixed

- **`mkrd <label>` re-run is idempotent.** A same-session re-run reused the RAM
  disk but assigned it a NEW mapping (its own prior alias read as "taken"),
  drifting `%<label>%` and leaving a duplicate alias. It now reuses the disk's
  existing mapping — reported as "reused" — so a `startup.nsh` can re-run it
  each boot.
- **`mkrd` is quiet by default.** Library `INFO`/`DEBUG` chatter (e.g. the
  driver-ensure "loaded '<embedded>'" line) is suppressed unless `-v` is given.
- **`mkrd` reports why auto-mapping failed** — it distinguishes "no
  `EFI_SHELL_PROTOCOL` on this firmware" from "the shell rejected SetMap", and
  notes the disk still exists (mount via `map -r`).
- **`mkrd -d <label>` now removes the destroyed disk's shell map alias.**
  Destroy freed the RAM disk's device path but left its `<alias>:` (or `fsN`)
  in the shell's global map, dangling at freed memory — a later bare
  `<alias>:` would dereference it. Destroy now captures the alias, unregisters
  the disk, then unmaps the alias.

### Changed

- **`mkrd` maps a RAM disk under its LABEL by default** (behavior change).
  `mkrd RD` now gives you `RD:` with no flag — the positional arg is the FAT
  label, `%<label>%`, AND the default map alias. The default is guarded: the
  label falls back to a free `fsN` (with a note) when it matches the reserved
  `fs<digits>` namespace (so `mkrd fs0` never clobbers the boot volume; case-
  and `:`-insensitive), is already in use by another volume, or is too long /
  has characters outside `[A-Za-z0-9_-]`. Re-runs stay idempotent by reusing
  the disk's current alias in any form (label / `fsN` / custom).
- **`mkrd`: renamed `-m`/`--map` to `-a`/`--alias`** (the value is an alias
  override, not a "map"). `-m` is removed. An explicit `-a fsN` (a reserved
  name) is now a hard error rather than claiming a shell slot.
- **`mkrd` with no arguments prints help** (previously a one-line error), and
  no longer loads the RAM disk driver just to report a usage error.

### Added

- **`axl_volume_map_alias()`** (`<axl/axl-fs.h>`): resolve a device path's
  current shell alias in ANY form (an `fsN` or a custom SetMap name), unlike
  `axl_volume_map_name()` which returns only `fsN`. Returns `AXL_ERR` on an
  over-long alias rather than a truncated one.
- **`axl_volume_unmap()`** (`<axl/axl-fs.h>`): remove a shell map name (SetMap
  with a NULL device path).
- **`axl_ramdisk_find()`** (`<axl/axl-ramdisk.h>`): return a registered RAM
  disk's device path by FAT label (the lookup `create`/`destroy` already did
  internally, now exposed).

## 2.8.1 — 2026-07-06

### Fixed

- **`axl_text_stream_wrap` / `axl_stdin_text()` hung on interactive stdin.**
  The wrapper's construction-time encoding sniff looped `src->read` to fill a
  64-byte probe (or reach EOF); the v2.8.0 interactive `axl_stdin` line-cooks
  and never returns EOF at a console, so the sniff swallowed line after line
  and never returned for a short typed answer (an interactive `do -f` read hung
  after the first Enter). The wrapper now skips the sniff for an interactive
  source and returns a UTF-8 passthrough; redirected / piped stdin still
  classifies BOM / UCS-2 as before. Regression-tested end-to-end over a serial
  console (feeds one short line, asserts it returns on a single Enter).

### Added

- **Output buffering (stdio `setvbuf` family).** `axl_stream_set_buffering()`
  with `AxlStreamBuffering { NONE, LINE, FULL }`, plus C-compatible shims
  `axl_setvbuf()` / `axl_setlinebuf()` / `axl_setbuf()` and
  `axl_stream_get_buffering()`. Coalesces writes in `axl_write` (which the
  `axl_print*` / `axl_fwrite` family all funnel through), ahead of any
  UTF-8 → UCS-2 transcode and tee. Default stays `NONE` (unbuffered, unchanged
  behavior): unlike C stdio, AXL does not auto-select buffering from tty-ness,
  because a UEFI crt0 exit path may run no atexit hook — buffering is opt-in and
  the caller owns the final `axl_fflush` (`axl_fclose` flushes then frees). See
  `docs/AXL-Stream-Buffering-Design.md`.
- **Interactive / no-EOF source marking.** `axl_stream_set_interactive()` /
  `axl_stream_get_interactive()` — the line-discipline axis, orthogonal to
  buffering. Marks a stream as line-cooked and never-EOF so
  `axl_text_stream_wrap` skips its classify read-ahead for it (generalizing the
  interactive-stdin fix above to any caller-owned no-EOF stream); the returned
  text wrapper inherits the mark.

## 2.8.0 — 2026-07-06

### Fixed

- **`axl_shared_driver_locate_sibling` / `axl_driver_load_sibling` on a
  path-searched launch.** When a launcher is found via the shell `path` (e.g.
  bare `do sysid` from a different CWD), some firmware sets
  `LoadedImage->FilePath` to just the matched command name, so
  `axl_app_image_path()` loses the launcher's directory and the sibling lookup
  collapsed to the volume root → `AXL_NOT_FOUND` (consumer symptom:
  "doDriver.efi not found in this directory" though both are staged together).
  The sibling resolver now falls back to re-running the shell's own `path`
  search for `argv0` and loads the driver from **that** directory — strictly
  beside the launcher the shell actually ran, preserving the sibling-only /
  version-pinning contract (never a copy from an unrelated directory).
  Real-hardware-only path (OVMF's shell doesn't path-search `.efi`); the
  decomposable search logic is unit-tested, the end-to-end path-launch is
  verified on target hardware.

### Added

- **`axl_path_search(search_list, name, out_path)`** (`<axl/axl-path.h>`) —
  searches a `;`-separated PATH-style directory list for a file, returning the
  first match (mirrors the UEFI Shell's `path` resolution). Backs the sibling
  path-launch fallback above.
- **`mkrd <label>` maps the RAM disk into the shell in ONE call — no `map -r`.**
  A non-interactive `startup.nsh` on read-only boot media (the ePSA `last.nsh`
  workflow) needs a writable volume it can select deterministically. `mkrd` now
  assigns the disk a shell map name via `EFI_SHELL_PROTOCOL.SetMap` (which
  targets the shell's *global* map, unlike a nested `Execute("map -r")`), so the
  name is usable by the launching script immediately:
  `mkrd RAMDISK` → `%RAMDISK%:` → write. By default it picks the lowest free
  `fsN`; `-m <name>` pins a chosen name and **fails if that name is already in
  use** (never clobbers). Either way it also sets shell var `%<label>%` to the
  chosen name and prints a summary (label, size, mapping, backing device path).
  Idempotent on the label. This mirrors the old EFI Toolkit `MKRAMDISK`, which
  likewise took the map name as an argument. Switch with the bare `<name>:` /
  `%<label>%:`, not `cd`.
- **`axl_volume_set_map(device_path, name)` / `axl_volume_map_taken(name)`**
  (`<axl/axl-fs.h>`) — assign a UEFI Shell map name to a device path (SetMap;
  usable without `map -r`, even from a child image), and test whether a map name
  is in use. Back the `mkrd` mapping above.
- **`axl_volume_map_name(device_path, out, out_size)`** (`<axl/axl-fs.h>`) —
  resolves the UEFI Shell's *actual* `fsN` alias for a device path via
  `GetMapFromDevicePath` (never the synthesized LocateHandle index that
  `axl_volume_enumerate`'s `.name` falls back to). For code that must read back
  a caller-usable `fsN`.

- **Interactive console line reader — `axl_console_readline` /
  `axl_console_readline_ex`** (`<axl/axl-console.h>`). The line-level peer of
  `axl_console_read_key`: reads keystrokes from the console, echoes printable
  characters, erases on Backspace, and returns the accumulated UTF-8 text
  (minus the CR/LF) on Enter. This is what a tool prompting a human needs
  (`do -f`, a REPL, a `name? ` prompt) — `axl_stdin` / `axl_readline` are
  shell-pipe/redirect readers and cannot line-edit the console. The `_ex` form
  adds a character cap (`max_len`) and echo suppression (`echo = false`) for
  password-style entry; the terminating newline is still echoed. The `timeout`
  is a whole-line deadline (Ctrl-C aborts); a raised-TPL block-forever read is
  discouraged for the same reason as `axl_console_read_key`.
- **`axl_stdin_is_interactive()`** (`<axl/axl-stream.h>`) — true when the
  shell's StdIn is an interactive console rather than a redirected file/pipe.
  Detected via `EFI_SHELL_PROTOCOL.GetFileSize` (a file/pipe reports a size; the
  console pseudo-file rejects the query). Works from a resident shared-driver
  too (the stdio bridge carries the launcher's real StdIn handle).

### Changed

- **`axl_stdin` now routes prompt-vs-pipe automatically (POSIX tty semantics).**
  When StdIn is redirected (`cmd | tool`, `tool < file`) `axl_stdin` /
  `axl_readline` / `axl_stdin_text` read the captured bytes byte-for-byte, as
  before. When StdIn is the **interactive console** they transparently fall back
  to the new console line editor, so an interactive prompt "just works" with no
  consumer change. **Behavior change:** a read of `axl_stdin` at an interactive
  console now **blocks until Enter** (canonical, echoed, line-buffered) instead
  of returning EOF immediately. Redirected and no-shell-params (BDS) contexts
  are unchanged — the latter still returns EOF and never blocks on a keyboard.
  This also applies to stdin-reading tools (`grep`, `cat`, `sed`, `hexdump`,
  `clip`) invoked with no redirection: they now wait for a typed line at an
  interactive console instead of seeing immediate EOF (matching POSIX
  tty-vs-pipe semantics). Use `axl_console_read_key` for raw keystrokes with no
  line assembly.

  **Unattended scripts:** "interactive console" is not "a human is present." An
  automated `startup.nsh` at boot has StdIn = the console but nobody typing, so a
  bare stdin read (`do -f`, `grep`, …) blocks until the boot timeout rather than
  returning EOF. In a script, redirect the input (`tool < in`) or gate the read
  on `axl_stdin_is_interactive()` and skip it (read-if-piped-else-EOF).

## 2.7.1 — 2026-07-04

### Added

- **Sibling-only shared-driver locate — version-pinned resolution.**
  `axl_shared_driver_locate_sibling(name, driver_filename, out_iface)` (and its
  turnkey wrappers `axl_shared_driver_run_sibling` + the
  `AXL_SHARED_DRIVER_LAUNCHER_SIBLING(name, file)` macro) resolve a resident
  driver, else cold-load the driver from the **launcher's own directory only**
  and **hard-fail** (`AXL_NOT_FOUND`) if it isn't staged beside the launcher —
  no `/drivers`, no volume-root, no cross-volume search. For thin launchers that
  must pair with the exact driver co-staged with them (the two halves share a
  cross-image vtable ABI, so a wrong-version driver is a silent-corruption
  hazard). Pinning governs the cold path only: once a driver of that identity is
  resident, the warm short-circuit returns it.

### Added

- **`axl-cc -c --depfile <dest>`** — writes Make dependency file(s) with
  **absolute** dependency paths while compiling the *bare* source(s) unchanged
  (objects stay bit-identical to a no-depfile compile). Neither forwarded gcc
  flag gives that: `-MMD` skips system headers, so it omits the SDK's entirely
  (they arrive via `-isystem`), and `-MD` lists them but records each path as
  gcc *resolved* it — relative for a relative `-I`, absolute for `-isystem` and
  toolchain headers — i.e. a MIXED file, which CMake's
  `add_custom_command(DEPFILE …)` (CMP0116) resolves against the *binary* dir
  and so misses — `--depfile` makes per-object header tracking work under CMake
  without perturbing the Makefile↔CMake bit-parity that bare-source compilation
  provides. One source → `<dest>` is the `.d` path; multiple sources →
  `<dest>` is a directory and each object gets `<base>.d` inside it (one `.d`
  per object, like gcc's bare `-MMD`; a single `-MF` file can't hold N).
  Requires `-c`.

### Changed

- **`axl-cc` / `axl-c++` now forward compiler and linker flags they don't
  consume, instead of mistaking them for source files.** Any single-dash flag
  the driver doesn't recognize is passed to the compiler (both C and C++) —
  e.g. `-O2`, `-g3`, `-pedantic`, and gcc dependency-generation flags
  (`-MMD -MP -MF <f> -MT <t>` …), so per-object incremental builds can emit
  `.d` files. Linker options pass through via `-Wl,<opt>[,<opt>…]` and
  `-Xlinker <opt>`. An unrecognized `--long` option is now a clear
  `unknown option` error rather than a "source file not found". (Previously
  only `-I/-D/-W*/-f*/-std=*` reached the compiler and nothing reached the
  linker.)
- **Default shared-driver search now prefers the co-located sibling.**
  `axl_shared_driver_locate` (and `axl_driver_ensure_with_embedded`) previously
  tried `/drivers/<arch>/<name>` on the image's volume *before* the driver
  staged beside the launcher, so a stale system-location copy could win. The
  sibling (`<image_dir>/<name>`) is now the first candidate; `/drivers/<arch>/`,
  volume-root, and other-volume locations follow as fallbacks.
- **`axl_shared_driver_locate*` return `AXL_NOT_FOUND` (not `AXL_ERR`) whenever a
  usable driver vtable can't be obtained** — not resolvable from any candidate
  path or the embedded blob, or a driver that loads but doesn't start / publish
  the expected protocol. `AXL_ERR` is now reserved for invalid arguments
  (`axl_shared_driver_locate_sibling` also returns `AXL_INVALID` for a non-bare
  filename). The `_sibling` and multi-path variants report the same code for the
  same condition. Callers testing `!= AXL_OK` are unaffected.

### Fixed

- **Shared-driver stdio bridge: closed the handle-reuse false-alive in the
  liveness gate.** The 2.7.0 gate compared a recorded
  `EFI_LOADED_IMAGE_PROTOCOL*` stored in the bridge's own (freed, recyclable)
  memory, so a relaunched thin launcher that exited via `gBS->Exit()` could
  have its handle *and* that recorded pointer recycled together — spuriously
  matching, letting a resident driver read a prior launcher's
  stdin/stderr/exit-status (wrong *data*, never a crash — the 2.6.1
  use-after-free was already fixed). The gate is now an active per-dispatch
  monotonic token held in driver-resident memory: the driver installs a small
  dispatch-token cell at publish, and each dispatch the launcher stamps a fresh
  `GetNextMonotonicCount()` into both its bridge and that cell — a bridge is
  live only when its token equals the cell's current value. Because the
  reference lives outside the recyclable bridge, correlated pool recycling can
  no longer forge a match. Internal only — no API change, and the
  `AxlStdioBridge` layout is unchanged (the token reuses the former
  recorded-pointer slot). A version-skewed shared-driver pair degrades to EOF
  (no bridging), never wrong data; build both halves against the same SDK, as
  before.

## 2.7.0 — 2026-07-04

### Changed

- **`axl_stderr` now writes the error console (`gST->StdErr`), not stdout.**
  `2>` redirection captures stderr and a plain `>` no longer does — matching
  POSIX. Diagnostic logging (`axl_log` / `axl_warning`) moved to stderr for
  the same reason, so `tool > out.txt` keeps `out.txt` free of AXL log lines.
  Scripts that scraped logs from a `>`-redirected file must use `2>`.

### Added

- **`axl_stderr_raw`** — binary stderr over the shell StdErr handle (sibling
  of `axl_stdout_raw`); works in a resident shared-driver via the stdio bridge.
- **`axl_shared_driver_apply_exit_status()` in `<axl/axl-shared-driver.h>`** — a
  resident driver verb's `axl_set_exit_status(N)` is reflected across the stdio
  bridge; the launcher calls this after dispatch to exit with `N` verbatim
  (`%lasterror%`). Also: raw stdout now works in a resident driver via the
  stdio bridge (sibling of the raw-stderr fallback).
- **Turnkey shared-driver ergonomics** — `AxlSharedDriverVtable` (standard
  `int run(int,char**)` entry, receiving the launcher's argv verbatim like
  `int main`), the `AXL_SHARED_DRIVER(name,init,run,unload)` driver macro, the
  `AXL_SHARED_DRIVER_LAUNCHER(name,file,embed)` and
  `AXL_SHARED_DRIVER_LAUNCHER_THIN(name,file)` (no-embed, disk-only) launcher
  macros, and `axl_shared_driver_dispatch` / `axl_shared_driver_run`. A
  shared-driver launcher collapses to one macro and the driver to three
  functions; the SDK owns resolve + stdio bridge + exit-status. Note: the UEFI
  Shell truncates an error-class (`ENCODE_ERROR`) exit status to its low bits in
  `%lasterror%` (small-int / success-class statuses survive intact). See
  `docs/AXL-Shared-Driver-Recipe.md`.

## 2.6.4 — 2026-07-03

### Fixed

- **`axl_input_attach_mouse` dropped the scroll wheel on firmware that routes
  the pointer through ConsoleInHandle.** `attach_mouse` binds every
  `EFI_SIMPLE_POINTER` handle ConsoleInHandle-first (so a virtual / BMC
  remote-console pointer is seen). But when the physical pointer is aggregated
  onto ConsoleInHandle by the ConSplitter, reading that aggregator's
  `GetState` first consumes the physical child's queued state while dropping
  `RelativeMovementZ` — so buttons and motion arrived but every scroll notch
  was silently eaten. The mouse now reads physical pointer handles before the
  ConsoleInHandle aggregator (which carries the wheel), so `MOUSE_WHEEL`
  events are delivered again; a virtual pointer published directly on
  ConsoleInHandle is still read (it comes last, and the physical handles are
  idle when only the virtual pointer moved). `attach_touch` is unchanged — the
  AbsolutePointer aggregator has no equivalent wheel-dropping behavior.

## 2.6.3 — 2026-07-03

### Fixed

- **`axl_volume_enumerate` reported the wrong `fsN` mapping.** It named each
  volume `fs<i>` from its position in `LocateHandle(SimpleFileSystem)` order,
  which need not match the UEFI Shell's own `fsN` numbering. The two agreed
  only by luck; any remap — a `mkrd` ramdisk, a USB hot-plug, or simply a
  second mounted filesystem — desynced them, so the SDK bound the wrong
  handle/device-path to a name and downstream tools printed the wrong type and
  volume label for a filesystem (e.g. `fs3`/`fs4` swapped after `mkrd`).
  `AxlVolume.name` now comes from the shell's device-path→map lookup
  (lowercased `fsN`), so name, handle, and device path all refer to the same
  volume the user and `map` / `vol fsN:` see; it falls back to the positional
  name only for a volume the shell has not mapped yet. Volume-name matching in
  the driver loader (`axl_driver_load_sibling` and friends) is now
  case-insensitive to stay robust against the shell reporting `FSn:` while a
  caller path uses `fsn:`.

## 2.6.2 — 2026-07-01

### Fixed

- **Shared-driver unload: cross-image synthesized-device-path leak.** A
  shared-driver launcher (a transient `do.efi` that loads a resident
  `doDriver.efi`) leaked a UEFI handle in `dh` on every unload/reload cycle.
  A buffer/embedded `LoadImage` has no firmware device path, so AXL
  synthesizes a `LoadedImageDevicePath` and must uninstall it before
  `gBS->UnloadImage`. That cleanup used a process-local table populated by
  the *loading* image, but in the launcher pattern a *different* image
  unloads the driver — so the synthesized device path was never removed and
  its now-image-less handle accumulated (`gBS->UnloadImage` still returned
  `EFI_SUCCESS` and reclaimed the image itself, so it looked benign). The
  cleanup record now lives on the driver's image handle as a private
  protocol, so whichever image unloads the driver can find and release it.
- **Shared-driver stdio bridge: dead instances no longer accumulate.** A
  launcher that skips its CRT0 atexit uninstall — an `axl-cc
  --minimal-runtime` image, or one that calls `gBS->Exit()` — left its stdio
  bridge protocol installed, and each fresh launcher image couldn't see prior
  images' handles, so the leaked bridges piled up in `dh` one per invocation.
  Dead-launcher bridge instances are now reaped (via the `launcher_image`
  liveness gate) at the start of every bridge install and in
  `axl_shared_driver_unload`, so they can't accumulate regardless of how a
  launcher terminates. (The `ea3b67d2` fix already made a stale bridge *safe*
  to consult; this stops the handles themselves from leaking.)

## 2.6.1 — 2026-06-23

### Fixed

- **Shared-driver stdio bridge: use-after-free on a stale bridge.** A
  resident driver's stdin consult dereferenced whatever bridge instance
  `LocateProtocol` returned first. If a launcher exited without uninstalling
  its bridge (any exit path that skips the CRT0 atexit uninstall), that stale
  instance — older, so returned first — carried a dangling pipe
  `SHELL_FILE_HANDLE`, and a subsequent warm pipe read `#GP`'d in the shell.
  The driver-side lookup now enumerates every bridge instance, skips (and
  uninstalls) any whose launcher image has exited via the `launcher_image`
  liveness gate, and reads the newest live one — so a leaked bridge can no
  longer crash a warm read.

## 2.6.0 — 2026-06-23

### Added

- **Shared-driver stdio bridge.** `axl_shared_driver_locate` now
  transparently bridges the launcher's StdIn into the resident driver, so a
  driver verb's `axl_readline(axl_stdin)` reads the launcher's piped (`|a`)
  / `<`-redirected / interactive input — no per-tool code required (an
  internal stdio-bridge protocol is installed by `axl_shared_driver_locate`
  and uninstalled on launcher exit via `axl_atexit`; the driver-side backend
  getter consults it when the driver image has no shell parameters of its
  own). Output redirection (`>`) already worked via the shell's ConOut
  handoff; this release adds StdIn bridging and documents the full
  transparent-stdio guarantee.
- **`axl_shared_driver_install_stdio_bridge()` in `<axl/axl-shared-driver.h>`**
  — public escape hatch that installs the same stdio bridge for launchers
  that resolve the resident driver themselves (warm-path
  `axl_protocol_find_guid`, `axl_driver_load_sibling`, an embedded-blob
  fallback, a custom `--reload` chain) instead of through
  `axl_shared_driver_locate*` (which still installs it automatically). Call
  it once from the launcher before dispatch; returns `AXL_OK` on install or
  when there are no shell handles to bridge, `AXL_ERR` only on install
  failure. See `docs/AXL-Shared-Driver-Recipe.md`.

## 2.5.0 — 2026-06-23

### Added

- **`<axl/axl-fw.h>` — AxlFw raw firmware-image parser.** Parses a raw `.fd`
  / SPI-flash image into an owned FV → FFS → section tree (`axl_fw_open` /
  `axl_fw_close`, cursor walk `axl_fw_root` / `axl_fw_node_first_child` /
  `axl_fw_node_next_sibling`, accessors `axl_fw_node_kind` / `_type` / `_guid`
  / `_data` / `_offset`, and `axl_fw_find`). Decodes GUIDED-LZMA and
  uncompressed COMPRESSION sections and nested firmware volumes. The offline,
  raw-bytes sibling of the runtime `<axl/axl-fv.h>`; backend-free, so the same
  source builds for UEFI and the host.
- **`fwtool` — firmware-image inspection CLI** (`list` / `extract` / `find`),
  built both as a UEFI app and as a host binary. The host build extracts the
  UEFI Shell from an OVMF/AAVMF image byte-identically to the Python
  `extract-fv-shell.py`, and is now the preferred Shell-extraction tier in the
  run-qemu harness (shedding the python3 dependency where a C toolchain is
  present; Python and `uefiextract` remain as fallbacks).
- **`AXL_COMPRESS_LZMA` in `<axl/axl-compress.h>`** — LZMA "alone" (`.lzma` /
  EDK2 GUIDED-LZMA) encode + decode, backed by the vendored public-domain
  7-Zip LZMA SDK (`deps/lzma/`).
- **`axl_guid_equal(a, b)` in `<axl/axl-sys.h>`** — readable GUID equality test
  (the 2-arg wrapper over `axl_guid_cmp`). Plus EFI_GUID-typed
  `axl_efi_guid_cmp` / `axl_efi_guid_equal` for pure-UEFI code (generated UEFI
  header).
- **`AXL_REGEX_BRE` in `<axl/axl-regex.h>`** — POSIX Basic-RE syntax (the
  grouping / interval / alternation / `+` / `?` metacharacters in their
  backslashed forms; bare forms literal), with the common GNU-BRE extensions.
  Default stays ERE.
- **`axl_memchr()` in `<axl/axl-str.h>`** — `memchr` semantics (first byte
  equal to `c` in the first `n` bytes, or NULL).
- **`sed` — stream-editor tool** (POSIX + common GNU): `s///` with flags and
  alternate delimiters, line / `$` / `/re/` addresses and ranges with `!`,
  hold space, `N` / `n` / `d` / `p` / `a` / `r` / `y`, and the `-n` / `-e` /
  `-f` / `-s` / `-z` / `-E` options. Input reads are bounded (64 MiB/record)
  for untrusted input; `-z` reads NUL-delimited records.

### Changed

- **BREAKING: `axl_guid_cmp` now returns `int`, strcmp-style** (0 if equal, a
  negative/positive value for a stable byte-lexicographic ordering) instead of
  `bool` (true on equal). The old boolean form silently inverted the reading of
  every call site (`if (axl_guid_cmp(a, b))` meant "if equal"). Replace such
  uses with the new `axl_guid_equal(a, b)`, or compare `== 0`. All in-tree
  consumers were updated.

## 2.4.0 — 2026-06-22

### Added

- **`AxlEmbeddedImageInfo` + `axl_driver_load_buffer_with_image_info` /
  `axl_shared_driver_locate_with_image_info`.** Let a caller give a
  buffer/embedded driver load a real loaded-image identity (file name,
  optional `DeviceHandle`, optional Vendor() GUID prefix). `<axl/axl-driver.h>`,
  `<axl/axl-shared-driver.h>`.
- **`axl_driver_load_sibling(file_name, &handle)`.** Loads a driver
  restricted to the running app's own directory (resolved from
  `axl_app_image_path()`); rejects any `file_name` containing `/`, `\\`,
  or `:` (`AXL_INVALID`) so it cannot escape the app directory, and
  returns `AXL_NOT_FOUND` if no such file is staged beside the app. The
  on-disk load gives it a real `MEDIA_FILEPATH` device path. `<axl/axl-driver.h>`.

### Fixed

- **Buffer/embedded driver loads no longer leave a NULL device path, so
  the aarch64 shell's `dh -p` / `dh -v` don't fault on them.**
  `axl_driver_load_buffer` calls `gBS->LoadImage` with `DevicePath=NULL`,
  which left `LoadedImage->FilePath` and the handle's
  `gEfiLoadedImageDevicePathProtocol` interface NULL. The aarch64 shell
  dereferences the device-path pointer while rendering the handle and
  raised a `Synchronous Exception` (x64's DEBUG shell hit an assert
  too). AXL now reads the image's `ImageBase`/`ImageSize` back from
  `EFI_LOADED_IMAGE_PROTOCOL` and synthesizes a
  `MemoryMapped(...)/FilePath("\\<name>")` device path, installing it as
  the loaded-image device path and pointing `FilePath` at the file node.
  The plain `axl_driver_load_buffer` and `axl_shared_driver_locate` get
  this automatically (the leaf name defaults to the driver/app name), so
  existing consumers are fixed with no source change. `axl_driver_load_buffer`
  now also clears `*out_handle` to NULL on argument-validation failure,
  matching its documented contract and the new `_with_image_info`/sibling
  entry points.

### Changed

- **UEFI images are now ~⅔ smaller** — a trivial app dropped from ~91 KB to
  ~30 KB. Two compounding causes are fixed: (1) `axl-cc` did not pass
  `--gc-sections` at link, so consumer builds did no dead-code elimination at
  all; and (2) every `.efi` links `ld -shared`, which exported every `axl_*`
  symbol into the dynamic symbol table and made it a `--gc-sections` root, so
  even where gc ran it could drop nothing. A UEFI image needs no exported
  symbols (the firmware enters via the PE header), so a linker version script
  (`scripts/efi-localize.ver`) now localizes everything but the entry point,
  and `axl-cc` passes `--gc-sections`. Together they let dead code finally be
  collected: `.text` ~50 KB → ~17 KB, `.dynsym` ~9.3 KB → 48 B. Applies to
  every image type (app, driver, `--minimal-runtime`), both arches; validated
  across the full unit suite, drivers, C++, and async/TLS network tools.
- **The tier-1 resource registry stores a per-entry destructor** instead of
  switching on resource kind, and the runtime calls the default event loop
  through function pointers armed on first use. This removes the always-linked
  registry/cleanup's static references to `axl_loop_free` / `axl_event_free` /
  etc., so `--gc-sections` can now drop the entire event-loop subsystem
  (~8 KB `.text` + 6 KB `.bss`) from an app that never creates a loop. Internal
  refactor; no API or behavior change.

## 2.3.1 — 2026-06-22

### Fixed

- **`run-qemu.sh` now stages a UEFI Shell with zero external
  dependencies, so `startup.nsh` runs on a stock box even with a guest
  NIC.** Previously, on a host with no EDK2 build, no distro Shell
  package, and no `uefiextract` (e.g. a stock Ubuntu / WSL / CI box --
  Ubuntu's `ovmf` ships no standalone Shell and `uefiextract` is not an
  apt package), `find_shell_efi` found nothing, so no
  `EFI/BOOT/BOOTX64.EFI` was staged, the disk boot failed, and a guest
  NIC attached via `--qemu-arg` sent OVMF to PXE, whose IPv4/IPv6
  timeouts consumed the whole `--timeout` budget before any shell ran.
  The 2.3.0 distro-Shell tier did not help where no such package exists
  (Ubuntu). Added a dependency-free extractor (`scripts/extract-fv-shell.py`,
  Python stdlib only) that pulls the Shell PE32 out of the OVMF/AAVMF
  firmware volume itself -- decompressing the LZMA-wrapped DXE volume and
  walking the FV/FFS/section tree -- producing byte-identical output to
  `uefiextract` with nothing to install. `find_shell_efi` now uses it
  before falling back to `uefiextract`. The staged shell makes the ESP
  disk boot (which OVMF orders before PXE) succeed, so `startup.nsh` runs
  regardless of NIC/PXE. Verified with the consumer's exact smoke test
  (virtio-net NIC via `--qemu-arg` + custom `--nsh`) on a host with the
  distro Shell and `uefiextract` both removed.

## 2.3.0 — 2026-06-22

### Added

- **`<axl/axl-tpm.h>` — PCR-bound TPM2 seal/unseal.** `axl_tpm_seal` /
  `axl_tpm_unseal` seal a small secret (e.g. a TLS private key) to the
  chosen SHA-256 PCRs and recover it only under the same measured-boot
  state, over `EFI_TCG2_PROTOCOL.SubmitCommand`: `CreatePrimary`
  (deterministic ECC SRK) -> `PCR_Read` (PolicyPCR digest computed in
  software) -> `Create` (keyedhash sealed object) for seal; `Load` ->
  `StartAuthSession` -> `PolicyPCR` -> `Unseal` for unseal, returning
  `AXL_DENIED` when the measured state changed. The blob is opaque
  ciphertext the caller persists. Validated against swtpm
  (`test-tpm-seal-qemu.sh`); the shared TPM2 marshaling moved to an
  internal header (`src/tpm/axl-tpm-internal.h`). Gates SoftBMC's
  TLS-key-at-rest.

- **Auth-hardening primitives: PBKDF2, constant-time compare, SCRAM-SHA-256.**
  Three standalone (no-`AXL_TLS`) additions for password auth:
  `axl_pbkdf2_hmac_sha256` (RFC 8018, in `<axl/axl-digest.h>`, layered on
  `axl_hmac`); `axl_consttime_equal` (`<axl/axl-crypto.h>`) — OR-accumulate,
  no early exit, for every secret/MAC/token compare; and a new
  `<axl/axl-scram.h>` server-side **SCRAM-SHA-256** engine (RFC 5802 / 7677,
  plain `n,,`, no channel binding): `axl_scram_sha256_derive` (enrollment →
  `{salt, iterations, StoredKey, ServerKey}`, never the password) plus a
  two-step `axl_scram_server_first` / `axl_scram_server_final` whose
  serializable `AxlScramState` parks across the two HTTP requests of a login.
  The proof is verified in constant time; a wrong proof or nonce returns
  `AXL_DENIED` with no oracle. A matching client engine
  (`axl_scram_client_first` / `_final` / `_verify`, with `AxlScramClientState`)
  authenticates to a SCRAM server and verifies the server signature
  (mutual auth) — for a client tool / agt / tests. Both sides are pinned to
  the RFC 7677 `user`/`pencil` vector (byte-exact client-first, server-first,
  client-final incl. proof, and server-final) and RFC 7914 PBKDF2 vectors,
  plus a full client<->server round-trip; 25 unit assertions on both arches.
  Unblocks SoftBMC's SCRAM browser login.

- **`<axl/axl-hii.h>` — UEFI HII setup-form reader.** Enumerate the
  platform's BIOS-Setup form sets and get/read/write their questions
  without touching the raw HII protocols or decoding IFR. The module
  locates the HII database/string/config-routing protocols, exports every
  form package once, walks the IFR opcode stream into a cached typed model,
  and projects each setting as an `AxlHiiQuestion` (ONE_OF, CHECKBOX,
  NUMERIC, STRING — with per-type options / min-max-step / size). API:
  `axl_hii_available`, `axl_hii_formset_count`, `axl_hii_formset_get`
  (title, help, form-set GUID as an `AxlGuid`, and question count),
  `axl_hii_question_get`, `axl_hii_question_read`, `axl_hii_question_write`,
  `axl_hii_question_read_string`, `axl_hii_question_write_string`.
  Integer values read/write via `GetVariable`/`SetVariable` for EFI-backed
  stores, falling back to the config-routing/config-access path
  (`ExtractConfig`→`ConfigToBlock` / `BlockToConfig`→`RouteConfig`) for
  driver-private block stores; STRING questions use the `_string` pair,
  which converts the `CHAR16` field at the question's offset to/from UTF-8.
  The IFR struct
  layouts are byte-packed hand-written types in `axl-uefi-extra.h` (cast
  directly onto the raw firmware byte stream). Tested against live OVMF /
  AAVMF HII in QEMU (enumeration, a known form set's ONE_OF + NUMERIC
  projection, and a benign read→write→restore round-trip of the platform
  resolution setting). Unblocks SoftBMC's remote BIOS-Setup tab.

### Fixed

- **`run-qemu.sh` boots the UEFI Shell again on hosts without
  `uefiextract`.** A discovery rewrite (`ddcc63b6`) had reduced
  `find_shell_efi` to a `uefiextract`-only chain, so a host that lacks
  `uefiextract` staged no `EFI/BOOT/BOOTX64.EFI`; the disk boot option
  then failed and — when a NIC was attached via `--qemu-arg` — OVMF's
  IPv4/IPv6 PXE attempts timed out and starved the shell before
  `startup.nsh` ran, so the run hung until `--timeout`. Restored the
  system-package tier: `find_shell_efi` now searches the distro Shell
  paths (`/usr/share/edk2/ovmf/Shell.efi`,
  `/usr/share/edk2-shell/<arch>/Shell.efi`,
  `/usr/share/qemu/edk2-*-shell.efi`, …) before the `uefiextract` tier,
  so no external tool is needed where a distro ships a Shell. Fixes the
  QEMU smoke test for consumers (e.g. axl-utils) that pass a virtio-net
  NIC and a custom `--nsh`.

## 2.2.1 — 2026-06-21

### Fixed

- **`axl_console_mirror_inject_text` now maps the terminal Backspace byte
  (0x7f DEL) to UEFI backspace (0x08)**, so Backspace works in a browser-driven
  UEFI shell. xterm.js (and most terminals) send 0x7f for the Backspace key,
  but the decoder injected it as `UnicodeChar=0x7f`, which the shell ignores —
  backspace did nothing. It now remaps 0x7f→0x08 in the ASCII path, exactly as
  UEFI `TerminalDxe` does (the Delete *key* still arrives as the CSI `3~`
  escape and decodes to `SCAN_DELETE`, unaffected). Regression test:
  `inject_text: 0x7f (terminal Backspace) -> UEFI backspace 0x08` (a new test
  seam drives the real decoder without installing the mirror). SoftBMC
  RemoteShell needs no change.

- **crt0 now zeroes `.bss` itself — fixes garbage zero-init globals on firmware
  that doesn't zero NOBITS pages** (a regression from the `.bss`-as-NOBITS
  change). That change relied on the UEFI loader to zero-fill the new
  uninitialized `.bss` section, but the GCC crt0 never cleared it, so on
  firmware that hands back dirty `AllocatePages` memory every zero-initialized
  static booted with garbage — e.g. axl-input's `static MouseSource` →
  pointer/scroll stuck (the keyboard path, with different state, survived). The
  crt0 `_start` (the PE entry for every image — apps and drivers) now zeroes
  `[_bss, _bss_end)` before relocation and any C code; the loader's zero-fill is
  no longer relied upon. New firmware-independent guard `make check-bss-clear`
  (disassembles `_start`, asserts it clears both bounds) wired into CI — it
  fails deterministically if the clear is ever removed, unlike the boot probe
  which only reflects one firmware's behavior. This also closes the real-HW
  caveat noted on the original `.bss` change: correctness no longer depends on
  loader zero-fill.

- **`axl_input_attach_mouse` now delivers a virtual / BMC remote-console
  pointer's motion and scroll wheel.** It bound a single `EFI_SIMPLE_POINTER`
  via `axl_input_locate_physical_pointer`, which deliberately *skips*
  `gST->ConsoleInHandle` — but that is exactly where a virtual pointer
  (`axl_virtual_pointer_*`, `also_simple`) and a BMC remote-console mouse
  publish, so `axl_virtual_pointer_scroll()` and relative motion never reached
  an `attach_mouse` consumer (zero `AXL_INPUT_MOUSE_WHEEL` / `MOUSE_MOVE`
  events), even though the AbsolutePointer/`attach_touch` twin worked.
  `attach_mouse` now binds **every** SimplePointer handle **ConsoleInHandle
  first** (`collect_pointers`, the same model `attach_touch` already used),
  registers a `WaitForInput` source per handle, and re-resolves each handle's
  interface per dispatch; `detach_mouse` removes them all. Unblocks SoftBMC
  RemoteKvm mouse-wheel and AGT app wheel support. Regression:
  `test_virtual_pointer_e2e_mouse_wheel`.

### Changed

- **Produced `.efi` images now carry `.bss` as a real uninitialized PE
  section** instead of folding it into `.data`. The linker scripts
  historically merged `.bss`/`COMMON` into the file-backed `.data` output
  section (a stale "the EFI loader doesn't like a .bss section" workaround),
  so every zero-initialized global was materialized as literal zero bytes in
  the image — an N-byte static array added N bytes to the `.efi`. `.bss` is
  now its own NOBITS output section emitted (via `objcopy -j .bss`) as an
  `IMAGE_SCN_CNT_UNINITIALIZED_DATA` section with `SizeOfRawData == 0`; the
  UEFI loader allocates and zero-fills it at load. Bss-heavy binaries shrink
  for free (an 8 MiB-static-array probe: 8.5 MB → ~110 KB; `AxlTestNet`
  −317 KB), with no change to typical small-bss binaries and no per-tool
  discipline needed. Validated on OVMF + AAVMF (both arches): a probe asserts
  the section is zero-initialized and writable at runtime, and the full unit
  suite passes (6964/6964) since every binary uses the new layout.
  Regression: `test-bss-probe-qemu.sh` (size ceiling + boot).

## 2.2.0 — 2026-06-18

### Added

- **Text-console mode enumeration + selection** (`<axl/axl-console.h>`) — the
  surface the UEFI Shell's `mode` command exposes, and the graphics-free peer
  of the AxlGfx display-mode API. `axl_console_text_mode_count`,
  `axl_console_text_query_mode`, `axl_console_text_current_mode`,
  `axl_console_text_find_mode`, `axl_console_text_max_mode`, and
  `axl_console_text_set_mode` enumerate the active console's character-cell
  geometries (80x25, 100x31, ...) and switch between them, returning geometry
  as `AxlConsoleTextMode {index, columns, rows}`. Robustness matches and
  exceeds the GOP mode API: the signed `MaxMode`/`Mode` fields are guarded (a
  non-positive count clamps to 0; an unset current mode `-1` or an
  out-of-range current mode is surfaced as `AXL_ERR`, never a bogus index),
  and the inventory-walking `find`/`max` helpers skip modes whose `QueryMode`
  legally fails — which real OVMF/AAVMF exercise (their graphics console
  reports an in-range text mode the firmware doesn't support). Verified by a
  live enumerate + switch + restore round-trip under a virtual GPU
  (`test-console-text-mode-qemu.sh`) on both arches; the `-nographic` unit
  suite locks the enumerate + NULL-arg + bounds contract.
- **`axbench` — AP-pool benchmark tool** (`tools/axbench.c`). Measures
  `AxlTaskPool` (Application-Processor offload) vs the BSP across 8 scenarios
  — topology, pool spin-up, dispatch latency, a compute-bound break-even
  sweep, a granularity sweep, a bandwidth-bound (box-blur) sweep,
  BSP-participates, and a summary verdict — adapting to the worker count and
  writing the report to stdout or a file. Ctrl-C aborts cleanly (the
  orchestration loops poll the shell break and tear down the AP workers + bench
  buffers via an `axl_atexit` hook; `test-axbench-ctrlc-qemu.sh`). Real-HW
  validated on a dual-socket 96-core box (W=95): 192 ns dispatch, compute-bound
  ~95x at 99% of the worker ceiling, bandwidth-bound NUMA-capped at ~9x. The
  decision it informed lives in `docs/AXL-Concurrency.md` ("AP offload").

### Fixed

- **`AxlTaskPool` torn-read race** in `axl_task_pool_available()` /
  `axl_task_pool_submit()`. The slot's `task`/`done`/`running` flags were read
  as three separate volatile loads, so a worker completing concurrently
  (`done` 0→1 then `running` 1→0) between the `done` and `running` reads made a
  just-completed slot read as idle — `available()` over-reported and `submit()`
  could clobber an unreaped completion, dropping a task and hanging the caller's
  wave (observed on a 95-AP machine). The three flags are now one atomic `state`
  word (FREE/SUBMITTED/DONE), so the read is never torn: `available()` is exact
  and `submit()` cannot clobber. Regression: `test-task-pool-mp-qemu.sh`
  (QEMU `-smp 4`, 4000 waves).
- **`AxlTaskPool` completion-store ordering race + `axl_task_pool_done()`
  predicate** — a worker's result store could become visible after its
  done-flag, and `done()` could report a mid-execution slot as finished; both
  are now fenced/ordered correctly.

## 2.1.0 — 2026-06-18

### Added

- **Launch the firmware-embedded UEFI Shell from a Firmware Volume** — present
  a real UEFI Shell with no `Shell.efi` staged on any filesystem.
  **`axl_shell_launch_fv(load_options, &exit)`** (`<axl/axl-shell.h>`) locates
  the platform's built-in ShellPkg Shell in a readable FV and runs it in the
  foreground (same blocking contract as `axl_shell_launch`).
  **`axl_shell_locate()`** reports where a Shell is available —
  `AXL_SHELL_FILE` (a staged `Shell.efi`), `AXL_SHELL_FIRMWARE` (FV-embedded),
  or `AXL_SHELL_NONE` — without launching, for a consumer's availability flag.
  The reusable primitive underneath, **`axl_image_run_fv_file(name_guid, args,
  &exit)`** (`<axl/axl-image.h>`), loads + runs any
  `EFI_FV_FILETYPE_APPLICATION` from a Firmware Volume by its FFS file name
  GUID. The Shell search matches both `gUefiShellFileGuid`
  (`7C04A583-…`, how OVMF/AAVMF and most platform FDFs name the embedded Shell)
  and the ShellPkg `Shell.inf` FILE_GUID (`EA4BB293-…`). Verified under
  OVMF/AAVMF (both embed the Shell as an FV application): the FV Shell launches
  and holds the foreground while a background HTTP server keeps serving off the
  driver tick. **Note:** the FV file name GUID is `gUefiShellFileGuid`, *not*
  the `Shell.inf` module GUID a standalone `Shell.efi` binary carries — the
  launcher matches both so it works regardless of how the firmware embeds it.
- **IP4Config2-free network bring-up** — `axl_net_init` / `axl_net_auto_init`
  now transparently fall back when the firmware lacks
  `EFI_IP4_CONFIG2_PROTOCOL` (some OEM laptops, e.g. HP, ship a full
  SNP/MNP/IP4/TCP4 + `Dhcp4ServiceBinding` stack but not the IP4Config2 policy
  layer). The ladder: IP4Config2 → `EFI_DHCP4_SERVICE_BINDING`
  (CreateChild→Configure→Start, child kept alive to hold the lease) →
  `EFI_PXE_BASE_CODE_PROTOCOL.Dhcp` (last resort). The leased address is cached
  so `axl_net_get_ip_address` / `axl_net_get_dhcp_lease` report it without
  IP4Config2. **`axl_net_last_config_method()`** (new) returns which rung
  succeeded (`AxlNetConfigMethod`: IP4CONFIG2 / DHCP4_SB / PXE_BC / NONE).
  One-shot, in-process scope (matches SoftBmcDiag, the tool this lets netcfg
  replace); durable cross-process config without IP4Config2 would need a
  resident driver. The Dhcp4-SB/PXE paths are real-hardware-only — OVMF always
  provides IP4Config2, so QEMU/CI can't trigger them.
- **`axl_net_takeover_if_no_snp()`** (`<axl/axl-net.h>`) — orchestrated NIC
  takeover, gated on zero SimpleNetwork handles: a no-op (returns AXL_OK) when
  the firmware already exposes SNP, so it can't destroy a working stack; only
  when SNP is absent does it load staged drivers and, if still absent,
  disconnect the firmware drivers from network-class PCI controllers and
  rebind. Added `EFI_PXE_BASE_CODE_PROTOCOL` to the generated UEFI headers.

### Changed

- **Produced `.efi` images now advertise NX-compatibility** (the `NX_COMPAT`
  bit in the PE `DllCharacteristics`). The images were already linked W^X-clean
  (R-X `.text`, RW non-executable `.data`); `pe-set-debug` now sets the bit so
  firmware that enforces memory protection — the norm under Secure Boot —
  applies it instead of warning on or rejecting the image. No source or
  linker-script change; signing remains a downstream step. New `make
  check-nx-compat` gate (`scripts/check-pe-nx.py`, wired into CI) asserts the
  bit on a representative app + driver; `libaxl.a` now depends on the PE
  post-processor so a `pe-set-debug` change relinks every dependent `.efi`.

### Added

- **RTC-write API: `axl_time_set_realtime()` and `axl_time_set_unix()`**
  (`<axl/axl-time.h>`) — the write counterpart to `axl_time_realtime()`, over
  `EFI_RUNTIME_SERVICES.SetTime`. `axl_time_set_realtime` programs an
  `AxlRealtime` (fields interpreted identically to the read path, including the
  `AXL_TIME_TZ_UNSPECIFIED` sentinel and the daylight flag);
  `axl_time_set_unix` is the ergonomic NTP path — it splits Unix seconds (UTC)
  to a calendar date and writes the RTC as UTC, rejecting pre-epoch and
  beyond-year-9999 input up front. Backend-neutral via a new
  `axl_backend_set_time`. Round-trip QEMU test: `test-time-qemu.sh` (sets a
  known calendar time and a known Unix instant, reads each back via
  `axl_time_realtime`, and restores the clock). Unblocks SoftBMC's Time/NTP
  clock-set routes.

- **MAC-keyed DHCP-lease accessor: `axl_net_get_dhcp_lease_by_mac()`**
  (`<axl/axl-net.h>`) — the robust multi-NIC counterpart to
  `axl_net_get_dhcp_lease(nic_index)`. The index form resolves the NIC via the
  IP4Config2 handle buffer (a different index space than
  `axl_net_list_interfaces` / `axl_net_get_link_stats`, which index
  SimpleNetwork) and clamps an out-of-range index to handle 0, so a list-index
  lookup can return the wrong NIC's lease on multi-NIC hosts. The by-MAC form
  correlates IP4Config2 to the NIC by its SimpleNetwork MAC — the same way
  `axl_net_list_interfaces` already resolves IPv4 — and returns AXL_ERR (no
  clamp) for an unknown MAC. The `nic_index` form's doc now carries an explicit
  index-space warning. Round-trip coverage in `test-netdiag-qemu.sh` (by-MAC
  lease equals the index-0 lease byte-for-byte; unknown MAC -> AXL_ERR).

## 2.0.1 — 2026-06-17

### Fixed

- **A large single HTTP GET (a whole multi-MB body in one `axl_http_get`) no
  longer fails/wedges** (regression in v2.0.0). When the sync client started
  wrapping the async core, the async receive path capped a Content-Length
  response **body** at 1 MiB, so a ~1 MB single GET — e.g. `gBS->LoadImage`
  reading a whole `.efi` over an AxlFsProvider-over-HTTP mount — returned
  `AXL_ERR` mid-body, wedging the caller's read loop. The async core now
  pre-reserves the response buffer to exactly the declared Content-Length in one
  allocation (mirroring the pre-v2.0.0 path) and lets the body grow to its full
  size, bounded only by a generous 256 MiB OOM-by-declaration ceiling; the old
  1 MiB constant now correctly guards only header accumulation. Also hardened the
  TLS receive path to complete a body even when the server coalesces the final
  TLS record and the close-notify alert into one TCP segment. Regression test:
  `test-http-async-qemu.sh` now GETs a 1.5 MiB body in one call (http + https)
  and verifies it byte-for-byte. Reported by the axl-webfs mount driver.

- **A raised-TPL sync HTTP GET with a `Connection: close` response no longer
  leaks the socket** (pre-existing since v2.0.0). When the sync client wraps the
  async core, the per-request connection drop ran `axl_tcp_close` from inside the
  ephemeral loop's own dispatch; the async close registered a completion event on
  that loop, which is freed the moment the request returns — so at a raised TPL
  (the `gBS->LoadImage`-over-HTTP context, where the firmware notify is starved)
  the close never finalized, orphaning a loop source and leaking the socket +
  close context. `axl_tcp_close`'s synchronous fallback now completes the close
  **loop-free** (drives `tcp4->Poll()` + polls the close event directly) whenever
  it is reached from inside a loop callback, instead of spinning up a nested
  `axl_loop_new()` — which both leaked the source and would re-introduce the
  nested-loop re-entrancy warning the async model removed. The genuine async
  close path (a persistent service loop) is unchanged; it drains between pump
  ticks. Regression test: `test-http-async-qemu.sh` asserts a raised-TPL sync GET
  leaves no orphaned loop source and no `AxlMem` leak.

- **ATA/SATA SMART + IDENTIFY no longer fail intermittently from a deep call
  stack** (`<axl/axl-ata.h>`, new in 2.0.0). `ata_exec()` aligned the data
  buffer to the controller's `Mode->IoAlign` but passed the command (`Acb`) and
  status (`Asb`) blocks as raw stack pointers. EDK2's
  `EFI_ATA_PASS_THRU_PROTOCOL.PassThru` (AtaAtapiPassThru) requires **all three**
  buffers to satisfy `IoAlign` and rejects an unaligned one with
  `EFI_INVALID_PARAMETER` before the device is reached — so whether
  `axl_ata_identify` / `axl_ata_smart` / `axl_ata_self_test_start` worked
  depended on the caller's stack depth: a shallow caller (`smart` CLI) was
  aligned by luck, a deep one (an HTTP request handler several frames down)
  failed. The command/status/data buffers are now all bounced through
  `IoAlign`-satisfying allocations. No API change. Regression test:
  `axl-test-ata.c` drives `_axl_ata_exec` against a fake pass-thru with a
  deliberately misaligned caller `Acb`/`Asb` and asserts the issued packet's
  three buffers are all `% IoAlign == 0`. Reported by the SoftBMC storage module.

## 2.0.0 — 2026-06-16

### Added

- **Storage health — AxlAta, AxlScsi, and the AxlSmart rollup**
  (`<axl/axl-ata.h>`, `<axl/axl-scsi.h>`, `<axl/axl-smart.h>`): Phases 2-4 of
  the storage-access family AxlNvme opened (`docs/AXL-Storage-Design.md`),
  completing `smartctl`-for-UEFI across every transport. **AxlAta** reads
  ATA/SATA identity + health over `EFI_ATA_PASS_THRU_PROTOCOL`: enumerate
  devices (`axl_ata_next`), `axl_ata_identify`, `axl_ata_smart` (the pass/fail
  verdict, temperature, power-on hours, reallocated sectors), the off-line
  `axl_ata_self_test_start` / `_result`, and a raw `axl_ata_passthru`.
  **AxlScsi** does the same over `EFI_EXT_SCSI_PASS_THRU_PROTOCOL`
  (`axl_scsi_inquiry`, `axl_scsi_read_capacity`, `axl_scsi_health`, raw
  `axl_scsi_passthru`). **AxlSmart** is the synthesis: `axl_storage_next` walks
  NVMe controllers, then ATA/SATA, then SCSI back-to-back, and `axl_smart_health`
  reports each as one normalized `AxlSmartHealth` (`axl_storage_get_location`
  gives a stable per-transport key). Each typed reader delegates to pure
  decoders (`axl_ata_decode_*` / `axl_scsi_decode_*` / `axl_smart_from_*`),
  unit-tested against spec-faithful buffers with no device; absent fields carry
  documented sentinels, never guesses. Read-and-health only. The `smart` tool is
  a thin renderer over it.

- **Driver / device / protocol discovery** (`<axl/axl-driver-info.h>`) — the
  UEFI Shell `drivers` / `devices` / `dh` / `devtree` views as a read-only API,
  the companion to lifecycle-authoring `<axl/axl-driver.h>`. Answers the
  recurring "the NIC driver is on the box but unbound and I can't find it"
  question. `axl_driver_list_loaded` enumerates loaded DriverBinding drivers;
  `axl_handle_name` / `axl_protocol_guid_name` / `axl_net_protocol_name` name
  handles + protocol GUIDs; `axl_pci_to_handle` + `axl_pci_driver_bound` map a
  PCI function to its controller and report what manages it; `axl_driver_bind`
  does a targeted ConnectController; and `axl_handle_list` / `_protocols` /
  `_drivers` / `_children` / `_parents` are the handle/protocol enumeration
  behind a Devices tab + parent/child devtree walk. One fixed-buffer truncation
  contract (NULL to count; `count > cap` signals truncation). No EDK2 types in
  the API.

- **AxlConsoleMirror — real-Shell remote console** (`<axl/axl-console-mirror.h>`)
  — wrap the firmware console (`gST->ConIn`/`ConOut`/`StdErr`) so a loop-owning
  app can host the real UEFI Shell (or any console app) mirrored to + driven
  from a remote terminal, including full-screen apps like `edit`.
  `axl_console_mirror_install` swaps in wrappers that emit console output as a
  terminal byte stream (UTF-8 + ANSI/VT) to a caller sink and push injected
  input into the key queue; `axl_console_mirror_inject_key` / `_inject_text`
  (the latter decodes xterm/VT escapes into UEFI keys), `_set_size`, `_reset`.
  The physical console keeps working in parallel; the mirror creates no pump
  (drive the loop in the background while the foreground app blocks). The
  reusable firmware surgery — the sink-to-WebSocket bridge + RBAC are the
  consumer's.

- **AxlShell + generic foreground launch** (`<axl/axl-shell.h>`,
  `<axl/axl-image.h>`) — `axl_image_run(path, args, &exit_code)` runs any
  blocking UEFI app in the foreground and returns its exit code;
  `axl_shell_launch(&exit_code)` is the Shell-specific policy over it (locate
  `Shell.efi` across the conventional paths, run with `-nostartup` so a child
  Shell from `startup.nsh` doesn't recurse). Pairs with AxlConsoleMirror to put
  the real Shell behind a remote terminal.

- **`axl_net_ping_ex` — TTL / Don't-Fragment ICMP probes** (`<axl/axl-net.h>`) —
  one ICMP echo with explicit `ttl`, `dont_fragment`, and `payload_len`,
  returning an `AxlPingResult` that classifies the reply (echo-reply /
  time-exceeded / unreachable / frag-needed / no-reply), the responding hop, the
  RTT, and a next-hop MTU. The building block for traceroute (increment TTL) and
  path-MTU discovery (DF + large payload). The multi-hop / MTU reply types need
  a real router, so they appear only on hardware.

- **Network diagnostics — SNTP, ARP cache, link stats** (`<axl/axl-net.h>`) —
  `axl_sntp_query` gets the time from an SNTP/NTP server over UDP (RFC 4330,
  Unix seconds + local-RTC offset); `axl_net_arp_list` reads the firmware IPv4
  neighbor cache (`arp -a`); `axl_net_get_link_stats` reports a NIC's media/link
  state from SimpleNetwork (`link_up` authoritative; speed/duplex best-effort).

- **`axl_net_resolve_ptr` — reverse DNS** (`<axl/axl-net.h>`) — the reverse of
  `axl_net_resolve`: the `in-addr.arpa` PTR lookup for an IPv4 over DNS4. "No
  PTR record" is a normal negative (`AXL_ERR`), matching the forward direction.

- **`axl_net_get_dhcp_lease` — the active DHCP lease view** (`<axl/axl-net.h>`)
  — read a NIC's live IP4Config2 config (address, mask, gateway, resolvers) into
  an `AxlDhcpLease`. A purely local synchronous read with no round-trip, and it
  persists across app exits (IP4Config2 is a resident DXE driver). A static-policy
  or unleased NIC returns `AXL_ERR`. Lease lifetimes / granting server / domain
  option are out of scope (IP4Config2 discards them on apply).

- **Static-IP / DNS / hostname config layer** (`<axl/axl-net.h>`) — the ifconfig
  policy group for an on-box network UI: `axl_net_set_dns` programs the
  IP4Config2 resolver list (works on static + DHCP NICs); `axl_net_set_hostname`
  / `axl_net_get_hostname` persist a hostname to an AXL non-volatile variable
  (UEFI has no firmware hostname — this is the single source of truth an on-box
  UI shares); and `axl_net_wait_ip_settled` polls IP4Config2 until an address
  change actually takes (the diagnostic-correct replacement for a blind sleep,
  with a strong "equals these octets" check that closes the read-back-old-IP
  race).

- **NIC driver-selection substrate** (`<axl/axl-net.h>`) — the "my NIC needs a
  different driver" toolkit. `axl_net_get_driver_info` resolves, by MAC, which
  driver image is bound to a NIC, at which layer (NII3.1 / NII / SNP), and the
  NIC's stable PCI/USB location. `axl_net_list_available_drivers` enumerates the
  NIC drivers staged under `drivers/<arch>/` on every mounted volume.
  `axl_net_try_driver` loads + starts one driver, connects the stack, and
  reports the MACs that newly came up — rolling the image back out on failure so
  the next candidate starts clean (try iPXE last). `axl_net_connect_stack`
  exposes the global ConnectController + per-SNP reconnect (for ARM64 firmware
  that doesn't auto-connect).

- **WebSocket per-connection API** (`<axl/axl-http-server.h>`) —
  `axl_http_server_add_websocket_ex` registers an endpoint with the richer
  `AxlWsConnHandler`, which receives the `AxlWsConn` each event is for: per-client
  reply (`axl_ws_send`), identity captured at upgrade (`axl_ws_conn_auth`), peer
  address (`axl_ws_conn_peer`), per-connection state (`axl_ws_conn_set_user_data`
  / `_user_data`), explicit `axl_ws_conn_close`. `AXL_WS_CONNECT` fires after the
  101 (a greeting is valid) and may reject the upgrade (return `AXL_ERR`); the
  upgrade is gated by the route's auth flags. Replaces the broadcast-only
  `AxlWsHandler` when a consumer must address individual clients.

- **Serial byte I/O** (`<axl/axl-serial.h>`) — the read/write peer of the serial
  *enumeration*: `axl_serial_open`, `axl_serial_set_mode` (baud / framing /
  timeout), `axl_serial_write` / `axl_serial_read` (read is non-blocking; "no
  bytes" is not an error), `axl_serial_read_async` (a loop-integrated polling
  receive delivering bytes to a callback), `axl_serial_close`. The first time
  AXL moves bytes over a serial port (the enumeration readers stay
  descriptor-only).

- **`AxlStatus` gains richer, mappable failure codes + EFI translators**
  (`<axl/axl-macros.h>`, `<axl/axl-efi-status.h>`) — `AXL_INVALID`,
  `AXL_NOT_FOUND`, `AXL_DENIED`, `AXL_UNSUPPORTED`, `AXL_NO_RESOURCES`,
  `AXL_IO_ERROR` (−4..−9) give a blessed vocabulary for consumers that need to
  distinguish or translate outcomes (e.g. a status→HTTP map) without coupling to
  `EFI_*`. Additive: `AXL_ERR` remains the generic catch-all and any negative is
  still failure. Two `static inline` translators ship alongside —
  `axl_status_to_efi` / `axl_status_from_efi` — for a driver/protocol boundary
  returning or consuming an `EFI_STATUS`; both are lossy by design (the enums are
  deliberately not numerically aligned, so the generic bucket doesn't
  round-trip).

- **SMBIOS Type 17 exposes form factor, widths, and rank**
  (`<axl/axl-smbios.h>`) — the memory-device record gains `form_factor` (e.g.
  9 = DIMM, 0x0D = SODIMM), `total_width` (data + ECC bits), `data_width` (data
  bits), and `rank`. The 0xFFFF "unknown" width is normalized to 0. Additive —
  existing fields unchanged.

- **Virtual pointer + scroll-wheel injection** (`<axl/axl-input.h>`) —
  `axl_virtual_pointer_install(&vp, cfg)` publishes a synthetic
  `EFI_ABSOLUTE_POINTER_PROTOCOL` (and, with `cfg.also_simple`, a relative
  `EFI_SIMPLE_POINTER_PROTOCOL`) the caller drives:
  `axl_virtual_pointer_inject(vp, x, y, buttons)` reports absolute motion +
  buttons (clamped to the configured range, default = the active GOP resolution
  so an RFB/VNC client maps 1:1), `axl_virtual_pointer_scroll(vp, dy)` injects
  vertical wheel notches (needs `also_simple`), `axl_virtual_pointer_uninstall`
  restores the console. Singleton. Lets a headless remote-KVM consumer drive the
  firmware Setup browser with a mouse.

- **`axl_tcp_connect_timeout` — bounded connect-phase wait** (`<axl/axl-tcp.h>`)
  — the timeout-aware form of `axl_tcp_connect_via`: bound the SYN/handshake with
  an explicit `connect_timeout_ms` instead of the fixed ~10 s, so an unreachable
  host doesn't stall the loop for ten seconds. The timeout is an AXL-side
  deadline that fires on the loop and cancels the connect; `axl_tcp_connect` /
  `_via` are this call with `connect_timeout_ms == 0` (the 10 s default). The
  HTTP client's connect phase uses it too.

### Changed

- **`<axl.h>` is now uefi-free — `AXL_APP` / `AXL_DRIVER` / `AXL_SERVICE_DRIVER`
  emit AXL-native entry points.** The umbrella previously pulled
  `<uefi/axl-uefi.h>`, leaking `EFI_STATUS` / `EFI_*` into every consumer — only
  because the entry-point macros expanded to EFI types. They are rewritten in
  AXL-native types (`AxlEfiStatus`, `AxlHandle`, `AxlSystemTable *`, `AXLAPI`),
  all binary/ABI-compatible with their EFI peers, so the emitted firmware entry
  symbols are byte-for-byte identical and nothing about loading or booting
  changes. **Consumer note:** an app `main` body that referenced the EFI types
  the umbrella used to leak must now include `<uefi/axl-uefi.h>` itself — the
  public API never exposed them; the incidental leak just let some code compile
  without the include.

### Fixed

- **A WebSocket broadcast burst over TLS no longer desyncs the stream.**
  `axl_tls_write_async` ran `mbedtls_ssl_write` (which advances the TLS sequence
  number) before the one-send-in-flight `axl_tcp_send_async`, so a second
  back-to-back broadcast was encrypted then dropped — leaving the TLS state ahead
  of the bytes on the wire and wedging the connection (deterministic on a single
  console keystroke fanning out to ≥3 broadcasts). `axl_tls_write_async` now
  returns `AXL_BUSY` before encrypting if a send is in flight, and a
  per-connection outbound FIFO serializes all outbound WS frames (broadcast /
  `ws_send` / PONG), enqueuing pre-encryption so a drop-on-overflow can't desync.

- **A WebSocket teardown no longer wedges a driver-tick loop.** The WS frame
  handlers did *synchronous* sends (the `WS_OP_CLOSE` echo, a PONG, an
  `axl_ws_conn_close`), each spinning a nested ephemeral `AxlLoop` that can't
  progress at the raised TPL of an `axl_loop_attach_driver` tick — an infinite
  spin. PONGs now send asynchronously and the redundant CLOSE echo is dropped
  (the TCP FIN conveys the close).

- **Inbound WebSocket frames are no longer dropped over TLS, and `AXL_WS_CONNECT`
  fires after the 101.** A WS upgrade over HTTPS could swallow the client's
  frames, and the connect event fired before the 101 was sent (so a greeting
  from it was lost). The TLS path now delivers inbound frames and defers CONNECT
  until the 101 is on the wire (with CONNECT/DISCONNECT correctly paired on the
  deferred-connect / failed-101 paths).

- **The WebSocket 101 handshake now sends `Connection: Upgrade`** (RFC 6455).
  The upgrade response omitted the header, which strict clients and proxies
  require to complete the switch.

- **AxlAta no longer drops every directly-attached SATA device.** The ATA
  enumeration skipped devices on a controller port directly (the common
  non-port-multiplier case), so `axl_ata_next` returned nothing on ordinary SATA
  disks.

### Changed

- **The sync UDP API (`axl_udp_send`, `axl_udp_sendrecv`) now wraps the async
  cores** (`axl_udp_send_async` / `axl_udp_recv_async`) on a private event loop,
  for the same one-I/O-implementation consistency as the HTTP/TCP sync paths —
  removing the duplicate transmit-token building and fragment-gathering. A
  raised-TPL Poll tick drives the socket while the loop blocks (the standalone
  `_axl_udp_wait` helper this replaced is deleted). Behavior is preserved
  (datagram truncation to the caller's buffer, timeouts); one refinement: a sync
  send now respects the socket's one-send-in-flight state.

- **The sync HTTP client (`axl_http_get/post/put/delete/request`) now wraps the
  async core** (one HTTP I/O implementation): each spins a private event loop,
  runs `axl_http_*_async`, and harvests the response, instead of the old
  standalone blocking `do_request`. Behavior is preserved (redirects, chunked
  vs Content-Length, keep-alive, stale-connection retry, source.ip pinning) and
  the sync calls still progress at a raised TPL (a Poll tick drives the client's
  socket while the ephemeral loop blocks). The streaming variant
  (`axl_http_request_streaming` / `_stream_file`) keeps its own I/O — there is
  no async streaming peer. One **behavior refinement**: `timeout.ms` is now an
  **idle/per-phase** bound (re-armed on progress: connect, handshake, send, each
  recv) rather than a whole-operation ceiling — this preserves the old per-op
  semantics so a slow-but-steadily-progressing large transfer no longer trips a
  fixed deadline. Both the sync streaming path and the async core now share one
  overflow-safe request-header builder, which also **retired a latent
  stack-overflow** in the old `do_request` builder (the unsafe `len +=
  axl_snprintf(buf + len, cap - len, ...)` idiom on a 2 KB stack buffer; same
  class as the heap overflow the async builder fixed). No public API change.

### Fixed

- **HTTPS now works when the server is driven by a resident event loop**
  (an AxlService / DXE driver-tick loop via `axl_loop_attach_driver`),
  not only a top-level `axl_loop_run`. The TLS server handshake was
  synchronous — it read with `axl_tcp_recv` and sent the close_notify
  (`axl_tls_free`) with `axl_tcp_send`, each of which spins a nested
  ephemeral `AxlLoop`. A nested loop cannot make progress at the raised
  TPL of a driver-tick dispatch, so the handshake never produced a
  ServerHello (curl hung, exit 28); and the ephemeral loop's source ids
  collided with the outer loop's, silently killing the listener's accept
  source after the first connection (the `adbf5461` hazard, re-exposed at
  close). The accept callback now drives the handshake asynchronously on
  the server's own loop via the new `axl_tls_handshake_async` (recv staged
  + handshake output sent async), and `axl_tls_free` no longer performs a
  blocking close_notify send (the advisory shutdown alert is generated but
  not transmitted — HTTP framing plus the TCP FIN convey the close). Plain
  HTTP was unaffected. Regression: `test-https-driver-qemu.sh` (AxlTestNet
  `serve-tls-driver` — HTTPS under `axl_loop_attach_driver`); the
  `axl_loop_run` path (`test-https.sh`) and the shared-loop multi-server
  case (`test-http-multi-qemu.sh`) still pass. Reported by the axl-webfs
  `serve --tls` resident driver.

- **HTTP bodies larger than one TLS record (16 KiB) now transfer over
  HTTPS instead of dropping the connection.** Three distinct bugs each
  truncated or dropped any body that spanned more than one TLS record
  (curl `rc=52`), in both directions and both transfer encodings:
  (1) **request read** — TLS ciphertext was decrypted in place in the
  same buffer that held the plaintext, so when one TCP read carried the
  tail of one record plus the start of the next, decrypting the first
  record's plaintext clobbered the next record's not-yet-consumed
  ciphertext; reads now stage ciphertext in a dedicated buffer kept
  separate from every plaintext buffer. (2) **response write** —
  `axl_tls_write` / `axl_tls_write_async` issued a single
  `mbedtls_ssl_write`, which emits at most one record, so a response
  body over 16 KiB failed; the write now loops over records (the async
  path accumulates them into one send). (3) **chunked decoding** — a
  chunk whose data exceeded one receive buffer had its continuation
  misparsed as a new chunk size-line; the decoder now carries the
  in-progress chunk's remaining data length and trailing-CRLF state
  across receives. Reported by the SoftBMC port (VirtualMedia image
  uploads over HTTPS). Extends the single-segment fix in the previous
  release.

- **TLS upload no longer deadlocks when a TCP segment carries multiple
  records.** A streaming-upload `PUT` over HTTPS with both
  `Transfer-Encoding: chunked` and `Expect: 100-continue` (what `curl -T -`
  sends by default) hung the client (curl rc=28). `on_conn_data` decrypted
  exactly one TLS record per TCP read, then re-armed a transport recv — but
  a single segment can carry several records (a chunked body's data chunk +
  its `0\r\n\r\n` terminator), so the buffered terminator was never drained
  and the server waited forever on bytes that had already arrived.
  `axl_tls_read`-based reads now drain every buffered record (new
  `axl_tls_pending`) before idling on the transport: the connection
  receive loop iterates over all records a segment yields and owns a single
  re-arm once the TLS buffer empties. As a side effect a TLS record split
  across TCP reads is now reassembled rather than dropping the connection.
  Plain HTTP was unaffected (one read = one segment) and is unchanged.
  Reported by the SoftBMC port (`/dav` over HTTPS).

- **Empty-body responses now carry `Content-Length: 0` on keep-alive
  connections.** A response with no body but a body-permitting status —
  notably `201 Created` from WebDAV `PUT`/`MKCOL`/`MOVE`/`COPY`, and an
  empty `200` from `OPTIONS` — was sent with neither `Content-Length`
  nor `Transfer-Encoding`, so a keep-alive client (davfs2, Finder,
  Explorer, rclone) blocked waiting for a body that never arrived. It
  only worked under `Connection: close` (EOF-delimited). `send_response`
  now emits `Content-Length: 0` for any body-less, non-streaming
  response whose status permits a body (everything except 1xx / 204 /
  304), skipping it when the handler already set its own `Content-Length`
  (a HEAD reports the entity length with an empty body — the two must
  not collide). Reported by the SoftBMC port (`/dav` over keep-alive
  HTTPS).

- **A WebDAV PUT to a read-only mount now returns 405, not 500.** The
  upload framework forced 500 on any chunk-handler abort, ignoring a
  status the handler had set — so a PUT *with a body* to a mount whose
  `write_open` is NULL got 500 while an empty-body PUT correctly got
  405. The chunk-abort path now preserves a handler-set status (default
  500 only if untouched), matching the clean-EOF path, and the WebDAV
  PUT handler sets 405 for a NULL `write_open`.

- **Multiple `axl_http_server` instances on one `AxlLoop` now each
  dispatch.** A second server on a shared loop (e.g. a plain HTTP:80
  redirect alongside a TLS HTTPS:443 server) would bind and accept TCP
  but never produce a response once the first (TLS) server had handled a
  connection. Root cause: the synchronous `axl_tcp_recv` used by the TLS
  handshake registers its cancel source on an ephemeral loop; on the
  completion path the sock kept that stale source id after the ephemeral
  loop was freed, and a later `axl_tcp_close` removed the id from the
  sock's (now restored to the shared) loop — deleting whatever source
  shared that id, which was the second server's accept source. The sync
  `axl_tcp_recv` / `axl_tcp_send` wrappers now clear the per-op source
  ids after the ephemeral loop is freed (mirroring the accept/connect
  wrappers). Reported by the SoftBMC port (HTTPS + HTTP-redirect).

### Added

- **Async HTTP client — `axl_http_get_async` / `axl_http_post_async`**
  (`<axl/axl-http-client.h>`): the loop-integrated peers of the sync
  `axl_http_get` / `axl_http_post`. The whole request (DNS resolve, TCP
  connect, TLS handshake, send, receive, redirects) runs as events on a
  caller-supplied `AxlLoop` with **no nested ephemeral loop**, so it is safe
  to issue from inside a loop callback or a resident driver-pump tick at
  raised TPL (`axl_loop_attach_driver`) — where the sync calls nest a loop
  and trip the synchronous-wait re-entrancy warning. Contract: returns
  `AXL_OK` ⇒ the callback fires later (never re-entrantly); `AXL_BUSY` if a
  request is already in flight on the client (one in flight per client —
  separate clients for concurrency); any other error ⇒ the callback does not
  fire. The callback owns the response (non-2xx is success with a response);
  `cb == NULL` is fire-and-forget. The body is borrowed until the callback
  fires. https requires `axl_tls_init()`. Internally this is now the single
  HTTP I/O implementation that the sync API will wrap (Option A) in a
  follow-up. TLS is reached only through the strippable ops vtable, so a
  plain-HTTP consumer still strips mbedTLS. Peer of the new
  `axl_net_resolve_async` (the sync `axl_net_resolve` already wraps it).
  Driven by `test-http-async-qemu.sh` (GET + POST, http + https, issued from
  a raised-TPL driver tick, asserting zero re-entrancy warnings). Implements
  Items 2–3 of `docs/AXL-Loop-Reentrancy-Plan.md`; tracker in
  `docs/AXL-Async-HTTP-Plan.md`.

- **`AXL_DEBUG_ASSERT` — debug-build invariant guards** (`<axl/axl-debug.h>`).
  `AXL_DEBUG_ASSERT(expr)` / `AXL_DEBUG_ASSERT_MSG(expr, msg)` enforce an
  internal invariant: loud, grep-able log (`AXL_DEBUG_ASSERT FAILED: …`)
  plus a `_axl_debug_assert_count()` test hook on violation, compiled out
  to `((void)0)` under `NDEBUG` (release). Catches a concurrency/lifecycle
  fault at its cause in a debug or test build instead of as a downstream
  symptom. Used internally to guard the TLS write-ordering invariant
  (no seqno advance while a TCP send is in flight). See
  `docs/AXL-Concurrency.md` § "Testing the model".

- **AxlNvme — NVMe identity + SMART/health** (`<axl/axl-nvme.h>`): a
  Platform Access module over `EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL` (the
  `smartctl`-for-NVMe the SDK lacked). Enumerate controllers
  (`axl_nvme_next`) and namespaces, Identify Controller/Namespace, the
  decoded SMART/Health log (`axl_nvme_smart` — healthy, temperature,
  endurance used, power-on hours, data units, media errors), Device
  Self-test (start + poll), and a raw admin pass-thru escape hatch that
  exposes the NVMe Status Field. Read-and-health only: no destructive
  typed command (Format/Sanitize/firmware-download stay raw-only). The
  typed readers delegate to pure public decoders (`axl_nvme_decode_*`),
  unit-tested against spec-faithful buffers with no device; the device
  path is covered by `test-nvme-qemu.sh` (`-device nvme`). `tools/nvme`
  is the dogfood renderer; `tools/mkfixture` now captures NVMe Identify
  through this module. First of the storage-access family
  (NVMe/ATA/SCSI + a normalized health rollup) in
  `docs/AXL-Storage-Design.md`.

- **JOSE — JWS / JWT / JWK** (`<axl/axl-jose.h>`, requires `AXL_TLS=1`):
  signed-token verification and signing for API-token auth, OIDC/identity,
  and signed configuration, built on AxlCrypto. JWS Compact sign + verify
  (`axl_jws_sign` / `axl_jws_verify`), JWT registered-claim validation with
  a caller-supplied clock (`axl_jwt_verify` — `exp`/`nbf` with leeway,
  `iss`, `aud` string-or-array), and JWK / JWK-Set parse, `kid` lookup, and
  public export (`axl_jwk_parse` / `axl_jwks_parse` / `axl_jwks_find` /
  `axl_jwk_export_public`). Algorithms: ES256, ES384, RS256, PS256, HS256
  (JWE is reserved for a later pull). Security model is allow-list-driven,
  never header-`alg`-driven: verification takes a mandatory allow-list,
  `none` is unrepresentable, the HMAC secret and public key are separate
  key fields, and a symmetric+asymmetric allow-list is rejected — so the
  RS256↔HS256 confusion is structurally impossible. The padding scheme is
  bound to the allow-listed `alg`, so a PS256 (RSA-PSS) token never verifies
  under RS256 (PKCS#1 v1.5) and vice versa. JWK import pins the EC curve
  (P-256 / P-384, rejecting off-curve points) and a 2048-bit RSA floor.
  Verified against the RFC 7515 A.1 (HS256) and A.3 (ES256) known-answer
  vectors plus independently cross-checked ES384/PS256 vectors,
  sign/verify round-trips, a rejection matrix, and JWK round-trips
  (`test-jose-qemu.sh`, AXL_TLS=1; the unit suite covers the fail-closed
  branch on both arches). `sdk/examples/jose-demo.c` dogfoods the full
  lifecycle through the SDK consumer path (`test-jose-cc-qemu.sh`). Without
  `AXL_TLS=1`, `axl_jose_available()` returns false and every call fails
  closed.

- **`axl_pk_keygen` / `axl_pk_key_sign` / `axl_pk_key_verify` gain
  ECDSA P-384** (`<axl/axl-crypto.h>`, `AXL_PK_ECDSA_P384`, `AXL_TLS=1`):
  the ECDSA hash follows the curve (P-256 → SHA-256, P-384 → SHA-384) and
  the `AXL_PK_SIG_RAW` signature is r‖s = 96 bytes for P-384. Underpins
  JOSE ES384.

- **`axl_json_value_string`** (`<axl/axl-json.h>`) — read a JSON reader's
  own value as a string (no key lookup), for a sub-reader returned by
  `axl_json_array_next`. The only way to read a bare-string array element,
  such as a member of a JWT `aud` array.

- **`AxlConfigDesc` gains `.min` / `.max` numeric range metadata**
  (`<axl/axl-config.h>`) — trailing `int64_t` bounds (0 = none, like
  `AxlArgDesc`) that `axl_service_main` propagates into the CLI it
  synthesizes from the descriptor table, so the flag validates its range
  (and a settings-UI builder can size a spinner). Synthesis-only, like
  `.short_name` / `.choices`: `AxlConfig` parsing ignores them (no
  clamping on set). Additive — existing descriptor tables are
  unaffected. `sdk/examples/service-demo.c` now declares a `[1,65535]`
  port range, and `test-axl-cc-service.sh` asserts the synthesized CLI
  rejects an out-of-range `--port` at parse time.

- **HTTP `WWW-Authenticate` challenge on 401** (`<axl/axl-http-server.h>`)
  — `axl_http_server_set_auth_challenge(s, scheme, realm)` makes a gated
  route's 401 carry `WWW-Authenticate: <scheme> realm="<realm>"`, so
  interactive clients (browsers, macOS Finder, Windows Explorer) prompt
  for credentials instead of showing a bare 401. Previously the auth gate
  only worked with clients that send credentials preemptively (`curl -u`,
  davfs2 secrets). NULL scheme clears the challenge (the default,
  preemptive-only behavior — backward compatible); scheme/realm are
  rejected if they contain a quote or CR/LF (header-injection guard).
  Requested by the axl-webfs port (`--auth` browser flow).

- **base64url encode/decode** (`<axl/axl-str.h>`) —
  `axl_base64url_encode` (RFC 4648 §5 URL-safe alphabet, unpadded) and
  `axl_base64url_decode` (accepts `-`/`_`, tolerates missing padding,
  rejects standard-base64 `+`/`/` and `=` so a standard-base64 string
  won't silently decode; takes an explicit length, not NUL-terminated).
  The encoding JWS/JWT/JWK and web tokens use; the groundwork for the
  forthcoming `axl-jose` module.

- **TPM 2.0 Endorsement Key public read** (`<axl/axl-tpm.h>`) —
  `axl_tpm_read_ek_pub(buf, buf_size, out_len, out_alg)` plus
  `axl_tpm_ek_available()`. Returns the EK public key's canonical bytes
  (ECC P-256 point X||Y, or RSA-2048 modulus, big-endian) — a per-device,
  hardware-rooted identity stable across reboots and `TPM2_Clear`, for
  attestation / device enrollment / platform binding. The bytes are
  returned raw (no baked-in hashing or policy) so the consumer can hash
  them into its own domain-separated machine id. Derived with
  `TPM2_CreatePrimary` in the endorsement hierarchy using the standard
  TCG EK template (ECC P-256 first, RSA-2048 fallback), marshalled over
  `EFI_TCG2_PROTOCOL.SubmitCommand` (the same protocol the capability
  query uses) — deterministic, works on fTPMs with no EK certificate,
  and persists nothing (the transient primary is flushed). The
  `out`-buffer follows the size-query protocol (NULL `buf` queries the
  size; too-small returns `AXL_ERR` with the required size).
  `axl_tpm_ek_available()` is false when no TPM is present, so a consumer
  can fall back (e.g. to the SMBIOS UUID). Requested by the SoftBMC port
  (hardware-rooted machine identity; licensing policy stays in SoftBMC).

- **Public-key signature verification** (`<axl/axl-crypto.h>`) —
  `axl_pk_verify(alg, pubkey, pubkey_len, msg, msg_len, sig, sig_len)`
  verifies a detached signature over a message against a public key the
  consumer ships. A generic crypto primitive for signed firmware
  updates, signed config blobs, or Secure-Boot-style image checks —
  pure verification, with no signing side (the private key never ships).
  `AXL_PK_ECDSA_P256` (ECDSA over NIST P-256 with SHA-256) is supported:
  `pubkey` is a DER SubjectPublicKeyInfo, `sig` is a DER ECDSA
  signature, and `msg` is hashed with SHA-256 internally. `AXL_PK_ED25519`
  is reserved but unsupported by the current mbedTLS build (it needs PSA
  crypto, which AXL does not enable) and returns `AXL_ERR`. Like
  `<axl/axl-tls.h>`, the real implementation requires an `AXL_TLS=1`
  build; without it verification fails closed and `axl_pk_available()`
  returns false (use it to distinguish "not compiled in" from
  "invalid"). Any non-`AXL_OK` result means "not verified, untrusted".
  Requested by the SoftBMC port (signed-blob verification building
  block; license policy stays entirely in SoftBMC).

- **Public-key key handles: generation, signing, serialization**
  (`<axl/axl-crypto.h>`) — the signing-side peer of `axl_pk_verify`. An
  opaque `AxlPkKey` (private or public) created by `axl_pk_keygen`
  (ECDSA P-256 or RSA-3072), `axl_pk_key_load_private` (PKCS#8 DER), or
  `axl_pk_key_load_public` (SubjectPublicKeyInfo DER); serialized with
  `axl_pk_key_get_private_der` / `_get_public_der`; used by
  `axl_pk_key_sign` / `axl_pk_key_verify` (SHA-256 prehash) with
  `axl_pk_key_alg` and `axl_pk_key_free` (zeroizes, `AXL_AUTOPTR`-able).
  `AxlPkSigFormat` selects the ECDSA signature layout — `AXL_PK_SIG_RAW`
  (fixed-width r||s, what SSH/JWS/COSE use) or `AXL_PK_SIG_DER` (the
  X.509 form the raw-bytes `axl_pk_verify` consumes); ignored for RSA.
  Lets a consumer make and persist a host/identity key, sign, and verify
  peer signatures over AXL's API. Requires `AXL_TLS=1` (mbedTLS; RSA
  keygen pulled in `MBEDTLS_GENPRIME`); without it all calls fail closed.
  Requested by the SoftBMC port (SSH host key + user-auth signatures).

- **Authenticated encryption (AEAD)** (`<axl/axl-crypto.h>`) —
  `axl_aead_seal` / `axl_aead_open`, one-shot AEAD over `AxlAeadAlg`
  (`AXL_AEAD_AES_128_GCM`, `AXL_AEAD_AES_256_GCM`,
  `AXL_AEAD_CHACHA20_POLY1305`) with a 12-byte nonce and 16-byte tag
  (`AXL_AEAD_NONCE_LEN` / `AXL_AEAD_TAG_LEN`). `seal` encrypts and tags
  (ciphertext same length as plaintext, tag returned separately,
  in-place aliasing allowed); `open` verifies before decrypting and
  writes no plaintext on a bad tag (fail closed). The caller supplies a
  fresh nonce per message — the API does not generate nonces. Requires
  `AXL_TLS=1` (mbedTLS; ChaCha20-Poly1305 pulled in `MBEDTLS_CHACHA20_C`
  / `_POLY1305_C` / `_CHACHAPOLY_C`); without it the calls fail closed.
  Requested by the SoftBMC port (SSH transport cipher).

- **AES-CTR stream cipher** (`<axl/axl-crypto.h>`) — a stateful
  `AxlCipher`: `axl_cipher_ctr_new(alg, key, key_len, iv)` (AES-128/256,
  16-byte initial counter), `axl_cipher_ctr_xcrypt` (encrypt or decrypt —
  CTR is symmetric; in-place allowed; the keystream carries across calls
  so a stream can be processed in arbitrary chunks), and
  `axl_cipher_free` (`AXL_AUTOPTR`-able). For an SSH `aes*-ctr` transport
  or any continuous keystream. Requires `AXL_TLS=1` (mbedTLS;
  `MBEDTLS_CIPHER_MODE_CTR`); without it the constructor returns NULL and
  xcrypt fails closed. Requested by the SoftBMC port.

- **ECDH key agreement** (`<axl/axl-crypto.h>`) — ephemeral
  Diffie-Hellman over `AxlEcdh`: `axl_ecdh_new(alg)` (NIST P-256 or
  X25519), `axl_ecdh_get_public` (P-256 SEC1 point `0x04||X||Y`, or
  X25519 32-byte u-coordinate), `axl_ecdh_compute` (32-byte shared
  secret — P-256 X coordinate big-endian, X25519 little-endian per
  RFC 7748), and `axl_ecdh_free` (zeroizes; `AXL_AUTOPTR`-able). For an
  SSH key exchange (`ecdh-sha2-nistp256`, `curve25519-sha256`) or any
  ephemeral-DH handshake; the raw secret must be run through a KDF before
  use. Requires `AXL_TLS=1` (mbedTLS;
  `MBEDTLS_ECP_DP_CURVE25519_ENABLED`); without it the constructor
  returns NULL. Requested by the SoftBMC port.

- **Free-form `key=value` config-file map** (`<axl/axl-config-file.h>`)
  — `AxlConfigFile`: parse a `key=value` text file (`#` comments, blank
  lines, values trimmed) into a flat string map with typed getters
  (`axl_config_file_get` / `_get_uint` / `_get_int` / `_get_bool`) that
  fall back to a caller default for any missing or unparseable key, plus
  `_set` and `_save`. A missing file yields an empty map (not an error),
  so every lookup returns its default. This is the open-vocabulary
  counterpart to descriptor-bound `AxlConfig`, which validates keys
  against a fixed table and rejects unknown ones — use `AxlConfigFile`
  when keys aren't known at compile time (a module config where features
  invent their own `prefix.key` names). Requested by the SoftBMC port
  (its `Core/Config` module + `session_timeout`).

- **`axl_log_ring_clear`** (`<axl/axl-log.h>`) — empty an attached log
  ring in place, leaving it attached and ready to receive new messages
  (no detach / free / recreate churn). NULL-safe. Requested by the
  SoftBMC port (a dashboard "clear logs" / `DELETE /api/logs` action).
  The `axl_log_ring_get` doc now also spells out that the returned
  `message`/`domain` borrow the ring's single shared scratch buffer
  (valid only until the next `get`), so a serializing loop must copy
  each entry out and not log mid-iteration.

- **Register a pre-populated image buffer as a typed RAM disk**
  (`<axl/axl-ramdisk.h>`) — `axl_ramdisk_register_image(image, size,
  AXL_RAMDISK_DISK | AXL_RAMDISK_CDROM, &dev_path)` registers a
  page-aligned, caller-owned buffer via EFI_RAM_DISK_PROTOCOL **without
  formatting** (the image carries its own filesystem) as either a raw
  `gEfiVirtualDiskGuid` disk or an El Torito `gEfiVirtualCdGuid` CD-ROM,
  then connects controllers so the firmware binds it and the device
  becomes bootable. `axl_ramdisk_unregister(dev_path)` detaches it. The
  backing memory is caller-owned: not copied, must outlive the
  registration, and the caller frees its `axl_alloc_pages` buffer after
  unregistering (contrast `axl_ramdisk_create` / `axl_ramdisk_destroy`,
  which own and free the FAT disk they allocate). Requested by the
  SoftBMC port (the VirtualMedia / "mount an uploaded ISO" feature).

- **Filesystem-backed WebDAV file server** (`<axl/axl-http-server.h>`)
  — `axl_http_server_serve_fs(s, prefix, fs_root, flags, auth_flags)`
  mounts a mounted volume (or a subtree) as a read/write WebDAV server
  in one call, plus `axl_fs_webdav_ops()` / `axl_fs_root_new` for
  consumers that want to wrap or extend it. The generic `AxlWebDavOps`
  glue maps every callback to an `<axl/axl-fs.h>` primitive (list →
  `axl_dir_*`, stat → `axl_file_info`, GET → `AxlFileView`, PUT →
  `AxlFileWriter`, MKCOL → `axl_dir_mkdir`, DELETE →
  `axl_file_delete`/`rmdir`, MOVE → `axl_file_move`, COPY → streamed
  read+write, recursive for deep collections), so a consumer no longer
  hand-writes the ~13 callbacks. Traversal-contained: a `..` escaping
  `fs_root` is rejected (404). Flags `AXL_SERVE_FS_READONLY` /
  `NO_DELETE` / `NO_OVERWRITE` gate mutations (forbidden verbs answer
  405; a refused overwrite is 409). The `auth_flags` argument
  (`AXL_ROUTE_*`) gates the whole mount — including the streaming PUT —
  via the server's auth callback. Requested by the SoftBMC port
  (replaces ~474 lines of hand-rolled WebDAV).

- **Auth-gated WebDAV and upload routes** (`<axl/axl-http-server.h>`)
  — `axl_http_server_add_webdav_auth` and
  `axl_http_server_add_upload_route_auth`, the `_auth` siblings of
  `add_webdav` / `add_upload_route`. Streaming uploads bypass the
  normal dispatch path, so their `auth_flags` were previously
  unenforceable; the auth callback now runs before the first body byte
  (a failed check is 401, an admin route presented a lesser role is
  403). A WebDAV mount's PUT rides that same upload path, so the whole
  mount is now gateable. Requested by the SoftBMC port (it can require
  login/admin on `/dav` and on streaming-upload endpoints).

- **GOP-inventory accessors** (`<axl/axl-gfx-surface.h>`,
  `<axl/axl-gfx-types.h>`) — `AxlGfxOutput` gains `framebuffer_size`
  (GOP `Mode->FrameBufferSize`, distinct from the base address);
  `axl_gfx_output_query_mode(output_index, mode_index, AxlGfxOutputMode*)`
  enumerates a *specific* output's modes (the existing
  `axl_gfx_query_mode` reads only the active GOP, so a multi-monitor
  consumer couldn't list a non-active output's modes), each mode
  carrying its own `pixel_format`; and
  `axl_gfx_output_get_pixel_bitmask(output_index, AxlGfxPixelBitmask*)`
  reads a non-active output's channel masks (the per-output peer of
  `axl_gfx_get_pixel_bitmask`). For faithful `/api/hwinfo/display`
  reporting on real heterogeneous-GOP hardware. Requested by the
  SoftBMC port.

- **Streaming file writer** (`<axl/axl-fs.h>`) — `AxlFileWriter` /
  `axl_file_writer_open` / `_write` / `_tell` / `_close`, the
  out-of-core write peer of `AxlFileView`. Writes a file incrementally
  without buffering the whole payload in RAM (a WebDAV PUT or a multi-GB
  upload), unlike the whole-buffer `axl_file_set_contents`.
  Replace/truncate-in-place by default; `AXL_FILE_WRITER_APPEND` and
  `AXL_FILE_WRITER_EXCL` (create-only, for PUT `If-None-Match`) flags.
  `_close` returns the final flush status (a failed flush must surface
  as 5xx, not 201) and there is deliberately no AXL_AUTOPTR binding so
  that status can't be silently dropped. Backed by a new
  `axl_backend_file_set_size` (SetFileInfo truncate).

- **FAT RAM disk library** (`<axl/axl-ramdisk.h>`) — the
  create/list/destroy orchestration from the `mkrd` tool, promoted to a
  reusable module so consumers don't copy it. `axl_ramdisk_create()`
  allocates, FAT16/FAT32-formats, and registers a RAM disk via
  `EFI_RAM_DISK_PROTOCOL` (idempotent on the label, returns the device
  path); `axl_ramdisk_destroy()` unregisters and frees by label;
  `axl_ramdisk_list()` enumerates registered RAM disks into
  `AxlRamDisk[]`. `axl_ramdisk_ensure_driver()` wraps the
  firmware→disk→embedded-RamDiskDxe fallback (the embedded blob is a
  caller-supplied parameter, so `libaxl.a` carries no driver bytes).
  `mkrd` is now a thin CLI over the module. Requested by the SoftBMC
  port, whose VirtualMedia feature creates a RAM disk at startup. (The
  registered device path embeds the backing physical address, which
  varies run to run — an inherent limit, not something the API fixes.)

### Changed

- **BREAKING: loop source ids are now a 64-bit `AxlSourceId`, not
  `uint32_t`.** The `axl_loop_add_*` / `axl_loop_remove_source` API, and
  the source-id-returning wrappers (`axl_input_attach_{mouse,key,touch}`,
  `axl_cursor_attach[_ex]`,
  `axl_compositor_attach_{pointer,touch,keyboard,frame_clock}`), now use
  `AxlSourceId` (a `uint64_t` typedef in `<axl/axl-loop.h>`; 0 still means
  "no source"). Ids are also now drawn from a single **process-global**
  monotonic counter rather than a per-loop one, so a stale id can never
  collide with a source on another loop — closing the class of bug where
  a sync wrapper's ephemeral-loop source id was later removed from a
  different loop and silently deleted an unrelated source (the `adbf5461`
  second-server-on-a-shared-loop dead-accept). 64-bit means the counter
  never wraps in any realistic lifetime. **Consumer action:** store
  returned source ids in `AxlSourceId` (or `uint64_t`), not `uint32_t` —
  a `uint32_t` holder silently truncates once ids exceed 2^32 and then
  fails to remove the source. Passing an id *into* `axl_loop_remove_source`
  is unaffected. `axl_defer` / `axl_pubsub` handles are unchanged
  (`uint32_t`, loop-local).

- **`https` HTTP *client* consumers must now call `axl_tls_init()` once at
  startup** before issuing `https://` requests. mbedTLS is now strippable
  from plain-HTTP clients: the always-linked HTTP client no longer
  statically references `axl_tls_*` (it routes through an ops vtable that
  `axl_tls_init()` registers), so a consumer that only speaks `http://`
  and never references TLS lets `ld --gc-sections` drop all of mbedTLS
  (~280 KB) — a plain client drops from ~562 KB to ~228 KB. https clients
  error clearly if `axl_tls_init()` wasn't called. `axl_http_server_use_tls`
  already calls it, so servers are unaffected; the `fetch`/`rfbrowse`/
  `mkfixture` tools call it automatically (and follow an `http`→`https`
  redirect by initializing TLS unconditionally, fatal only when the URL
  itself is https).

- **`axl_acpi_next` / `axl_acpi_find` now surface the FADT's DSDT and
  FACS** (`<axl/axl-acpi.h>`) — these hang off the FADT
  (`FirmwareCtrl`/`Dsdt`), not the RSDT/XSDT, so the previous walk
  omitted them. The catalog now folds them in (preferring the 64-bit
  extended pointers) and yields them **first** — FACS then DSDT, ahead
  of the RSDT/XSDT tables — matching the order
  `EFI_ACPI_SDT_PROTOCOL.GetAcpiTable` / `acpidump` /
  `/sys/firmware/acpi/tables` use, so a consumer diffing against that
  oracle needs no reordering. The iteration is the complete table set.
  The DSDT is a
  normal SDT; the FACS is surfaced with only its `signature`/`length`
  valid (it is not a standard SDT — `axl_acpi_checksum_ok` does not
  apply). Lets the SoftBMC `/api/hwinfo/acpi` route drop its
  FADT-pointer prologue. (`MAX_ACPI_TABLES` raised 64→128 with an
  overflow warning so the late-appended children can't be silently
  dropped on table-heavy servers.)

### Added

- **TPM 2.0 presence and capability** (`<axl/axl-tpm.h>`) —
  `axl_tpm_present()` reports whether the firmware publishes the TCG2
  protocol; `axl_tpm_get_capability()` calls `GetCapability` and
  projects `EFI_TCG2_BOOT_SERVICE_CAPABILITY` into a typed
  `AxlTpmCapability` (TPMPresentFlag, structure/protocol versions,
  manufacturer ID, max command/response sizes, PCR-bank count, and the
  supported + active hash-algorithm bitmasks). A singleton reader (no cursor):
  the protocol is located once and cached. When the protocol is absent
  it returns `AXL_ERR` and the consumer reports the TPM as not present
  (the QEMU-default `{"present":false}` golden). The TCG2 protocol +
  capability struct are hand-written in `axl-uefi-extra.h` (TCG-spec
  types, not UEFI/PI); the GUID is pinned in `axl-tpm.c`. Requested by
  the SoftBMC port for its `/api/hwinfo/tpm` route. The populated path
  is validated by a new swtpm-backed integration test
  (`test/integration/test-tpm-qemu.sh`).
- **Firmware-volume enumeration** (`<axl/axl-fv.h>`) —
  `axl_fv_next()` is a cursor over the firmware's
  `EFI_FIRMWARE_VOLUME2_PROTOCOL` handles returning an `AxlHandle`;
  `axl_fv_get_attributes()` decodes the volume's current read/write/lock
  status into a typed `AxlFvAttributes`, and `axl_fv_count_files()`
  reports the file count from a `GetNextFile` walk (clean end is
  success; a hard read error part-way returns `AXL_ERR` rather than a
  truncated count; an empty volume is `AXL_OK` with `0`). Same
  image-lifetime handle cache and position-from-handle cursor as
  AxlBlock / AxlSerial; device-path text reuses the existing
  `axl_handle_get_protocol(h, "device-path", ...)`. Read-only inventory
  probe — no file contents or sections are read. The FV2 protocol is
  hand-written in `axl-uefi-extra.h` (the spec HTML's typedef is
  mangled) and its GUID is pinned in `axl-fv.c`. Requested by the
  SoftBMC port for its `/api/hwinfo/fv` route.
- **Serial-port enumeration** (`<axl/axl-serial.h>`) —
  `axl_serial_next()` is a cursor over the firmware's
  `EFI_SERIAL_IO_PROTOCOL` handles returning an `AxlHandle`;
  `axl_serial_get_mode()` reads the port's current line settings into a
  typed `AxlSerialMode` (`baud_rate`, `data_bits`, `parity`,
  `stop_bits`, `timeout`, `receive_fifo_depth` — `parity`/`stop_bits`
  are raw enum codes the consumer names), and
  `axl_serial_get_control()` decodes the modem control/status lines
  (`cts`, `dsr`, `ri`, `dcd`, `hw_flow_control`) via the protocol's
  GetControl. Same image-lifetime handle cache and position-from-handle
  cursor as AxlBlock; device-path text reuses the existing
  `axl_handle_get_protocol(h, "device-path", ...)`. Read-only
  descriptor probe — no port is opened and no byte I/O is performed.
  `SERIAL_IO_MODE` + `EFI_SERIAL_IO_PROTOCOL` are now generated from the
  UEFI spec. Requested by the SoftBMC port for its `/api/hwinfo/serial`
  route.
- **Block-device enumeration** (`<axl/axl-block.h>`) —
  `axl_block_next()` is a cursor over the firmware's
  `EFI_BLOCK_IO_PROTOCOL` handles (disks, partitions, CD-ROMs, RAM
  disks) returning an `AxlHandle`; `axl_block_get_media()` reads that
  device's `EFI_BLOCK_IO_MEDIA` into a typed `AxlBlockMedia`
  (`media_id`, `removable_media`, `media_present`, `logical_partition`,
  `read_only`, `write_caching`, `block_size`, `last_block`). The handle
  set is located once and cached for the image lifetime; position is
  recovered from the handle the caller passes back, so independent
  walks do not interfere. Device-path text reuses the existing
  `axl_handle_get_protocol(h, "device-path", ...)` +
  `axl_device_path_to_text()` — no new API. Fields are raw readouts
  (geometry valid only when `media_present`); the consumer derives
  capacity and device type. Requested by the SoftBMC port for its
  `/api/hwinfo/storage` route. (`EFI_BLOCK_IO_MEDIA` is now generated
  from the UEFI spec; the thin `EFI_BLOCK_IO_PROTOCOL` wrapper is
  hand-written in `axl-uefi-extra.h` because the spec HTML's struct
  closer carries a stray-space typo the generator can't match.)

## 1.8.0 — 2026-06-11

### Added

- **USB device-info, endpoint-count, and port-topology accessors**
  (`<axl/axl-usb.h>`) — `axl_usb_get_device_info()` returns a curated
  `AxlUsbDeviceInfo` (bcdUSB, device class/subclass/protocol,
  bNumConfigurations); `axl_usb_get_num_endpoints()` returns an
  interface's bNumEndpoints; `axl_usb_get_port_info()` surfaces the
  hub-port chain AxlUsb already parses — the immediate parent-port plus
  a `'.'`-joined root-first port path (e.g. `"4.1"`, `lsusb -t` shape).
  Device-level fields are raw (a composite device's `device_class` is 0;
  fall back to `axl_usb_get_class`), keeping policy in the consumer.
  Requested by the SoftBMC port for its `/api/hwinfo/usb` route.
- **Processor topology reader** (`<axl/axl-cpu.h>`) — `axl_cpu_topology()`
  enumerates the machine's logical processors over
  `EFI_MP_SERVICES_PROTOCOL`, filling a caller-sized, index-keyed
  `AxlCpuProcessor` array with each processor's physical location
  (package / core / thread) and status flags (bootstrap, enabled,
  self-test healthy), plus decoupled total / enabled counts. Single
  query-then-fill idiom (`out == NULL` reports counts only). On
  single-processor firmware that does not publish MP services it reports
  the uniprocessor floor (`total == enabled == 1`, no per-CPU entry)
  rather than failing or fabricating status. Headless mechanism: a
  consumer "CPU inventory" view formats the array itself, with no EFI
  types in app code. Requested by the SoftBMC port (its `/api/hwinfo/cpu`
  route, replacing a direct `EFI_MP_SERVICES_PROTOCOL` reach).

### Fixed

- **Absolute-pointer drain coalescing no longer drops clicks**
  (`axl_input_set_touch_drain`) — the v1.7.1 drain kept only the *last* of the
  states it read, so a full press+release (or a press buried behind a motion
  backlog) that landed within one drained batch was coalesced away and the
  click was lost. The drain now collapses only pure-motion runs and **stops at
  any contact transition**, processing the press/release before continuing.
  Motion compresses (killing the iDRAC/BMC "lags seconds behind" backlog),
  button edges never do — so a consumer can raise the drain even where clicks
  matter. The coalescing policy is now a unit-tested pure helper
  (`axl_input_touch_coalesce`) driven by a scripted state sequence.
- **`axl-cc --help` / `-h` printed `ar`'s usage instead of its own** — the
  help text used an unquoted heredoc (`<<HELP`), so the backticked
  `` `ar rcs` `` in the staged-build example ran as a command substitution
  and `ar` printed its usage. The delimiter is now quoted (`<<'HELP'`).
- **`EFI_PROCESSOR_INFORMATION` was undersized by its `ExtendedInformation`
  tail** — the generated UEFI header modeled the member as `void *`
  (8 bytes) instead of the spec's embedded `EXTENDED_PROCESSOR_INFORMATION`
  union (24 bytes, wrapping `EFI_CPU_PHYSICAL_LOCATION2`), so the struct was 16 bytes
  short of what EDK2-derived firmware writes through `GetProcessorInfo` — a
  latent stack-buffer overflow for any caller passing the struct by value
  (including the MP-services task-pool enumerator). The manifest now declares
  the union so the struct carries its full firmware footprint.

## 1.7.1 — 2026-06-10

### Fixed

- **`axl_set_exit_status` now works under `--minimal-runtime`** — the v1.7.0
  exit-status feature patched only the standard CRT0
  (`axl-crt0-native.c`); the minimal entry point (`axl-crt0-minimal.c`,
  linked by `axl-cc --minimal-runtime`) still collapsed `main`'s return to
  `EFI_SUCCESS` / `EFI_ABORTED`, so a minimal-runtime tool that armed a status
  and returned from `main` got `0x15` instead of the armed value. This is
  exactly the thin-launcher case the feature's split-image note describes.
  The minimal CRT0's return path now resolves through the same
  `axl_backend_resolve_exit_status`, symmetric to the native one (it does NOT
  route through `axl_exit`, which is unsound under the minimal runtime — no
  `_axl_init`, so the cleanup registries are absent). `test-exit-status-qemu.sh`
  now builds and asserts a `--minimal-runtime` selftest alongside the
  full-runtime one (the gap that let this regress).

## 1.7.0 — 2026-06-10

### Added

- **Exit with an arbitrary `EFI_STATUS`** (`<axl/axl-signal.h>`) —
  `axl_set_exit_status(AxlEfiStatus)` and `AXL_NORETURN
  axl_exit_status(AxlEfiStatus)`. The runtime otherwise collapses every
  nonzero `main` return / `axl_exit(int)` to `EFI_ABORTED` (0x15); these let
  a tool exit with a caller-chosen, **verbatim** status — including
  non-error-class codes (top bit clear, e.g. `0x34`) — so the UEFI shell's
  `%lasterror%` reflects the exact value (the `do err <N>` parity case). Both
  exit paths honor it (a normal `return` from `main` via CRT0, and
  `axl_exit`), and **cleanup is preserved** (atexit LIFO + tier-1 resource
  sweep still run). The pending status is per-image (set in the calling
  image's libaxl instance) — under a thin-launcher + resident-driver split,
  call it in the image whose `main`/CRT0 returns to the firmware, or plumb
  the value back; the docstring spells this out.
- **Opt-in compact flag syntax** (`<axl/axl-args.h>`) — a new
  `bool compact_flags` field on `AxlArgsNode`. Set on the root, the flag
  tokenizer additionally accepts a DOS / legacy-CLI option style tree-wide:
  a colon value separator (`--name:value`, `-x:value`), an attached short
  value (`-xvalue`, `/xvalue`), and a `/` short-flag prefix (`/x`, `/x:value`,
  `/sVarName`) — so a tool porting a legacy CLI can drop its hand-rolled
  pre-stripper. Opt-in; default false = strict GNU-style parsing (unchanged).
  `/` introduces a single-char short flag only (no long `/name`); flag values
  keep their case. Only the root node's flag is consulted (like
  `case_insensitive`).

## 1.6.0 — 2026-06-10

### Added

- **Opt-in case-insensitive verb matching** (`<axl/axl-args.h>`) — a new
  `bool case_insensitive` field on `AxlArgsNode`. When set **on the root
  node**, verb / sub-verb name matching is case-folded throughout the whole
  command tree (`do CDUMP` == `do Cdump` == `do cdump`; `do bios MAP` ==
  `do BIOS map`), matching a legacy CLI that is wholly case-insensitive (the
  Dell `do` tool). Default false = exact-case matching (unchanged), so
  existing tools are unaffected. **Only verb names are relaxed** — positional
  values, flag values, and flag names keep their original case (`sysid D`,
  `-o:File.txt`); an `AXL_ARG_CHOICE` positional still honors its own
  `choices_case_insensitive`. Node names display in their declared case in
  `--help`; only the match is folded.

## 1.5.1 — 2026-06-10

### Fixed

- **Console / log output is now pure ASCII** — Unicode punctuation in
  emittable string literals (chiefly the em-dash `U+2014`, plus a few
  arrows `U+2192`) drew as a white block on a UEFI text console. The
  driver-locate debug line (`driver_try_candidates`: `hit: <path> —
  attempting`) was the reported case; an audit found ~88 more across the
  library, tools, and examples (log messages, `axl_printf` output, AxlArgs
  `.help` strings). All replaced with ASCII (`-`, `->`). Comments are
  untouched (never reach the console). Added `scripts/check-output-ascii.py`
  — a C tokenizer that flags non-ASCII inside string/char literals (ignoring
  comments; deliberate UTF-8 fixtures opt out with an inline `ascii-allow`
  marker) — wired into CI's lint job and `make check-ascii`, with a `--fix`
  mode, so this can't regress.

## 1.5.0 — 2026-06-10

### Fixed

- **`AxlArgs` help is now pure ASCII** (`<axl/axl-args.h>`) — the
  auto-generated tree-help header separated a node's name from its
  description with a Unicode em-dash (`U+2014`), which renders as a white
  block on a UEFI text console (no UTF-8). The separator is now an ASCII
  `-` (e.g. `do - Dell hardware-diagnostic CLI`). The one renderer
  (`print_help_for`) serves both the root header and every per-node /
  sub-verb header, so all generated help is clean. A renderer test asserts
  no non-ASCII byte appears in generated help (root + sub-verb).

### Added

- **`AxlCursor` tracks the absolute pointer too** (`<axl/axl-cursor.h>`)
  — `axl_cursor_attach` now binds the absolute pointer
  (`EFI_ABSOLUTE_POINTER` — a touchscreen / digitizer / BMC remote-console
  virtual mouse, and QEMU's `usb-tablet` over VNC) in addition to the
  relative mouse, so the convenience cursor path tracks a remote-console /
  VNC pointer correctly instead of mis-tracking or lagging. Once any
  absolute event is seen it is **authoritative** for cursor position (its
  coordinate is mapped onto the scene); the relative mouse drives position
  until then and afterward only contributes button / wheel events.
  - New `axl_cursor_attach_ex(c, loop, cb, data, cfg)` + `AxlCursorConfig`
    to choose which source(s) to bind (`skip_mouse` / `skip_touch`) and
    configure the absolute read path (method / **ConsoleIn-only** / poll
    interval / drain) in one call — a zero-initialized config binds both
    with library defaults. `axl_cursor_detach` tears down whichever sources
    were bound. **Behavior change:** existing callers gain the absolute
    binding and claim the single process-wide absolute-pointer slot — a
    saved source ID no longer fully tears the cursor down (use
    `axl_cursor_detach`), and a consumer that separately binds the absolute
    pointer (`axl_compositor_attach_touch` / `axl_input_attach_touch`)
    should pass `cfg.skip_touch = true`.
- **`pointer-tune-demo`** (`sdk/examples/pointer-tune-demo.c`) — a live
  bench for the remote-console absolute-pointer "catch-up lag": drag a
  pointer and retune every absolute-read lever at runtime with a keypress
  — drain `N` (1/2/4/8/16), read method (EVENT_AND_POLL / EVENT_ONLY /
  POLL_ONLY), poll interval (10/20/30/50 ms), and **ConsoleIn-only** — with
  a HUD (live config, cursor position, abs/rel counts, events/sec) and a
  fading trail that makes the lag appear under a single-read drain and
  vanish once it coalesces. `make pointer-tune-demo`.

### Changed

- **Absolute-pointer capture now defaults to ConsoleIn-only**
  (`<axl/axl-input.h>`) — `axl_input_set_touch_config`'s `console_only`
  default flips from `false` (bind every `EFI_ABSOLUTE_POINTER` handle) to
  `true` (bind only `gST->ConsoleInHandle`). On the firmware this matters
  for — UEFI ≥ 2.30 that multiplexes pointers, including a BMC remote-console
  virtual mouse, through ConIn — that one handle carries the live events, and
  binding only it avoids double events from a separate physical handle that
  mirrors the same device. So `axl_input_attach_touch`, the `AxlCursor`
  attach path, and the compositor seat all default to ConsoleIn-only now.
  A platform whose absolute pointer is published ONLY on a separate physical
  handle must opt back in with `axl_input_set_touch_config(.., console_only =
  false, ..)`, or, via `AxlCursorConfig`, `cfg.touch_all_handles = true`
  (the cursor config field is the opt-OUT, so a zeroed config still gets the
  new ConsoleIn-only default). Validated as the right path for the BMC
  remote-console target; QEMU/OVMF can't distinguish the two (its only
  absolute handle is the ConIn aggregator), so this is a real-hardware call.
- **Backdrop-blur is cached** (compositor, the dialog veil) — a
  full-screen backdrop-blur surface re-ran a whole-rect blur on every
  present (any damage intersects it), pegging the CPU when content
  animates behind a static veil. The blur is now cached per surface and
  reused while the composited backdrop is byte-identical to the cached
  pre-blur snapshot; a changed backdrop or radius recomputes. Internal
  optimization — `axl_surface_set_backdrop_blur` is unchanged.

## 1.4.0 — 2026-06-09

### Added

- **Regex `NOTBOL` / `NOTEOL` match flags** (`<axl/axl-regex.h>`) —
  `AXL_REGEX_MATCH_NOTBOL` and `AXL_REGEX_MATCH_NOTEOL`, the POSIX
  `REG_NOTBOL` / `REG_NOTEOL` semantics: treat `from_offset` / the end of
  the buffer as **mid-stream** so `^` / `$` (and the start/end anchors) do
  not match there, while a multiline `^` / `$` at an embedded `\n` still
  does. This lets a consumer scan a larger source in **overlapping
  windows** without `^` / `$` falsely binding at every window boundary —
  e.g. an out-of-core hex/binary regex find. With neither flag set the
  matcher behaves exactly as before. (Engine: the `I_BOL` / `I_EOL`
  threads keep their original short-circuit, with `sp > 0` / `sp < len`
  guards so a suppressed boundary anchor never reaches `in[sp-1]` /
  `in[sp]`.)

## 1.3.1 — 2026-06-09

### Fixed

- **`AxlPci` capability walk skipped descending chains** (`<axl/axl-pci.h>`)
  — `axl_pci_cap_next` rejected any `next` pointer `<=` the current offset
  on the false premise that cap lists ascend. Real hardware routinely
  chains **downward** (a QEMU pcie-root-port: `0x54` PCI-Express → `0x48`
  subsystem-IDs; a virtio endpoint: `0xDC` MSI-X → … → `0x40` PCI-Express),
  so the iterator returned only the chain *head* and every deeper cap was
  invisible — silently breaking any consumer that looks up a specific cap
  (PCI-Express, bridge subsystem-IDs, MSI/MSI-X when not first). The guard
  now rejects only a self-loop (`next == prev_off`, the all-1s case an
  absent device returns) while allowing `next < prev_off`; out-of-range
  (`< 0x40 || > 0xFC`) still terminates. `axl_pci_ext_cap_next` got the
  same fix plus an explicit `0x100..0xFFC` range check. (Multi-hop cycles
  remain the caller's iteration-bound concern — the step is stateless.)

### Added

- **Absolute-pointer / touch seat** (`<axl/axl-input.h>`,
  `<axl/axl-compositor.h>`) — a pre-boot app reached over a BMC remote
  console (or fed by a touchscreen / digitizer) gets its pointer as
  `EFI_ABSOLUTE_POINTER_PROTOCOL`, which modern firmware multiplexes
  through `gST->ConsoleInHandle`. `axl_input_attach_touch` now binds every
  absolute-pointer handle (ConsoleInHandle first) with a `WaitForInput`
  event source per handle plus a poll fallback, and
  `axl_compositor_attach_touch` / `axl_compositor_detach_touch` drive the
  seat (cursor + hit-testing + double-click/drag) from it. New
  `axl_input_detach_touch`, `axl_input_set_touch_config`
  (`AxlInputTouchMethod`: event / poll / both, poll interval,
  ConsoleInHandle-only), and an `axl_input_probe_pointers` diagnostic that
  reports which pointer protocols a platform actually exposes.

- **`AxlArgs` `?` help alias** (`<axl/axl-args.h>`) — a lone `?` at any
  node that accepts a flag now prints that node's help, like `-h` /
  `--help` (`tool ?`, `tool verb ?`), matching legacy CLIs. Previously `?`
  was consumed as a positional value. After `--` it remains an ordinary
  positional.

- **`axl_input_set_touch_drain`** (`<axl/axl-input.h>`) — coalesce a
  backlog of queued absolute-pointer states: one dispatch consumes up to N
  states and reports only the latest position, so a slow protocol-safe
  poll still catches up to a fast move on firmware (e.g. a BMC virtual
  mouse) that queues states FIFO. Default 1 (legacy single-read).

### Changed

- **`AxlArgs` help is terser** (`<axl/axl-args.h>`) — the generated
  `--help` / usage now renders as a `Usage:` line plus a single aligned
  list of positionals, flags, and one `-h, --help` row, with the left
  column auto-sized to the longest entry. Dropped the `Arguments:` and
  `Flags:` section headers and the ` (optional)` suffix on optional
  positionals (the `[<name>]` brackets in the Usage line already convey
  it), so the output reads like a hand-written legacy usage block. Also
  fixed the `-x,  --name` double space after the short flag. Affects every
  axl-args consumer's help text; no API or parsing change.

### Fixed

- **`AxlCursor`** scene-bound present is now atomic: the sprite is
  composited *into* the bound scene as its top layer and the `old∪new`
  region is presented in a single GOP operation (then unfolded to keep the
  scene byte-clean), instead of a separate erase-then-draw. This removes a
  cursor-less intermediate frame that flickered at low present rates (e.g.
  a throttled ~10 Hz BMC remote-console pointer).

- **`AxlInput` use-after-free (#GP) on absolute-pointer re-enumeration** —
  the touch path cached raw `EFI_ABSOLUTE_POINTER_PROTOCOL` interface
  pointers; when the providing driver is `Stop()`'d (USB re-enum / console
  reconnect over a BMC remote console) it `FreePool()`s its interface, so
  the next `GetState()` called through freed memory. The seat now binds by
  **handle**, re-resolves the interface via `HandleProtocol` each dispatch
  (fails safely if the protocol is gone), and rebinds on re-install via a
  protocol-notify source, so the cursor survives re-enumeration.

## 1.2.0 — 2026-06-08

### Added

- **`AxlCompress`** (`<axl/axl-compress.h>`) — DEFLATE/gzip/zlib codec
  (`axl_compress`/`axl_decompress` + `axl_compress_writer`/`_reader` +
  `axl_gzip_*` stream filters over `AxlStream`) backed by a vendored
  `sdefl`/`sinfl`, with CRC-32/Adler-32 (`axl_crc32`/`axl_adler32`) in
  AxlDigest. `tar -z` + transparent gzip auto-detect; the HF2.4 fixture
  POST is gzipped.
- **Hardware-fixture device replay** in `axl-emulate`: `--mac` (replay a
  captured NIC MAC from `net.json`) and `--cpu-from-fixture` (replay CPU
  identity from `cpu.json`: x86 vendor/family/model, aarch64 MIDR). New
  generic `run-qemu.sh` primitives `--mac` / `--cpu` back them.
- **HF4 SPD capture** — `mkfixture --spd` dumps SMBus DIMM SPD EEPROMs
  to `spd/0xNN.bin` + decoded `spd.json`.
- **`AxlTar`** (`<axl/axl-tar.h>`) — a POSIX **ustar** reader/writer
  (create / list / extract over `AxlStream`), plus a new `tar` tool
  (`tar c` / `t` / `x`, with `-z` gzip via AxlCompress). Archives are
  padded to GNU tar's 10240-byte record boundary for compatibility.
- **`AxlEdid`** (`<axl/axl-edid.h>`) — a pure VESA E-EDID base-block
  parser (`axl_edid_parse` → vendor/product, preferred timing /
  native mode, physical size) with no dependency on a live display.
- **AxlGfx display / EDID accessors** (`<axl/axl-gfx.h>`) —
  `axl_gfx_get_pixel_format` and raw-EDID `axl_gfx_get_edid`;
  multi-output enumeration `axl_gfx_output_count` / `axl_gfx_output_get`;
  and EDID-driven helpers `axl_gfx_set_native_mode`, `axl_gfx_get_dpi`,
  `axl_gfx_scale_for_dpi`, and `axl_gfx_recommended_scale`. (EDID-present
  paths are real-hardware-only; QEMU publishes no EDID.)
- **HF2.3 device-manifest capture** in `mkfixture` — PCI (`pci.json`),
  USB (`usb.json` + descriptors), network (`net.json`), video
  (`video.json` + `edid/*.bin`), and NVMe (`nvme/<n>.json`) manifests,
  plus an **HTTP write target** (`mkfixture` can POST the fixture as a
  tarball to an `http(s)://` destination for disk-less capture).

### Changed

- **`run-qemu.sh` is now a generic, HF-agnostic launcher.** All
  hardware-fixture *platform-identity injection* moved into `axl-emulate`
  (the fixture layer), which builds the SMBIOS/ACPI/SPD/TPM QEMU device
  args itself — and supervises `swtpm` — passing them through
  `run-qemu.sh`'s `--qemu-arg`. This keeps the released, multi-project
  run-qemu.sh CLI small and confines fixture knowledge to its one
  consumer. See `docs/AXL-Hardware-Fixture-Design.md`.
- `run-qemu.sh --qemu-arg` now appends each value as **one literal token
  (no word-splitting)**, so a token may contain spaces (e.g. a device
  spec with a space in a file path). Pass one `--qemu-arg` per token.

### Removed

- **`run-qemu.sh` hardware-fixture flags** `--smbios-file`,
  `--acpi-table`, `--spd`, `--tpm`/`--tpm-state`/`--tpm-model`, and
  `--ipmi`/`--ipmi-extern`/`--ipmi-prop`. These were only ever consumed
  by `axl-emulate` and axl-sdk's own tests (no external project used
  them); their behavior now lives in `axl-emulate`. Use
  `axl-emulate <fixture>` for fixture replay.

### Fixed

- **`axl_dir_walk`** used a hardcoded `/` path separator, which broke
  recursion on strict UEFI volumes (the `tar` tool surfaced it). It now
  uses the volume's separator.

## 1.1.0 — 2026-06-07

### Added

- **`AxlMemRegion`** (`<axl/axl-mem-region.h>`) — a physical-memory region
  map + fault-safe range access, layered on the raw `<axl/axl-mem-phys.h>`
  primitives so a tool (e.g. a hex / live-memory editor) can let a user type
  an **arbitrary** physical address without faulting the image. Classifies
  the physical address space into typed regions (`AxlMemRegion` /
  `AxlMemRegionType`: RAM / RESERVED / ACPI / MMIO / UNMAPPED) sourced from
  the UEFI memory map overlaid with the PI **GCD** memory-space map (so MMIO
  the EFI map omits — PCI BARs — is classified). `axl_mem_phys_region_at` /
  `_region_count` / `_region_get` / `_region_refresh` enumerate it;
  `axl_mem_phys_is_accessible` gates access (best-effort, no pre-boot fault
  handler); an `AxlMemAccessPolicy` (`axl_mem_phys_get_policy` /
  `_set_policy`) permits every mapped type by default and can be tightened
  (RAM-only, read-only); and `axl_mem_phys_read_range` / `_write_range` do
  bulk width- and alignment-aware access (1/2/4/8), refusing a misaligned or
  inaccessible span with `AXL_ERR` instead of faulting.

- **`AxlIoRegion`** (`<axl/axl-mem-region.h>`) — the I/O-port-space sibling of
  `AxlMemRegion`. Classifies the x86 I/O port address space (`AxlIoRegion` /
  `AxlIoRegionType`: IO / RESERVED / UNMAPPED) from the PI GCD I/O-space map
  via `axl_io_region_at` / `_count` / `_get` / `_refresh`, with
  `axl_io_is_accessible` and width-aware `axl_io_read_range` / `_write_range`
  (1/2/4) over the existing `axl_io_port_*`. Classification works on any arch
  (empty on AArch64); port access is x86-only (`AXL_ERR` elsewhere), and an
  I/O read may have device side effects.

- **`axl_loop_set_intercept_break`** / **`axl_loop_get_intercept_break`**
  (`<axl/axl-loop.h>`) — control whether a bare Ctrl-C quits the loop, so a
  GUI app can own the keystroke instead of the runtime exiting.

- **`axl_piece_tree_load_encoded_cached`** (`<axl/axl-piece-tree.h>`) — load
  a text file with encoding detection while sharing a page cache across
  documents (the buffer-source path for an editor that opens many files).

- **`AxlRegex` bounded repetition** (`<axl/axl-regex.h>`) — the matcher now
  supports `{n}` / `{n,}` / `{n,m}` / `{,m}` interval quantifiers (desugared
  to the base quantifiers; a repeated capture group resolves to its last
  match, POSIX-style).

### Changed

- **`axl_sys_get_memory_size`** now sums the `AXL_MEM_REGION_RAM` regions of
  the shared region map (one memory-map walk instead of its own). The total
  is GCD-aware, so on firmware where the GCD reports usable system memory the
  EFI map omits it is counted too — normally identical to the previous value
  (pinned by a test on the reference platform).

- **`run-qemu.sh`** gained `SHOT_WAIT` to decouple the pre-screenshot settle
  delay from `--timeout` (steadier screenshot/GUI captures in tests).

## 1.0.1 — 2026-06-05

Build/CI hygiene only — no API or behavior change from 1.0.0 (the 1.0.0
artifacts are functionally identical). Fixes two CI-only failures that
surfaced on the 1.0.0 tag:

### Fixed

- Compositor: spell a correct array-of-pointers allocation as
  `sizeof(AxlGfxRegion *)` so CI's older clang-tidy stops raising a
  `bugprone-sizeof-expression` false positive (the allocation size was
  already correct).
- CI: `test-input-modifiers-qemu.sh` no longer runs in CI — its QMP pointer
  injection does not deliver on headless GitHub runners. It remains a local
  pre-release check. `docs/RELEASING.md` gained a "watch CI green on `main`
  before tagging" gate to catch this class of failure before a tag.

## 1.0.0 — 2026-06-05

First stable release. Since 0.24.0: a deferred surface compositor with a
cursor and exact region algebra, a linear-time regular-expression engine,
a first-class UEFI driver-authoring surface (Type-A protocol publishers and
Type-B Driver Model bindings), `AxlBytes` / `AxlHmac` / `AxlRand`, GOP
display-mode control, and a built-in monospace font — plus the one breaking
cleanup below.

### Added

- **UEFI driver-authoring surface** (`<axl/axl-driver.h>`) — author UEFI
  drivers in plain AXL C, with no EDK2 source tree and no raw EFI types in
  the common path. `axl_protocol_install` / `axl_protocol_uninstall`
  publish a protocol interface — the Type-A resident-service /
  protocol-publisher driver. For full UEFI **Driver Model** (Type-B)
  drivers, `AxlDriverBinding` + `axl_driver_binding_install` /
  `axl_driver_binding_uninstall` manage the `EFI_DRIVER_BINDING_PROTOCOL`
  mechanics for you — the EFIAPI `Supported`/`Start`/`Stop` thunks and the
  `OpenProtocol(BY_DRIVER)` ownership bookkeeping — so you write three
  callbacks against `AxlHandle`, no EFI types. `axl_driver_connect_handle`
  / `axl_driver_disconnect_handle` drive a controller's bind / unbind.
  Worked examples in `sdk/examples/{smbus-hc-shim,binding-driver}.c` and a
  guide in `docs/AXL-Driver-Authoring-Guide.md`.

- **GOP display-mode control** (`<axl/axl-gfx.h>`) — query and switch the
  firmware's graphics output modes: `axl_gfx_mode_count`,
  `axl_gfx_query_mode` (an `AxlGfxMode`'s index + dimensions),
  `axl_gfx_current_mode`, `axl_gfx_find_mode` (by width × height),
  `axl_gfx_set_mode` (switch), and `axl_gfx_max_mode` (pick the
  largest-area mode — the usual "use the biggest display" call).

- **`axl_ttf_mono_default`** (`<axl/axl-truetype.h>`) — a built-in
  monospace font, the fixed-width companion to the bundled default
  proportional face (no external font file required).

- **`AxlCompositor`** (`<axl/axl-compositor.h>`) — a deferred,
  retained-mode surface compositor (a wlroots-`wl_scene`-style scene
  graph for UEFI GOP). Surfaces are nodes in a tree (`axl_surface_create`
  / `_destroy`, `_move` / `_resize` / `_raise` / `_lower` /
  `_set_parent`); each carries an off-screen buffer (`axl_surface_buffer`)
  you draw into, and the compositor composites the tree to its output and
  presents it (`axl_compositor_composite` / `_present`). Composition is
  damage-driven and occlusion-aware: per-surface `axl_surface_damage`
  accumulates an exact region, `axl_compositor_get_damage_region` reports
  it, opaque surfaces (`axl_surface_set_opaque`) cull what is hidden
  behind them, and only the damaged region is recomposited. Surfaces
  support per-surface `axl_surface_set_opacity` (gamma-correct),
  `axl_surface_set_per_pixel_alpha`, `axl_surface_set_backdrop_blur` (the
  dialog veil), visibility, and absolute/output coordinate mapping
  (`axl_surface_get_absolute` / `_to_output` / `_from_output`). A **seat**
  routes input: `axl_compositor_pointer_event` hit-tests and dispatches to
  the surface under the pointer (with an optional `axl_surface_set_input_region`),
  `axl_compositor_pointer_grab` / `_pointer_grab_chain` / `_pointer_ungrab`
  implement modal and popup-chain grabs, and
  `axl_compositor_set_keyboard_focus` / `_key_event` route keys —
  delivered to an `AxlSurfaceListener` (`axl_surface_set_listener`) whose
  pointer callbacks carry the live keyboard modifiers and a click count.
  **Frame callbacks** (`axl_surface_request_frame` + an attached frame
  clock) throttle redraw to present.

- **`AxlCursor`** (`<axl/axl-cursor.h>`) — a software mouse-cursor
  compositor (the "never writes the scene" overlay): `axl_cursor_new` /
  `_free`, a built-in arrow or a custom `axl_cursor_set_image`,
  `_show` / `_hide`, and motion by absolute position (`axl_cursor_move`)
  or relative delta (`axl_cursor_move_rel`). It composites the cursor as
  the topmost overlay above any scene (the compositor wires it in
  automatically) and offers a standalone save-under mode
  (`axl_cursor_lift` / `_drop`) for direct-to-screen consumers that have
  no compositor.

- **`AxlGfxRegion`** (`<axl/axl-gfx-region.h>`) — exact banded
  rectangle-set algebra (a pixman/X11-`miregion`-style region), the
  substrate under the compositor's damage and occlusion tracking. A
  region is a canonical, y-x-banded set of rectangles supporting
  `axl_gfx_region_union` / `_subtract` / `_intersect` (and `_rect`
  variants), `_translate`, `_bounds`, `_contains_point` /
  `_intersects_rect`, `_equal`, and rectangle iteration
  (`_num_rects` / `_get_rect`). Exact by construction, with an
  `axl_gfx_region_is_lossy` flag for the bounded-storage fallback.

- **Animated-image decode** — `axl_pixmap_decode_anim`
  (`<axl/axl-pixmap.h>`) decodes a multi-frame GIF into an `AxlPixmapAnim`
  (per-frame pixmaps + delays), freed with `axl_pixmap_anim_free`. Feeds
  an animated-image widget driven by the compositor frame clock.

- **`AxlNTree` move + reverse iteration** (`<axl/axl-ntree.h>`) —
  `axl_ntree_move_after` / `_move_before` re-order a node among its
  siblings without detaching and re-inserting, and
  `axl_ntree_iter_init_reverse` walks children last-to-first (the order
  the compositor needs for top-down hit-testing).

- **Input: `axl_input_detach_key`** (`<axl/axl-input.h>`) — releases a
  keyboard slot taken by a compositor key attach (the counterpart to the
  attach), and pointer events now carry the live keyboard modifier state
  so consumers can implement Shift+wheel / Ctrl+click.

- **`AxlRegex`** (`<axl/axl-regex.h>`) — a regular-expression matcher
  over the `axl-find.h` byte-source seam. A *compiled* pattern
  (`axl_regex_new` → search many times) driven by a Thompson NFA / Pike
  VM, so match time is linear (O(pattern × input)) for every pattern —
  no catastrophic backtracking / "ReDoS". Supports literals, `.`, greedy
  and lazy quantifiers, `^ $`, `|`, capture groups `( )`, classes
  `[...]`/`[^...]`, and `\d \w \s` (+ negations); `axl_regex_search` /
  `_search_buf` / `_search_captures`, `axl_regex_capture_count`, compile
  flags `CASELESS`/`MULTILINE`/`DOTALL`, and an `ANCHORED` match flag.
  `axl_text_buffer_find_regex` / `axl_piece_tree_find_regex` run a
  compiled regex over those sources. Leftmost (Perl/`grep -P`) semantics;
  backreferences are intentionally unsupported (they would force
  backtracking). Verified on both arches incl. capture, anchored, flags,
  compile-error, find-all, ReDoS, and OOM-injection paths.

- **`grep -E`** — the `grep` tool gains `-E` / `--extended-regexp` to
  match an AxlRegex pattern (the first consumer of the new engine);
  without `-E` it keeps the literal Boyer-Moore-Horspool fast path. `-i`
  composes (case-insensitive regex), and a malformed pattern reports its
  offset and exits 2.

- **AxlBytes adoption** — consumers of the new buffer type, all
  additive (existing APIs unchanged):
  - `axl_file_get_bytes` (`<axl/axl-fs.h>`) reads a whole file into an
    `AxlBytes`, wrapping the read buffer with no extra copy.
  - `axl_clipboard_get_bytes` (`<axl/axl-clipboard.h>`) returns a stable
    snapshot of the clipboard that survives later `axl_clipboard_set` /
    `_clear` — unlike the borrowed pointer from `axl_clipboard_get`,
    which the next clipboard operation invalidates. (The shm-backed,
    boot-persistent clipboard storage is unchanged.)
  - `axl_http_response_set_bytes` (`<axl/axl-http-server.h>`) serves an
    `AxlBytes` as the response body (copies into the owned body — the
    contiguous send copies into the transmit buffer regardless, so use
    `set_file` / `set_streamer` for large payloads), and
    `axl_http_client_response_get_bytes` (`<axl/axl-http-client.h>`)
    snapshots a response body into an `AxlBytes` that outlives the
    response.

- **`AxlBytes`** (`<axl/axl-bytes.h>`) — an immutable, reference-counted
  byte buffer mirroring GLib's `GBytes`. A read-only `(data, size)` blob
  shared across owners without copying: `axl_bytes_new` (copy) /
  `_new_take` (own a heap block) / `_new_static` (borrow static data),
  `_ref` / `_unref` (+ AUTOPTR), `_get_data` / `_get_size`, and
  `_new_from_bytes` for a zero-copy sub-range that keeps its parent
  alive. Content `axl_bytes_hash` / `_equal` / `_compare` (usable as
  AxlHashTable key callbacks). The shared currency for data flowing
  between subsystems — HTTP bodies, file contents, shared-memory
  segments. Single-threaded refcount (UEFI BSP).

- **`AxlHmac`** (`<axl/axl-hmac.h>`) — keyed-hash message
  authentication (HMAC, RFC 2104) over the existing AxlChecksum digest
  engine (MD5 / SHA-1 / SHA-256), mirroring GLib's `GHmac`. No
  `AXL_TLS=1` / mbedTLS dependency. `axl_hmac_new` / `_update` /
  `_get_string` / `_get_digest` / `_free` (+ AUTOPTR) and a one-shot
  `axl_compute_hmac`. Keys longer than the 64-byte block are hashed
  down per the RFC; empty keys are valid. For API tokens, signed
  cookies, webhook signatures — prefer HMAC-SHA256 for new designs.
  Verified against the RFC 2202 / 4231 test vectors.

- **`AxlHashTable` / `AxlArray` GLib-parity functions.** Hash table:
  `axl_hash_table_get_keys` / `_get_values` (borrowed-pointer
  `AxlList`s), `_find` (first value matching a predicate), `_add`
  (set idiom — value aliases the key), `_remove_all` (clear, keeping
  the bucket array), and the built-in hashers/comparators
  `axl_int_hash`/`_int_equal`, `axl_int64_hash`/`_int64_equal`,
  `axl_double_hash`/`_double_equal` (the double hasher normalizes
  -0.0 so it matches +0.0). Array: `axl_array_insert` / `_prepend`
  (value mode) and `axl_array_insert_ptr` / `_prepend_ptr` (pointer
  mode), matching `g_array_insert_val` / `g_ptr_array_insert`.

- **`AxlRand`** (`<axl/axl-rand.h>`) — a deterministic, seedable
  pseudo-random number generator mirroring GLib's `GRand`
  (xoshiro256** seeded through SplitMix64). The complement to
  `axl_rng_bytes` (`<axl/axl-rng.h>`), which draws hardware entropy
  and has no reproducible mode: reach for `AxlRand` when you want
  repeatable streams — test fixtures, sampling, retry-backoff jitter,
  procedural graphics, shuffling. A given seed produces a
  byte-identical stream on x86-64 and AArch64 (the output is a defined
  64-bit word sequence; `uint32` is its high half, `double` uses the
  53-bit construction, `double_range` avoids fused multiply-add, and
  `bytes` is little-endian). `axl_rand_int_range` is unbiased
  (rejection-sampled). Includes a process-global stream
  (`axl_random_*`, mirroring `g_random_*`). NOT cryptographically
  secure — use `axl_rng_bytes` for nonces, keys, and tokens.

### Removed

- **`axl_protocol_register_guid` / `axl_protocol_unregister_guid`**
  (`<axl/axl-sys.h>`) — removed. They duplicated the newer
  `axl_protocol_install` / `axl_protocol_uninstall`
  (`<axl/axl-driver.h>`), hand-rolling the same raw firmware install over
  a parallel path. Callers that registered a *bare GUID* should use
  `axl_protocol_install(guid, iface, &handle)`; the name-based
  `axl_protocol_register` / `_register_multiple` / `_unregister` family is
  unchanged (it now sits on the same primitive).

## 0.24.0 — 2026-06-01

### Added

- **`AxlRBTree`** (`<axl/axl-rb-tree.h>`) — a generic, intrusive,
  augmentable red-black tree. The caller embeds an `AxlRBNode` in its own
  struct (the tree never allocates nodes) and descends to the insertion
  point itself, so one tree serves ordered maps, order-statistic trees,
  and weighted positional trees. An optional `recompute` callback keeps a
  cached subtree aggregate (size, byte/newline sums, …) exact across
  every edit in O(log n). Distinct from `AxlTree` (a non-intrusive
  key→value AVL map). Clean-room Apache-2.0 implementation.

- **`AxlPieceTree`** (`<axl/axl-piece-tree.h>`) — an out-of-core,
  editable text buffer for large files. The original file is read on
  demand through `AxlFileView` while edits accumulate in an append-only
  add buffer; a piece tree (spans in an `AxlRBTree` augmented with
  subtree byte/newline sums) gives O(log n) offset↔line mapping and
  edits, so editing a multi-gigabyte file costs memory proportional to
  the edits, not the file. Streaming crash-safe `axl_piece_tree_save`.
  **Built-in unlimited undo/redo** (`axl_piece_tree_undo` / `_redo`,
  configurable depth, nestable grouping, and `_undo_checkpoint` for
  accumulate-until-break / VS Code-style smart grouping) — zero-copy
  because the buffers are immutable/append-only. Line semantics match
  `AxlTextBuffer`
  (interchangeable for a renderer); for memory-resident buffers use
  `AxlTextBuffer`.

- **`AxlTextBuffer`** (`<axl/axl-text-buffer.h>`) — a growable, editable
  byte buffer with an integral line index, for interactive text editing:
  a gap buffer for O(1) amortized inserts/deletes at a moving cursor,
  plus an incrementally-maintained newline index giving O(log n)
  byte-offset ↔ line-number mapping (`axl_text_buffer_line_of_offset` /
  `_line_bounds` / `_line_count`). Byte-oriented (`'\n'` is the only
  special byte); content is read out via `axl_text_buffer_get`. Mirrors the
  piece tree's caret surface — `axl_text_buffer_get_alloc` and
  `axl_text_buffer_cp_align` / `_cp_next` / `_cp_prev` (UTF-8 navigation) —
  so one editor renderer can drive both the small in-memory fields and the
  large out-of-core document.

- **`axl_file_write_atomic`** — crash-safe whole-file write: writes to a
  temp sibling, flushes, then replaces the target by rename, so a power
  loss never leaves the target half-written (the complete data is always
  in either the target or `<path>.tmp`). Cleans up the temp on failure.
  Builds on the existing `axl_file_rename` / `axl_file_delete`.

- **`AxlPageCache`** (`<axl/axl-page-cache.h>`) — a fixed-capacity LRU
  cache of equal-sized pages backed by a caller-supplied fill function.
  Zero-copy: a lookup returns a borrowed pointer into the resident
  frame; on a miss the least-recently-used frame is evicted and refilled
  in place. Capacity-only and integer-indexed, distinct from the TTL,
  string-keyed, copy-in/copy-out `AxlCache`. Windows any large,
  randomly-addressed backing store where only the hot pages should stay
  resident.

- **`AxlFileView`** (`<axl/axl-file-view.h>`) — an mmap-like windowed
  view over a file, built on `AxlPageCache` + `axl_pread`. The file is
  never loaded whole; only the hot pages are resident. `axl_file_view_read`
  copies a range out (spans pages, clamps at EOF) and `axl_file_view_page`
  borrows a zero-copy pointer into one resident page. The software-cache
  design is deliberate: MMU demand paging is unworkable in UEFI (a fault
  handler would block on filesystem I/O outside the TPL model, and a FAT
  file has no physical backing to zero-copy map), so a "real" mmap would
  copy pages in through the FS driver anyway.

- **`AxlPieceTree` editor substrate** — additions that turn the piece
  tree into the backing store for a VS Code / Notepad++-class editor:
  - **Search** — `axl_piece_tree_find(pt, needle, len, from, flags, *out)`
    finds a byte substring across the virtual document (cross-piece),
    with `AXL_FIND_CASE_INSENSITIVE` / `_BACKWARD` / `_WHOLE_WORD`. Backed
    by the same Boyer–Moore–Horspool engine `grep` uses (sub-linear
    average) — via `axl_strstr_len` / `axl_strcasestr_len` (forward) and
    `axl_strrstr_len` / `axl_strrcasestr_len` (backward); a byte-exact
    fallback preserves needles that contain a NUL.
  - **Dirty tracking** — `axl_piece_tree_is_modified` is save-point aware
    (cleared by save / load, set by edits, cleared again when undo/redo
    returns to the saved state).
  - **Batch edits** — `axl_piece_tree_apply_edits(pt, AxlEdit[], n)`
    applies a set of original-coordinate edits as one undo group
    (replace-all, multi-cursor, column edit).
  - **Line iterator** — `axl_piece_tree_line_iter_init` / `_next`
    (`AxlPieceLineIter`) for one O(n) pass over every line;
    `axl_piece_tree_line_iter_init_at(pt, it, start_line)` starts deep in a
    file in O(log n) (render a viewport without walking earlier lines).
  - **Caret support** — `axl_piece_tree_undo` / `_redo` report the
    affected range (`affected_offset` + `affected_len`, both optional) so
    the editor can place the caret / re-select at the edit site;
    `axl_piece_tree_cp_align` / `_cp_next` / `_cp_prev` step UTF-8
    codepoint boundaries (caret left/right, click-to-offset snapping);
    `axl_piece_tree_get_alloc` returns a malloc'd NUL-terminated copy of a
    range (selection-copy / measurement).
  - **Encoding-aware I/O** — `axl_piece_tree_load_encoded` detects a
    file's encoding (UTF-8 ± BOM, UTF-16 LE/BE) and decodes to a UTF-8
    document — plain UTF-8 stays out-of-core, others are transcoded
    (surrogate-aware) into a memory-resident document — reporting the
    detected encoding + BOM so `axl_piece_tree_save_encoded` can
    round-trip them (crash-safe write; plain UTF-8 streams via
    `axl_piece_tree_save`).
  - **Line endings** — `axl_piece_tree_detect_eol` classifies the
    document's terminators (`AXL_EOL_LF` / `CRLF` / `CR` / `MIXED`);
    `axl_piece_tree_set_eol` makes save normalize every terminator to a
    chosen style while streaming (line-ending conversion without
    materializing); and `line_bounds` / the line iterator now exclude a
    trailing `\r` of a CRLF pair so a renderer gets clean line content.
  - **Read-only mode** — `axl_piece_tree_set_read_only` /
    `_is_read_only` make insert / delete / apply_edits return AXL_ERR
    without changing the document (reads, search, and save still work).
  - **Backing-change detection** — `axl_piece_tree_backing_changed`
    compares the backing file's current size / mtime against the values
    captured at open, so an out-of-core editor can detect an external
    change (or deletion) and offer a reload.
  - **Shared page cache** — `axl_piece_tree_open_cached(path, cache)`
    opens a document out-of-core borrowing a caller-owned shared
    `AxlPageCache`, so one bounded frame budget serves many open files.
  - **Save-over-self recipe** (documented, no new API) — saving over the
    open out-of-core file is a rebase; the consumer composes
    `save`→`free`→`axl_file_move`→`open` (undo resets, reopened document
    is a bounded single piece), or Save-As to a new path (keeps undo).
    See `src/data/README.md`.

- **Multi-tenant `AxlPageCache`** — `axl_page_cache_new_shared` creates a
  fill-less cache that several owners share through `axl_page_cache_fetch`
  (keyed by `(owner, page_index)`, one global LRU, fill supplied per
  call); `axl_page_cache_drop_owner` reclaims a closing owner's frames.
  The single-tenant `axl_page_cache_get` is now the same primitive with
  the cache as its own owner. `axl_file_view_open_cached(path, cache)`
  opens a view that borrows a shared cache (dropping only its own frames
  on close) — so an editor caps total resident pages across every open
  file instead of giving each its own fixed pool.

- **UTF-16 ↔ UTF-8 transcoding** (`<axl/axl-str.h>`) —
  `axl_utf16_to_utf8` / `axl_utf8_to_utf16`: surrogate-aware,
  length-counted (NUL is not a terminator), with a NULL-destination
  measuring mode and clean truncation on a codepoint boundary. Unlike the
  BMP-only UCS-2 helpers these handle U+10000..U+10FFFF.

- **`axl_strrcasestr` / `axl_strrcasestr_len`** (`<axl/axl-str.h>`) —
  reverse (last-occurrence) case-insensitive substring search, completing
  the {forward, reverse} × {sensitive, insensitive} family (the missing
  fourth corner — used by `AxlPieceTree`'s backward case-insensitive find).

- **`axl_detect_encoding`** (`<axl/axl-stream.h>`) — sniff a text file's
  encoding from a leading sample: UTF-8 / UTF-16 LE / UTF-16 BE BOM, then
  a BOM-less interleaved-NUL heuristic, reporting whether a BOM was
  present (so a caller can round-trip it on save).

- **`AxlShm`** (`<axl/axl-shm.h>`) — boot-persistent named shared memory,
  the UEFI analog of POSIX `shm_open` + `mmap` (or System V
  `shmget`/`shmat`). `axl_shm_open(name, size, flags, &size)` creates or
  opens a fixed-size region that **outlives the app that created it** and
  is reachable by any later app in the same boot; `axl_shm_unlink` /
  `axl_shm_exists` round it out. UEFI's single flat address space means
  there is no map/attach step — the returned pointer *is* the region.
  Volatile (boot-services RAM; gone at reboot / `ExitBootServices`) — not
  NVRAM, no flash wear, capacity is system RAM. The name is hashed to a
  GUID and the region published as a data-only protocol from
  `EfiBootServicesData`, which survives image unload.

- **`AxlClipboard`** (`<axl/axl-clipboard.h>`) — a cut / copy / paste
  clipboard (UEFI has no system one). One owned byte buffer plus an
  optional MIME type: `axl_clipboard_set` copies bytes in (OOM leaves the
  prior clipboard intact), `axl_clipboard_get` borrows a pointer valid
  until the next set/clear, and `axl_clipboard_clear` empties it.
  Byte-oriented and content-agnostic (text, a UTF-16 region, a serialized
  selection, an image). Backed by an `AxlShm` segment, so it is
  **cross-app within a boot** — copy in one app, paste in another, with no
  driver.

- **`clip` / `paste` tools** — command-line clipboard front end
  (pbcopy / pbpaste for UEFI): `some-tool | clip` copies stdin to the
  clipboard (`-m` tags a MIME type, `--clear` empties it); `paste` writes
  the clipboard to stdout (`--mime` prints the MIME type). Because the
  clipboard is `AxlShm`-backed, `clip` and `paste` are separate
  invocations that share it across the boot.

## 0.23.0 — 2026-06-01

### Added

- **`axl_gfx_blit_rect`** — blit a `w×h` sub-rectangle of a larger
  source image to the active target. `src_stride` is the source's full
  row width in pixels and `(src_x, src_y)` is the sub-rect's top-left, so
  a consumer can blit one cell of a sprite sheet each frame without
  CPU-copying the cell out first. Raw target coordinates (not
  transform-aware), same pixel/alpha semantics and destination clipping
  as `axl_gfx_blit`; returns `AXL_ERR` on NULL buffer or zero width/
  height. `axl_gfx_blit` is now a thin wrapper over the same core
  (`stride = w`, source origin `0,0`).

- **`axl_json_get_object`** — navigate into a named nested object and
  get back a sub-reader scoped to it (the object analog of
  `axl_json_array_begin`). Chains for deeper paths and composes with all
  the flat getters and `axl_json_array_begin`. Like array elements, the
  sub-reader borrows the parent's token array (do not free it). Closes
  the JSON reader's gap of having flat key getters + array iteration but
  no named-nested-object navigation.

## 0.22.0 — 2026-05-31

This release is the AGT dependency floor. It rounds out the 2D
graphics stack (a retained display list, arbitrary-path and
quad/transform clipping, multi-line text, stroking, blend modes, an
analytic FreeType rasterizer, gamma-correct compositing, pattern fill,
and a direct-framebuffer present path with dirty-rectangle damage), adds
a new SIMD substrate (`AxlCpu` feature detection + AVX state-enable, with
SIMD-dispatched blur and source-over blend kernels), consolidates all 2D
transforms into a single 3×3 `AxlTransform` type, and adds the `AxlNTree`
/ `AxlTree` containers, a shortest-round-trip `axl_dtoa`, and a standalone
`axl_qsort`.

### Added

- **AxlCpu (new module, `<axl/axl-cpu.h>`).** Runtime SIMD feature
  detection and CPU state management. `axl_cpu_simd_tier` reports a
  per-CPU tier; a broad feature catalog covers SSE through AVX-512 plus
  SHA-NI, AES, and bit-manipulation on x86, and AES/SHA/CRC32/FP16/
  DotProd/SVE on aarch64. `axl_cpu_enable_avx` / `axl_cpu_enable_avx512`
  arm the AVX state bits (CR4.OSXSAVE + XSETBV); SSE4.x / AES / SHA-NI
  need no enable step.

- **SIMD-accelerated AxlGfx kernels.** Buffer blur is SIMD-dispatched
  (AVX2 / SSE4.1 / NEON, bit-exact with the scalar path, ~1.2x–1.9x).
  Source-over blend fill is SIMD-dispatched (SSE2 / AVX2 / NEON,
  ~7x–11x, bit-exact via the integer /255 trick). Both fall back to
  scalar on CPUs without the feature.

- **AxlGfx present pipeline (Phases G17/G18).** `axl_gfx_pack_pixel`
  + `AxlGfxPixelOrder` pack a buffer into a framebuffer's native pixel
  layout; the present path writes the framebuffer directly (BGRA memcpy
  / RGBA swap) with a Blt fallback for bitmask / blt-only / unknown-FB
  modes. Dirty-rectangle damage tracking lands via
  `axl_gfx_buffer_present_rect` and per-buffer
  `add_damage` / `present_damage` / `clear_damage` / `get_damage`. On
  x86 the present uses non-temporal streaming stores (`_mm_stream` +
  `SFENCE`) to avoid polluting the cache with write-only framebuffer
  traffic.

- **AxlGfx gamma-correct compositing (Phases G15/G15b).** Opt-in
  linear-light blending via `axl_gfx_set_gamma_correct` /
  `axl_gfx_get_gamma_correct` / `axl_gfx_reset_gamma_correct`, plus
  public `axl_gfx_srgb_to_linear` / `axl_gfx_linear_to_srgb`. Gradient
  ramps are gamma-corrected when the mode is enabled. Off by default —
  existing sRGB-space output is unchanged unless a caller opts in.

- **AxlGfx pattern fill (Phase G12).** `axl_gfx_fill_pattern` tiles a
  source buffer across a destination region with `AxlGfxRepeat`
  (repeat / reflect / pad).

- **AxlGfx retained display list (Phase G9).** `axl_gfx_display_list`
  records draw / gradient / transform ops for replay, with a textual
  dump for inspection and testing.

- **AxlGfx clipping (Phase G10 + transform).** `axl_gfx_push_clip_path`
  (arbitrary-path clip), `axl_gfx_push_clip_quad` (convex-quad clip),
  and `push_clip_rect_transformed` (perspective-correct transformed-rect
  clip), honored by all writers.

- **AxlGfx text (Phases G11, G7).** `axl_ttf_draw_box` for multi-line
  text with word wrap; a glyph cache with horizontal subpixel
  positioning; `axl_ttf_draw_affine` for rotated / sheared vector text.

- **AxlGfx stroking (Phase G8).** Width-honoring stroke with round
  joins / caps, configurable cap/join styles (`AxlGfxStrokeStyle`), and
  dashed strokes. The stroker now lives in `src/gfx/axl-gfx-stroke.c`.

- **AxlGfx blend modes (Phase G13).** Separable blend modes
  (multiply / screen / overlay / darken / lighten / add) with
  `axl_gfx_reset_blend_mode`.

- **AxlGfx analytic rasterizer (Phase G14).** Anti-aliased path fills
  via a vendored, standalone FreeType `ftgrays` rasterizer.

- **AxlGfx transform-aware blit + color parsing.**
  `axl_gfx_blit_affine` (bilinear, transform-aware image blit);
  `axl_gfx_color_parse` parses CSS hex color strings
  (`#RGB` / `#RGBA` / `#RRGGBB` / `#RRGGBBAA`) to an `AxlGfxPixel`.

- **Drop shadow + buffer blur (Phase G6).** `axl_gfx_buffer_blur` and
  `axl_gfx_draw_shadow`.

- **Path + rounded-rect gradient fills (completes Phase G5).** The
  follow-ups promised in 0.21.0 — gradients now fill arbitrary paths and
  rounded rectangles, not just axis-aligned rectangles.

- **AxlNTree + AxlTree containers.** `<axl/axl-ntree.h>` (generic n-ary
  tree, the GLib `GNode` equivalent) and `<axl/axl-tree.h>` (balanced,
  AVL-backed sorted map, the GLib `GTree` equivalent), both with
  callback-free pull iterators. They reuse the existing
  `AxlDestroyNotify` / `AxlCompareDataFunc` callback types.

- **`axl_dtoa` (AxlFormat).** Shortest round-trippable double→decimal
  conversion (Grisu2), with no precision cap. AxlFormat's `%f` is
  rebuilt on it and `%e` / `%g` are added.

- **`axl_qsort` (AxlSort).** A standalone introsort API;
  `axl_array_sort` now delegates to it.

- **AxlMath matrix/vector ops.** `mat3` inverse / determinant, `vec2`
  geometry helpers, and affine↔mat3 converters. `<axl/axl-math.h>` is
  now included directly from the `<axl.h>` umbrella (previously only
  reachable transitively).

- **AxlInput modifier state.** Keyboard modifier and lock state are
  plumbed through to input events.

### Changed

- **One transform type — `AxlTransform`.** The 2D transform surface is
  consolidated into a single 3×3 (projective) `AxlTransform`. The
  separate `AxlGfxAffine` type is removed and multiplication now follows
  cairo's a-first convention. Rasterization is perspective-correct for
  blit and text. Callers that used the interim `AxlGfxAffine` API must
  move to `AxlTransform`.

### Fixed

- **ftgrays pool-estimate underflow.** A `max_ex - min_ey` typo (should
  be `-min_ex`) in the vendored FreeType rasterizer could hang or
  corrupt `fill_path` on narrow-x / high-y bounding boxes (e.g. a glyph
  or rect rotated high on the surface). The `FT_QNEW_ARRAY` shim is also
  hardened against `size_t` overflow.

- **Stroke dashing divide-by-zero.** Guarded the `dash_advance_` modulo
  against a static-analyzer-flagged divide-by-zero.

### Notes

- **SIMD negative result (documented, not a regression).** The
  `blit_transform` sampler was measured ~4.6x *slower* under AVX2 (the
  per-pixel `set_pd` plus AVX/SSE transition costs dominate a gather +
  put-bound kernel), so it is kept scalar. SIMD wins multi-pixel integer
  kernels (blur, blend) and loses on per-pixel float / gather / put-bound
  ones.

- **Build/CI.** The CI `clang-tidy` lint runs one file per process to
  stop intermittent `security.ArrayBound` false positives.

## 0.21.0 — 2026-05-29

### Added

- **`axl_ttf_default()`** — a shared, built-in TrueType font (a ~23 KB
  DejaVu Sans subset: ASCII + Latin-1 + common typographic
  punctuation) so consumers get vector text without bundling a font
  asset. Caller does not own it (mirrors `axl_gfx_default_font`).
  Dropped by `--gc-sections` when unreferenced, so consumers that load
  their own font pay no size cost. DejaVu's Bitstream Vera license is
  vendored and documented in `THIRD_PARTY.md` (stb_image / stb_truetype
  are now documented there too).

- **AxlGfx gradients (Phase G5)** — `<axl/axl-gfx-gradient.h>`:
  `AxlGfxGradient` (linear axis or radial center+radius),
  `axl_gfx_gradient_add_stop` (up to 16 stops, alpha interpolated), and
  `axl_gfx_fill_rect_gradient`. Per-pixel sampling with clamped
  offsets; per-channel sRGB interpolation. Path / rounded-rect gradient
  fills are planned follow-ups.

- **`axl_pci_next_unfiltered()`** — enumeration variant that does NOT
  skip 0x0000 phantom slots (see Changed), for the rare consumer that
  needs raw config space.

### Changed

- **AxlGfx header layout.** `<axl/axl-gfx.h>` is now a thin umbrella
  over focused sub-headers — `axl-gfx-types.h`, `-surface.h`, `-draw.h`,
  `-path.h`, `-gradient.h` (plus `axl-font.h` / `axl-truetype.h` /
  `axl-pixmap.h`). `#include <axl/axl-gfx.h>` is unchanged for
  consumers; no function/type renames. Fixes a gap where `<axl.h>` did
  not surface AxlTtf / AxlPixmap — it now pulls the whole 2D library.

- **`axl_pci_format_name` renders VID:DID uppercase** (`%04X`), matching
  lspci / pci-ids / vendor-tool convention; lowercase was the outlier.
  The lspci and netinfo tools render PCI VID:DID uppercase to match.
  USB (`axl_usb_ids_format_name`) stays lowercase per lsusb convention;
  PCI bus addresses stay lowercase.

- **AxlArgs accepts negative-number positionals and POSIX `--`.**
  `-<digit>` / `-.<digit>` (e.g. `-1`, `-.5`) are treated as positionals
  rather than unknown flags, and a bare `--` ends option parsing for the
  current node (everything after is positional). Both are additive — the
  affected tokens previously errored, so no working invocation changes.

- **`axl_pci_next` skips 0x0000 phantom slots by default.** Some chipsets
  return all-zero config reads for disconnected slots/functions instead
  of all-ones, producing bogus 0000:0000 devices. Both 0x0000 and
  0xFFFF are reserved "no device" vendor IDs and are now skipped; use
  `axl_pci_next_unfiltered` to opt back in.

### Fixed

- **Stack out-of-bounds write in `axl_pci_addr_parse`** (security): an
  over-long address such as `"1:2:3:4.5"` wrote one element past the
  4-element `parts[]` array before returning an error, corrupting
  adjacent stack on hostile input. Now bounds-guarded.

- **`--help` alignment.** Positional argument help text now aligns in a
  fixed column with the flag / `-h` lines regardless of argument-name
  length (was a drifting, fixed-pad layout).

### Internal

- Static-analyzer cleanups (two `clang-analyzer` ArrayBound findings,
  one a real bug — see above), and a sweep removing downstream-consumer
  product names from example code and design docs (hardware-vendor
  names the library targets, e.g. the Dell IPMI transport, are kept).

## 0.20.1 — 2026-05-29

### Fixed

- **Packaged C++ artifacts.** The 0.20.0 `.deb`/`.rpm` shipped a
  C-only tree — `axl-c++` and `libaxl-cxx.a` were silently absent
  because the release runner lacked the ARM bare-metal toolchain
  and `install.sh` ran in auto (build-if-present) mode. The release
  workflow now installs the pinned toolchain, stages with `--cpp`
  (require mode — hard-fails if the toolchain is missing rather than
  silently dropping C++), and the package smoke test asserts
  `libaxl-cxx.a` (both arches) is present and links a C++ example to
  a PE32+ EFI binary. C-only consumers are unaffected. Adds
  `sdk/examples/hello.cpp`.

## 0.20.0 — 2026-05-29

This release adds three new public modules (AxlMath, AxlPixmap,
AxlTtf), an AxlInput event module, a full C++ toolchain, and a
broad 2D upgrade to AxlGfx. Pre-1.0, so the new gfx/input surface
may still move in subsequent releases.

### Added

- **C++ support.** New `axl-c++` compiler driver and `libaxl-cxx.a`
  per arch (operator `new`/`delete`, `__cxa_pure_virtual`). The
  runtime now walks `.init_array` so C++ static initializers run
  before `main`. `axl-cc` dispatches by file extension; `install.sh`
  auto-detects the ARM bare-metal C++ toolchain (no opt-in flag).
  `libaxl-cxx.a` does not link libstdc++, so pure-C consumers incur
  no new runtime dependency. AArch64 library, `axl-cc`, and CMake
  invocations now pass `-ffixed-x18`.

- **AxlMath** — new module `<axl/axl-math.h>`. libm-free
  `axl_sin`/`cos`/`sqrt`/`floor`/`ceil`/`fabs`/`fmod`/`ln`/`exp`/
  `pow`/`atan`/`atan2`/`asin`/`acos`; `axl_lerp` + a 9-function
  easing palette; `clamp`/`min`/`max`/`remap`/`step`/`smoothstep`;
  bit math (`clz`/`ctz`/`popcount`/`log2i`/`round_up_pow2`);
  saturated arithmetic (`sat_add_u8`/`sat_sub_u8`/`sat_mul_u16`);
  geometry helpers (rect/segment/circle); `AxlVec2` + `AxlMat3`
  linear algebra; math constants. Compile-time hardware fast paths
  for sqrt/floor/ceil/fabs and an FMA Horner evaluation are gated
  on the target `-march`.

- **AxlPixmap** — new module. Image decode via stb_image
  (PNG/JPG/GIF/BMP).

- **AxlTtf** — new module. Vector text via stb_truetype, including
  glyph rasterization (`axl_ttf_draw`).

- **AxlInput** — new module `<axl/axl-input.h>`. Input event types
  attached to the event loop via the source pattern:
  `axl_input_attach_mouse` (EFI_SIMPLE_POINTER_PROTOCOL),
  `axl_input_attach_key`, `axl_input_attach_touch`.

- **AxlGfx 2D upgrade.** Line / rect-outline / polyline primitives
  (Bresenham); alpha compositing with RGB convenience macros and a
  named color palette; double-buffering (`AxlGfxBuffer` + draw-target
  redirect) and `axl_gfx_get_current_target()` to save/restore the
  active target across nested callers; push/pop clipping-rectangle
  stack applied to fill/blit/draw_text; `AxlGfxPath` with scanline
  `fill_path` + `fill_rounded_rect`; an affine transform stack
  integrated path-side; `axl_gfx_fill_rect_i` (signed-coordinate
  variant for off-screen widget rendering). Font handling refactored
  into an `AxlFont`/`AxlGlyph` abstraction with font-metrics queries
  for text layout; GNU Unifont 16.0.04 added as a second built-in
  font; text rendering is now UTF-8-first (`axl_utf8_decode`).

- **`axl-cc -c`** (compile-only) plus `.o`/`.a` pass-through, for
  staged/multi-file builds.

- **`run-qemu.sh --gpu`** flag with arch-aware GPU device wiring;
  `--screenshot` now honors the destination extension (PIL fallback
  for PNG).

### Changed

- **All public macros now carry a module prefix.** Math macros were
  renamed accordingly (e.g. `AXL_MATH_PI`); bare `AXL_`-prefixed
  names are reserved for project-wide infrastructure. Consumers using
  the previous unprefixed math macros must update to the new names.

## 0.19.2 — 2026-05-23

### Added

- **`axl_shared_driver_unload(name)`** — launcher-side teardown
  primitive in `<axl/axl-shared-driver.h>`. Resolves the driver's
  protocol-bearing handle via `LocateHandleBuffer(ByProtocol,
  GUID)`, then `axl_driver_unload` (which fires the driver's
  registered unload callback → `axl_shared_driver_unpublish` runs
  → image pages freed). Returns AXL_OK when the driver isn't
  loaded — post-condition "driver not resident" already holds.
  **Must not be called from inside the driver image itself**;
  `gBS->UnloadImage` on a self-executing image is undefined.
  Symmetric counterpart to `axl_shared_driver_publish` for
  `--reload`-style developer flags and crash-recovery
  scenarios. Demo `sdk/examples/shared-driver-demo/` extended
  with `--reload` to exercise the pattern.

### Changed

- **`axl_shared_driver_publish` now defaults `*out_handle` to the
  driver's `gImageHandle` when the consumer passes in NULL.**
  Previously a fresh handle was minted via
  `InstallProtocolInterface`, which made the protocol-bearing
  handle distinct from the loaded-image handle — and that
  separation meant the new `axl_shared_driver_unload` couldn't
  resolve a single handle to feed `UnloadImage`. The change is
  effectively invisible to existing consumers: the published
  handle's identity isn't read externally except to pass back to
  `axl_shared_driver_unpublish`. Consumers that pin a specific
  handle by pre-setting `*out_handle` to a non-NULL value retain
  the old reuse semantics; only the NULL-default behavior
  changed.

## 0.19.1 — 2026-05-23

### Fixed

- **clang-tidy CI gate cleared.** v0.19.0 shipped artifacts
  successfully (Release + Docs workflows green) but the CI
  workflow's clang-tidy step failed with one finding — same
  shape as the v0.18.0→v0.18.1 sequence.
  `src/stream/axl-stream.c:118` (`clang-analyzer-deadcode.DeadStores`):
  the CRLF-expansion branch of `console_expand_into` assigned
  `last_cu = '\r'` immediately before the loop tail wrote
  `last_cu = '\n'` (the value of `cp`), so the `'\r'` assignment
  was unobservable. Dropped the dead store; left a comment
  explaining why the CRLF-state contract is preserved via the
  subsequent next-line assignment. Behavior is unchanged
  (back-to-back bare-LF still expands; `\r\n` still suppresses
  the expansion).

## 0.19.0 — 2026-05-13

### Added

- **POSIX-shaped `axl_clock_gettime` / `axl_clock_getres`** in
  `<axl/axl-time.h>`. New `AxlClockId` enum (`AXL_CLOCK_MONOTONIC`,
  `AXL_CLOCK_REALTIME`) + `AxlTimespec` struct mirror the Linux
  `clock_gettime(2)` shape. The monotonic clock is backed by the
  architecture's cycle counter (x86 RDTSC / aarch64 CNTPCT_EL0);
  on x86 the calibrated tick-frequency is now cached via a
  private boot-services-pool protocol so subsequent processes in
  the same boot skip the 10 ms `gBS->Stall` calibration and read
  the cached value in microseconds. REALTIME goes through the
  firmware RTC + a single shared Gregorian→Unix-seconds helper
  (`civil_to_unix_seconds`) — `axl_backend_efi_time_to_unix` and
  `axl-mbedtls-platform.c`'s `time()` now both delegate through
  it, eliminating ~25 LOC of duplicated O(N-year) Gregorian
  arithmetic.
- **`<axl/axl-shared-driver.h>`** — thin helpers for the
  synchronous-RPC "thin launcher + resident driver" pattern.
  `axl_shared_driver_publish` / `_unpublish` (driver side) wrap
  `axl_protocol_register_guid` + name-derived GUID via
  `axl_guid_v5` against a fixed namespace.
  `axl_shared_driver_locate` / `_locate_with_load_options`
  (launcher side) compose `axl_driver_ensure_with_embedded` +
  `axl_protocol_find_guid`. No new SDK type — the consumer owns
  its vtable struct and CRT wiring (`AXL_DRIVER` on the driver
  side, `int main` on the launcher side). Sibling to
  `<axl/axl-service.h>` for consumers whose driver image has no
  event loop and is purely a vtable-publishing RPC server.
- **`axl_image_set_load_options`** in `<axl/axl-image.h>` —
  mirrors `axl_driver_set_load_options` at the image level for
  consumers that drive `LoadImage`/`StartImage` directly. Reuses
  the existing driver-side LoadOptions-tracking table; copy is
  freed on `axl_image_unload`.
- **CMake helpers** in `axl-config.cmake` (generated by
  `install.sh`): `axl_add_driver(TARGET sources...)` for
  driver-subsystem builds (DriverEntry, PE subsystem 11), and an
  `EMBEDS` keyword on `axl_add_app` that takes `path[=name]`
  entries. Both `axl_add_app` and `axl_add_driver` export
  `${TARGET}_EFI_PATH` for cross-target embedding without manual
  path derivation. Internal `_axl_build_efi` helper factored out.
- **`sdk/examples/shared-driver-demo/`** — multi-file example
  (driver + launcher + shared header + shared format TU) +
  CMakeLists.txt. Demonstrates the single-binary distribution
  pattern (driver `.efi` bytes embedded via `.incbin`) and the
  multi-TU shared-helpers pattern (the same `.c` file listed in
  both `axl_add_driver` and `axl_add_app` source lists).
- **`docs/AXL-Shared-Driver-Recipe.md`** + matching Sphinx guide.
  Covers when to use, code shape, build pattern, cross-TU symbol
  audit (the "what symbols go in a shared TU vs. each side's own
  TU" decision matrix), performance properties, and hazards.

### Changed

- **`axl_time_get_us` / `axl_time_get_ms` epoch widened to
  boot-relative.** Both are now thin wrappers over
  `axl_clock_gettime(AXL_CLOCK_MONOTONIC)`. Each call returns
  microseconds (or milliseconds) since CPU power-on — no
  per-process first-call calibration tick, no "first call returns
  0" sentinel. Two values captured in different UEFI processes
  within the same boot are directly comparable for delta
  measurement. Source-compatible: existing delta-based usage is
  unchanged. Any code comparing against a hardcoded magnitude
  (none in-tree) would break.
- **`axl_print*` path now buffers into a 512-byte stack scratch
  before issuing a single `axl_write`.** Previously
  `axl_print` / `axl_printf` / `axl_fprintf` / `axl_vfprintf`
  passed `fprintf_write` directly into `axl_vformat`, so every
  literal run and every format conversion produced its own
  `axl_write` call. A typical line with seven `%X` specifiers
  emits ~15 chunks; on consumers that print 150+ lines per verb
  (e.g. PCI walks) the per-chunk fragmentation cost shows up
  measurably. The buffered path collapses that to one
  `axl_write` per line. Overflow falls through to direct-write
  mode so single prints larger than the stack scratch still
  produce complete output. Return-value contract (bytes written /
  -1 on error) unchanged.
- **`console_write`'s transcode + CRLF path is now single-pass
  stack-only.** The prior shape allocated three buffers per
  console write (NUL-terminating copy + UCS-2 transcode pass +
  CRLF expansion pass), with three `axl_malloc`/`axl_free` cycles
  and two separate sizing passes. The new shape does one
  decode-and-emit pass into a stack buffer
  (`2*AXL_PRINTF_STACK_BUFFER+1` UCS-2 units, ~2 KB); oversized
  single writes spill to one `axl_malloc` instead of three.
  `console_expand_newlines` is gone — CRLF expansion lives inline
  in the transcode loop. Output is byte-for-byte identical to the
  prior path.
- **`axl_driver_init` now populates `mImagePath`** for the driver
  image. Prior to this, drivers reached `axl_app_image_path() ==
  NULL` because the driver CRT path skipped `_axl_args_init`.
  Now the same `_capture_image_path` helper runs for both apps
  (via `_axl_args_init`) and drivers (via `axl_driver_init`).
  Sidecar autodiscovery (`axl_pci_ids_load(NULL)` and friends)
  works from driver image entry points.
- **`axl-mbedtls-platform.c::time()`** now delegates to
  `axl_clock_gettime(AXL_CLOCK_REALTIME)` rather than an inline
  O(N-year) Gregorian loop. Drops ~25 LOC of duplicated date
  arithmetic; the new path goes through the same
  `civil_to_unix_seconds` helper as the file-timestamp converter.
- **`_axl_prepend_volume_mapping`** now inserts a `\` separator
  between the volume mapping and the FILEPATH suffix when
  neither side carries one. The prior shape produced
  `fs0:app.efi` for shell-launched cwd-relative invocations
  (`fs0:app.efi`-style), which broke `axl_path_get_dirname` and
  in turn broke companion-style sidecar discovery. Absolute-form
  `FilePath` (`\app.efi`) is unchanged.

### Fixed

- **axl-cc + CMake `_axl_build_efi` now pass `--no-undefined` to
  ld.** The library's own Makefile already used this flag, but
  the consumer-facing build paths (axl-cc shell wrapper, CMake
  helper) silently accepted undefined symbols under `-shared`.
  In a freestanding UEFI binary with no dynamic loader, an
  unresolved reference at link time becomes a zero-relocation
  (or garbage) call target at runtime — manifesting as a
  RIP-points-at-nonsense KVM internal error on the first
  dispatch through the unresolved path. Surfaced by a consumer
  splitting an existing app into launcher+driver: the driver's
  source list omitted a TU defining helpers a sibling TU called,
  silently linked through, and crashed on the first verb that
  reached the unresolved code. The fix makes the missing-symbol
  case a hard build error pointing at the exact call site
  instead of an opaque runtime fault.
- **Buffer-loaded driver images inherit a sidecar-discovery
  anchor from the launcher.** When `axl_driver_ensure_with_embedded`
  reaches step 4 (LoadImage from an embedded blob), the firmware
  sets `LoadedImage->FilePath = NULL`. `_capture_image_path` now
  walks `LoadedImage->ParentHandle` until it finds an ancestor
  with a non-NULL FilePath (depth-bounded at 8). The driver
  image's `axl_app_image_path()` returns the launcher's path, so
  `axl_pci_ids_load(NULL)` and other sidecar consumers
  autodiscover their data file next to the launcher `.efi` —
  even when no driver `.efi` is on disk.
- **TSC frequency cache is sanity-bounded** against a poisoned
  cross-process value. The prior shape trusted the published
  protocol's `freq_hz` unconditionally; a buggy or hostile
  earlier publisher could land an extreme value and overflow the
  `(ticks % freq) * 1e9` arithmetic. Now bounded to
  [1 MHz, 100 GHz]; out-of-range cached values force a
  recalibration.

## 0.18.1 — 2026-05-12

### Fixed

- **clang-tidy CI gate cleared.** v0.18.0 shipped artifacts
  successfully (Release + Docs workflows green) but the CI
  workflow's clang-tidy step failed on four findings — same
  shape as the v0.16→v0.17.1 sequence. `src/fs/axl-fs-provider.c`:
  - line 116 (`bugprone-branch-clone`): the three "wrong-kind /
    bad-args" `AxlFsStatus` cases all map to `EFI_INVALID_PARAMETER`.
    Collapsed into a single fallthrough block with a note about
    why the AXL enum still distinguishes them.
  - lines 168/170 (`clang-analyzer-core.NullDereference` on
    `pub_list_remove`): the analyzer can't prove
    `f->pub != NULL` across function boundaries given the
    orphan-on-unpublish contract that nulls it. Added an
    explicit guard at function entry.
  - line 463 (`clang-analyzer-core.NullDereference` in
    `thunk_close`): same reason — `self->dead` was checked but
    the `self->pub` deref wasn't independently guarded. Added
    `self->pub != NULL` to the guard.
- **`scripts/watch-release-runs.sh` now prints
  `RELEASE_VERDICT: PASS` / `RELEASE_VERDICT: FAIL`** on the
  last line so callers can grep for the verdict when the script
  is invoked through a pipe (`... | tail -N`) that swallows the
  exit code. Bit me on the v0.18.0 release run — the watcher
  correctly `exit 1`'d on CI failure but the wrapping pipe in
  the automation reported PASS.

## 0.18.0 — 2026-05-12

Phases A + B + C of the EFI-encapsulation plan
(`docs/AXL-EFI-Encapsulation-Plan.md`). Consumers of axl-sdk can
now write zero `EFI_*` identifiers across crash-handler /
boot-volume / image-introspection (Phase A), CPU-exception
handlers (Phase B), and filesystem publishers (Phase C). The
filesystem-entry struct surface is unified — one `AxlFsEntry`
replaces the four near-duplicates (`AxlDirEntry`, `AxlFileInfo`,
`AxlFsProviderInfo`, `AxlWebDavEntry`). Plus a build-system fix
that retires the long-standing `debug: alloc fill 0xDA` flake.

### Changed

- **Filesystem-entry struct unification.** `AxlDirEntry`,
  `AxlFileInfo`, `AxlFsProviderInfo`, and `AxlWebDavEntry` were
  four near-identical structs carrying file/directory metadata.
  Collapsed into a single canonical `AxlFsEntry` (in
  `<axl/axl-fs.h>`) with the union of fields:
  `struct_size + version + name + size + alloc_size + mtime_unix +
  attributes`. Used everywhere — `axl_file_info`, `axl_dir_read`,
  `axl_dir_walk` callbacks, `axl_dir_list_json`, the
  `<axl/axl-fs-provider.h>` `get_info` / `read_dir` / `set_info`
  callbacks, and the `<axl/axl-http-server.h>` WebDAV
  `list_dir` / `stat` callbacks. Two convenience accessors
  (`axl_fs_entry_is_dir`, `axl_fs_entry_is_read_only`) replace
  the old `bool is_dir` / `bool read_only` field shape — the
  attribute bitmask carries the same data plus four more bits
  (HIDDEN, SYSTEM, ARCHIVE) without growing the struct.
- **`AXL_FS_OPEN_*` and `AXL_FS_ATTR_*` constants** moved from
  `<axl/axl-fs-provider.h>` to `<axl/axl-fs.h>`. They're shared
  between the consumer (read) and publisher (callback) sides.
- **Pre-1.0 API churn**, no back-compat typedefs — consumers
  rebuild against the new shape per `feedback_change_apis_freely`.

### Added

- **`<axl/axl-fs-provider.h>`** — backend-neutral
  filesystem-publisher abstraction. Phase C of the
  EFI-encapsulation plan. Lets a consumer publish a UEFI-visible
  filesystem (`fsN:` mapping; Shell `dir` / `cd` / `mkdir` /
  `LoadImage` work against it) without writing a single `EFI_*`
  identifier.
  Consumer fills an `AxlFsProvider` vtable in pure UTF-8 +
  snake_case + `AxlFsStatus` terms; `axl_fs_provider_publish`
  synthesizes the matching `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` +
  `EFI_FILE_PROTOCOL` vtables, marshals UCS-2 ↔ UTF-8 at the
  boundary, lays out `EFI_FILE_INFO` / `EFI_FILE_SYSTEM_INFO`
  trailers in caller-supplied buffers (with probe-then-resize
  `EFI_BUFFER_TOO_SMALL` semantics), maps `AxlFsStatus` → spec
  `EFI_STATUS` codes, and installs both protocols on a fresh
  handle. `axl_fs_provider_unpublish` force-closes every
  outstanding `AxlFsProviderFile` (so AXL_SERVICE_DRIVER teardown
  is clean even with stale UEFI consumer pointers — they get
  `EFI_DEVICE_ERROR` on next call). 32 unit tests pin the
  contract, including an end-to-end UCS-2/UTF-8 round-trip
  against `résumé.txt` + `日本語.bin` filenames.
- **`<axl/axl-device-path.h>`** — surfaces the vendor device-path
  constructor (`axl_device_path_make_vendor`) consumers used to
  hand-roll. Allocates a two-node chain (HW_VENDOR_DP + END
  terminator) ready for `axl_protocol_register("device-path", ...)`.
- **`sdk/examples/memfs.c`** — worked example: read-only RAM-disk
  filesystem published via `axl_fs_provider_publish` in ~190 LOC,
  including all callbacks. Compares favorably with the ~1500 LOC
  of EFI plumbing axl-webfs's `src/mount/` carried before the
  Phase C migration.

### Fixed

- **`axl_utf8_to_ucs2_buf` now decodes multi-byte UTF-8.** Earlier
  implementation cast bytes through Latin-1
  (`dst[i] = (unsigned short)src[i]`), silently corrupting any
  filename outside ASCII (`résumé.txt` smeared across CHAR16 cells
  as `r\xC3\xA9sum\xC3\xA9.txt`). The fs-provider thunks need the
  real decoder for the `EFI_FILE_INFO` UCS-2 trailer round-trip
  to work; while there, the allocating cousin
  `axl_utf8_to_ucs2` was already correct so this brings the two
  into parity. Five new unit tests pin the multi-byte encode +
  decode path.

- **Path-helper dogfooding.** `sdk/examples/memfs.c` and the
  fs-provider unit-test mock both rolled their own basename loops
  (`while (*path == '/') path++`); switched to
  `axl_path_get_basename` so the example we ship doesn't teach
  consumers to reinvent SDK primitives.

- **Round-trip dogfooding test.** New `test_volume_enumerate_round_trip`
  in `axl-test-fs-provider.c` proves the *consumer* side of
  `<axl/axl-fs.h>` (`axl_volume_enumerate`,
  `axl_volume_get_label_by_handle`) sees fs-provider publications
  transparently: publish a mock with `default_label = "MockFs"`,
  walk volumes, locate by handle, retrieve label, expect "MockFs".
  Three new assertions.

## 0.17.1 — 2026-05-11

### Fixed

- **clang-tidy CI gate cleared.** v0.17.0 shipped artifacts
  successfully (Release + Docs workflows green) but the CI
  workflow's clang-tidy step failed on three findings — release
  was published before that gate fired, hence the patch bump.
  `src/data/axl-xml-writer.c:84,116` — added explicit
  `default: break;` to the body-escape and attribute-escape
  character switches (both intentionally only replace a known
  sub-alphabet; `bugprone-switch-missing-default-case`).
  `src/net/axl-http-route.c:164` — added
  `// NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)` on
  the first `va_arg` of `axl_http_server_add_routes`; the
  analyzer loses track of `va_start` through the `for(;;)` +
  `va_arg` macro expansion, no early-return shape to hoist per
  `feedback_clang_tidy_valist_false_positive`. No runtime
  behavior change.

## 0.17.0 — 2026-05-11

### Added

- **`<axl/axl-net-opts.h>` + `axl_config_descs_net` /
  `axl_config_descs_append`** — canonical `AxlNetOpts` sub-struct
  with three stateless connection-side fields: `nic_index`
  (with `AXL_NET_NIC_AUTO` sentinel for auto-detect),
  `local_ip` (IPv4 to `bind(2)` the local socket end — outbound
  source for clients, listen address for servers; same syscall,
  role implied by what the consumer does next), and `port`
  (`uint16_t`). `axl_net_init` / `axl_net_init_from_opts` are
  the one-call DHCP bring-up — static-IP setup is intentionally
  out of scope (it's the firmware `ifconfig` layer's job; call
  `axl_net_set_static_ip` directly or use UEFI Shell
  `ifconfig`). The paired `axl_config_descs_net` / `_append`
  helpers in `<axl/axl-config.h>` emit the standard descriptor
  entries into a consumer-owned accumulator with offsets shifted
  by the consumer's embedded-sub-struct `offsetof`, so consumers
  compose their own `AxlConfigDesc[]` table without copy-paste;
  preserves `short_name` / `choices` and keeps full C
  type-checking on the consumer's own fragment.
  `AXL_NET_OPT_SOURCE_IP` (CLIENT preset) and
  `AXL_NET_OPT_LISTEN_IP` (SERVER preset) both target the same
  `local_ip` field, differing only in CLI vocabulary
  (`--source-ip` vs `--listen-ip`). `tools/fetch.c`,
  `tools/rfbrowse.c`, and `tools/netinfo.c` gain `--nic` and
  `--source-ip` flags via the new helpers (the hand-rolled
  `--source` flag was renamed `--source-ip` for consistency).
  Solves a four-consumer pain point filed by axl-webfs; future
  `_log` / `_tls` group helpers slot in the same pattern when a
  second consumer asks.

- **Streaming HTTP client request bodies** in
  `<axl/axl-http-client.h>` — `axl_http_request_streaming(c,
  method, url, streamer, ctx, cleanup_fn, total_size, content_type,
  extra_headers, &out_resp)` lets a producer callback feed the
  request body chunk-by-chunk; `axl_http_request_stream_file(c,
  method, url, path, ...)` is the file-source convenience. Passing
  `total_size = (size_t)-1` selects `Transfer-Encoding: chunked`;
  any other value emits `Content-Length`. Closes the
  client/server symmetry — the server already streamed both
  response bodies (`axl_http_response_set_streamer`) and request
  bodies (upload routes). Mount clients hitting multi-chunk PUTs
  through axl-webfs were forced into per-chunk-PUT (broken, each
  Write overwrites) or buffer-all-then-flush (bounded by ~256 MB
  UEFI RAM); both go away.

- **`axl_http_response_set_streamer(r, total, streamer, ctx,
  cleanup_fn)`** in `<axl/axl-http-server.h>` — server-side
  streaming response bodies via a producer callback. Caller
  emits the body chunk-by-chunk through `AxlStream` writes;
  the server frames `Transfer-Encoding: chunked` when
  `total = (size_t)-1` or `Content-Length: <total>` otherwise.
  Used by the WebDAV GET path and any consumer that needs to
  hand the body off to an in-process generator without RAM-
  resident assembly.

- **`AxlUrl` userinfo + fragment parsing** in `<axl/axl-url.h>` —
  `AxlUrl` gains `user`, `password`, and `fragment` fields
  populated by `axl_url_parse`. Full RFC 3986
  `scheme://user:pass@host:port/path?query#fragment` round-trip
  through `axl_url_to_string`. Lets clients carry credentials
  in a single URL token (e.g. Basic-auth Redfish endpoints) and
  honor fragment identifiers without a separate field plumbing
  pass.

- **`axl_file_move(src, dst)`** in `<axl/axl-fs.h>` —
  cross-directory file move via copy-then-delete. Complements
  `axl_file_rename`, which is restricted to the same parent
  directory by UEFI's `SetInfo(FileInfo)` semantics.

- **`AxlFileInfo.mtime_unix` + `AxlDirEntry.mtime_unix`** fields
  in `<axl/axl-fs.h>` — Unix-epoch modification time surfaced
  from `EFI_FILE_INFO.ModificationTime` (previously discarded).
  Source-compatible addition; consumers that initialize these
  structs with `{...}` get the new field zero-initialized for
  free. Unblocks WebDAV `<D:getlastmodified>` and any consumer
  that needs file mtime without an extra `SetInfo` round-trip.

- **`axl_efi_find_config_table(guid, out_ptr)`** in
  `<axl/axl-sys.h>` — looks up an entry in the EFI System
  Table's `ConfigurationTable` array by GUID. Used by mkfixture
  to snapshot ACPI / SMBIOS / ESRT root pointers without
  re-implementing the walk; usable directly by any consumer
  reaching into the configuration-table set.

- **WebDAV `before_response` hook** in `axl_http_server_add_webdav`
  — opt-in last-call header-mutation callback fired after the
  response status/headers are assembled but before bytes hit the
  wire. Used by the WebDAV `Want-Digest` path (below) to emit
  `Digest:` only when the client requested it. Consumer-supplied
  callbacks can also inject custom headers or tweak status codes
  per-request.

- **WebDAV RFC 3230 digest support** in `axl_http_server_add_webdav`
  — server-side response-body digest emission when the client
  sends `Want-Digest: <alg>` (responding `Digest: <alg>=<base64>`
  via the new `before_response` hook), plus PUT-side
  validation: when the request carries a `Digest:` header,
  the upload path computes the configured algorithm over the
  received body and rejects on mismatch with `409 Conflict`.
  Algorithms surfaced from `<axl/axl-digest.h>`. Lets clients
  catch in-flight body corruption without an out-of-band hash
  fetch.

- **`AxlXmlWriter` + `AxlXmlReader` (`<axl/axl-xml.h>`)** — streaming
  XML writer and pull-token reader. Writer mirrors `AxlJsonWriter`'s
  shape: value-typed state struct, caller-owned `AxlString` backing,
  flags bitmask at init (`AXL_XML_WRITER_PRETTY`), void emitters,
  sticky-error flag. Auto-escapes `&` `<` `>` in body and `&` `<` `"`
  in attribute values. Bit-packed per-depth flags decide self-close
  vs `</foo>` and pretty-mode newline-before-close. Reader is opaque
  with `axl_xml_reader_next(r, &token)` pulling one
  `AXL_XML_TOKEN_{START_ELEMENT, END_ELEMENT, TEXT, END_DOCUMENT}` at
  a time; attribute lookup via `axl_xml_reader_attr(r, "name")` while
  positioned at a START. Entity decoding into a reusable scratch
  buffer: 5 named (`&amp;` `&lt;` `&gt;` `&quot;` `&apos;`) plus
  decimal and hex numeric character refs (UTF-8 encoded; U+0000 and
  UTF-16 surrogates U+D800-U+DFFF rejected per XML 1.0 §4.1). CDATA
  passes through as TEXT with `is_cdata` set. Comments, processing
  instructions, and DOCTYPE declarations are skipped silently — the
  reader balances `[`/`]` brackets inside DOCTYPE so entity
  definitions never get processed (closes the billion-laughs class).
  Strict well-formedness: tag balance, single root, no
  non-whitespace content before/after root. Error reporting via
  `axl_xml_reader_error(r, &line, &col, &msg)`. Namespace-aware
  resolution: when the reader hits an `xmlns:` declaration it
  populates `AxlXmlToken.ns_uri` so WebDAV / SOAP-style envelope
  consumers can dispatch on namespace URI rather than prefixed
  local name. Companion helpers `axl_xml_token_local_name(&tok)`
  and `axl_xml_token_attr_local_name(&tok, "name")` strip the
  prefix when the consumer wants the bare local-name half of a
  prefixed `QName`. 53 unit tests covering both halves including
  a WebDAV PROPFIND envelope round-trip through both writer and
  reader. Out of scope: DTD / XSD / RelaxNG validation, XPath,
  XSLT, XML signatures.

- **`axl_guid_v5(namespace, name, out)`** in `<axl/axl-sys.h>` —
  name-based UUIDv5-shaped GUID derivation. SHA-1 of namespace
  bytes + name bytes, version + variant bits set per RFC 4122
  §4.3. Used internally by `axl_service_guid` to derive each
  service's identity from `AxlService.name`; useful directly to
  any consumer that wants stable GUIDs from string keys.

- **`axl_service_guid(svc, out)`** in `<axl/axl-service.h>` —
  convenience wrapper that derives a service's protocol identity
  GUID via `axl_guid_v5` against the AXL_SERVICE namespace.

- **`axl_service_supervise(deploy)`** in `<axl/axl-service.h>` —
  the standard "block on default loop, then stop the service"
  body extracted from `axl_service_main`'s `start` verb. Custom
  consumer mains compose this directly instead of duplicating the
  `loop_run` / `stop` / rc-translate sequence.

- **`axl_protocol_find_guid` / `axl_protocol_enumerate_guid`** in
  `<axl/axl-sys.h>` — GUID-keyed counterparts to `axl_protocol_find`
  / `_enumerate`, symmetric with the existing
  `axl_protocol_register_guid` / `_unregister_guid`. Lets consumers
  that already hold a GUID skip the name-registry lookup. AxlService
  uses these to drop its raw `axl_bs()->LocateProtocol` /
  `LocateHandleBuffer` calls — its body is now UEFI-type-free
  outside the firmware-ABI unload-stub signature.

- **`axl_net_drivers_up()`** in `<axl/axl-net.h>` — load NIC
  drivers, connect SNP, wait for link-up. Decoupled from address
  assignment so static-IP callers don't burn the 10 s default DHCP
  timeout that `axl_net_auto_init` otherwise imposes. Used
  internally by `axl_net_auto_init` (refactored to call this first)
  and by the new `axl_net_bring_up`.

- **`axl_net_bring_up(nic, static_ipv4, netmask, gateway, timeout, addr_out)`**
  in `<axl/axl-net.h>` — one-call DHCP-or-static bring-up + address
  read-back. NULL `static_ipv4` → DHCP path; non-NULL → static IP
  with caller-supplied or `/24`-default netmask + optional gateway,
  then 500 ms IP4Config2 settle. The "what every networked tool
  does at startup" preamble factored out so HTTP services, REST
  tools, and one-shot fetch utilities don't reinvent it.
  `tools/{fetch,rfbrowse,netinfo}` migrated to use it.

- **REST request helpers** in `<axl/axl-http-server.h>`:
  `axl_http_request_accepts(req, mime)` (routes through the existing
  `axl_http_accepts` matcher — case-insensitive, wildcard-aware, q-value
  tolerant), `axl_http_request_wants_json(req)` (the
  `application/json` shorthand), and `axl_http_request_get_json(req,
  out)` (parses `req->body` into a caller-owned `AxlJsonReader`).
  Lifted from axl-webfs's serve module so any HTTP consumer (REST
  API, one-shot fetch, future services) can reuse them.

- **`axl_http_server_add_routes(server, ...)`** variadic in
  `<axl/axl-http-server.h>` — batch route registration.
  `(method, path, handler, data)` groups terminated by a sentinel
  `NULL` method. Replaces 5–6 separate `add_route` calls + their
  per-call `!= 0` checks with one call + one error path.
  `sdk/examples/{http-server,http-server-driver}.c` migrated to
  dogfood it.

- **`axl_log_file_detach()`** in `<axl/axl-log.h>` — symmetric
  teardown for `axl_log_file_attach`. Flushes the buffer, removes
  the internal handler, closes the file. NULL-safe on
  not-attached state. Closes the gap surfaced by axl-webfs's
  serve service: AxlService driver setup attaches a log file but
  had nothing to call from teardown — the consumer either accepted
  the leak or hand-rolled a parallel implementation.

- **Busybox build (`make axl-busybox`)** — opt-in single-binary
  deployment shape. One `axl.efi` hosts all 18 tools as
  subcommands (`axl.efi cat foo`, `axl.efi grep pattern file`,
  `axl.efi --help`). Default `make tools` is unchanged — per-tool
  `.efi` binaries remain the supported shape. New
  `AXL_TOOL_MAIN(name)` macro in `<axl.h>` switches each tool's
  entry point between `int main(...)` (standalone) and
  `int axl_tool_<name>_main(...)` (busybox) based on
  `-DAXL_BUSYBOX`. Sizes: 18 standalone tools = 3.21 MB total; one
  axl.efi = 569 KB (X64) / 624 KB (AARCH64). Disk savings aren't
  the point — "one file" deployment is.

- **`make tool-sizes` target** — prints per-tool `.efi` size
  sorted ascending plus total + libaxl.a archive size.
  Demonstrates the selective-linking benefit (each tool carries
  only the libaxl.a slices it references).

- **`sdk/examples/service-demo-custom.c` — worked example with
  consumer-visible AxlArgs + AxlConfig usage.** Same single-file
  dual-compile pattern as `service-demo.c`, but `main()` is
  written by hand instead of via the `AXL_SERVICE` macro. Shows
  the consumer how to:

  - mix the standard `start` / `stop` / `status` verbs (which
    `axl_service_main` builds automatically) with a custom verb;
  - call `axl_args_get_uint` / `_bool` / `_string` directly to
    populate the option struct;
  - walk an `AxlConfigDesc[]` table to format option values
    against their descriptor metadata (the `config` verb prints
    parsed values + descriptions + defaults).

  ```
  service_demo_custom.efi start --port 9090 --verbose --name foo
  service_demo_custom.efi stop
  service_demo_custom.efi status
  service_demo_custom.efi config --port 9090   # foreground only
  ```

  The `config` verb is foreground-only (no driver involvement) and
  exists specifically to demonstrate the AxlArgs ↔ AxlConfig
  connection that `axl_service_main` packages internally.

  Output binaries: `service_demo_custom.efi` (launcher) +
  `service_demo_custom-dxe.efi` (driver). New Makefile target
  `service-demo-custom`. Different protocol GUID from
  service-demo so both demos can run side-by-side.

- **WebDAV class-1 + MOVE/COPY (`axl_http_server_add_webdav`)** in
  `<axl/axl-http-server.h>`. Generic WebDAV server adjunct to
  `AxlHttpServer` (RFC 4918 §9). The SDK owns all the protocol
  bits — verb dispatch, PROPFIND 207 Multi-Status XML, Depth /
  Destination / Overwrite header parsing, DAV: 1 advertisement;
  the consumer fills in an `AxlWebDavOps` callback table mapped
  onto its own filesystem. Verb scope: OPTIONS, PROPFIND, GET,
  HEAD, PUT, DELETE, MKCOL, MOVE, COPY. PROPPATCH, LOCK, UNLOCK,
  and If-header conditionals remain out of v1 scope (modern
  clients — Windows Explorer, macOS Finder, davfs2, cadaver —
  work without them when the server doesn't advertise the lock
  class). COPY adds the spec-strict Depth parser (only "0" or
  "infinity" per §9.8.3; "1" → 400) and pre-stats the source for
  an RFC-correct 404 on missing-source (rather than the generic
  409 the MOVE handler still emits).

  GET and PUT inherit the streaming primitives from earlier in
  this run: `axl_http_response_set_streamer` for response bodies
  (multi-GB safe) and the `AxlUploadHandler` abort contract for
  request bodies. Range requests on GET use
  `axl_http_response_set_content_range` to advertise the slice;
  unsatisfiable ranges return 416 per RFC 7233 §4.4. Up to 4
  WebDAV mounts per server. Per-mount single-in-flight PUT — the
  SDK refuses concurrent PUTs on the same mount rather than
  trampling either request's state. Two consumers ready to
  migrate: axl-webfs's serve service (~120 lines of adapter +
  one setup line) and SoftBMC's BMC web UI (deletes ~540 lines
  of WebDav.{c,h}, replaces with ~100 lines of adapter).

- **`axl_hash_table_owns_entries(h)`** in `<axl/axl-hash-table.h>` —
  predicate returning true iff `!copy_keys && key_destroy != NULL
  && value_destroy != NULL`. Used by `set_content_range_header`
  (and WebDAV's `ensure_headers`) to detect when a caller
  pre-allocated `r->headers` with the wrong destroy-func contract
  (e.g. `axl_hash_table_new_str()` would silently leak both
  strdup'd key and value).

- **TCP/UDP API parity sweep** — UDP catches up to TCP across the
  async, addressing, and lifecycle surfaces:

  - **`AXL_DEFINE_AUTOPTR_CLEANUP(AxlTcp, axl_tcp_close)`** —
    `AXL_AUTOPTR(AxlTcp)` now works (UDP already had it).

  - **`axl_udp_get_local_addr(sock, addr, size, *port)`** in
    `<axl/axl-udp.h>` — read back the bound station address +
    port. Required for the ephemeral-port case
    (`axl_udp_open(&s, 0)` had no readback path before).

  - **`axl_udp_recv_async(sock, loop, cancel, cb, data)`** —
    replaces `axl_udp_recv_start` / `_stop`. New `AxlUdpCallback`
    typedef receives per-event `AxlStatus` and returns `bool`
    (true = re-arm, false = stop in-place). Optional
    `AxlCancellable`. Mirrors `axl_tcp_recv_async`. Breaking
    change for the old typedef + start/stop functions.

  - **`axl_udp_send_async(sock, dest, port, buf, len, loop,
    cancel, cb, data)`** — non-blocking Transmit with completion
    callback. Single in-flight enforced (returns AXL_ERR if a
    previous send hasn't completed). Mirrors `axl_tcp_send_async`.

  - **`axl_udp_open_via(out, port, source_ip)`** — source-IP
    pinning for multi-NIC hosts. Mirrors `axl_tcp_listen_via`.
    Internally factored out the per-NIC service-binding picker
    (`axl_net_locate_sb` in `axl-net-internal.h`) so TCP and UDP
    share the same selection ladder.

  - **`AxlUdpSocket` → `AxlUdp`** — type rename for naming
    parity with `AxlTcp`. Breaking; no external consumers in
    sibling repos at the time of the change.

  - **`axl_udp_connect(sock, peer, port)` / `axl_udp_disconnect(sock)`** —
    Linux-style peer lock for UDP. After connect, the kernel
    filters incoming datagrams to the peer; subsequent
    `axl_udp_send` / `_send_async` accept NULL `dest` (uses the
    configured peer). Explicit dest still overrides per-packet
    (per UEFI 2.x §27.4.1).

  - **`axl_udp_join_multicast(sock, group)` /
    `axl_udp_leave_multicast(sock, group)` /
    `axl_udp_set_broadcast(sock, enable)`** — IGMP join/leave via
    `EFI_UDP4_PROTOCOL.Groups()`, broadcast recv-filter toggle.
    `leave_multicast(NULL)` leaves all groups (matches UEFI
    semantics). Multicast-addr validation (224.0.0.0/4) at the
    API boundary.

- **`axl_http_response_set_content_range(r, start, end, total)`** in
  `<axl/axl-http-server.h>` — formats and inserts the
  `Content-Range` header without touching status or body. For
  partial-content responses sent via
  @ref axl_http_response_set_streamer (or any other path that
  doesn't go through @ref axl_http_response_set_range), where the
  consumer manages status separately. Lazy-allocates `r->headers`
  with `axl_str_hash` + `axl_free_impl` destructors so it composes
  cleanly with the request-side header-table allocation pattern.

- **`axl-cc --service NAME source.c` — single-file service build.**
  Compiles the source twice and produces both `.efi` outputs:

  1. `axl-cc --type driver -DAXL_SERVICE_BUILD_DRIVER source.c
      -o NAME-dxe.efi` (driver image)
  2. `axl-cc --embed NAME-dxe.efi=NAME source.c -o NAME.efi`
      (launcher app, with the driver baked in)

  NAME must be a valid C identifier — the embed symbol the
  `AXL_EMBED_DECLARE(svc)` inside the `AXL_SERVICE(svc)` macro
  expects must match `axl_embedded_<NAME>`. So passing
  `--service my_service` pairs with `AXL_SERVICE(my_service)` in
  the source.

  All other flags (`--debug`, `--release`, `--arch`, `--verbose`,
  `--minimal-runtime`, `-I`, `-D`, `-W`) forward to both
  invocations. `-o` is ignored — output filenames are fixed at
  `NAME.efi` and `NAME-dxe.efi` in the current directory.

  ```
  axl-cc --service my_service service.c
  # → my_service.efi + my_service-dxe.efi
  ```

  Step 4 of 5 toward the `AXL_SERVICE` single-file pattern.
  Service-demo migration in step 5 proves the end-to-end path.

- **`AXL_SERVICE(svc)` — single-file service macro.** New macro in
  `<axl.h>` that emits whichever entry point the current build
  needs:

  - When `AXL_SERVICE_BUILD_DRIVER` is defined (driver-image
    compile), expands to `AXL_SERVICE_DRIVER(svc)` — emits
    `DriverEntry`.
  - Otherwise (launcher-app compile), expands to `main()` that
    declares the embedded driver blob (matching the symbol name
    `axl-cc --service` emits) and delegates to `axl_service_main`
    with a pre-filled deploy descriptor.

  Pairs with the forthcoming `axl-cc --service NAME source.c` flag
  that compiles the same source twice (once with
  `-DAXL_SERVICE_BUILD_DRIVER` for the driver, once without for the
  launcher; embeds the driver into the launcher). Result: **one
  source file, two `.efi` outputs, zero handwritten launcher
  boilerplate**.

  ```c
  /* service.c — entire file */
  #include <axl.h>

  typedef struct { uint16_t port; bool verbose; } MyOpts;
  static MyOpts opts;
  static const AxlConfigDesc opts_descs[] = { /* ... */ };
  static int my_setup(AxlLoop *loop, void *user) { /* ... */ }
  static int my_teardown(void *user) { /* ... */ }

  static const AxlService my_service = {
      .name           = "my-service",
      .opts_descs     = opts_descs,
      .setup          = my_setup,
      .teardown       = my_teardown,
      .user           = &opts,
      .driver_tick_ms = 50,
  };

  AXL_SERVICE(my_service);
  ```

  Multi-service tools and consumers wanting custom verbs don't use
  the macro — they write their own `main()` and call
  `axl_service_main` directly (or wire `axl_args_run` themselves).

- **`axl_service_main(deploy, argc, argv)` — default launcher /
  supervisor body for service consumers.** Builds a default
  `axl_args_run` verb tree (`launch [--detach]`, `stop`, `status`)
  from the deploy descriptor and dispatches argv. The `launch`
  verb auto-populates `svc->user` from the parsed args
  (synthesizing an `AxlArgDesc[]` from `svc->opts_descs` so the
  consumer doesn't repeat the descriptor in two formats), calls
  `axl_service_launch_embedded`, and either:

  - exits if `--detach` was passed (driver continues to run);
  - blocks on `axl_loop_default` until Ctrl-C, then calls
    `axl_service_stop` on the way out.

  Most service consumers won't need their own `main()` — the
  forthcoming `AXL_SERVICE` macro will emit one that delegates to
  this. Direct use is for consumers who want to mix the default
  verbs with their own (extra verbs, custom help prolog) — they
  call `axl_args_run` themselves and dispatch the standard verbs
  in.

  AXL_CFG → AXL_ARG type mapping (UINT field-size dispatch
  matches the existing `axl_config_target_to_string` pattern):
  BOOL→BOOL, UINT→U8/U16/U32/U64 by field_size, INT→S64,
  STRING→STRING. AXL_CFG_MULTI is mapped to STRING (single-value;
  consumers needing multi populate `svc->user` manually).

- **`AxlService.restart_max` + `.restart_backoff_ms` — setup retry.**
  New fields on the AxlService struct:

  ```c
  uint32_t restart_max;        // default 0 = no retry
  uint32_t restart_backoff_ms; // delay between retries
  ```

  When `restart_max > 0` and setup returns non-AXL_OK, the framework
  retries up to `restart_max` more times (total attempts =
  `restart_max + 1`) with `restart_backoff_ms` between attempts.
  Useful for services where setup may transiently fail (NIC not yet
  up, DHCP pending, fs not yet enumerated). axl-webfs's `serve` is
  the canonical case — `serve` over a NIC that just came up may
  need a beat before TCP4 is bindable.

  Foreground only. Driver-mode setup failure aborts DriverEntry; the
  firmware decides what to do next (typically unload, then maybe
  reload on a later dispatch).

  Test pinning: succeeds-on-attempt-3 with restart_max=2,
  exhaust-budget returns AXL_ERR, exact attempt counts in both
  cases.

- **AxlService lifecycle banner logs.** `axl_service_main` /
  `axl_service_supervise` emit two info-level log lines around
  the run, tagged with the framework's `service` domain:

  ```
  [service]  service 'axl-webfs-serve' starting
  ...
  [service]  service 'axl-webfs-serve' stopped (rc=-1, 8423 ms)
  ```

  Consumers don't write any code — the banner appears whenever a
  service runs to completion (or setup fails, in which case the
  stopped line includes "setup failed"). Elapsed milliseconds use
  `axl_time_get_ms` (monotonic). The existing `axl_debug`
  setup/teardown ENTER/EXIT markers stay; the banner is at info
  level so it surfaces under default log levels without
  `AXL_LOG_LEVEL=debug`.

  Driver-mode services (deployed via `AXL_SERVICE_DRIVER`) keep
  the existing debug-level lifecycle markers — there's no clean
  "stopped" moment in the driver's DriverEntry-returns-immediately
  shape, and noisy info-level lines per tick would be wrong.

- **`AxlService.watchdog_keep_armed` — opt-out for the auto-disarm.**
  New field on the AxlService struct: `bool watchdog_keep_armed`.
  Default false (zero-init). At run start, the framework calls
  `axl_watchdog_disarm()` so the firmware's 5-min boot-services
  timeout doesn't reset the system mid-session — long-running
  foreground services would otherwise hit the watchdog.

  Set `.watchdog_keep_armed = true` if your service is short-lived
  enough to want the safety net or you manage the watchdog
  yourself (`axl_watchdog_set` / `axl_watchdog_pet`). No restore
  on teardown — the consumer that disables this flag owns the
  watchdog policy.

- **`AxlService.on_signal` — opt-in Ctrl-C / break callback.** New
  field on the AxlService struct: `AxlSignalHandler on_signal`.
  When non-NULL, the framework installs it via
  `axl_signal_install` for the duration of the run and restores
  the default on teardown. NULL keeps the existing behavior — the
  loop polls the break flag and quits, which then unwinds through
  teardown.

  Use this for custom shutdown logging or to set application
  flags before the loop returns. Doesn't replace the loop's
  built-in poll-and-quit; it composes with it.

  Test pinning: precondition (no handler), loop_new,
  run-to-quit, setup ran, handler observed installed during
  setup, handler restored to default after.

- **`axl_driver_load_buffer(buf, len, *out_handle)` — buffer-source
  counterpart to `axl_driver_load(path, *out)`.** LoadImage from a
  memory buffer with no DevicePath; returns the driver handle for use
  with `axl_driver_set_load_options` / `axl_driver_start` /
  `axl_driver_unload`. Use case: tools that `.incbin` a companion
  driver into the app and need per-call LoadOptions and explicit
  handle tracking. `axl_driver_ensure_with_embedded` doesn't fit when
  (a) the protocol isn't unique-per-driver — its step-1 short-circuit
  on `LocateProtocol` skips your load if some unrelated handle is
  already registered (e.g. EFI_FILE_PROTOCOL), or (b) the consumer
  needs the AxlDriverHandle to set per-call LoadOptions. For the
  AxlService case keep using `axl_service_start_embedded`.

  Internal `driver_load_embedded` refactored to call this
  primitive + `driver_start_and_verify`, so there's one
  LoadImage-from-buffer call site. The existing
  `test-service-driver.sh` integration test continues to PASS as
  positive-path coverage.

- **`axl-cc --embed PATH[=NAME]` and `<axl/axl-embed.h>` macros.**
  axl-cc now generates the `.incbin` sidecar and links it into the
  output, replacing the hand-rolled `.S` file consumers used to
  carry alongside their launcher `.c`. NAME defaults to the basename
  of PATH with non-identifier chars replaced by `_`; emitted symbols
  are `axl_embedded_<NAME>` and `axl_embedded_<NAME>_end` to match
  the existing in-tree convention. Repeatable.

  The framework is content-agnostic — driver `.efi` is the canonical
  use case but anything works (TLS CA bundles, static JSON5 config,
  HTML for an embedded server, lookup tables, license text, calibration
  data). `sdk/examples/embed-asset.c` is the non-driver worked example;
  `sdk/examples/service-demo/launch.c` covers the driver case.

  The new public header `<axl/axl-embed.h>` provides
  `AXL_EMBED_DECLARE(name)` / `AXL_EMBED_DATA(name)` /
  `AXL_EMBED_SIZE(name)` so the C-side declarations stay in sync
  with whatever generated the symbols (axl-cc or a hand-written
  `.S`). Use:

  ```c
  AXL_EMBED_DECLARE(my_driver);
  /* ... */
  d.driver_blob     = AXL_EMBED_DATA(my_driver);
  d.driver_blob_len = AXL_EMBED_SIZE(my_driver);
  ```

  Intended pairing: `axl_service_launch_embedded` / mkrd-style
  embedded-driver tools.

  `sdk/examples/service-demo/launch.c` updated to use the macros as
  the in-tree proof. New integration test
  `test/integration/test-axl-cc-embed.sh` (wired into CI alongside
  test-yield-ctrlc.sh) builds the launcher via `axl-cc --embed` and
  asserts the same cross-binary `LoadOptions` round-trip the
  Makefile-built path produces.

- **`axl_service_stop(deploy)` — symmetric counterpart to
  `axl_service_launch_embedded`.** Resolves the running driver
  image's handle by `LocateHandleBuffer` against
  `deploy->service->protocol_guid`, then `axl_driver_unload`s each
  match. The driver image's `AXL_SERVICE_DRIVER` unload stub fires
  synchronously and runs the framework's teardown sequence
  (loop_detach → service teardown → protocol unregister →
  axl_config_free → loop_free). Idempotent: returns AXL_OK when the
  protocol isn't currently published. Doxygen documents the
  consumer-held interface dangling hazard (UEFI's protocol model
  has no ref counting; same risk as the shell's `unload -n`).

  **Macro change:** `AXL_SERVICE_DRIVER` now publishes the service
  GUID on the driver image's own handle (gImageHandle) rather than
  a fresh sentinel. This is what makes stop work — `LocateHandleBuffer`
  now returns the image handle directly, no separate metadata
  lookup needed. Drivers that want to layer additional protocols
  on the image handle continue to compose normally; the service GUID
  just shares the handle's protocol list.

  Reasoning: the side-table / dual-install / metadata-protocol
  alternatives all worked around a sentinel that didn't need to
  exist. UEFI itself routinely installs identity-marker protocols
  on the driver image's own handle (LoadedImage, DevicePath, etc.).

  Tests
  - `test_service_stop_validates` in axl-test-util — NULL safety +
    "stop on never-running deploy is no-op success." 2793/2793
    both arches.
  - `test/integration/test-service-driver.sh` extended to
    launch → stop → relaunch round-trip. Asserts the driver's
    teardown actually fired (proves UnloadImage triggered the
    AXL_SERVICE_DRIVER unload stub) AND that relaunch sees a fresh
    setup (no stuck-protocol short-circuit).

  Example: `sdk/examples/service-demo/stop.c` — three calls
  (`is_running`, `stop`, `is_running`).

- **`AxlService` — structured-lifecycle wrapper over `AxlLoop`.** New
  public header `<axl/axl-service.h>`. A typed shape for
  "long-running event loop with setup/teardown/options," shipped
  as a UEFI driver image and supervised from a foreground
  launcher. The driver image carries the `setup`/`teardown`
  callbacks + an `AxlConfigDesc[]` for its options; the launcher
  passes options through `EFI_LOADED_IMAGE_PROTOCOL.LoadOptions`
  as a URL-encoded query string, the driver decodes them back
  into the same `svc.user` struct, and `axl_loop_attach_driver`
  runs the loop under the firmware notify-timer.

  Deployment surface (end-state — see the individual entries
  elsewhere in this release for each piece):

  - **Driver image**: `AXL_SERVICE_DRIVER(svc)` macro emits the
    DXE driver shim (LoadOptions decode + `setup` + supervised
    loop + UnloadImage teardown).

  - **Single-source-file build**: `AXL_SERVICE(svc)` macro +
    `axl-cc --service NAME source.c` produce both binaries
    (launcher + driver) from one `.c`.

  - **Launcher supervision**: `axl_service_main(deploy, argc,
    argv)` provides default `start [--detach]` / `stop` /
    `status` verbs; consumers that want custom verbs call
    `axl_service_supervise` directly from their own `main`.

  - **In-process driver-tick mode**:
    `axl_service_attach_driver` / `_detach_driver` /
    `_teardown` for consumers that drive the loop themselves
    (no separate driver image — same launcher binary).

  - **Embedded-binary lifecycle**: `axl_service_start_embedded` /
    `axl_service_stop` / `axl_service_is_running` for explicit
    foreground control of an embedded driver image.

  Setup-failure contract: setup owns its own unwind; framework
  only calls teardown after a successful setup.

  Cross-binary ABI tripwire is documented on the header — same
  source tree, identical compile flags (`AXL_TLS`,
  `AXL_MEM_DEBUG`, arch) for both binaries. Identity GUID is
  derived from `AxlService.name` via `axl_guid_v5` so launcher
  and driver agree without manual GUID plumbing.

  NOT a Unix daemon (no fork/setsid/chdir/umask). Closer in
  shape to a systemd unit. Naming retired the alternative
  `AxlDaemon` candidate to avoid the Linux baggage.

  axl-webfs is the empirical consumer. mkrd doesn't fit
  (one-shot protocol publish, no setup/teardown lifecycle) and
  stays on the underlying `axl_driver_ensure_with_embedded`
  directly.

- **`axl_config_to_string` / `axl_config_from_string` /
  `axl_config_target_to_string`** in `<axl/axl-config.h>` —
  three primitives behind the service API's cross-binary option
  hand-off, also useful standalone for diagnostics or any other
  cross-binary state passing. Wire format is URL-encoded query
  string per RFC 3986 (`key=value&...`) — both keys and values
  percent-encode special bytes so `&` / `=` / Unicode in values
  round-trip cleanly. Dogfoods `axl_url_encode`/`_decode`.

- **`axl_driver_ensure_with_embedded` extended with
  `load_options` + `load_options_size` arguments.** Installs
  the bytes via `axl_driver_set_load_options` between LoadImage
  and StartImage on both the disk-load and embedded-blob paths;
  the side-table from the LoadOptions-leak fix catches the
  unload-time release automatically. Existing callers
  (`axl_driver_ensure`, mkrd) updated to pass `NULL/0` — no
  behavior change.

- **`axl_driver_get_load_options_raw(out_buf, out_size)`** —
  raw-bytes counterpart to `axl_driver_get_load_options`
  (which assumes UCS-2). `AXL_SERVICE_DRIVER` uses this to
  read the UTF-8 query-string payload without encoding
  misinterpretation.

- **`AXL_DRIVER(entry, unload)` macro + `AxlHandle` /
  `AxlSystemTable` / `AXLAPI` / `AxlEfiStatus`** — DXE drivers can
  now be written without spelling `EFI_*` or `EFIAPI` in consumer
  source. The `AXL_DRIVER` macro emits the firmware-side
  `DriverEntry` + unload stubs and wires `axl_driver_init` /
  `axl_driver_set_unload` automatically; consumer entry/unload
  take `AxlHandle` parameters and return `int` (0 = OK, non-zero
  aborts the load). New header `<axl/axl-efi-status.h>` exposes
  `AxlEfiStatus` (binary-compatible with `EFI_STATUS`) and 27
  curated UEFI 2.11 Appendix D status constants
  (`AXL_EFI_NOT_FOUND`, `AXL_EFI_INVALID_PARAMETER`, etc.) for
  spec-protocol publishers (`EFI_FILE_PROTOCOL`,
  `EFI_BLOCK_IO_PROTOCOL`, driver-binding callbacks) that need to
  return spec-mandated values without pulling all of
  `<uefi/axl-uefi.h>`. `_Static_assert`s in `src/util/axl-driver.c`
  pin the binary compat. `sdk/examples/driver.c` is the reference
  for the AXL-only path (zero `EFI_*` identifiers in the consumer
  source after this refactor). `AXLAPI` is published as an alias
  for `EFIAPI` for the rare consumer-defined firmware-called
  callback that lives outside the macro path.

- **`axl_loop_attach_driver(loop, interval_ms)` /
  `axl_loop_detach_driver(loop)`** — drive an `AxlLoop` from a
  firmware-managed periodic timer when the consumer is a DXE
  driver image with no foreground caller. `axl_loop_run` is the
  foreground driver and blocks in `gBS->WaitForEvent`; UEFI driver
  entry points return to the firmware after publishing protocols,
  so without this helper anything async in the loop is dead. The
  attach helper installs an `EVT_TIMER | EVT_NOTIFY_SIGNAL` event
  at `TPL_CALLBACK` (the lowest TPL legal for signal events per
  UEFI 2.11 §7.1) whose notify drains the loop in non-blocking
  mode every `interval_ms`. Idle callbacks, defer-queue work, and
  source events all dispatch from this notify the same way they
  would inside `axl_loop_run`. New backend primitive
  `axl_backend_event_create_notify_timer` exposes the underlying
  EVT_TIMER+EVT_NOTIFY_SIGNAL machinery; the matching close path
  cancels the timer first then frees the bridging context, so
  `axl_backend_event_close` is the symmetric teardown.

  Documented contract — every consumer's source callback must run
  fast (< ~1 ms is the rule of thumb). At `TPL_CALLBACK` the loop
  shares the firmware-driver notify FIFO with TCP4 / MNP / SNP, so
  a slow callback holds the TPL and starves co-located firmware
  drivers. `axl-webfs` is the reference consumer (HTTP server
  inside a DXE driver image). See `src/loop/README.md` § "Driver
  Mode" and `<axl/axl-loop.h>` doxygen for the full contract.

### Changed

- **`AxlUploadHandler` gains a 6th parameter `bool aborted`.** When
  the TCP peer disconnects mid-upload, `reset_connection` now calls
  the registered upload handler with `(req, resp, NULL, 0, data,
  true)` while the request state is still valid. Mirrors the
  existing `AXL_WS_DISCONNECT` event for WebSocket handlers — same
  lifecycle gap. Handlers that hold per-request state across chunk
  calls (open file handles, accumulators, allocations) MUST release
  it on the abort call; without this signal, that state leaked
  into the next request on the same handler globals (caused
  cross-request data corruption in axl-webfs's PUT path). Mutually
  exclusive with the existing clean-EOF NULL/0 call. Breaking
  change for existing handlers — add the parameter and an
  `if (aborted)` early branch.

- **HTTP middleware now runs ahead of upload routes.** Routes
  registered via `axl_http_server_add_upload_route` previously
  bypassed `axl_http_server_use` middleware entirely (they were
  routed before the dispatch path that runs middleware). Any
  cross-cutting concern (auth, read-only gating, rate limiting)
  silently failed to apply to uploads. Middleware now fires once
  per upload, before the first chunk reaches the handler. On
  rejection, the connection is force-closed (the client almost
  always sent body bytes before reading the rejection — staying in
  keep-alive desyncs the next request). Same fix shape as
  `send_error_response`. Symmetric leak fix in the existing
  `dispatch_request` middleware-rejection path: it was leaking
  `resp.body` / `resp.headers` via early return.

- **`AxlService.protocol_guid` field removed; identity is derived
  from `AxlService.name`.** Consumers no longer hand-allocate a
  UUID per service. The new public helper `axl_guid_v5(namespace,
  name, out)` (in `<axl/axl-sys.h>`) does name-based UUIDv5-shaped
  derivation; `axl_service_guid(svc, out)` is the convenience
  wrapper that uses AxlService's fixed namespace. Both binaries
  in a single-source-tree build see the same derived GUID by
  construction. **Source break, ABI break** for any consumer with
  a static `AxlService` initializer that set `.protocol_guid`:
  delete the line. axl-webfs's `webfs_serve` migrated; demos
  migrated. `AxlService.name` is now REQUIRED — consumers that
  previously omitted it as a log-only label must set it. Per
  `feedback_change_apis_freely` — pre-1.0, no compat bar.

- **`AxlService.driver_tick_ms` is now the single source of truth
  for the firmware-tick period.** `axl_service_attach_driver`
  loses its `tick_ms` parameter — it reads `svc->driver_tick_ms`
  with `0 → AXL_SERVICE_DEFAULT_TICK_MS` (a new public 50 ms
  constant). Eliminates the field/parameter double state and the
  prior "field 0 = default" vs "param 0 = AXL_ERR" asymmetry.
  `AXL_SERVICE_DEFAULT_TICK_MS` is now exposed in
  `<axl/axl-service.h>` so consumers can reference the default.

- **`AxlConfigDesc` gains trailing `.short_name` and `.choices`
  fields** for consumers that want a single descriptor table to
  drive both AxlConfig auto-apply AND a synthesized `AxlArgDesc[]`
  CLI surface. AxlConfig parsing ignores both fields;
  `axl_service_main`'s synthesizer passes them through —
  `.short_name` becomes the AxlArgs short flag, and a non-NULL
  `.choices` on a STRING-typed entry elevates to `AXL_ARG_CHOICE`
  so the CLI parser validates the value and `--help` lists the
  options.

- **AxlService verb / API renamed `launch` → `start`** (systemctl-
  flavored). The default verb tree from `axl_service_main` is now
  `start [--detach]` / `stop` / `status`, mirroring `systemctl`'s
  semantics directly. The matching foreground API renames:

  ```
  axl_service_launch_embedded(deploy)  →  axl_service_start_embedded(deploy)
  ```

  axl_service_stop / _is_running / _teardown unchanged. Per
  `feedback_change_apis_freely` — pre-1.0, no compat bar. axl-webfs
  is the only known consumer; its `serve` handler will need
  s/launch_embedded/start_embedded/ when it migrates.

- **`sdk/examples/service-demo/` collapses into a single
  `sdk/examples/service-demo.c`.** The whole demo — types,
  descriptor, setup/teardown, AxlService struct, and the
  `AXL_SERVICE` macro that emits the entry point — now fits in
  one ~100-line file. No subdirectory, no `core.c` / `main.c` /
  `driver.c` split, no shared.h.

  Build (in-tree): `make service-demo` produces `service_demo.efi`
  + `service_demo-dxe.efi` from the same source compiled twice
  (once with `-DAXL_SERVICE_BUILD_DRIVER` for the driver image,
  once with `EMBED_BLOB(service_demo, ...)` for the launcher).

  Build (consumer / SDK release):
  ```
  axl-cc --service service_demo service-demo.c
  ```

  Output binaries renamed: `service-demo.efi` / `service-demo-launch.efi` /
  `service-demo-stop.efi` / `service-demo-driver.efi` from earlier
  in this branch all collapse into `service_demo.efi` (with default
  verbs `launch` / `stop` / `status` from `axl_service_main`) and
  `service_demo-dxe.efi`.

  `test-axl-cc-embed.sh` renamed to `test-axl-cc-service.sh` —
  asserts the same cross-binary round-trip via the driver-side
  setup log line, but builds the demo via `axl-cc --service`
  instead of two-step manual compile. `test-service-driver.sh`
  asserts on the new log lines (`service-demo: setup: ...`,
  `teardown`, `stopped`) instead of the old `LAUNCH-PASS` /
  `STOP-PASS` printf markers.

  Step 5 of 5 — completes the AxlService driver-only redesign +
  single-file pattern. Total: 5 commits, ~1600 LOC removed,
  consumers writing single-service tools now have a 100-line
  shape with zero handwritten launcher boilerplate.

- **DHCP completion is event-driven instead of 1 Hz polled.**
  `axl_net_auto_init` now registers an `EFI_IP4_CONFIG2_PROTOCOL`
  DataNotify on `Ip4Config2DataTypeInterfaceInfo` and waits on the
  event via the AxlWait infrastructure. EDK2's IP4Config2 driver
  fires the event from `Ip4Config2OnDhcp4Complete` when the address
  commits, so the wakeup is sub-millisecond after DHCP finishes —
  the prior code wasted up to 1 second of dead time per startup.
  The 1 s tick is kept as a fallback for firmware that returns
  `EFI_UNSUPPORTED` on RegisterDataNotify or doesn't fire DataNotify
  on completion.

  `axl_net_drivers_up`'s link-up wait is similarly routed through
  AxlWait's event-loop infrastructure with a condition function
  (no portable SNP-side notify event for link-state, so this stays
  a 100 ms tick poll, just centrally managed and Ctrl-C-cancellable).

- **PIIX4 SMBus IMC retry stalls via `axl_msleep`, not
  `axl_backend_stall`.** The IMC semaphore acquire-loop's
  inter-retry wait was a 1 ms busy-spin, up to 2 seconds at 100% CPU
  on a contended bus. Now event-driven; the host CPU idles between
  checks. Same 2 s budget, no busy waste. (The remaining
  `axl_backend_stall` callers all run at sub-ms cadence below
  firmware timer resolution and are correctly documented:
  KCS IPMI 100 µs, PIIX4 50 µs poll, PIIX4 500 µs hardware errata
  pre-stall, x86_64 TSC calibration 10 ms one-time, NORETURN spin
  after `gBS->Exit`.)

- **`--gc-sections` enabled in LDFLAGS_EFI.** Combined with the
  existing `-ffunction-sections / -fdata-sections` in CFLAGS, this
  gives per-symbol selective linking — dead functions inside an
  otherwise-referenced .o get dropped at link time. Per-binary
  saving today is ~0.2% (the static-archive .o-member granularity
  was already doing most of the work); the real value is
  regression-proofing against future bloat. `KEEP(*(.dbgdir))`
  added to both linker scripts since gc-sections strips it
  otherwise (no symbol references it; only the post-link
  `pe-set-debug` tool reads it).

- **`AXL_SERVICE_DRIVER` macro is now a one-line shim.** The macro
  body — ~145 lines of LoadOptions decode, protocol publish, loop
  creation, attach_driver, and the matching unload-stub teardown
  sequence — moved into a SDK library function
  `_axl_service_driver_init`. The macro shrinks to:

  ```c
  #define AXL_SERVICE_DRIVER(svc)                                    \
    EFI_STATUS EFIAPI                                                \
    DriverEntry(EFI_HANDLE _img, EFI_SYSTEM_TABLE *_st) {            \
      return _axl_service_driver_init(_img, _st, &(svc));            \
    }
  ```

  `tick_ms` is no longer a macro arg — it's a new `.driver_tick_ms`
  field on `AxlService` (0 means use the 50 ms default). Driver-image
  consumers update from:

  ```c
  AXL_SERVICE_DRIVER(my_service, 50)
  ```

  to:

  ```c
  static const AxlService my_service = {
      ..., .driver_tick_ms = 50,
  };
  AXL_SERVICE_DRIVER(my_service);
  ```

  A single AxlService instance now describes everything the driver
  side needs; the macro only emits the firmware-mandated DriverEntry
  symbol that delegates to the library. Library-side per-image
  static state (loop, cfg, handle, svc pointer) replaces what was
  previously per-TU statics inlined by the macro — single instance
  per .efi, no behavior change. Test-service-driver.sh PASS
  both arches.

  Caught in pre-commit code review: the unload stub MUST carry
  `EFIAPI` calling convention. The first cut declared it as plain
  `int(void *)` and crashed at unload time on x64 (firmware called
  it with ms_abi but the function expected SysV; corrupted-stack
  reads showed the heap-poison 0xAF pattern in registers).

- **In-tree `.S` sidecar files retired.** The Makefile grew an
  `EMBED_BLOB(name, path)` function that generates the `.incbin`
  sidecar on the fly (mirrors what `axl-cc --embed` does for SDK
  consumers). `sdk/examples/service-demo-launch-blob.S`,
  `sdk/examples/embed-asset-blob.S`, and `tools/mkrd-blob.S` are
  deleted; the three call sites use `$(eval $(call EMBED_BLOB,...))`
  + `$(BLOB_OBJ_<name>)` instead. `tools/mkrd.c` migrates from
  hand-rolled `extern axl_embedded_ramdiskdxe[]` decls to
  `AXL_EMBED_DECLARE` / `AXL_EMBED_DATA` / `AXL_EMBED_SIZE`. No
  behavior change; same symbols, same .efi bytes.

- **`axl_service_detach_driver` no longer runs teardown.** The
  function is now timer-detach only; callers that own a "setup
  ran → teardown should run" relationship invoke
  `axl_service_teardown` explicitly. Previously teardown was
  hidden inside detach_driver's success path, which silently
  skipped teardown when `axl_loop_detach_driver` returned ERR —
  any future failure mode there inherited the silent skip.
  Splitting makes each function's responsibility narrow and the
  `AXL_SERVICE_DRIVER` macro's unload-time flow visible. New
  public `axl_service_teardown(svc)` returns the teardown
  callback's rc so the macro's unload stub propagates it into
  the `EFI_STATUS` `gBS->UnloadImage` observes — teardown
  failure surfaces to the firmware rather than getting
  absorbed.

  Migration: any consumer that called `axl_service_detach_driver`
  expecting it to run teardown follows up with
  `axl_service_teardown(&svc)` directly. axl-webfs (the only
  out-of-tree consumer) uses `axl_service_stop` / the
  `AXL_SERVICE_DRIVER` macro, neither of which call
  detach_driver directly — so no migration on the consumer
  side.

- **AxlService gains visible teardown logging.** All
  framework-driven teardown invocations log `service '<name>':
  teardown ENTER` / `teardown EXIT rc=N` at debug level; a
  non-OK rc gets promoted to `axl_warning`. Bumped via
  `AXL_LOG_LEVEL=debug` from the consumer side; off by default.
  Prevents future "did teardown actually run?" misdiagnoses
  without forcing every consumer to add their own printf.

- **AxlService held-protocol hazard documented** on
  `<axl/axl-service.h>` typedefs (AxlServiceSetup,
  AxlServiceTeardown) and in `src/util/README.md`'s AxlService
  section. UEFI's `gBS->UnloadImage` performs a post-callback
  refcount check that refuses with `EFI_ACCESS_DENIED` if the
  image still holds open protocol references — the most common
  cause of "stop fails" on a service-shaped driver. Documented
  with the exact warning shape from `axl_driver_unload` so a
  consumer who hits this in the wild can search for and find
  the relevant explanation.

- **`axl_driver_init` signature tightened from `(void *, void *)`
  to `(AxlHandle, AxlSystemTable *)`.** Drivers using the
  `AXL_DRIVER` macro never see this — the macro casts internally.
  Hand-rolled `DriverEntry` consumers (tier-2 spec-protocol
  publishers like axl-webfs's filesystem driver) add two casts
  at the call site:

  ```c
  // before:
  axl_driver_init(ImageHandle, SystemTable);
  // after:
  axl_driver_init((AxlHandle)ImageHandle, (AxlSystemTable *)SystemTable);
  ```

  Pointer values are bit-identical; the cast is a typing-only
  formality. The strict signature keeps the AXL_DRIVER path's
  contract honest (consumers there really do work with
  `AxlHandle` and never see `EFI_*`).

- **Renamed the AXL service registry to the AXL protocol registry.**
  The abstraction is a thin name-keyed wrapper over UEFI's
  `InstallProtocolInterface` / `LocateProtocol` family — calling it
  "service" was colliding with a separate "service" concept in the
  paused axl-kernel POC (long-running, systemd-shaped process units),
  which we want to spin off as a sibling repo. Renaming now is cheap
  while the only consumer outside the SDK is axl-webfs (already
  migrated in lock-step). Pre-1.0 — no compat shim.

  Migration for downstream consumers — straight identifier renames:

  | Old                              | New                                |
  |----------------------------------|------------------------------------|
  | `axl_service_find`               | `axl_protocol_find`                |
  | `axl_service_enumerate`          | `axl_protocol_enumerate`           |
  | `axl_service_register`           | `axl_protocol_register`            |
  | `axl_service_register_name`      | `axl_protocol_register_name`       |
  | `axl_service_register_multiple`  | `axl_protocol_register_multiple`   |
  | `axl_service_unregister`         | `axl_protocol_unregister`          |
  | `axl_handle_get_service`         | `axl_handle_get_protocol`          |

  No verb changes — the rename is purely on the noun. Built-in
  well-known names (`"smbios"`, `"shell"`, `"simple-fs"`, `"tcp4"`,
  etc.) are unchanged. Internal file rename: `src/util/axl-service.c`
  → `src/util/axl-protocol.c`; doxygen / Sphinx / README sections
  updated to match. The PASS-line names in `sdk/examples/driver.c`
  changed from `driver-service-pin`/`-find` to
  `driver-protocol-pin`/`-find`; consumers parsing those strings
  must update.

### Removed

- **AxlService is now driver-only — `axl_service_run*` removed.**
  The four foreground-runs-the-service entry points
  (`axl_service_run`, `_with_loop`, `_args`, `_with_loop_args`) and
  the operational fields tied to them (`.on_signal`,
  `.watchdog_keep_armed`, `.restart_max`, `.restart_backoff_ms`)
  are gone. AxlService now describes a DXE driver, period — long-
  running work in UEFI lives in driver images, not foreground apps.

  Surviving entry points:
  - `AXL_SERVICE_DRIVER(svc)` — driver-image macro (unchanged).
  - `axl_service_attach_driver` / `_detach_driver` — used by the
    macro; rare for direct consumer use.
  - `axl_service_launch_embedded` / `_stop` / `_is_running` —
    foreground-side primitives for loading, stopping, querying a
    driver image.
  - `axl_service_teardown` — internal teardown hook.

  Surviving struct fields:
  ```c
  typedef struct {
      const char           *name;
      const AxlConfigDesc  *opts_descs;
      AxlServiceSetup       setup;
      AxlServiceTeardown    teardown;
      void                 *user;
      AxlGuid               protocol_guid;
      uint64_t              driver_tick_ms;  /* 0 = 50ms default */
  } AxlService;
  ```

  Migration: foreground apps that called `axl_service_run*` switch
  to `axl_service_launch_embedded` + (optional) supervise loop +
  `axl_service_stop`. The forthcoming `axl_service_main` helper
  packages this pattern; for now consumers wire it explicitly.

  service-demo's `run` verb migrated: launches the embedded
  driver, blocks on the default loop until Ctrl-C, then stops the
  driver. `test-service.sh` (which only tested the foreground sync
  path) deleted; `test-service-driver.sh` covers launch/stop/
  relaunch.

  Pre-1.0, no wire-compat bar. axl-webfs is the only known
  consumer; its in-process serve handler will need the same
  launch+supervise+stop migration.

### Fixed

- **`axl_file_rename` rejects cross-directory renames and
  malformed paths.** The UEFI backend's `SetInfo(FileInfo)` was
  receiving the full new-path (including `fs0:\` volume prefix)
  as the FileName, which FAT drivers documented-reject. Every
  WebDAV `MOVE` returned 409. Fix: extract basename before
  passing to the backend, verify any directory prefix matches
  the source, refuse cross-directory rename (use the new
  `axl_file_move` for that). Empty-basename inputs also
  rejected up-front.

- **`axl_url_parse` strict port parsing.** The `:PORT` segment
  now rejects non-digit characters, overflow past `uint16_t`,
  and trailing garbage; previously these silently parsed as
  truncated or zero. Callers that round-tripped malformed URLs
  through `axl_url_parse` will now see `AXL_ERR` instead of a
  silently-broken `AxlUrl`. Fixes a class of "why is my port
  wrong" bugs in URL-driven config.

- **`axl_http_response_set_range` now emits the `Content-Range`
  header per RFC 9110 §15.3.7.** The function set `status_code = 206`
  and copied the slice into the response body, but never wrote the
  `Content-Range: bytes <start>-<end>/<total>` header that 206
  responses MUST carry. Tolerant clients (curl, browsers) accepted
  the header-less 206 because `Content-Length` matched the requested
  slice; strict clients (some download managers, range-stitching
  HTTP libraries) would reject it. The function's docstring already
  promised the header — the implementation just never delivered.

- **DXE-driver-mode HTTP delivered headers but no body, server stuck
  in CLOSE-WAIT.** `send_response` called the synchronous
  `axl_tcp_send`, which spins a private `AxlLoop` and `axl_loop_run`s
  on it. In driver mode dispatch fires at `TPL_CALLBACK`, where
  `gBS->WaitForEvent` returns `EFI_UNSUPPORTED`. The wait failed
  silently, `sock->send_source` stayed pinned to the freed ephemeral
  loop, and the body send was rejected by `axl_tcp_send_async`'s
  "previous send still pending" guard. `reset_connection`'s
  `axl_tcp_close` then cancelled the queued Transmit before TCP4
  put it on the wire. `send_response` is now a single
  `axl_malloc(headers+body)` + one `axl_tcp_send_async` (or
  `axl_tls_write_async` for TLS) + one `on_response_sent` callback
  that frees `tx_buf` and decides keep-alive vs reset_connection.
  WS-upgrade 101 also async-ified. All `send_error_response`
  callsites stripped of the synchronous `reset_connection` they
  used to chain after — error responses now force `keep_alive=false`
  and let `on_response_sent` drive teardown after the wire transmit
  completes. Reproducer in `test/integration/test-driver-http.sh`
  (8 sequential GETs + body + CLOSE-WAIT settle check).

- **Driver-mode listener wedged after exactly `HTTP_DEFAULT_MAX_CONNS`
  requests.** `driver_dispatch_notify` was processing exactly one
  event source per 50 ms tick. Under HTTP load, recv-data callbacks
  synchronously submit `axl_tcp_send_async` (TCP4 typically completes
  the Transmit inline), but the corresponding tx-event was only
  checked on the NEXT tick. Each tick handled the older accept
  signal first (slot 0), so eight sequential GETs filled the conn
  pool with `active=true` slots whose `on_response_sent` never ran.
  The 9th connection saw `NO FREE SLOT` and the listener appeared
  wedged. Driver-mode dispatch now drains all signaled events per
  tick (capped at `2 × AXL_MAX_SOURCES` as runaway guard, hitting
  the cap is logged). Matches the doxygen contract on
  `axl_loop_attach_driver` ("processes whatever's pending in this
  tick before returning"). `axl_loop_run` was unaffected. Regression
  test in `test/integration/test-driver-http.sh` adds a malformed
  request after the 8 successful GETs to exercise the failure mode.

- **`axl_driver_set_load_options` leaked one allocation per
  load+set+unload cycle (142 bytes per driver instance in the
  axl-webfs reproducer).** The function `axl_malloc(size)`s a copy of
  the caller's data and hands the pointer to the firmware via
  `LoadedImage->LoadOptions`. The firmware retains the pointer for
  the loaded-image lifetime and provides no callback to free it;
  `axl_driver_unload` only called `gBS->UnloadImage`. AXL now tracks
  the copy in a fixed-capacity (16-slot) side table mapping
  `AxlDriverHandle → owned LoadOptions copy`, mirroring the
  `NotifyTimerEntry` pattern from
  `src/backend/native/axl-backend-native-event.c`.
  `axl_driver_unload` calls `load_options_release(handle)` BEFORE
  `gBS->UnloadImage` so a UnloadImage failure still doesn't leak.
  Re-set on a handle frees the old copy and reuses its slot.
  Out-of-slots returns `AXL_ERR` and frees the would-be copy
  rather than installing-and-leaking. Regression test in
  `test/integration/test-driver-leak.sh` (3 phases: basic, re-set,
  table-full at 17th load).

### Documentation

- **`src/util/README.md` and Sphinx glossary now define what UEFI
  means by "protocol".** UEFI's term is awkward — a "protocol" is
  a C struct of function pointers identified by a GUID and bound
  to a handle (closer to a COM interface or a Java/Swift interface
  on an instance than anything wire-shaped). The README's "Protocol
  Registry" section opens with a "What UEFI Means by Protocol"
  preamble + cross-language analogs; the glossary entry expands to
  match; the public header banner in `<axl/axl-sys.h>` carries a
  short pointer to the README.

### Tests

Unit test ratchet at 3125/3125 on both X64 and AARCH64. HTTP
integration 177/0, Redfish 12/0, net-tools 15/0, tcp-echo 4/0,
tool 31/0, cpu-idle PASS. `BUILD=RELEASE` clean on both arches;
`AXL_TLS=1 BUILD=RELEASE` clean on X64. `scripts/build-docs.sh`
zero warnings.

## 0.16.0 — 2026-05-08

### Added

- **`axl_time_get_us()`** — public monotonic-microsecond clock,
  declared in `<axl/axl-time.h>` alongside `axl_time_get_ms`. Thin
  pass-through to the architecture's cycle-counter backend (x86 TSC
  / aarch64 CNTPCT_EL0); first call calibrates and returns 0,
  subsequent calls are cheap. Prefer this over `axl_time_get_ms`
  for sub-second timing — the latter is wallclock-derived and not
  useful below ~1 s on most firmware. Closes the last extern in the
  tools tree (`tools/timetest.c` previously declared
  `axl_backend_get_monotonic_us` directly to avoid pulling the
  internal backend header).

- **`AxlArgsNode.help_prolog` / `help_epilog`** — two optional
  free-form text fields rendered in `--help` output. The prolog
  appears between the `name — help` header and `Usage:`; the
  epilog appears after all auto-generated sections. Per-node
  (sub-verb help shows the sub-verb's prolog/epilog, not the
  root's). NULL = nothing printed; every existing caller's `--help`
  output is unchanged. Use cases: multi-paragraph descriptions,
  usage examples, environment-variable lists, "see also" pointers,
  "report bugs to ..." footers.

### Changed

- **Style cleanup passes A1 / A2 / B / C** — mechanical
  no-behavior-change sweep across the source tree per
  `docs/Style-Cleanup-Plan.md`. Pass A dropped redundant
  `extern` declarations and hoisted mid-file `#include` directives
  to top-of-file include blocks. Pass B hoisted scattered
  typedefs / macros / file-scope statics into each file's canonical
  top section across 20 files. Pass C split four large multi-concern
  files into per-sub-module siblings:

    - `src/acpi/axl-acpi.c` → core + `axl-acpi-mcfg.c`,
      `axl-acpi-madt.c`, `axl-acpi-fadt.c` (typed-table readers each
      own their on-wire struct definitions and offset macros).
    - `src/backend/native/axl-backend-native.c` → core +
      `axl-backend-native-event.c` (events + timers + close-debug
      ring) + `axl-backend-native-mp.c` (MP services).
    - `src/data/axl-str.c` → core + `axl-str-bmh.c`
      (Boyer-Moore-Horspool substring search), `axl-str-base64.c`
      (RFC 4648 codec), `axl-str-scan.c` (`AxlStrReader` + `axl_sscanf`).
    - `src/pci/axl-pci.c` → core + `axl-pci-cap.c` (capability walk
      + cap-ID name tables + VPD reader).

  Internal headers (`axl-acpi-internal.h`, `axl-pci-internal.h`,
  `axl-service-internal.h`) added where the split-out files share
  helpers with the core. No public-API impact.

- **`docs/AXL-Coding-Style.md` codifies the split-vs-hoist rule** —
  when a section accumulates its own typedefs / macros / statics,
  prefer a sibling `.c` file with an internal header over a
  giant top-of-file declaration block. Function-local helper macros
  that reference caller-scope variables (e.g. va_list-bearing
  `SCAN_STORE_*`, `ESC_APPEND_CHAR`) are an explicit exception —
  they have to live next to their single caller.

### Fixed

- **`test_dell_transport_dispatch` no longer cascade-fails 7 of 7
  assertions under QEMU.** The test installs a mock Dell IPMI
  vendor protocol then opens a session, expecting to dispatch
  through the mock. Auto-detect (correctly, for real hardware)
  prefers SMBIOS Type 38 over the Dell vendor protocol — and OVMF
  publishes a Type 38 entry — so the mock never got called. Test
  now pins the transport via
  `axl_ipmi_session_new_with_transport(AXL_IPMI_TRANSPORT_DELL)`;
  the dispatcher's wire-shape regression guard (`&resp[0]` vs
  `&resp[1]` one-byte shift bug) is preserved without weakening
  auto-detect priority for real-hardware use. `AxlTestIpmi.efi`:
  98 passed / 0 failed under both KCS-mode and SSIF-mode QEMU
  boots (was 91 / 7).

## 0.15.0 — 2026-05-07

### Added

- **`axl_http_response_set_static(resp, body, size, content_type)`** —
  install a borrowed read-only body the SDK will NOT free after
  sending. Suited for embedded `.rodata` HTML / JS / CSS assets and
  other immutable blobs (xxd'd binary, JSON5 sidecar contents).
  Avoids the silent copy that `axl_http_response_set_text` /
  `_json` would force, and avoids the heap-corruption footgun of
  assigning a static literal to `resp->body` directly.

### Changed

- **`AxlHttpResponse.body` ownership contract is now documented** in
  `axl-http-server.h`: the SDK calls `axl_free` on the pointer
  after the response is sent unless `body_static` is set. New
  `body_static` field (zero-init false → preserves existing
  behavior for every existing caller; no migration required).
  Reported by an axl-webfs consumer after a `.rodata`-assigned
  body caused first-OK / second-hangs heap corruption.

- **`scripts/run-qemu.sh --timeout` is now honored in `--background`
  mode.** Previously the timeout was foreground-only; an abandoned
  background QEMU (e.g. driver dying with SIGPIPE mid-run) leaked
  its hostfwd binding and camped the host port forever. The
  background launch now wraps QEMU with `timeout(1)` the same way
  the foreground branch does. `--gdb` continues to bump the
  timeout to 3600 s for both modes.

### Migration

- **`AxlHttpResponse` struct grew by one `bool` (+ padding).**
  Pre-1.0 ABI churn — consumer projects (e.g. axl-webfs) must
  rebuild against this version; no source changes required for
  callers that go through the `set_text` / `set_json` / `set_file`
  / `set_range` helpers. Direct `resp->body = …` assignments still
  work and behave identically (zero-init `body_static = false`).

## 0.14.0 — 2026-05-06

### Added

- **`axl_strjoinv(separator, count, argv)`** — count+array variant of
  `axl_strjoin` for argv-shape inputs (e.g. joining `argc, argv` from
  a UEFI shell entry point). Saves the small reshape allocation
  needed to build a NULL-terminated copy. `axl_strjoin` is now a
  thin wrapper over `axl_strjoinv`.

- **`axl_nvstore_set_str(ns, key, str, attrs)`** — convenience
  wrapper that writes `axl_strlen(str) + 1` bytes (including the
  trailing NUL) so callers don't repeat the `+1` ritual at every
  string-variable site. Empty string writes a 1-byte NUL payload;
  `NULL str` is rejected (use `axl_nvstore_delete` to remove a
  variable).

- **`axl_nvstore_get_str(ns, key, &str)`** — heap-allocating string
  read on top of `axl_nvstore_get_alloc`. Trailing NUL is
  guaranteed (the alloc path zero-extends by one byte) so the
  result is always a valid C string regardless of whether the
  firmware payload included a NUL.

- **`axl_args_get_uint_offset(args, name, &out)`** — accessor for
  STRING-typed positionals/flags holding `base[+offset]` syntax.
  Reads the slot via `axl_args_get_string` and parses with
  `axl_strtou64_with_offset`. Walks parents like the other
  `axl_args_get_*` family members. Lets tools that accept
  register/memory addresses in a single argv token shrink each
  handler's fetch+parse from ~5 lines to 1. Returns
  `AXL_OK / AXL_ERR` (no in-band sentinel for `uint64_t` parse
  failure).

### Changed

- **`axl_strtou64_with_offset` docstring** — dropped two leaked
  downstream-tool examples per project rule "no consumer names in
  upstream code/docs". Behavior unchanged.

## 0.13.2 — 2026-05-06

### Fixed

- **`axl-cc --version` reports "unknown" when invoked through
  `/bin/axl-cc` on usrmerge distros** (Fedora / Arch / Ubuntu
  ≥19). The wrapper resolved its install dir with
  `cd "$(dirname "$0")"` which follows symlinks logically; on
  `/bin → /usr/bin` systems, `dirname /bin` = `/`, making
  `SDK_DIR = /` and breaking version + every relative path
  lookup. Also surfaced under `sudo` (the secure-PATH typically
  puts `/bin` ahead of `/usr/bin`). Fix: switch the `cd` to
  `cd -P` for physical-path resolution. Reported by a
  downstream consumer after upgrading to v0.13.1; one-character
  patch in `scripts/install.sh`'s axl-cc heredoc.

## 0.13.1 — 2026-05-06

### Fixed

- **CI clang-tidy dead-store flags** in `src/net/axl-tcp-sync.c`
  (`tcp_find_service_binding`). A newer clang-tidy on the GitHub
  Actions runners flagged two `chosen_rank = N` assignments
  followed by unconditional `break` in the loop, with the var
  never read after the loop — same code passed CI on v0.12.0.
  Behavior unchanged; the conceptual rank scheme is preserved as
  comments at the corresponding sites. v0.13.0's tagged commit
  has a red CI for this reason despite all release artifacts
  shipping correctly. v0.13.1 restores green-CI on the release
  tag.

## 0.13.0 — 2026-05-06

### Added

- **AxlSmbus PIIX4 direct-I/O backend** (`src/smbus/axl-smbus-piix4.c`) —
  third transport alongside EFI_SMBUS_HC and EFI_I2C_MASTER. Targets
  the AMD FCH SMBus controller (1022:790b family) when the firmware
  declines to expose it via the standard EFI protocols. Mirrors
  Linux's `i2c_piix4` algorithm split: SMBSLVCNT IMC arbitration on
  the MAIN controller, plain access on the AUX. New transport enum
  `AXL_SMBUS_TRANSPORT_PIIX4`. Walker integration in
  `axl_smbus_new_with_probe` and `axl_smbus_visit_all`.

- **New AxlSmbus public APIs**:
  - `axl_smbus_describe(s)` — per-instance human-readable identity
    (e.g. "AMD FCH PIIX4 AUX port 1 at 0xB20"). Filled by each
    backend at session-open time.
  - `axl_smbus_quick(s, slave, is_read)` — SMBus QUICK protocol
    (address+R/W ACK probe, no command, no data). Linux's
    `i2cdetect` default mode.
  - `axl_smbus_receive_byte(s, slave, *out)` — SMBus Receive Byte
    (read 1 byte, no command). The safer EEPROM-area probe Linux's
    i2cdetect uses for 0x30..0x37 + 0x50..0x5F.

- **`tools/i2c.efi`** — Linux `i2c-tools`-style explorer over
  AxlSmbus. Verbs: `list`, `probe`, `get`, `set`, `dump`. AUTO mode
  matches Linux i2cdetect's per-address mode selection (QUICK
  default, Receive Byte for EEPROM-prone ranges).

- **JEDEC SPD5118 hub protocol** for DDR5 SPDs — modeled byte-for-
  byte on Linux's `drivers/hwmon/spd5118.c`. Identifies hubs via
  MR0:MR1=0x18:0x51 device-ID check, preserves the addr-mode bit
  on every page-select, caches the current page across reads, and
  implements the Renesas/ITD stuck-page recovery dance. Replaces
  the prior "blind write MR11=0 then read at 0x80" probe which
  silently mis-attributed any responder at 0x50..0x57.

- **`AXL_LOG_LEVEL` env var + `axl_log_init_from_env()`** — Glib-
  style log filter with per-domain syntax (`smbus:debug,net:info`,
  wildcard `*:warn,smbus:debug`, aliases `all` / `off`).
  Case-insensitive level keywords. Lazy init at every entry point
  that observes levels (both log-emission paths plus both
  setter functions). RUST_LOG precedence: env is the baseline,
  programmatic `axl_log_set_level` / `axl_log_set_domain_level`
  calls win.

- **`AXL_DIAG` env var** — gates `axl_diag_startup`'s cross-tool
  startup-diagnostic dump (SDK / arch / protocols / handles).
  Tools call the function unconditionally; it self-gates on the
  env var. Frees the `-v` short flag across tools to carry
  Linux-counterpart semantics.

### Changed

- **AxlSpd codec dispatch** now identifies SPD5118 hubs by reading
  MR0:MR1 first, falling through to the legacy "byte-2 mem-type"
  heuristic only when the device-ID doesn't match. Fixes a long-
  standing mis-route where DDR5 hubs took the unknown-codec branch
  because register 2 on a hub is MR2 (revision), not the SPD
  memory-type byte (which lives at content offset 2 = register
  0x82 after page-select).

- **AxlSpd / memspd platform limitations documented** at three
  layers (`<axl/axl-spd.h>` API doc, `src/spd/README.md`,
  `tools/memspd.c` help text). Empirically verified on UEFI AND
  on Linux from kernel context (kernel 6.19.10, spd5118 driver
  fails to bind on the same hardware): AMD FCH AUX controller
  exhibits the false-ACK + zero-data quirk Linux warns about in
  `drivers/i2c/busses/i2c-piix4.c` lines 968-974. On affected
  platforms callers fall back to SMBIOS Type 17 — same data
  source `dmidecode -t 17` exposes.

- **`grep -v` now means INVERT MATCH** — Linux grep semantics. The
  prior unused boolean labelled "Verbose output" is gone; the old
  `--verbose` long form moved to `--show-progress` (no short
  alias). **Breaking** for any script that used `grep -v`
  expecting the no-op verbose behavior.

- **`cat -v` shows non-printing characters** — Linux `cat -v` /
  `--show-nonprinting` semantics. Standalone `-v` doesn't enable
  `-E` or `-T` (the existing `-A` meta still bundles all three).

- **`lsusb --debug` and `lspci --debug` removed.** Both forced
  `axl_log_set_level(AXL_LOG_DEBUG)`. Use `AXL_LOG_LEVEL=debug` —
  or per-domain `usb:debug` / `pci:debug` — instead.

- **`memspd --verbose` removed entirely.** It was only a trigger
  for `axl_diag_startup`. Use `set AXL_DIAG 1` instead.

- **`mkrd --verbose` simplified** — no longer overrides log level
  or unconditionally calls `axl_diag_startup`. The flag still
  triggers the mkrd-specific `EFI_RAM_DISK_PROTOCOL` probe.

- **`netinfo --verbose` no longer overrides log level.** Tool-
  specific richer-payload behavior still works the same way.

- **`share/pci-ids.json5` is now the single PCI sidecar** — schema 2
  hierarchical, with two top-level sections: `vendors[]` (vendor /
  device / subsystem) and `classes[]` (base / subclass / progIF).
  Both `axl_pci_ids_load` and `axl_pci_class_load` consume this
  file; each ignores the section it doesn't care about. Closes the
  convention gap that left `pci-class.json5` at schema 1 (flat)
  while `pci-ids.json5` had moved to schema 2 (hierarchical).

- **`share/pci-class.json5` removed.** Class data lives in
  `pci-ids.json5`'s `classes[]` section now. Consumers that passed
  `"pci-class.json5"` via `override_path` need to repoint at
  `pci-ids.json5`. Auto-discovery callers see no behavior change —
  same lookup semantics, single canonical filename.

- **`scripts/pci-ids-to-json5.py` simplified** — `--schema 2`
  (default) emits the unified file in one pass (vendors + classes,
  both hierarchical). Drops `--unified` (now redundant) and
  `--emit-class` (the separate file is no longer generated).
  `--schema 1` retained as a back-compat path for the legacy flat
  vendors-only layout.

### Added

- **Schema 2 hierarchical class layout** in
  `<axl/axl-pci.h>`'s sidecar contract — `classes: [{ base, name,
  subclasses: [{ sub, name, progs: [{ prog, name }] }] }]`. The
  loader (`src/pci/axl-pci-class.c`) accepts both schema 1
  (legacy flat) and schema 2 (hierarchical) and routes both into
  the same composite-key hash tables, so lookups are
  layout-agnostic.

### Tests

- Schema 2 hierarchical class parser coverage in
  `axl-test-platform.c` — fixtures exercising the v1 → v2 result
  equivalence (same data, both layouts, same query results),
  nameless-base parent nodes, and schema-99 rejection. +10 tests
  (2555 → 2565 both arches).

## 0.12.0 — 2026-05-04

### Added

- **`AxlHashTableInsertResult` typed enum** in
  `<axl/axl-hash-table.h>` — `axl_hash_table_insert` and
  `axl_hash_table_replace` return type promoted from `int` to the
  new enum: `AXL_HASH_TABLE_NEW = 1`, `AXL_HASH_TABLE_REPLACED = 0`,
  `AXL_HASH_TABLE_ERR = -1`. Numeric values match the prior int
  contract — consumers comparing against literal `1`/`0`/`-1` keep
  working unchanged. New code can write `!= AXL_HASH_TABLE_ERR` /
  `== AXL_HASH_TABLE_NEW` for a status-shaped read. Follows the
  AxlSidecarStatus per-module-status precedent. (Phase H3.)

- **`axl_device_path_to_text(dp)`** in `<axl/axl-sys.h>` — wraps
  EFI_DEVICE_PATH_TO_TEXT_PROTOCOL, returning the firmware's
  canonical `PciRoot(0x0)/Pci(0x3,0x0)/MAC(...)` representation
  as UTF-8. Caller frees with `axl_free`. axl-boot.c had grown a
  private copy for boot-option decoding; promoted to a real
  public API and refactored to dogfood it.

- **`axl_net_ensure_drivers` breadcrumbs** — emits begin/end
  info-level log lines (`"starting (N SNP handles)"`,
  `"M drivers loaded, SNP handles N→K"`) so diagnostic tools
  can surface whether ensure_drivers actually contributed new
  SNP handles vs. found everything already there.

- **`netinfo` diagnostic verbs and flags** — turns netinfo from
  "make networking work" into a "what does this firmware
  natively provide" diagnostic. New surface:
  - `-n` / `--no-load`: skip `axl_net_ensure_drivers` entirely;
    enumerate only what the firmware natively provides.
  - `list-bundle`: walk every mounted volume's `drivers/<arch>/`
    and list staged `.efi`/`.efidrv` files.
  - `diag`: composite report — firmware vendor/rev/spec, mounted
    volumes with full device paths, all PCI Network Controllers
    with VID:DID lookup, driver bundle inventory, NIC handles
    before and after driver-load, ensure_drivers status, final
    interfaces table. Intended for copy-paste to a maintainer.
  - `-v` now wraps each verb with pre-/post-load NIC handle
    snapshots so the diff is visible.
  - Per-NIC `-v` output adds the device-path text (PCI BDF / USB
    topology / MAC in canonical form) and NII Revision when a
    BY_DRIVER agent is found for the NII protocol.
  - `list` no longer fail-closes when ensure_drivers fails — zero
    interfaces is itself useful information. `ping` keeps the gate.

- **`AxlStatus` enum** in `<axl/axl-macros.h>` — project-wide typed
  status for functions whose return value carries more information
  than success/failure. Promotes the prior `#define AXL_OK / AXL_ERR
  / AXL_CANCELLED` magic-int triple to a named enum and adds
  **`AXL_TIMEOUT (-3)`** as a fourth code so callers can distinguish
  "deadline elapsed" from "you passed garbage." Numeric values stay
  stable; new codes only ever extend the negative range. Code that
  compares against the constants (`rc == AXL_CANCELLED`) and code
  that compares against literal integers (`rc == -2`) both still
  work — promotion is purely additive at the value level.

  Adoption: the entire wait/event family promoted from `int` to
  `AxlStatus` return type — `axl_event_wait`, `axl_event_wait_timeout`,
  `axl_wait_for`, `axl_wait_for_with_tick`, `axl_wait_for_flag`,
  `axl_wait_for_word`, `axl_wait_ms`, plus the internal
  `_axl_event_wait_timeout_with_tick` and the per-protocol
  `_axl_{udp,tcp,dns,ip4}_wait` Tier 4 helpers.

  Behavior change inside the enum: `axl_event_wait_timeout` (and
  the rest of the wait family) now returns `AXL_TIMEOUT` on the
  deadline path instead of overloading `AXL_ERR (-1)` for both
  timeout and invalid-arg failures — the prior overload was a
  latent bug consumers couldn't disambiguate. Pure-sleep callers
  (`axl_sleep`/`axl_msleep`/`axl_usleep`/`axl_wait_ms` with no
  condition) collapse `AXL_TIMEOUT` → `AXL_OK` internally so
  ergonomics are unchanged.

- **`AxlStatus` migration policy in `axl-args.h`** — `axl_args_run`
  explicitly stays `int` (POSIX-exit-code shaped: 0/1 returned from
  `main()` flow into the process exit code, where `AxlStatus`'s
  negative values would round to 254/255). Documented inline so
  future readers don't propose flipping it.

### Tests

- **netinfo integration tests** — 4 new sections in
  `test/integration/test-net-tools.sh` covering `--no-load`,
  pre/post snapshot headers + device-path rendering under `-v`,
  `list-bundle` (asserts "no drivers staged" on the test image),
  and `diag` (asserts each section header lands).

- New `test_event_timeout_distinct_from_error` in
  `axl-test-event.c` — regression test for the AXL_TIMEOUT vs
  AXL_ERR disambiguation. Confirms NULL event yields `AXL_ERR`
  while a deadline yields `AXL_TIMEOUT` and the two are distinct
  values. Existing event/wait tests re-pointed at the named
  constants instead of bare `-1`.

### Fixed

- **Stale archive members across structural header changes** —
  the libaxl.a build recipe now deletes the archive before
  `ar rcs` rebuilds it. `ar` insert-or-replace was retaining
  stale `.o` members from renamed/removed sources; a future
  `ar` lookup picked the stale copy first since `ar` preserves
  insertion order. Resolves the documented "After any structural
  change to a public-header struct, run `make clean` first"
  footgun — incremental builds are now reliable.

- **Phase H1 typos from sed regex** — 8 docstring sites in
  `<axl/axl-tcp.h>`, `<axl/axl-socket.h>`, `<axl/axl-net.h>`,
  `<axl/axl-udp.h>` had `failure.or` / `error.or` (period instead
  of space) from an unescaped `.` in the H1a networking-cluster
  sed. Cosmetic, but fixed before tag.

- **`mkrd.efi` recipe-override warnings** — Makefile's generic
  tool foreach was emitting two-line "overriding recipe" /
  "ignoring old recipe" warnings on every build because the
  special mkrd-with-blob rule overrode it. `$(filter-out
  mkrd,$(TOOL_NAMES))` removes the conflict; mkrd has its own
  explicit recipe.

- **Test runner build hint** — `test_add_efi`'s "Build first"
  message now picks the right `make` target based on the missing
  artifact (`make tools` for `tools/*.efi`, `make tests` for
  `AxlTest*.efi`) instead of suggesting `./build.sh --rebuild`.

- **Doc-build warnings reduced from ~240 to 119** — Sphinx
  toctree pointed at a renamed `modules/stream-port` page (now
  `modules/port`); `@code{.json}` and ` ```json5` blocks
  containing JSON5-style content (unquoted keys, single-quoted
  strings) switched to `@code{.js}` / ` ```js` lexer that
  handles the shape; anonymous SMBIOS enums named (see Changed
  above). Remaining 119 warnings are Doxygen `\ref` cross-
  reference resolution failures — flagged for a future doc
  cleanup pass; non-blocking.

### Changed

- **`axl_mem_fail_next_alloc` works in RELEASE builds** — the OOM
  injection counter was previously gated by `#ifdef AXL_MEM_DEBUG`
  and no-op'd in RELEASE. The counter check is one well-predicted
  branch on the malloc path; lifting it out of the guard makes
  the public API contract hold universally and lets RELEASE
  builds exercise their own error-handling paths. The genuinely-
  costly debug machinery (alloc-fill 0xDA, fence words, leak
  list) stays DEBUG-only. Header docstring + src/mem/README.md
  updated to reflect the new contract.

- **Backend hygiene — `<axl/axl-backend.h>` operations use
  AXL_OK/AXL_ERR** — the internal backend abstraction (used by
  every library module) now follows the same single-failure
  convention as the public API. 25 backend ops converted; 64
  return statements + 12 ternary `EFI_ERROR(...) ? -1 : 0`
  rewrites in `src/backend/native/axl-backend-native.c`.

- **Anonymous SMBIOS enums named** — five `enum { ... }`
  declarations in `<axl/axl-smbios.h>` promoted to
  `typedef enum { ... } AxlSmbiosFoo`:
  `AxlSmbiosTableType`, `AxlSmbiosIpmiInterface`,
  `AxlSmbiosHostIfaceType`, `AxlSmbiosHostIfaceProtocol`,
  `AxlSmbiosBoardType`. Enumerator values and call-site int
  compatibility are unchanged. Resolves Sphinx C-domain
  duplicate-declaration noise (~104 build warnings).

- **`<axl/axl-http-server.h>` named-constants hygiene** — all 17
  HTTP-server operations (`axl_http_server_set`, `_set_max_connections`,
  `_set_body_limit`, `_set_keep_alive`, `_use`, `_add_route`,
  `_add_static`, `_attach`, `_run`, `_use_tls`, `_add_websocket`,
  `_ws_broadcast`, `_use_auth`, `_add_route_auth`, `_use_cache`,
  `_set_route_ttl`, `_add_upload_route`) now return `AXL_OK`/`AXL_ERR`.
  Three callback typedef contracts (`AxlHttpMiddleware`,
  `AxlAuthCallback`, `AxlUploadHandler`) updated to document
  `AXL_OK`/`AXL_ERR` returns — consumers can return either named
  constants or literal 0/-1 (numerically equivalent). The static
  middleware-runner in `src/net/axl-http-dispatch.c` updated to
  compare middleware callbacks against `AXL_OK`. Caller updates:
  `test/unit/axl-test-net.c` (1 site). Ninth and final module of
  Phase H1a from the originally-audited bool-sweep targets.

- **`<axl/axl-string.h>` named-constants hygiene** — all 11
  AxlString builder operations (`axl_string_append`, `_append_len`,
  `_append_printf`, `_append_c`, `_prepend`, `_prepend_len`,
  `_prepend_c`, `_insert`, `_erase`, `_truncate`, `_overwrite`)
  now return `AXL_OK`/`AXL_ERR`. The pointer producers
  (`axl_string_new`, `_new_size`, `_str`, `_steal`,
  `axl_asprintf`), the count returner (`axl_string_len` —
  `size_t`, NULL→0), and void functions are unchanged. The ternary
  return in `axl_string_append_printf` (`b->error ? -1 : 0`)
  converted. Header gains `#include <axl/axl-macros.h>` (was missing
  because the prior bool-sweep added `<stdbool.h>`; the H1a hygiene
  pass uses AXL_OK/AXL_ERR which need axl-macros.h directly).
  Caller updates: `src/data/axl-json-build.c` (3). Eighth module of
  Phase H1a.

- **`<axl/axl-driver.h>` named-constants hygiene** — all 12 int-
  returning operations (`axl_driver_load`, `_start`, `_connect`,
  `_disconnect`, `_unload`, `_set_load_options`, `_set_unload`,
  `_connect_handle`, `_locate`, `_ensure`, `_ensure_with_embedded`,
  `_load_dir`) now return `AXL_OK`/`AXL_ERR`. Two ternary returns
  (`EFI_ERROR(...) ? -1 : 0`) in the impl converted. Static
  helpers in src/util/axl-driver.c also converted for internal
  consistency. `axl_image_unload` (which passes through
  axl_driver_unload's rc) updated to use AXL_OK in its rc-init and
  NULL-arg paths. Call sites in `src/net/axl-net-dhcp.c`,
  `src/util/axl-image.c`, `test/unit/axl-test-util.c` (8 sites
  including 1 rc-indirection) updated. Seventh module of Phase H1a.

- **`<axl/axl-sys.h>` named-constants hygiene** — 9 single-failure
  ops (`axl_map_refresh`, `axl_sys_get_firmware_info`,
  `axl_sys_get_memory_size`, `axl_handle_get_service`,
  `axl_service_find`, `axl_service_enumerate`, `axl_service_register`,
  `axl_service_unregister`, `axl_service_register_multiple`) now
  return `AXL_OK`/`AXL_ERR`. The multi-shape iterator
  `axl_device_path_for_each` (returns 0 / callback-rc / -1 for
  malformed) and the count returner `axl_device_path_size` (size_t)
  deliberately keep literal returns. Call sites in
  `src/net/axl-net-dhcp.c`, `tools/sysinfo.c`, `tools/mkrd.c`,
  `tools/netinfo.c` updated. Sixth module of Phase H1a.

- **`<axl/axl-stream.h>` named-constants hygiene** — 5 single-failure
  ops (`axl_stream_set_stdout_tee`, `_stderr_tee`, `_encoding`,
  `axl_fseek`, `axl_fflush`) now return `AXL_OK`/`AXL_ERR`. The count
  returners (`axl_pread`, `axl_pwrite`, `axl_fread`, `axl_fwrite`,
  `axl_ftell`) deliberately keep their `-1` sentinel — return value
  carries information beyond status. The multi-shape iterator
  `axl_stream_for_each_line` (returns 0 / callback-rc / -1) also
  unchanged. Call sites in `tools/grep.c`, `test/unit/axl-test-util.c`,
  `test/unit/axl-test-io.c` updated. Fifth module of Phase H1a.

- **`<axl/axl-mem-phys.h>` named-constants hygiene** — all 10
  single-failure operations (`axl_mem_phys_map`, `_read8`, `_read16`,
  `_read32`, `_read64`, `_write8`, `_write16`, `_write32`, `_write64`,
  `_search`) now return `AXL_OK`/`AXL_ERR`. Header gains
  `#include <axl/axl-macros.h>` since the docstrings now reference
  the named constants. `@code` example block updated to compare
  against `AXL_OK`. Test sites in `test/unit/axl-test-platform.c`
  (18 sites including one `rc`-indirection) updated. Fourth module
  of Phase H1a.

- **`<axl/axl-tls.h>` named-constants hygiene** — single-failure
  TLS operations (`axl_tls_init`, `axl_tls_generate_self_signed`,
  `axl_tls_server_set_cert`, `axl_tls_write`, `axl_tls_write_async`)
  now return `AXL_OK`/`AXL_ERR` named constants. Both stub
  (AXL_TLS=0) and real (AXL_TLS=1) implementations updated. The
  multi-shape functions `axl_tls_handshake` and `axl_tls_read`
  (return 0 / 1 for "more data needed" / -1 for error) deliberately
  keep literal returns — those are not single-failure shape.
  Header `@code` example updated. Call sites in
  `src/net/axl-http-client.c`, `src/net/axl-http-response.c`,
  `src/net/axl-http-server.c`, `test/unit/axl-test-net.c` updated.
  Third module of Phase H1a.

- **`<axl/axl-ring-buf.h>` named-constants hygiene** — single-failure
  ring-buf operations (`axl_ring_buf_init`, `_init_fixed`,
  `_push_msg`, `_pop_msg`, `_peek_msg`, `_push_elem`, `_pop_elem`,
  `_peek_elem`, `_peek_nth_elem`, `_set_nth_elem`) now return
  `AXL_OK`/`AXL_ERR` named constants. Signatures unchanged. The
  byte-count returners (`_push`, `_pop`, `_peek`, `_discard`,
  `_*_regions`, `_get_*`) keep `uint32_t` returns with literal `0`
  for empty/no-op (count, not status). Call sites in
  `src/loop/axl-defer.c`, `experiments/axl-kernel/test/`,
  `sdk/examples/ring-buf-demo.c`, `test/unit/axl-test-data.c`
  updated. Second module of Phase H1a.

- **`<axl/axl-fs.h>` named-constants hygiene** — operation
  functions (`axl_file_get_contents`, `axl_file_set_contents`,
  `axl_file_info`, `axl_file_delete`, `axl_file_rename`,
  `axl_dir_mkdir`, `axl_dir_rmdir`, `axl_dir_list_json`,
  `axl_volume_enumerate`) now return `AXL_OK` / `AXL_ERR` named
  constants in their impl and standardized docstrings. **Signatures
  unchanged** — `int` return type is the documented shape per
  [docs/AXL-Coding-Style.md §"Return Value Conventions"](docs/AXL-Coding-Style.md)
  for single-failure operations, so consumers comparing against
  literal `0`/`-1` or against `AXL_OK`/`AXL_ERR` both compile and
  run unchanged. Call sites in `src/`, `tools/`, `test/`, `sdk/`
  updated to compare against the named constants for clarity.
  `axl_dir_walk` deliberately keeps literal `0`/`-1` because it
  propagates callback return values (multi-shape, not single-
  failure). First module of Phase H1a per
  [docs/ROADMAP.md §"API Hygiene"](docs/ROADMAP.md).

- **`AxlTcpCallback` and `AxlSocketCallback` `status` parameter**
  promoted from `int` to `AxlStatus` — the async TCP and AxlSocket
  completion callbacks receive a typed status code now. Numeric
  values are unchanged (`AXL_OK = 0`, `AXL_ERR = -1`, `AXL_CANCELLED
  = -2`), so existing callbacks compile and run unchanged at the
  ABI level. Internal callbacks (8 in `src/net/`), SDK examples
  (`echo-server.c`, `tcp-echo-server.c`), test callbacks, and the
  experimental axl-kernel POC updated to use `AxlStatus` parameter
  type and named constants in comparisons (e.g. `if (status !=
  AXL_OK)` instead of `if (status != 0)`). `axl-tcp.h` and
  `axl-socket.h` gain a `<axl/axl-macros.h>` include for the
  enum type. The `AxlTcp` synchronous wrappers' internal
  `SyncResult.status` field promoted to `AxlStatus` to match.

## 0.11.2 — 2026-05-04

### Added

- **`axl_console_read_key(timeout_ms, *AxlKey)`** + **`axl_console_flush_input()`**
  in new `<axl/axl-console.h>` — single-keystroke reader with a
  bounded timeout. Three modes: `0` non-blocking (returns -1
  immediately if the queue is empty), `UINT64_MAX` block forever,
  any other value bounds the wait in milliseconds. Wraps the
  backend's `WaitForKey` event + a freshly-created timer event,
  closed unconditionally on return so a slow key path doesn't leak.
  `flush_input` drains buffered keystrokes (eat type-ahead).
  Unblocks any interactive UEFI tool — `y`/`n` prompts, "press any
  key", arrow-key menus — that previously had to roll its own
  ConIn machinery.

- **`axl_image_verify_signature(path, consult_db, *info)`** +
  **`axl_image_signature_info_free()`** in new
  `<axl/axl-image-verify.h>` — PE Authenticode signature
  inspection without launching the image. Two-axis check:
  *presence* (parse the PE Certificate Table data directory from
  raw bytes — works regardless of Secure Boot state) and *db
  validity* (when `consult_db=true`, the firmware's PE loader
  dry-runs the signature check via `LoadImage(SourceBuffer)` +
  immediate `UnloadImage`; `EFI_SECURITY_VIOLATION` → invalid,
  `EFI_SUCCESS` → valid, anything else → "not consulted" with
  presence-only fallback). The `subject_cn` and `issuer_cn`
  fields populate from the first certificate in the PKCS#7
  SignedData bundle via a small in-tree DER walker —
  PrintableString and UTF8String CommonNames are extracted as
  heap-allocated UTF-8; T61String / BMPString / IA5String stay
  NULL (consumer renders "(unknown)" rather than risk
  malformed-string crashes). Best-effort, diagnostic-only —
  not a security-decision input (the formal way to identify the
  Authenticode signer is via SignerInfo's IssuerAndSerial; this
  walker assumes the conventional first-cert ordering). Unblocks
  offline integrity-check tooling — incident response on a
  suspect EFI binary, BIOS-update pre-flight, bootable-media
  verification — without committing to launching the image.

  Side-effect note (documented in the header): the dry-run path
  runs the firmware's PE loader, which allocates image memory,
  applies relocations, and invokes any registered
  `EFI_SECURITY2_ARCH_PROTOCOL` handlers. Production firmwares
  that hook those for audit logging, PCR measurement, or `dbx`
  notifications will trigger those side effects on every
  `consult_db = true` call. Pass `false` when those side effects
  are unacceptable.

### Test stats

2543 unit tests passing on both X64 and AARCH64 (was 2522 at
v0.11.1 cut; +21 across the new console primitive, image-verify
two-axis check, and the X.509 CN extractor — including 7 hand-
crafted DER fixtures exercising the Name walker against
PrintableString / UTF8String / RDN ordering / no-CN /
unsupported-encoding / truncated-input edge cases).

## 0.11.1 — 2026-05-03

### Changed

- **`axl_smbios_slot_usage_str(0x05)` returns spec-canonical
  `"Unavailable"`** (was `"CPU NOT INSTALLED"`, a vendor
  reinterpretation). SMBIOS 3.7 Table 12 row 0x05 is "Unavailable";
  vendor-flavored renderings (e.g. an OEM that wants
  "CPU NOT INSTALLED" for socket-associated 0x05 slots) belong in
  consumer code via the typed
  `AxlSmbiosSystemSlot.current_usage` raw byte. Breaking change for
  downstream tools that grep for the literal old string.

- **`axl_smbios_get_oem_string` signature gains a `*required` out
  parameter** (was `(idx, buf, buf_cap)`, now
  `(idx, buf, buf_cap, *required)`). The truncation contract
  changed: a too-small buffer now returns -1 without copying
  (instead of silently truncating), and `*required` is set to
  the byte count needed (string length + NUL). NULL `*required`
  is allowed. Breaking change for any consumer that called the
  old 3-arg form expecting silent truncation; the v0.11.0
  shipping signature was test-only so no in-tree callers are
  affected.

### Removed

- **`AxlIpmiCapabilities.dell_idrac_interface`** — informational
  probe of a reverse-engineered Dell iDRAC GUID with no public
  spec. Field removed from the struct, GUID + probe call removed
  from `src/ipmi/axl-ipmi.c`, and the corresponding
  `tools/ipmi.c probe` line removed. Pre-1.0 ABI reorder of
  subsequent fields. The Dell `EFI_IPMI_TRANSPORT` adapter
  (`axl-ipmi-dell.c`) and its `dell_ipmi_transport` capability
  bool stay — that protocol's shape is published in open-source
  uefi-ipmitool, so it qualifies as publicly knowable vendor-
  protocol naming.

### Fixed

- **Comments referencing "Mongoose Mini PC"** (Dell internal
  product name) for SMBIOS chassis type 0x23 changed to "Mini PC"
  per SMBIOS 3.7 spec. Bucket assignment unchanged
  (0x23 → `AXL_SMBIOS_CHASSIS_CLASS_EMBEDDED`); comment-only
  cleanup across `src/smbios/axl-smbios.c`,
  `include/axl/axl-smbios.h`, `src/smbios/README.md`, and the
  test description.

### Added

- **`axl_ipmi_chassis_identify(session, interval_sec, force_on)`**
  — typed wrapper around IPMI Chassis 0x04 (front-panel ID LED)
  next to `axl_ipmi_chassis_control`. Sends 1 byte (timed
  identify) when `force_on` is false, 2 bytes (with bit 0 of byte
  1 set) when true. Returns 0 on CC 0x00, -1 on transport error
  or non-zero CC; CC observable via
  `axl_ipmi_session_last_cc()`. 8 unit-test scenarios covering
  the wire-format paths and the CC=0xC1 / 0xCC / transport-error
  failure modes.

- **`axl_pci_get_header_type(addr, *type, *is_multi_function)`**
  — typed reader for PCI config offset 0x0E. Splits the byte into
  the new `AxlPciHeaderType` enum (`NORMAL` / `BRIDGE` / `CARDBUS`)
  and the bit-7 multi-function flag. Either out param NULL is
  allowed. Absent-function precheck via `VID == 0xFFFF` mirrors
  `axl_pci_get_vid_did`'s posture so callers don't get a bogus
  "0x7F multi-func" result on missing slots.

- **`axl_pci_get_subsystem(addr, *svid, *sdid)`** — typed reader
  for SVID/SDID at config offsets 0x2C/0x2E with header-type-0
  check baked in. Returns -1 for non-Type-0 functions (PCI-PCI
  bridges and CardBus bridges use those bytes for unrelated
  fields). Retires three raw config-space reads + manual header-
  type masking that consumers re-implement on every PCI walk.

- **`axl_nvstore_get_alloc(ns, key, **buf, *size)`** — read-with-
  malloc variant of `axl_nvstore_get`. Probes for size, allocates
  `needed + 1` bytes (zero-extended trailing byte so callers can
  treat the result as NUL-terminated), reads the payload, and
  hands the buffer back to the caller. Caller frees with
  `axl_free`. Probe-then-grow uses the probe's return code to
  distinguish a 0-byte variable (success, 1-byte NUL allocation)
  from a missing variable (-1). UEFI's `SetVariable(size=0)` means
  "delete" so the empty-variable path can't be exercised on the
  EFI backend; the distinction matters for any future backend
  (Linux `/sys/firmware/efi/efivars/`, in-process mock harness)
  that allows 0-byte values.

- **`axl_smbios_get_oem_string(idx, buf, buf_cap)`** — convenience
  reader for SMBIOS Type 11 OEM Strings by 1-based global index
  across all Type 11 records. Walks records in firmware order,
  accumulates per-record string counts, copies the string at the
  requested index into the caller's buffer with NUL-terminating
  truncation. Most platforms ship a single Type 11 record; the
  multi-record path is robustness for firmware that splits OEM
  strings across records. The existing
  `axl_smbios_read_oem_strings(hdr, ...)` is still available for
  callers that want raw string-pointer arrays.

- **`AXL_ARG_CHOICE` typed positional / flag** — new
  `AxlArgType` enum value plus a `choices` field on `AxlArgDesc`
  (NULL-terminated `const char *const *`). The framework rejects
  any input not in the allowed set with a breadcrumb-prefixed
  error matching the out-of-range numeric format, and emits the
  choice list as a `<a|b|c>` value hint in `--help`. NULL or
  empty `choices` array degrades to `AXL_ARG_STRING` (caller can
  declare CHOICE without a list when validation is custom).
  Comparison is case-sensitive. Field is appended to the end of
  `AxlArgDesc`, so existing zero-initialized literals via
  designated initializers keep working unchanged.

- **`assert_in_section LABEL SECTION_MARKER PATTERN`** in
  `scripts/axl-common.sh` — section-aware assertion helper for
  nsh-driven QEMU tests. Convention: bracket each command's
  output with `echo "=== <SECTION> ==="`; `assert_in_section`
  slices the log between markers and greps just that slice, so
  an assertion can target a specific command's output rather
  than the whole serial log. Reads the log path from the global
  `$LOG` (or `$TEST_CLEAN_LOG` as a fallback for scripts already
  using that name). PASS/FAIL output shape matches the existing
  test runner.

- **`axl_stream_set_stdout_tee(extra)`** + **`axl_stream_set_stderr_tee(extra)`**
  — log-tee primitive. After the call, every byte written to
  `axl_stdout` (resp. `axl_stderr`) via `axl_print` / `axl_printf` /
  `axl_fprintf(axl_stdout, ...)` / `axl_write(axl_stdout, ...)` is
  also written to @p extra. NULL clears; multiple calls replace
  (no chain). The caller owns the tee stream and is responsible
  for closing it (typical pairing with `axl_atexit`). Tee write
  errors are swallowed — a broken log file must not break the
  console. Replaces the ~50-line `do_printf` + tee-callback +
  atexit-cleanup pattern downstream consumers wrote per tool that
  needed a `-o:<file>` log option.

- **`run-qemu.sh --qemu-arg STRING`** (repeatable) — passthrough
  for literal QEMU CLI tokens. Each `STRING` is shell-word-split
  and appended to the qemu command line in order. Lets test
  scripts add device emulation, debug knobs, or anything the
  script doesn't natively expose without forking it. Shell
  quoting is NOT honored — values with spaces aren't supported
  via this flag.

- **`AxlArgDesc.choices_case_insensitive`** — additive bool flag
  on `AXL_ARG_CHOICE` positionals/flags. When true, validation
  uses ASCII case-folded comparison (`axl_strcasecmp`) instead of
  byte-equal (`axl_strcmp`); the value reaching the handler still
  reflects the user's original casing. Lets consumers preserve
  user-facing tolerances like "DD_CFG / dd_cfg / DD_cfg all valid"
  on a typed positional. Default false preserves the byte-equal
  contract; field is appended at the end of `AxlArgDesc` so
  existing zero-initialized literals via designated initializers
  keep working unchanged. `--help` appends ` (case-insensitive)`
  to the `<a|b|c>` value hint when the flag is set so users know
  the relaxed match is in effect.

- **`run-qemu.sh --ipmi`** + **`--ipmi-extern SOCK`** + **`--ipmi-prop K=V`**
  — IPMI BMC simulator shortcuts. `--ipmi` adds an in-process
  `ipmi-bmc-sim` + `isa-ipmi-kcs` at canonical port 0xca2 (matches
  AxlIpmi's KCS default and `test/integration/test-ipmi-qemu.sh`'s
  wiring). `--ipmi-extern` switches to `ipmi-bmc-extern` over a
  Unix-domain socket the caller provides (lets consumers exercise
  full BMC behavior including OEM commands and Chassis Identify
  via OpenIPMI's `ipmi-sim` or pyghmi-bmcsim). `--ipmi-prop K=V`
  appends K=V to the `ipmi-bmc-sim` device line (mfg_id,
  product_id, fwrev1, fwrev2, device_id, guid, slave_addr).
  AArch64 emits a clear `WARN: --ipmi not supported on AARCH64
  (skipping)` and continues without IPMI wiring rather than
  failing.

- **`QEMU_DRYRUN=1`** env var on `run-qemu.sh` — prints the
  constructed qemu command (one token per `QEMU_DRYRUN: <token>`
  line) and exits 0 without launching qemu. Used by the new
  `test-run-qemu-flags.sh` scenarios; also useful for "what
  would run-qemu.sh do?" debugging.

### Tests

- **AxlMemPhys read/write round-trip across 8/16/32/64 widths.**
  New `test_mem_phys_round_trip` allocates a real identity-mapped
  phys page via `axl_alloc_pages`, then for each width writes a
  width-specific sentinel via `axl_mem_phys_writeN`, reads it
  back via `axl_mem_phys_readN`, and cross-checks via a direct
  `volatile uintN_t *` deref. Closes the gap in earlier coverage
  where every write helper and read{16,32,64} were untested.

### Test stats

2522 unit tests passing on both X64 and AARCH64 (was 2424 at
v0.11.0 cut; +98 across `axl_ipmi_chassis_identify`,
`axl_mem_phys_*` round-trip, the four PCI / nvstore / SMBIOS /
AxlArgs typed wrappers, the new stdout/stderr tee primitive,
the case-insensitive CHOICE flag, and the JEDEC vendor-code
regression pins shipped late in v0.11.0). Plus 15/15 in
`test-run-qemu-flags.sh` covering the new `--qemu-arg` /
`--ipmi` flags via `QEMU_DRYRUN`.

## 0.11.0 — 2026-05-02

USB tooling release. Adds a new top-level **AxlUsb** module (Phases
A-F: enumeration → vid/pid → class triplet → string descriptors →
JSON5 vendor/device sidecar → real hub-port topology) plus a
**`lsusb.efi`** Linux-style USB lister built on top of it. AxlSpd
gains a handle/singleton vendor-name API mirroring AxlPciIds, and
the JSON5 sidecar load lifecycle that AxlPciIds / AxlPciClassDb /
the inline JEDEC loader were each carrying separately is hoisted
into a single public **AxlSidecar** scaffold (`AxlSidecarStatus`
enum replaces the `0/-1/-2` magic numbers). All four shipped
sidecars (`pci-ids`, `pci-class`, `usb-ids`, `jedec`) now ride in
every release artifact, and the JSON5 converters that emit them are
installed alongside.

Headlines: AxlUsb cursor enumeration + per-interface granularity +
hub-port-aware tree walk; `lsusb.efi` with `-t` tree, `-v / -vv`
verbose, `-d V[:P]` filter, `-s BUS:DEV`, `-n` numeric;
AxlSidecar public scaffold; AxlSpd handle API + `axl_spd_vendor_name`
singleton (memspd migrated off its inline JEDEC loader);
image-path-anchored sidecar discovery so an absolute-path
`startup.nsh` invocation no longer needs `cd \`; atexit-driven
sidecar cleanup so consumers stop leaking on shutdown.

### Added

**Tools**
- `lsusb.efi` — Linux-style USB lister. Default short form +
  `-t` tree view (mirrors lspci's tree shape with hub-port chains)
  + `-v / -vv` per-interface detail + `-d V[:P]` filter +
  `-s BUS:DEV` filter + `-n` numeric mode. Vendor/device/interface-
  class names auto-loaded from companion `usb-ids.json5`;
  `--ids-file` / `--debug` overrides.

**Library — AxlUsb (new module)**
- Cursor-style enumeration: `axl_usb_next(prev)` walks
  `EFI_USB_IO_PROTOCOL` handles in (bus, device, interface) order.
  Bus / addr / interface ordinals synthesized from each handle's
  device path so they're stable across UEFI shell invocations.
- Descriptor reads: `axl_usb_get_vid_pid`, `axl_usb_get_class`
  (interface-class triplet base / sub / protocol; per-field NULL-
  optional), `axl_usb_get_string` (UTF-8 manufacturer / product /
  serial via UsbGetStringDescriptor + UCS-2 → UTF-8).
- `axl_usb_class_string` / `_fmt` mirrors the AxlPci class-string
  shape (FMT_FULL / FMT_SUBCLASS / FMT_BASE; omits unknown tiers
  rather than printing `<unknown>`; `Class XXXXXX` numeric
  fallback for wholly unknown classes).
- Hub-port topology walker: `axl_usb_tree_for_each(fn, ctx)`
  emits each device with depth = number-of-USB-nodes − 1; direct
  attachment to the root hub yields depth 0, one hub in between
  yields depth 1, etc. `AXL_USB_TREE_MAX_DEPTH = 8` cap is
  generous against the USB-spec real-world cap of 5 hubs.
- Handle API for the vendor/device-name database: `AxlUsbIds`
  opaque type + `axl_usb_ids_open` / `_open_from_buffer` /
  `_close` / `_vendor_name` / `_device_name` / `_format_name` /
  `_foreach_*`. Singleton wrapper: `axl_usb_ids_load(override)` /
  `_free` + `axl_usb_vendor_name` / `_device_name` /
  `axl_usb_format_name`. Authoritative-override semantics, atexit
  cleanup, schema-field validation — all mirroring AxlPciIds.
- Per-name length contracts: `AXL_USB_VENDOR_NAME_MAX = 128`,
  `AXL_USB_DEVICE_NAME_MAX = 192`, `AXL_USB_CLASS_NAME_MAX = 128`,
  `AXL_USB_NAME_COMPOSED_MAX = 384`, `AXL_USB_STRING_MAX = 384`
  (worst-case BMP UTF-8 expansion + NUL).

**Library — AxlSidecar (new public scaffold)**
- Public `AxlSidecarStatus` enum: `AXL_SIDECAR_OK` /
  `_FILE_MISSING` / `_PARSE_ERROR` replaces the `0/-1/-2` magic
  numbers across every sidecar API. Numeric values unchanged so
  the ABI is preserved.
- `axl_sidecar_open_file` / `_open_buffer` / `_check_schema`
  package the JSON5 load + REQUIRED-schema-field validation that
  AxlPciIds / AxlPciClassDb / AxlSpdIds / AxlUsbIds all share.
  Diagnostics now uniformly list accepted schema versions when
  rejecting an unknown one.

**Library — AxlSpd (handle + singleton vendor-name API)**
- New: `AxlSpdIds` opaque type + `axl_spd_ids_open` /
  `_open_from_buffer` / `_close` / `_vendor_name` / `_format_name`
  / `_foreach_vendor`. Singleton: `axl_spd_ids_load` / `_free` +
  `axl_spd_vendor_name` / `axl_spd_format_name`. Mirrors AxlPciIds
  shape exactly.
- Reverses the v0.7.0 "library deliberately does not embed a
  vendor-name table" stance — every consumer was reinventing the
  same JEDEC table inline. JSON5 sidecar at
  `share/jedec.json5` is the curated source.
- `tools/memspd.c` migrated: g_jedec hash table + try_load_jedec
  + jedec_lookup → `axl_spd_ids_load` + `axl_spd_vendor_name`.
  ~70 lines of duplicated loader removed; `--jedec-file` flag
  flows through to the override path.

**Library — Image-path discovery**
- `axl_app_image_path()` returns the UTF-8 path UEFI itself used
  to locate the binary (decoded from
  `EFI_LOADED_IMAGE_PROTOCOL.FilePath` + `DeviceHandle`'s shell
  mapping). Volume-prefixed (`FS0:\app.efi` rather than
  `\app.efi`) so dirname-based companion-file resolution works
  regardless of the current shell directory.
- `axl_resolve_data_file` prefers the image-path anchor over
  `argv[0]` (which can be a basename when the shell normalizes).
  Sidecars resolve correctly when `startup.nsh` runs the binary
  by full or basename without `cd \` first.

**Sidecars + host scripts**
- `share/usb-ids.json5` — curated USB vendor/device starter set
  (QEMU usb-mouse, common server NICs / mass-storage / HID).
  Hierarchical schema 1 (devices nest under vendor).
- `scripts/usb-ids-to-json5.py` — bulk-converts canonical
  linux-usb `usb.ids` to the JSON5 schema. `--vendors-only`
  filter, `--self-test` that pins a known parser-misattribution
  edge case (column-0 `AT` / `HID` / etc. sections after `C XX`
  classes).
- `scripts/_ids_parser.py` — the line-level tab-indented hierarchy
  parser shared by `pci-ids-to-json5.py` and `usb-ids-to-json5.py`.
  Exposes `parse_ids(text, *, has_subsystems, allowed_vendors)` so
  any future consumer of the same file family (e.g. `oui.txt`)
  can plug in.
- `src/usb/CONSUMERS.md` — one-page integration playbook
  (lifecycle, what happens when the USB stack or DB is absent,
  per-device vs per-interface walks with three patterns spelled
  out, layered DB priority, thread-safety contract, sidecar
  shipping locations).

**Distribution**
- `.deb` / `.rpm` install all four sidecars to
  `/usr/share/axl/{pci-ids,pci-class,usb-ids,jedec}.json5`
  (was: only `pci-ids` / `pci-class`).
- The same packages install the JSON5 converter scripts to
  `/usr/share/axl/scripts/` (`pci-ids-to-json5.py`,
  `usb-ids-to-json5.py`, `_ids_parser.py`). End users no longer
  need a clone of the SDK source to convert canonical
  `pci.ids` / `usb.ids` to the JSON5 format.
- `axl-sdk-tools-<arch>.tar.gz` UEFI tarball stages all four
  sidecars next to the `.efi` binaries so AxlPciIds / AxlUsbIds /
  AxlSpdIds find their data via companion-path autodiscovery on
  a USB stick boot.
- `tools-tarball/README.txt` enumerates 14 tools (was: stale
  manual list missing `lsusb`, `lspci`, `memspd`, `cat`).

**Test infrastructure**
- QEMU runner now wires `qemu-xhci` + `usb-mouse` (port 1) +
  `usb-hub` (port 2) + `usb-tablet` (behind hub at port 2.1) so
  `axl_usb_*` / `lsusb -t` exercise a real multi-tier topology
  in CI on both arches. SKIP balancers retained for hardware
  paths the runner can't synthesize.
- `scripts/run-qemu.sh --bridges` mirrors the runner's USB
  topology for interactive smoke-tests.
- `scripts/run-qemu.sh` auto-stages `share/usb-ids.json5` and
  `share/jedec.json5` next to the EFI on the disk image.

### Changed

- `axl_pci_class_load` and the new sidecar loaders all REQUIRE a
  top-level `schema: N` field (validated through
  `axl_sidecar_check_schema`). Defaulting either way silently
  misparses files of the wrong version. Shipped fixtures already
  declare it.
- `axl_pci_class_string_fmt` and the new `axl_usb_class_string_fmt`
  share an internal output-shape resolver
  (`axl_class_string_fmt_resolve`) — same FMT_FULL / FMT_SUBCLASS /
  FMT_BASE rules for both APIs.
- `axl_path_join` now picks the separator (`/` or `\\`) from the
  anchor's existing style: an anchor containing `\\` or `:` gets
  backslash; otherwise forward-slash. Mixed-separator outputs
  (e.g. `fs0:\\dir/file`) were silently rejected by the UEFI
  shell.
- `axl_pci_ids_load` and `axl_pci_class_load` register an
  `axl_atexit` trampoline on first successful load. Consumers
  that load once and exit without explicit `_free` no longer leak
  the parsed hash tables. Calling `_free` explicitly still works
  (it unregisters the trampoline). New `axl_spd_ids_load` and
  `axl_usb_ids_load` use the same scaffold.
- `share/pci-class.json5` ships an empty `classes[]` block (the
  v0.10.0 demo `[overlay]` marker on Host bridge moved to a test-
  only fixture). Production output stays name-only.
- Public API return types: every `axl_*_ids_open` /
  `_open_from_buffer` / `_load` and `axl_pci_class_*` variant now
  returns `AxlSidecarStatus` instead of `int`. Numeric values
  unchanged (0/-1/-2) so legacy `if (rc != 0)` callers keep
  working at the ABI level. New code uses named constants.

### Fixed

- **`share/jedec.json5`** — three vendor codes had wrong bytes:
  Nanya (0x830B → 0x030B) and Crucial (0x859B → 0x059B) had the
  parity bit wrongly OR'd into the bank byte (the bank field is a
  raw JEP-106 continuation count, no parity); Patriot (0x051D →
  0x059D) had the id byte missing its odd-parity MSB (real Patriot
  DIMMs report 0x9D on the wire). All three would have missed
  every lookup against real hardware. The other 12 entries had
  bank indices and id bytes that happened to satisfy parity, so
  the bug was invisible by coincidence.
- **`tools/lsusb.c` / `tools/lspci.c`** — `expand_count_flags`
  (the `-vv` → `--vv` pre-expand) called `axl_malloc` without
  checking for NULL; on OOM the loop NULL-derefed `argv[i]`. Fix:
  on alloc failure, return the caller's `argv` unchanged with no
  expansion; main detects pointer equality and skips the free.
- **`AXL_USB_STRING_MAX`** bumped 256 → 384. USB string
  descriptors cap at 254 bytes of UCS-2 = 127 BMP code points; a
  code point in U+0800..U+FFFF expands to 3 UTF-8 bytes, so the
  worst-case payload is 127 × 3 + NUL = 382. The 256-byte cap
  silently truncated. The header comment also previously claimed
  supplementary-plane support, which `axl_ucs2_to_utf8_buf`
  doesn't implement (it would emit two CESU-8 sequences for a
  surrogate pair); corrected to "BMP only — surrogate pairs not
  decoded."
- **`axl_usb_tree_for_each`** dropped an unreachable
  `n_ports == 0` branch. `slice_device_path` rejects paths
  without a USB node and `extract_port_chain` always writes the
  leaf, so `n_ports >= 1` is guaranteed at walk time.
- **PCI cap-walk on aa64** — VID precheck at walk entry +
  monotonic-progress guard. Pre-fix, an unprogrammed function on
  AArch64 could send `axl_pci_cap_next` into an infinite loop
  (caught via a downstream consumer's session); post-fix, walks
  terminate cleanly even on devices that report `0xFFFF` for
  Vendor ID.
- **Memory leaks at shutdown** — singleton sidecar loaders no
  longer require explicit `_free` for leak-free shutdown (the
  atexit trampoline handles it). Heap entries flagged by
  `AXL_MEM_DEBUG` are gone post-load + exit.
- **Image-path-anchored sidecar discovery** — `startup.nsh` that
  ran `fs0:\app.efi` previously required `cd \\` for sidecar
  autodiscovery to work. `axl_app_image_path` + `axl_path_join`'s
  consistent-separator behavior fix this.
- **`axl_pci_class_string`** no longer emits `<unknown>` for
  tiers with no defined name — those tiers are omitted entirely
  (matches Linux lspci posture). Wholly unknown classes fall back
  to `Class XXXXXX` numeric form.

### Test stats

2424 unit tests passing on both X64 and AARCH64 (was 2316 at v0.10.0
cut; +108 across AxlUsb Phases A-F, AxlSidecar, AxlSpdIds, image-path,
atexit cleanup, and the new vendor-code regression pins).

## 0.10.0 — 2026-05-02

PCI tooling release: a Linux-style **`lspci.efi`** built on a fully
fleshed-out PCI topology + name-decoding API. Vendor / device /
subsystem / class names load opportunistically from JSON5 sidecars
(`pci-ids.json5`, `pci-class.json5`) so new entries land via a
`git pull` of the sidecar rather than rebuilding every consumer
binary. Both sidecars now ship in every release artifact.

Headlines: `axl_pci_tree_for_each` for depth-first PCI topology
walks (used by `lspci -t`), a handle-based name database with
multi-handle overlay support, a documented load-failure split
(`-1` missing vs `-2` parse error), per-name length contracts, a
hierarchical schema 2 layout for hand-maintained `pci-ids.json5`
files at scale, and a fix for an aa64 cap-walk infinite-loop that
surfaced in a downstream consumer.

### Added

**Tools**
- `lspci.efi` — Linux-style PCI lister. Default short form +
  `-t` SoftBMC-style tree view + `-v / -vv / -vvv` detail levels
  (caps, ext caps, subsystem) + `-x / -xx / -xxx` hex dumps
  (64 B / 256 B / 4 KiB ECAM) + `-s BDF` filter + `-d V[:D]`
  filter + `-n` numeric mode + `-D` always-show-domain. JSON5
  sidecars auto-loaded from companion path; `--ids-file` /
  `--debug` overrides.

**Library — PCI topology**
- `AxlPciBridge` + `axl_pci_bridge_info(addr, *out)` — read the
  PCI-PCI bridge bus-number tuple (primary/secondary/subordinate)
  with header-type validation.
- `axl_pci_tree_for_each(fn, ctx)` — depth-first per-segment
  topology walker with cycle detection (per-segment visited-bus
  bitmap) and recursion cap (`AXL_PCI_TREE_MAX_DEPTH = 16`).
- `axl_pci_cap_id_str` / `axl_pci_ext_cap_id_str` — cap-ID name
  lookup (legacy + PCIe extended).

**Library — Name databases**
- Handle API: `AxlPciIds` opaque type + `axl_pci_ids_open` /
  `_open_from_buffer` / `_close`.
- Per-tier handle lookups: `axl_pci_ids_vendor_name` /
  `_device_name` / `_subsys_name`.
- Composed-name helper: `axl_pci_ids_format_name(handle, vid,
  did, buf, buflen)` (handle) + singleton wrapper
  `axl_pci_format_name(vid, did, buf, buflen)`. Documented
  fallback chain so every consumer renders the same string for
  the same `(vid, did)` pair.
- Iter API: `axl_pci_ids_foreach_vendor` / `_device` / `_subsys`
  with non-zero-return early-stop semantics.
- Singleton API: `axl_pci_ids_load(override_path)` with
  authoritative explicit-path semantics; `axl_pci_ids_free`;
  `axl_pci_vendor_name` / `_device_name` / `_subsys_name`.
- Load failure split: `axl_pci_ids_load` / `_open` return
  `-1` for missing file, `-2` for parse error. Lets tools log
  deployment problems silently and authoring problems loudly.
- Per-name length contracts (compile-time stack-buffer sizing):
  `AXL_PCI_VENDOR_NAME_MAX = 128`, `AXL_PCI_DEVICE_NAME_MAX = 192`,
  `AXL_PCI_SUBSYS_NAME_MAX = 192`, `AXL_PCI_CLASS_NAME_MAX = 128`,
  `AXL_PCI_NAME_COMPOSED_MAX = 384`.

**Library — Class-name overlay**
- `AxlPciClassDb` opaque type + `axl_pci_class_open` /
  `_open_from_buffer` / `_close` + per-tier handle lookups.
- Singleton: `axl_pci_class_load` / `_free`.
  `axl_pci_class_string` and `_fmt` automatically consult the
  loaded overlay before the compiled-in tables.
- Output-shape selector: `AxlPciClassFmt` enum
  (`FULL` / `SUBCLASS` / `BASE`) + `axl_pci_class_string_fmt`.

**Sidecars + host scripts**
- `share/pci-ids.json5` — curated vendor/device/subsystem starter
  set (QEMU + common server NICs / NVMe / GPUs). Schema 2
  hierarchical layout (devices nest under vendor, subsystems
  nest under device).
- `share/pci-class.json5` — class triplet name overlay starter.
- `scripts/pci-ids-to-json5.py` — bulk-converts canonical pci.ids
  to the JSON5 schemas. Schema 2 default; `--schema 1` for the
  legacy flat layout; `--vendors-only` filter; `--emit-class FILE`
  also writes the class overlay; `--self-test` exercises the
  parser/emitters against an embedded fixture.
- `src/pci/CONSUMERS.md` — one-page integration playbook
  (lifecycle, return codes, DB-absent behavior, output
  convention, layered DB priority chain, thread-safety contract,
  bulk-population recipe).

**Test infrastructure**
- PCI bridge tree (one PCIe root port + virtio-rng-pci) injected
  into the QEMU integration runner so topology-walking code is
  exercised in CI.
- `scripts/run-qemu.sh --bridges` mirrors the runner topology
  for interactive smoke-tests.
- `scripts/run-qemu.sh` auto-stages `share/pci-ids.json5` and
  `share/pci-class.json5` next to the EFI on the disk image.

### Changed

- `axl_pci_class_string` no longer emits `<unknown>` for tiers
  with no defined name — those tiers are omitted entirely
  (matches Linux lspci posture). Wholly unknown classes fall
  back to `Class XXXXXX` numeric form.
- `axl_pci_ids_load(override_path)` is now AUTHORITATIVE for
  explicit overrides — non-NULL `override_path` does not fall
  back to companion/cwd discovery (was: fallback chain). NULL
  still autodiscovers. Old behavior silently masked typos by
  loading whichever file existed; new behavior surfaces them
  cleanly with `-1`. Same change applied to `axl_pci_class_load`.
- JSON5 sidecars now ship in every release artifact:
  `.deb` / `.rpm` install to `/usr/share/axl/{pci-ids,pci-class}.json5`;
  the `axl-sdk-tools-<arch>.tar.gz` tools tarball stages them
  next to the .efi binaries (UEFI auto-discovery picks them up).
- The pci-ids JSON5 `schema` field is now REQUIRED. Defaulting
  to either version would silently misparse files of the other
  version (a v2 file forgetting the declaration would parse as
  v1 with every nested device dropped).

### Fixed

- `axl_pci_cap_next` / `axl_pci_ext_cap_next` no longer
  infinite-loop on absent BDFs. ECAM all-1s reads on aa64 QEMU
  `virt` machine at `0:1f.0` fooled the iterator into a self-loop
  at offset `0xFC`. Fix: vendor-ID precheck at walk entry +
  monotonic forward-progress guard. Surfaced via downstream
  consumer running on aa64 hardware/emulation.

## 0.9.0 — 2026-05-01

Substantial release covering shell-pipe support, per-stream encoding,
a chunk-buffer line iterator, ASCII ctype helpers, device-path
iteration, BMH substring search, several extracted patterns, and
the headline restructure: the umbrella **AxlIO** module split into
**AxlStream** (byte-stream abstraction, the `<stdio.h>` analog) and
**AxlFs** (path-based filesystem operations, the `<sys/stat.h>` +
`<dirent.h>` analog), with a cleaner one-way dependency direction.
The `axl_io_port_*` x86 PIO module renamed to `axl_port_*` to
resolve the namespace overlap.

### Breaking

Consumers that include the umbrella `<axl.h>` need no header edits.
Direct includes and a few public renames:

- `<axl/axl-io.h>` → `<axl/axl-stream.h>` (and/or `<axl/axl-fs.h>`)
- `<axl/axl-io-port.h>` → `<axl/axl-port.h>`
- `axl_io_init()` → `axl_stream_init()` (called automatically by
  `axl_runtime_init` — most callers don't invoke it directly).
- **PCI**: `class24` field renamed to `class_code` across the
  `AxlPciInfo` API and helpers — the field is the standard
  3-byte (Class/Subclass/ProgIF) encoding from PCI config space,
  not specifically a 24-bit fragment of byte 0x18. Consumers
  accessing the field by name need to update.

### Added

- **Shell-pipe stdin/stdout** (`axl_stdin`, `axl_stdout_raw`):
  raw-bytes stdin backed by `EFI_SHELL_PARAMETERS_PROTOCOL.StdIn`
  and a binary-clean stdout-raw companion that bypasses the
  UTF-8→UCS-2 console conversion. Tools can now sit in a pipeline
  without mangling bytes.
- **Per-stream encoding** (`axl_stream_set_encoding` /
  `axl_stream_get_encoding`, `AxlEncoding`): configure a stream
  with its wire-side encoding (UCS-2 LE / UCS-2 BE / ASCII /
  UTF-8 default), and `axl_read`/`axl_write` transparently
  transcode. Permissive — bad input never errors. Default UTF-8
  is passthrough so existing streams are unaffected.
- **POSIX-shape stream conveniences**: `axl_fgets`,
  `axl_vfprintf`, `axl_ferror`, `axl_clearerr`, plus the bounded
  `axl_readline_max(stream, max_bytes)` to cap heap on no-newline
  inputs.
- **`axl_text_stream_wrap` headerless UCS-2 sniff**: in addition
  to BOM detection, the wrapper now content-sniffs UCS-2 LE/BE
  for files that lack a BOM (UEFI shell `cmd > out.txt` shape).
- **`AxlLineReader` + `axl_walk_lines`**: chunk-buffer line
  iteration with caller-supplied working buffer. Constant memory
  regardless of input size; lines longer than the buffer fire a
  truncated callback and the rest of the line is drained so line
  counting stays correct. `axl_walk_lines` is a callback wrapper
  for callers that prefer dispatch over iterator-style loops.
- **ASCII ctype helpers** (`axl_isdigit`, `axl_isxdigit`,
  `axl_isalpha`, `axl_isalnum`, `axl_isspace`, `axl_tolower`,
  `axl_toupper`) plus **`axl_hex_nibble`** and **`axl_strnlen`**.
  Closes a dogfood gap — 14 hand-rolled call sites swept.
- **`axl_strcasestr_len`** — length-bounded case-insensitive
  substring search. Mirror of `axl_strstr_len`. Lets callers with
  non-NUL-terminated slices (`AxlLineReader` line bodies, network
  buffers, etc.) avoid the copy-to-stack-buffer dance.
- **Device-path iteration** (`axl_device_path_for_each`,
  `axl_device_path_find`, `axl_device_path_size`): bounded-step
  walker for `EFI_DEVICE_PATH_PROTOCOL` with one safety floor
  (64 nodes) shared across all callers.
- **`AxlVolume.device_path`**: the volume descriptor now carries
  the firmware-owned DP pointer so callers don't need to look it
  up per-iteration via `axl_handle_get_service`.
- **`axl_dir_walk`**: recursive callback-style directory walker
  with POSIX `find -maxdepth` semantics. Replaces five hand-rolled
  walks (find, grep, driver, io-demo, etc.).
- **`axl_smbios_format_uuid`**: SMBIOS §7.2.1 mixed-endian UUID
  formatter — also fixes a bug in `tools/dmidecode` where UUIDs
  were emitted byte-raw and didn't match Linux dmidecode output.
- **`axl_resolve_data_file`**: sidecar-data lookup convenience
  (override → companion → cwd) for tools that ship optional JSON.
- **`tools/cat`**: new tool, file/stdin display with `-n -s -A
  -E -T --raw -e ENC`. First in-tree consumer of
  `axl_stream_set_encoding`. The `-e` flag forces a wire encoding
  (utf8/ucs2le/ucs2be/ascii); default BOM-probes via
  `axl_text_stream_wrap`.

### Changed

- **AxlIO → AxlStream + AxlFs split.** Public header `axl-io.h`
  replaced by `axl-stream.h` + `axl-fs.h`; source dir `src/io/`
  by `src/stream/` + `src/fs/`. Mirrors POSIX `<stdio.h>` vs
  `<sys/stat.h>` + `<dirent.h>`. One-way dependency: AxlFs
  depends on AxlStream; AxlStream is independent.
- **`axl_io_port_*` → `axl_port_*`** (x86 PIO). Header
  `axl-io-port.h` → `axl-port.h`. Resolves the "io" namespace
  overlap.
- **`tools/grep` rewrite**: streams its input via `axl_walk_lines`
  + a 64 KiB working buffer instead of slurping the whole file
  with a 1024-byte fixed line buffer and a 16 MiB size cap.
  Single code path for both file and stdin (Linux-grep parity).
  Memory bounded by the working buffer regardless of file size
  or line shape.
- **`axl_strstr_len` / `axl_strcasestr` switch to Boyer-Moore-
  Horspool** for needles ≥ 4 bytes — sub-linear average; what
  glibc `memmem` / musl twoway-fallback / BSD libc all use.
  Below the threshold, naive scan still applies. Speeds up grep,
  HTTP header parsing, JSON token boundary lookups, log
  filtering, and SMBIOS string searches transparently.
- **Build system: `-std=gnu2x`** (was implicit gnu17). The
  pre-standard alias for C23 — accepted by gcc 13/14 and forward.
  Codebase consumes the C23 features that matter
  (`[[nodiscard]]`, `[[noreturn]]`, the `static_assert` keyword)
  without forcing a toolchain bump. `AXL_WARN_UNUSED` and
  `AXL_NORETURN` macros now expand to those attributes.
- **Build system: ramdisk blob via `.incbin`** instead of
  xxd-generated headers. mkrd's embedded `RamDiskDxe.efi` is now
  emitted by `tools/mkrd-blob.S`'s `.incbin` directive — no more
  multi-MB C array literals to parse on every mkrd build.

### Fixed

- `tools/dmidecode` UUID output now matches Linux `dmidecode` and
  Windows `wmic` (was emitting raw bytes, missing the SMBIOS
  §7.2.1 mixed-endian Data1/2/3 swap).
- `axl_handle_get_service` defensively NULLs `*interface` on
  every error path. UEFI HandleProtocol doesn't guarantee
  preservation on failure, and callers like `AxlVolume.device_path`
  now gate on NULL — a stale pointer would have been a footgun.
- `axl_dir_walk` semantics aligned with POSIX `find -maxdepth`
  (was off by one).
- `axl_walk_lines` and `AxlLineReader` correctly drain the rest
  of an over-cap line from the stream so line counting stays
  meaningful.
- `axl_text_stream_wrap` rejects write-only sources at
  construction (was a NULL-deref on the eager BOM probe).
- `axl_stream_set_encoding` and `axl_fseek` reset transcode
  buffers — partial sequences from a prior encoding/position
  no longer splice onto the new byte stream.

### Internal

- `axl-stream-internal.h` shrunk to just the `struct AxlStream`
  body + `axl_stream_new()` — the previous `*_internal`
  trampolines (`axl_fopen_internal`, `axl_bufopen_internal`,
  `axl_file_get_contents_internal`, etc.) were collapsed; the
  public names are the implementations directly.
- 5 patterns extracted from cross-file duplication:
  `axl_smbios_format_uuid`, `axl_device_path_*`,
  `AxlVolume.device_path`, `axl_dir_walk`,
  `axl_resolve_data_file`. mkrd's three near-identical
  iteration loops shed ~30 lines each.

## 0.8.1 — 2026-05-01

Backward-compatible polish on top of v0.8.0's AxlArgsNode unification:
branch nodes can now carry a default handler (the `do bios` →
"print summary" pattern), AXL_ARG_S64 honors min/max bounds, two
new generic PCI helpers, and follow-up fixes from the post-v0.8.0
review.

### Added

- **Branch nodes with a default handler.** A node can now set BOTH
  `.verbs` AND `.handler`; the handler runs only when no sub-verb
  is supplied. Useful for the established CLI pattern where a
  category like `do bios` has subverbs (info, test, pci) but also
  prints a default summary when invoked alone. Dispatch is
  unambiguous: matched verb → recurse, unmatched non-flag → error
  ("unknown verb"; the handler is not a catch-all), no non-flag
  args → handler with parsed branch flags. `--help` still wins
  at every level. Help output annotates the verb whose handler
  matches the default with `(default)` so users see which sub-verb
  the no-arg form is equivalent to.
- **`axl_pci_dump(addr, buf, bytes, *out_read)`** — single-call
  PCI config-space dump. Reads up to 4096 bytes (PCIe ECAM cap)
  in 32-bit chunks; folds endian-pack, absent-detection (VID ==
  0xFFFF at offset 0), and ECAM cap into one call. Replaces the
  per-tool hand-rolled `for (reg = 0; reg + 4 <= bytes; reg += 4)
  read32; pack into buf` loop.
- **`axl_pci_class_string(class24, buf, buflen)`** — decode the
  24-bit PCI class code to a human string per the PCI Code and
  ID Assignment Spec. Output shape: `"<base> / <sub> / <prog>"`
  (e.g. `"Serial bus controller / USB / xHCI"`). ~140 LOC of
  static lookup tables; vendor/device-name lookup intentionally
  out of scope (pci.ids is too large for AXL).
- **`AXL_ARG_S64` min/max enforcement.** Pre-existing v0.7.x gap
  closed: U8/U16/U32/U64 enforced bounds but S64 silently
  accepted any in-type value. Same independent-bound check (each
  0 = no bound) with int64 cast — for negative lower bounds the
  descriptor sets `.min = (uint64_t)(int64_t)-N`, two's-complement
  round-tripped via the cast. Documented in `AxlArgDesc`.

### Changed

- **Documented vendor-naming policy** in `docs/AXL-Coding-Style.md`.
  AXL is hardware-vendor-agnostic at its public API surface (no
  `axl_dell_*` function names), but vendor-named files
  (`axl-ipmi-dell.c`) and vendor-named enum values
  (`AXL_IPMI_TRANSPORT_DELL`) / probe-struct fields are explicitly
  allowed where they describe vendor-specific external state.
  Codifies existing convention; no code changes.

### Fixed

- **Two nested-args tests no longer pass trivially.** Post-v0.8.0
  review found `test_args_nested_unknown_verb_at_branch` and
  `test_args_nested_branch_help_lists_subverbs` only asserted
  `rc + handler-not-called`; nothing verified the breadcrumb
  appeared in the error message or that help listed the right
  subverbs. Both now swap `axl_stdout` for an `axl_bufopen()`
  in-memory stream during the test and assert on captured bytes.
- **Whitespace alignment in 11 tools.** The earlier
  `.global_flags` → `.flags` sed left a 10-space indent that
  diverged from the 8-space sibling alignment. Tightened.

### Compatibility

Backward-compatible with v0.8.0:

- Branches without a default handler keep the v0.8.0 "show help
  on no-verb" behavior (regression-tested).
- Consumers that previously got the leaf+branch config error
  now silently work — additive, not breaking. Note: a consumer
  who *relied* on that error to catch an accidental
  handler-pasted-onto-branch programming mistake loses that
  signal; if you depend on validation-as-lint, audit branch
  nodes after upgrade.

## 0.8.0 — 2026-05-01

Source-incompatible AxlArgs unification: the dual `AxlArgsApp` /
`AxlVerb` types collapse into one naturally-recursive `AxlArgsNode`
that describes the program root, every inner branch ("category"),
and every leaf verb. Surfaced by a downstream consumer migrating
off the deprecated `axl_subcommand_dispatch` and discovering the
single-level constraint that forced consumers to either flatten
unhelpfully or hand-roll the deprecated dispatch pattern.

Also: `k`-prefix removed from every static-const-table identifier
across the codebase (33 names: `kVerbs`/`kFlags`/`kPositionals`/etc.
→ `verbs`/`flags`/`positionals`/etc.) to match the documented
naming convention.

### Breaking

- **`AxlArgsApp` removed; `AxlVerb` removed.** Both replaced by a
  single recursive `AxlArgsNode`:

  ```c
  struct AxlArgsNode {
      const char           *name;
      const char           *help;
      const AxlArgDesc     *flags;         // per-node, was global_flags at root
      const AxlArgDesc     *positionals;   // leaf only
      const AxlArgsNode    *verbs;         // branch only
      AxlVerbHandler        handler;       // leaf only — mutually exclusive with verbs
      AxlPreRunFunc         pre_run;
      void                 *user_data;
  };
  ```

  Mechanical migration: rename `AxlArgsApp` → `AxlArgsNode`,
  `AxlVerb` → `AxlArgsNode`, `global_flags` → `flags`. Existing
  single-level apps work unchanged after the rename.

- **`AxlArgsApp.usage` field removed.** Was unused by any in-tree
  consumer.

- **k-prefix dropped from static const tables.** Any consumer
  referencing in-tree symbols by name (none expected) needs
  `kFoo` → `foo`. Internal-only convention change otherwise.

### Added

- **Nested verb trees in AxlArgs.** A node is leaf-XOR-branch:
  leaves set `handler` + optional `positionals`; branches set
  `verbs` (NULL-terminated array of child nodes). Branch verbs
  recurse via the framework — consumers never call `axl_args_run`
  themselves at non-top level.
- **Parent-flag visibility.** Flags declared on a parent node are
  visible to descendant handlers via the same `axl_args_get_*`
  accessors (which now walk the parent chain on miss). A
  `--verbose` declared on the root is readable from a leaf two
  levels deep without re-declaration. Same pattern for
  `axl_args_user_data`: descendants inherit the nearest non-NULL
  parent value.
- **Breadcrumb error attribution.** Errors are prefixed with the
  full path: `do bios test: missing required <slot>` rather than
  `do: missing required <slot>`. Helps users isolate which level
  rejected their input.
- **`pre_run` per node, runs top-down.** Parent's `pre_run` fires
  before recursion into the child or before the leaf handler, so
  shared resources (config files, opened sessions) can be set up
  at any level and inherited by descendants.

### Changed

- **Documented `snake_case` for static const tables** in
  `docs/AXL-Coding-Style.md` (no `k`-prefix). Existing tables
  renamed to match — `tools/*.c` (11 tools), `src/ipmi/axl-ipmi.c`
  (vendor GUID tables), `test/data/spd-*.h` (auto-generated by
  `gen-spd.py` — also updated), and the `test/unit/axl-test-*.c`
  test files.
- **`docs/ROADMAP.md` build-system entry** rewritten with the
  v0.8.0 build incident (`ar rcs` + GNU make `-MD` not catching
  cross-archive deps after a public-header struct restructure)
  as the concrete trigger to migrate to meson+ninja or
  cmake+ninja. Stated preference: meson.

## 0.7.2 — 2026-05-01

JSON5 reader and writer support; PCI helper expansion driven by a
downstream-consumer cat-3 PCI subcommand family; nvstore/IPMI
quality-of-life fixes from real-hardware bring-up.

### Added

- **JSON5 reader** — opt-in via `AXL_JSON_PARSER_JSON5` flag on the
  new `axl_json_parse_flags` / `axl_json_load_file_flags`. Accepts
  the json5.org grammar superset: `//` and block comments, trailing
  commas, single-quoted strings, unquoted (identifier-name) object
  keys, hex number literals (`0x...`) and `+`/`-` number prefix,
  extended string escapes (`\'`, `\v`, `\0`, `\x##`, line
  continuations). Strict-JSON consumers (`axl_json_parse`) stay on
  jsmn unchanged. Hand-rolled, no new vendored dependencies. Token
  layout matches `jsmntok_t`, so the existing accessors
  (`axl_json_get_string`, `axl_json_array_begin/next`, ...) work
  without modification.
- **JSON5 writer** — `AXL_JSON_WRITER_TRAILING_COMMAS` flag and a new
  `axl_json_comment(w, text)` call. Pretty mode emits comments on
  their own line at the current indent; compact mode emits inline
  block comments with embedded close-comment sequences split for
  safety. Comments don't disturb the writer's container state.
  Unquoted-key emission deliberately not supported (escape-correctness
  footgun, no consumer benefit).
- **`axl_pci_addr_parse` / `axl_pci_addr_format`** — parse and format
  the canonical lower-hex `SSSS:BB:DD.F` form. Both 3-component
  (`bus:dev.func`, segment defaults to 0) and 4-component
  (`seg:bus:dev.func`) variants accepted; range-checked at parse
  time. Symmetric round-trip.
- **`axl_pci_get_vid_did` / `axl_pci_get_class24`** — boilerplate-
  killer wrappers over the standard PCI header offsets. `vid_did`
  folds the `vid == 0xFFFF` "function absent" sentinel into the
  return code. `class24` folds offsets 0x09/0x0A/0x0B into the
  canonical `(base << 16) | (sub << 8) | prog_if` form consumed by
  `axl_pci_find_by_class`.
- **`axl_pci_vpd_iter`** — full-walk callback complement to the
  existing `axl_pci_vpd_read` (which only fetches a single keyword).
  Visits both Read-Only and Read-Write resource sections; vendor-
  specific `V0..V9` and `Y0..Y9` keywords reach the callback
  alongside the standard set. Internal walker shared with
  `vpd_read` — one cap-list lookup, one tag walk, no duplicated
  logic.
- **`AxlIpmiDeviceId.firmware_minor_decoded`** — BCD-decoded
  companion to the existing raw `firmware_minor` byte. Both stay
  populated; consumers that care about BCD validity can compare
  decoded vs raw.

### Changed

- **AxlSubcommand deprecated.** The framework was superseded by
  AxlArgs in v0.7.0; deprecation warnings ride v0.7.2. Removal in
  a later release once the last out-of-tree consumer migrates.
- **`share/jedec.json` → `share/jedec.json5`.** The file uses the
  JSON5 grammar (real hex numbers `0x002C` instead of hex-encoded
  strings, unquoted keys, single-quoted names, `//` header comment
  block in place of the bogus `_comment` string field). Renamed
  so VS Code's strict-JSON validator stops flagging every JSON5
  feature; `tools/memspd.c`'s auto-discovery default updated.

### Fixed

- **`axl_nvstore_get` / `axl_nvstore_delete` log noise.**
  `EFI_BUFFER_TOO_SMALL` (canonical probe-then-grow size query) and
  `EFI_NOT_FOUND` (key-existence probe) on `get`, plus
  `EFI_NOT_FOUND` (idempotent delete) on `delete`, demoted from
  warning to debug. Both are normal control flow for callers and
  were producing visible WARN noise during `do boot show` on Dell
  hardware.
- **6-second hang on phantom KCS interface.** `axl_ipmi_kcs_open`
  now rejects a status-byte read of `0xFF` as a floating-bus /
  phantom KCS. Previously, SMBIOS Type 38 advertising a KCS that
  no real BMC answered caused the first `axl_ipmi_get_device_id`
  to spin for the full `KCS_POLL_TIMEOUT_US` (~5 s) inside
  `kcs_wait_ibf_clear` — a 6 s hang on every misadvertised
  interface. `axl_ipmi_session_new` doc updated to set the new
  "fast NULL on phantom transport" expectation.

## 0.7.1 — 2026-05-01

CI-only patch — released v0.7.0 artifacts are unaffected.

### Fixed

- **`test_rng` SKIP-path balancer count off by one.** When OVMF
  doesn't publish `EFI_RNG_PROTOCOL`, `test_rng` takes the SKIP
  path and emits balancer `test_check(true)` calls instead of
  exercising the protocol. The populated path emits 4 conditional
  checks (bytes succeeds, second fill, distinct, non-zero); the
  SKIP path was emitting only 3, leaving the count short by one
  vs the populated count. Surfaced as the v0.7.0 CI ratchet
  failure: local OVMF publishes EFI_RNG_PROTOCOL (full path,
  1905 tests) but the GitHub Actions runner's OVMF doesn't (SKIP
  path, 1904 tests). Adds the missing fourth balancer so total
  stays 1905 regardless of environment. Pre-existing class of
  bug per the cross-arch parity convention.

## 0.7.0 — 2026-05-01

Phase B3 (Platform Access) lands in full — AxlAcpi, AxlBoot, AxlPci,
AxlImage, AxlMemPhys, AxlWatchdog, AxlRng, and AxlSpd. Tools layer
gets a new declarative CLI parser (AxlArgs) and five reusable
helpers; all 11 in-tree tools migrate to the new framework, and the
dual-purpose CLI side of AxlConfig retires.

### Added

- **AxlAcpi** — ACPI table discovery via RSDP/RSDT/XSDT walk with
  cursor iteration matching `axl_smbios_find_next`. Public checksum
  verifier; typed readers for MCFG (PCIe ECAM segments), MADT (IOAPIC
  on x86, GIC regions on aa64), and FACP/FADT (SMI cmd, PM1 blocks,
  DSDT pointer). No AML interpretation. Header: `axl/axl-acpi.h`.
- **AxlBoot** — typed boot-option management. `AxlBootOption` struct
  (description / device-path text / opt_data) with
  `_option_get/_set/_delete/_free` and `_order_get/_set`.
- **AxlPci** — PCI/PCIe configuration-space access via ECAM (no
  legacy 0xCF8/CFC). Cursor iteration honours multi-function header
  bit; lookups by class24 and VID/DID; capability-list walker.
  Header: `axl/axl-pci.h`.
- **AxlImage** — load / start / unload UEFI executable images via
  EFI shell-style paths.
- **AxlMemPhys** — typed physical-memory access (read8/16/32/64,
  one-shot or mapped, byte search). Bounds-checked, never crashes
  on bad addresses.
- **AxlWatchdog** — `axl_watchdog_set / pet / disarm` wrapping
  `gBS->SetWatchdogTimer`.
- **AxlRng** — `axl_rng_bytes` over `EFI_RNG_PROTOCOL`. Returns
  -1 if the protocol isn't published.
- **AxlSpd** — DDR4/DDR5 SPD reader on AxlSmbus. Cursor iteration
  over 0x50..0x57; key byte at SPD offset 2 dispatches to the codec
  (0x0C=DDR4, 0x12=DDR5). DDR4 paging via EE1004 SPA pseudo-slaves
  (0x36/0x37); DDR5 paging via SPD5118 MR11 across eight 128-byte
  windows. Pure-decoder entry point `axl_spd_decode(buf, len, *out)`
  for offline analysis. DDR3 deliberately deferred.
- **`axl_io_port_*`** — promoted from internal backend to public.
  16/32-bit variants. Build-gated to x86 (compile error on aa64).
- **`axl_nvstore_*` extensions** — namespace registration for OEM
  variable GUIDs; `_delete`, `_iter`, `_get_attrs`. Behaviour
  change: unregistered namespace is now an error (was: silent
  fallback to global).
- **AxlSmbus byte ops** — `axl_smbus_read_byte` / `_write_byte`
  (SMBus spec §5.5.4 / §5.5.5). Required for SPD EEPROMs (24Cxx-
  style, no block framing) and SPD5118 register access.
- **Five new tool-layer helpers** — `axl_app_argv0` (new
  `<axl/axl-app.h>`), `axl_path_companion`, `axl_format_bytes`
  (IEC binary units), `axl_json_load_file`, plus the AxlHashTable
  adoption pattern in tools/memspd. Caught as gaps during the R+4
  post-commit review; `<axl/axl-spd.h>` was also missing from the
  umbrella `<axl.h>` and is now included.
- **AxlArgs** — new declarative CLI parser (`<axl/axl-args.h>`).
  Tools declare an `AxlArgsApp` tree (name, global flags, verbs,
  per-verb flags, typed positionals) and call `axl_args_run` from
  main; the framework parses argv, validates types and bounds,
  generates `--help`, and dispatches to a verb handler. Supports
  single-handler and multi-verb modes. Flag types: BOOL, STRING,
  U8/U16/U32/U64 with optional [min,max], S64 (delegates to
  `axl_str_to_s64`), MULTI (repeatable). Auto `--help`/`-h`.
  Compact short-flag groups (`-vh`) explicitly rejected; extra
  positionals past a fully-filled list rejected. Lifetime contract
  documented for AxlLoop interop.
- **`tools/memspd`** — `decode-dimms`-equivalent. Verbs `list /
  show <slot> / decode <slot>`. Vendor lookup is data-driven via
  `share/jedec.json` (15-vendor stub); auto-discovered next to the
  `.efi` or via `--jedec-file`. Missing sidecar prints raw 16-bit
  hex codes.
- **`scripts/qemu-patches/0001-smbus-eeprom-add-memdev-link.patch`**
  — adds a `memdev=<link<memory-backend>>` property to QEMU's
  `smbus-eeprom` device. Models on `pc-dimm`'s `memdev=` link.
  Unblocks `test/integration/test-spd-qemu.sh` end-to-end coverage
  of the AxlSpd wire path. Candidate for upstreaming.
- **`test/integration/test-spd-qemu.sh`** — wire-path AxlSpd test
  through the patched QEMU + SmbusHcShim chain.
- **`test/integration/test-net-tools.sh`** — closes the
  `test-tools.sh` coverage gap for the network-using tools (`fetch`,
  `netinfo`). All 11 in-tree tools now have direct tool-binary
  integration coverage.
- **`docs/AXL-Hardware-Fixture-Design.md`** + ROADMAP HF1–HF6 —
  proposal for vendor-neutral capture-and-replay of UEFI platform
  identity (SMBIOS, ACPI, PCI manifest, IPMI, Redfish) so axl-sdk
  tools can be exercised against real-world platforms under QEMU
  without lab access. No code yet; planning artifact.

### Changed

- **All 11 tools migrated to AxlArgs**: `memspd`, `dmidecode`,
  `sysinfo`, `fetch`, `grep`, `find`, `hexdump`, `ipmi`, `netinfo`,
  `mkrd`, `rfbrowse`. Per-tool boilerplate (~12–25 lines of
  AxlConfig + manual dispatch) collapses to ~5–10 lines of
  declarative verb tree. Consistent `--help` format across the
  toolchain; typed positional-arg validation for free.
- **SmbusHcShim** extended with `smb_byte_read` / `smb_byte_write`
  (ICH9 SMBus PROT_BYTE_DATA). Required for AxlSpd over the I2C
  Master path on QEMU.
- **`axl_smbus_write_block`** now rejects `len == 0` (was a hole
  the I2C path could route as a byte write under the new shim).

### Removed

- **AxlConfig CLI surface** — `axl_config_parse_args`,
  `axl_config_pos`, `axl_config_pos_count`, `axl_config_usage`,
  and the `short_flag` field in `AxlConfigDesc`. AxlConfig now
  has a focused purpose: live object property bag with typed
  accessors, auto-apply via offsetof, and dynamic-key callbacks
  (used by AxlHttpClient/Server). The dual-purpose "config or CLI
  parser?" confusion is gone. External consumer migration:
  `aximcode/axl-webfs` cmd-serve + cmd-mount — committed in that
  repository separately.
- `sdk/examples/config-demo.c` — only demonstrated the retired
  CLI surface. Removed.

### Fixed

- **AxlSmbus I2C path** zero-length block-write was misroutable as
  byte-write under SmbusHcShim's byte-op extension; now rejected
  at the public API layer.
- **DDR5 SPD** decoder was masking the ECC-presence bit with
  `& 0x07`, allowing dual-channel non-ECC modules' channel-count
  bits to spoof ECC=true. Fixed to `& 0x03` (caught by independent
  code-review pass before AxlSpd commit).
- **AxlArgs S64** initial impl reproduced the v0.5.0 INT64_MIN
  signed-overflow UB by hand-rolling negate-after-strip-minus.
  Replaced with `axl_str_to_s64` delegation (no live exposure —
  no tool currently uses S64 — but the bug was in new code).
- Several minor doc / lifetime-contract clarifications to the new
  helpers and AxlArgs surface, all caught by the independent
  review passes.

### Test ratchet

1842 → 1905 on both x86_64 and AArch64 (+63 across R+1..R+4 + the
tool-helpers + AxlArgs framework + offsetting -13 retired
test_config_args). All 11 tools have direct integration coverage
(`test-tools.sh`, `test-net-tools.sh`, `test-redfish.sh`,
`test-spd-qemu.sh`, `test-ipmi-{qemu,ssif-qemu}.sh`).

## 0.6.1 — 2026-04-30

### Added

- **`axl-sdk-host-tools.rpm`** — RPM artifact for the host-tools
  package (run-qemu.sh + helpers). Was a v0.2.9 oversight; the SDK
  package always shipped both `.deb` and `.rpm` but host-tools only
  shipped `.deb` and `.tar.gz`. Now `axl-sdk-host-tools.rpm` is on
  the release page next to its `.deb` counterpart, with stable URL
  `…/releases/latest/download/axl-sdk-host-tools.rpm`. Both
  packages are noarch (the host-tools payload is shell + python
  scripts + linker scripts + C source, no pre-compiled binaries).
  Fedora/RHEL deps tracked separately from Debian: `qemu-system-x86`,
  `qemu-system-aarch64`, `edk2-ovmf`, `edk2-aarch64`, `virtiofsd`,
  `mtools`, `dosfstools`.

## 0.6.0 — 2026-04-30

Network tools (`netinfo`, `fetch`, `rfbrowse`) now Just Work on
minimal firmware that doesn't ship a NIC SNP driver — the tools
tarball ships a universal NIC driver bundle and the SDK auto-loads
it on demand. Motivating real-user case: a 2010-era Dell EDK1
firmware that lacks UEFI 2.6+ NIC drivers entirely.

See [`docs/AXL-Network-Driver-Bundle-Design.md`](docs/AXL-Network-Driver-Bundle-Design.md)
for the full design + four-test validation matrix.

### Added

- **`drivers/<arch>/` directory in `axl-sdk-tools-{x64,aa64}.tar.gz`** —
  a self-contained driver bundle that consumers extract alongside
  the tools `.efi` files onto a FAT USB stick:
  - `ipxe-all.efidrv` — universal NIC driver built from upstream
    iPXE at a pinned commit (currently `df4eec8c`). Single ~1.1 MB
    blob covering Intel (e1000 / e1000e / i219 / i225), Broadcom
    (BCM4401 / 5760x / 957454), Realtek PCI/USB (RTL8139 / 8169 /
    8125 / 8153 USB), Atheros, 3Com, AMD, USB CDC-ECM/NCM/RNDIS,
    AX88179/178a, SMSC75xx/95xx — ~2.9k chip IDs total. Built
    fresh in CI by [`scripts/build-ipxe.sh`](scripts/build-ipxe.sh);
    GPL-2.0-or-later, attribution + GPL §3(b) written offer in
    `third_party/ipxe/README.md`.
  - `RamDiskDxe.efi` (also embedded in `mkrd.efi` since v0.5.2).
  - A few small auxiliary EDK2 USB-network drivers for firmware
    that benefits from them. See `third_party/edk2/README.md` for
    details.
- **[`scripts/build-ipxe.sh`](scripts/build-ipxe.sh)** — clones iPXE
  at the pinned commit and builds the universal `.efidrv` for one
  or both architectures. Reproducible build (~35s on 16 threads);
  prints upstream URL+SHA at the end for source-availability
  documentation.
- **[`scripts/run-qemu.sh`](scripts/run-qemu.sh) flags** for testing
  driver-bundle coverage:
  - `--nic-model MODEL` — choose the QEMU NIC type (virtio-net-pci,
    e1000, e1000e, rtl8139, pcnet, ne2k_pci, ...). Implies `--net`.
  - `--nic-no-rom` — suppress QEMU's bundled iPXE PXE option ROM
    (passes `romfile=`). Required for "firmware lacks NIC driver"
    tests; without it OVMF wraps the option-ROM-provided UNDI as
    SNP and hides the gap.
  - `--extra SRC:DEST` — relative-path staging of additional files
    into the boot disk image (was previously root-only). Lets tests
    drop drivers under `drivers/<arch>/...` to exercise the
    canonical search path.
- **`netinfo -v`** prints a "NIC Drivers" section identifying the
  driver image bound to each SNP handle, walking the
  `NII3.1 → NII (legacy) → SNP` fallback chain. Surfaces the
  actual NIC-binding driver (e.g.
  `\drivers\x64\ipxe-all.efidrv`) instead of just the SnpDxe
  wrapper above it.

### Fixed

- **`axl_driver_load(path)` now uses DevicePath, not memory buffer.**
  The previous implementation read the .efi file into memory and
  called `gBS->LoadImage(SourceBuffer=...)`, which leaves
  `LoadedImage->FilePath = NULL`. iPXE's UEFI driver-binding entry
  reads `FilePath` to locate its install directory and bails with
  `EFI_INVALID_PARAMETER` from `StartImage` when it's NULL.
  `axl_driver_load` now constructs a `<volume DP> +
  MEDIA_FILEPATH_DP` device path (via the new
  `driver_build_file_dp` helper) and calls
  `LoadImage(DevicePath=that, SourceBuffer=NULL)` — matching what
  UEFI Shell's `load` command does. Memory-buffer load is preserved
  as a fallback for drivers that don't read `FilePath` (e.g.
  `RamDiskDxe`).
- **`axl_driver_connect(NULL)` actually does something now.**
  UEFI's `gBS->ConnectController(NULL, NULL, NULL, TRUE)` returns
  `EFI_INVALID_PARAMETER` per spec; the old implementation called
  it directly and silently succeeded as a no-op. Now enumerates
  every handle via `LocateHandleBuffer(AllHandles)` and per-handle
  `ConnectController`, mirroring UEFI Shell's `connect -r`.
- **`axl_net_ensure_drivers` candidate-list reordered** so
  `ipxe-all.efidrv` is tried first; existing names retained as
  back-compat for users with their own staged drivers.

### Validated

- 1695/1695 unit tests pass on x64 and aa64 (DEBUG build).
- 23/23 tool integration tests pass.
- End-to-end: `--nic-model {e1000, e1000e, rtl8139, pcnet}
  --nic-no-rom` (firmware lacks NIC driver) → tarball-staged
  `ipxe-all.efidrv` self-loads via `axl_net_ensure_drivers` →
  iPXE binds NII → SnpDxe wraps as SNP → DHCP → HTTP 200.
  Consumer-flow proven: extract release tarball, run
  `fetch.efi http://...`, no env overrides, leak-clean exit.

## 0.5.3 — 2026-04-29

Fixes a regression introduced in v0.2.9 where `--mount` (and other
features that depend on QEMU-bundled firmware) silently fell through
to system OVMF/AAVMF on hosts that have both a system QEMU and a
custom QEMU build with bundled firmware.

### Fixed

- **`QEMU_DIR` not propagating from `find_qemu` to `find_firmware`** —
  v0.2.9 (commit `ae3c3a6`) removed the top-level
  `QEMU_DIR="${QEMU_DIR:-$HOME/projects/qemu/install/bin}"` default
  in `scripts/axl-common.sh` and moved that resolution inside
  `find_qemu()`. But `find_qemu` is called via `QEMU_BIN=$(find_qemu
  "$ARCH")` — the `$()` runs it in a subshell, and its
  `export QEMU_DIR=...` dies with the subshell. `find_firmware` in
  the parent then sees `QEMU_DIR=""`, computes
  `qemu_share="/share/qemu"`, fails the QEMU-bundled firmware lookup,
  and falls through to system OVMF/AAVMF. Symptoms: `--mount`
  produces no `fs1:` (system OVMF lacks `VirtioFsDxe`); aa64 unit
  tests silently produce 0 results when the system aa64 QEMU lacks
  slirp networking. Fix: re-derive and export `QEMU_DIR` in the
  parent shell after the `$()` call in
  [`scripts/run-qemu.sh`](scripts/run-qemu.sh) and
  [`test/integration/common-test.sh`](test/integration/common-test.sh).
  Idempotent on user-pre-set `QEMU_DIR`. Two call sites; no other
  `$(find_qemu)` invocations exist in the tree.

## 0.5.2 — 2026-04-29

`mkrd.efi` now ships as a self-contained binary that runs on minimal
firmware that omits the optional UEFI 2.6 `EFI_RAM_DISK_PROTOCOL`
(observed in the wild on a 2010-era Dell EDK1 firmware). Stock EDK2
`RamDiskDxe.efi` is embedded at build time and `LoadImage`-from-memory'd
at runtime if `LocateProtocol` and the on-disk search both miss.

### Added

- **`axl_driver_ensure_with_embedded()`** — generalization of
  `axl_driver_ensure()` for tools that bake a driver blob into their
  own `.efi`. Resolution order: `LocateProtocol` short-circuit →
  `--driver`-style override (caller-provided name, disk-only) →
  canonical disk search → `LoadImage` from embedded buffer. The old
  `axl_driver_ensure(g, n)` is now a thin wrapper passing
  `embedded_buf=NULL, override_name=NULL`. See
  [`include/axl/axl-driver.h`](include/axl/axl-driver.h).
- **`mkrd --driver <name>`** — override the embedded RamDiskDxe and
  search disk for the named driver instead. When this flag is set,
  the embedded fallback is intentionally disabled — caller explicitly
  opted into a specific external driver. Useful for testing patched
  or vendor-specific RAM-disk drivers.

### Changed

- **`mkrd.efi` is now self-contained.** Embedded
  [`third_party/edk2/RamDiskDxe-{x64,aa64}.efi`](third_party/edk2/)
  (BSD-2-Clause-Patent, stock EDK2 `MdeModulePkg`) — adds ~33 KB
  to the x64 binary and ~41 KB to aa64. Goes into `.rodata`. Most
  OEM firmware ships the protocol baked-in so the embedded blob is
  never executed; it's purely a safety net for minimal firmware.
- **Tools tarballs and SDK packages carry the new
  `third_party/edk2/{LICENSE,README.md}`** — attribution is required
  for binary-form redistribution under BSD-2-Clause-Patent §2.
  Apache-2.0 §4(a) compliance for the SDK package preserved.

## 0.5.1 — 2026-04-29

Cross-compile fix for installed packages on RHEL-family hosts.

### Fixed

- **aa64 cross-build under installed `axl-cc` on AlmaLinux/RHEL** —
  the v0.5.0 package staged headers directly under `<prefix>/include/`,
  so on a system install (`PREFIX=/usr`) `axl-cc` compiled with
  `-I /usr/include`. Host glibc's `stdint.h` then shadowed cross-gcc's
  freestanding `stdint.h`, chaining into `gnu/stubs-32.h`, which
  RHEL/Alma don't ship — failing every `--arch aa64` build with
  "gnu/stubs-32.h: No such file or directory". Ubuntu only masked
  this because `gcc-multilib` happens to ship the 32-bit stubs.

### Changed

- **Installed header layout namespaced under `axl-sdk/`** —
  package now stages headers at `<prefix>/include/axl-sdk/{axl.h,
  axl/, uefi/}` instead of `<prefix>/include/`. `axl-cc`,
  `axl-config.cmake`, and `axl.pc` all point at the namespaced
  subdirectory, so `/usr/include` itself never appears in the
  compiler command line and cross-gcc's freestanding headers win
  via the normal built-in search order. User code (`#include <axl.h>`,
  `#include <axl/axl-mem.h>`) is unchanged. Dev-tree library build
  unchanged — the namespacing is install-time only.
- **`-I` → `-isystem` for the SDK include path** — secondary
  benefit: SDK headers are now treated as system headers, so
  warnings from them don't bubble up into user-app builds.

## 0.5.0 — 2026-04-29

String parsing companion to `AxlString` (the builder). Closes the
"every consumer hand-rolls the same length-guard plumbing" gap one
level up from the v0.4.0 SMBIOS spec-table decoders.

### Added

- **`AxlStrReader`** — cursor-based string parser. Borrows a
  `const char *`, tracks a position + sticky-error flag. Operations
  short-circuit when `ok` is false, so parse chains compose without
  per-call error checking:

  ```c
  AxlStrReader r;
  uint64_t v;
  axl_str_reader_init(&r, "N[03A8]");
  axl_str_reader_consume_char(&r, 'N');
  axl_str_reader_consume_char(&r, '[');
  axl_str_reader_take_u64(&r, 16, &v);
  axl_str_reader_consume_char(&r, ']');
  if (!r.ok || !axl_str_reader_eof(&r)) { /* parse failed */ }
  ```

  Twelve primitives covering init, eof/peek/remaining,
  skip-whitespace, consume-char/literal-string, take-until-delim,
  take-while-pred, take-u64 (auto-detects `0x` prefix or takes an
  explicit base), and take-ident (`[A-Za-z_][A-Za-z0-9_]*`). No
  allocation. Header docs live alongside `AxlString` in
  `axl-str.h`.

- **`axl_sscanf` / `axl_vsscanf`** — printf's symmetric partner,
  built on `AxlStrReader`. Supports a useful subset of C99 sscanf:
  `%c %d %i %u %o %x %X %s (with width) %[set] %% %n`, length
  modifiers `hh h l ll z j`, `*` assignment suppression, and width
  specifiers. Width is required for unsuppressed `%s` so the
  destination buffer is bounded. Returns the count of stored
  conversions or -1 on a malformed format.

  ```c
  unsigned a, b, c, d;
  int n = axl_sscanf("192.168.1.42", "%u.%u.%u.%u", &a, &b, &c, &d);
  /* n == 4 */
  ```

### Changed (dogfood)

- **`axl_strtou64_with_offset`** rewritten on top of `AxlStrReader`.
  Behavior unchanged — all 23 existing test cases still pass — but
  the implementation is now ~14 lines of cursor calls vs. the
  previous 40 lines of hand-rolled `endptr` plumbing.

- **`axl_ipv4_parse`** rewritten on top of `axl_sscanf`. Replaces a
  ~30-line hand-rolled state machine (digit accumulation, dot/NUL
  switching, octet count tracking) with a 4-line scanf call plus
  range checks. The trailing `%n` captures bytes consumed so we
  reject trailing garbage like "1.2.3.4junk" without an extra
  strlen.

  ```c
  unsigned int a, b, c, d;
  int consumed;
  int n = axl_sscanf(str, "%u.%u.%u.%u%n", &a, &b, &c, &d, &consumed);
  if (n != 4 || str[consumed] != '\0') return -1;
  if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
  ```

### Test stats

1693 unit tests passing (was 1607; 86 new across `AxlStrReader`
and `axl_sscanf`, plus 0 regressions on the dogfooded
consumers).

## 0.4.0 — 2026-04-29

Second consumer-driven release (cat 1 / 1.5 SMBIOS landed; this round
is decoder-table consolidation). Pulls four pieces of SMBIOS spec-
table machinery upstream so future spec-fixes propagate to every
consumer via an SDK bump rather than a manual backport. The motivating
fixes match recent OEM diagnostic-tool slot-type updates:
- `0c558a930` (2024-09-25) — OCP NIC SFF/LFF + EDSFF E1.S/E1.L + E3.S/E3.L
- `aab01c48d` / `569491b6c` (2025-03-27) — SMBIOS slot type 0x25 is Gen 5,
  not Gen 4

### Added

- **`AxlSmbiosBaseboardInfo.board_type` field** + `AXL_SMBIOS_BOARD_TYPE_*`
  enum — exposes the BoardType byte at offset 0x0D of Type 2. The
  canonical "is this a server blade?" detector is `board_type == 3`,
  NOT Type 3 chassis 0x1C/0x1D (which Dell BIOS doesn't reliably set
  — see `ADDF/Libs/SAL/AddfSAL.cpp:fIsBladeSmbios`). 0 if not
  published (rare; field has been part of Type 2 since spec 2.0).
- **`axl_smbios_slot_type_str` / `axl_smbios_slot_width_str` /
  `axl_smbios_slot_usage_str`** — Type 9 spec-value decoders. Pure
  table lookups, no allocation, return a static const string or
  NULL for unknowns (caller can fall back to raw "0x%02X").
  Values match SMBIOS 3.7 spec / EDK2's `MISC_SLOT_TYPE` enum.
  `slot_type_str` covers PCIe Gen 1..6 (`0xA5`-`0xC4`), M.2 Keys
  A/E/B/M (`0x14`-`0x17`), PCIe Mini variants (`0x21`-`0x23`),
  PCIe SFF-8639 / U.2 family Gen 2-5 (`0x1F`/`0x20`/`0x24`/`0x25`),
  OCP NIC 3.0 SFF/LFF / Prior to 3.0 (`0x26`-`0x28`), and EDSFF
  E1/E3 form factors (`0xC5`/`0xC6`). `slot_usage_str` renders
  0x05 as "CPU NOT INSTALLED" — Dell convention from
  `dowin/Init.cpp:3833` + `SmBioslib.h:391`, what every consumer's
  scripts grep for. **Note**: in-the-wild OEM diagnostic-tool
  slot-type tables have been observed with values shifted by 4
  from the spec (PCIe at `0xA1` instead of `0xA5`) and PCIe-Mini /
  U.2 codes (`0x22`-`0x25`) labelled as M.2 keys. This release uses
  the canonical spec values; downstream consumers that switch from
  a local decoder to `axl_smbios_slot_type_str` will see corrected
  decoding for any slot whose firmware reports a value the local
  table got wrong.
- **`axl_smbios_strings_byte_len(hdr)`** — byte length of the
  inline strings region between the formatted area and the spec's
  end-of-region double-NUL. Useful for "raw record" dumps that
  need to know the full record span on disk including its strings
  (e.g. dowin's `fDumpSmbios` PIMS-381674 fix). Bounded against
  the SMBIOS table memory range so a malformed record without a
  terminator can't run past the end.
- **`AxlSmbiosChassisClass` enum + `axl_smbios_chassis_class(type)`** —
  pure-spec interpretation of the Type 3 chassis byte into
  `DESKTOP` / `NOTEBOOK` / `SERVER` / `EMBEDDED` / `OTHER` /
  `UNKNOWN` buckets. Strips the 0x80 lock bit before classifying.
  Vendor-specific overrides (sysId tables, PCI audio-device
  probes, etc.) live in consumer code. Bucket assignments
  match SMBIOS spec Table 17 +
  `ADDF/Libs/SAL/AddfSAL.cpp:fIsNotebookSmbios`. Pitfalls
  defended in tests:
    - 0x18 ("Sealed-case PC") is **DESKTOP**, not server.
    - 0x23 (Dell convention "Mongoose Mini PC") is **EMBEDDED**.

### Test stats

1598 unit tests passing (was 1555; 43 new across the four
additions, including explicit pitfall coverage for the 0x18 and
0x23 cases the consumer flagged).

## 0.3.1 — 2026-04-29

CI fix on top of v0.3.0 (cut the same day). v0.3.0's release artifacts
are functional — the bug only triggers on `argv == NULL`, which no
real caller does — but the CI green-bar matters for consumers
pinning against a tag.

### Fixed

- **`axl_subcommand_dispatch` clang-tidy null-deref** —
  the early-help check `if (argc < 2 || argv[1] == NULL)` would
  dereference a NULL `argv` when called with `argc >= 2 && argv == NULL`
  (a technically-valid but unlikely shape that test harnesses
  occasionally exercise). Add an explicit `argv == NULL` guard
  short-circuiting the array access. CI was running clang-tidy
  with `-warnings-as-errors='*'`.

## 0.3.0 — 2026-04-29

Consumer-driven release for a downstream hardware-diagnostic CLI
port. Closes the SMBIOS audit gap on Types 8/9/11/16/19/20/41,
adds a multi-command CLI dispatch helper, and standardizes the
hex+offset parser the consumer's categories 3-5 share.

### Added

- **Seven new typed SMBIOS readers** matching the existing pattern
  (`axl_smbios_read_*` returns 0 on success, -1 on NULL/wrong-type/
  too-short record):
    - `axl_smbios_read_port_connector` (Type 8)
    - `axl_smbios_read_system_slot` (Type 9) — length-aware across
      spec-version creep: SegmentGroup/Bus/DeviceFunc (2.6+),
      DataBusWidthBase + PeerGroupingCount (3.2+), all with documented
      `0xFFFF` / `0xFF` / `0` "not published" sentinels
    - `axl_smbios_read_oem_strings` (Type 11) — array-of-pointers
      shape capped at 16 entries (matches existing typed-reader idiom)
    - `axl_smbios_read_physical_memory_array` (Type 16) — resolves
      the 32→64-bit `MaxCapacity` / `ExtendedMaxCapacity` fallback
      automatically
    - `axl_smbios_read_memory_array_map` (Type 19) — same 64-bit
      address fallback story
    - `axl_smbios_read_memory_device_map` (Type 20) — same again
    - `axl_smbios_read_onboard_device_ext` (Type 41)
- **`AXL_SMBIOS_TYPE_*` enum additions** — Type 20
  (`MEMORY_DEVICE_MAP`), Type 41 (`ONBOARD_DEVICE_EXT`), plus a
  `PHYSICAL_MEMORY_ARRAY` alias for the existing Type-16 enum so
  callers can use the spec's full name.
- **`axl_smbios_copy_string_utf8(hdr, idx, buf, buf_size)`** —
  reentrant, length-bounded alternative to
  `axl_smbios_get_string_utf8`. Truncates safely on overflow,
  always NUL-terminates if `buf_size > 0`, returns the byte count
  written. The original `axl_smbios_get_string_utf8` was already
  reentrant after the v0.2.5 string-area refactor — its docstring
  is updated to match.
- **`<axl/axl-subcommand.h>`** — multi-command CLI dispatch helper.
  Caller-owned `AxlSubcommand` table + a single
  `axl_subcommand_dispatch(table, count, argc, argv, prog_name)`
  call covers `<prog>`, `<prog> help`, `<prog> help <cmd>`,
  `<prog> -h`/`--help`, `<prog> <cmd> ...`, "did you mean" typo
  suggestions via edit-distance matching, and shifted argv so
  subcommands see their own name as `argv[0]`. Pairs with
  `axl_config_*` for per-command flag parsing — `mkrd` is the
  "single-purpose" pattern, `do` will be the "multi-command"
  pattern.
- **`axl_strtou64_with_offset(s, &out)`** — hex/decimal value with
  an optional `+offset` suffix. Accepts `"0x100"`, `"256"`,
  `"0x100+0x10"`, `"256+16"`. Strict: trailing garbage, whitespace
  around `+`, dangling `+`, and overflow on the sum all return -1.
  Standardizes the `crb tag+offset reg` / `rb physAddr+offset
  count` parsing across the downstream consumer's categories 3-5.

### Changed

- `axl_smbios_get_string_utf8` documentation now correctly describes
  it as reentrant. (The v0.2.5 refactor switched it from a static
  buffer to a direct pointer into the SMBIOS table — the docstring
  hadn't caught up.)
- `src/smbios/README.md` rewritten: added the typed-reader table,
  split the string-accessor section into "direct pointer vs
  caller-buffer" guidance, removed the stale "static 128-byte
  buffer" / "future axl_smbios_next" notes that the v0.2.5
  refactor and earlier releases had already obsoleted.
- `docs/AXL-Coding-Style.md` adds a "CLI Patterns" section
  documenting single-purpose vs multi-command tool shapes.

### Test stats

1552 unit tests passing (was 1488; 64 new across SMBIOS, subcommand,
and `strtou64_with_offset`).

## 0.2.9 — 2026-04-28

Downstream-consumer release. The motivating case is a downstream
util project that needs `run-qemu.sh` and the host-side helpers
on machines that have system QEMU/OVMF installed via the package
manager but can't `git clone` the SDK source (corporate MITM
proxies break TLS verification on `git clone`; a pinned
`curl` + `sha256sum` doesn't). Four changes:

### Added

- **`axl-sdk-host-tools.tar.gz` and `axl-sdk-host-tools.deb`
  release artifacts** — flat tarball plus an installable .deb
  carrying just the host-side runtime tooling: `run-qemu.sh`,
  `axl-common.sh`, the ELF/PE linker scripts, `gdb-syms.py`,
  `pe-set-debug.c`, `rsod-decode.py`, and `uefi-manifest.json5`.
  The .deb declares `Depends: qemu-system-x86 qemu-system-arm
  ovmf qemu-efi-aarch64 virtiofsd mtools dosfstools` so a
  single `sudo apt install ./axl-sdk-host-tools.deb` brings the
  whole pipeline. Drops scripts to `/usr/share/axl-sdk-host-tools/scripts/`
  with an `/usr/bin/run-qemu` wrapper on PATH. The tarball
  ships scripts at `scripts/` for unprivileged extraction
  anywhere. CI smoke-tests the .deb install end-to-end.

### Changed

- **`scripts/axl-common.sh` discovery is now a 3-tier search.**
  Previously `QEMU_DIR` defaulted to `$HOME/projects/qemu/install/bin`,
  which broke for downstream consumers without that custom
  build tree. New order: (1) explicit `$QEMU_DIR` override,
  (2) `command -v qemu-system-*` on `$PATH` (system install),
  (3) the legacy `$HOME/projects/qemu/install/bin` path as
  last-resort fallback. `MKIMAGE_DIR` lost its default
  similarly — left unset means the script falls through to
  the mtools recipe (already the working default for
  consumers without the AXL mkimage tree). Power-user
  override semantics preserved.

- **Actionable error messages from `find_qemu` / `find_firmware`.**
  Missing dependencies now print the apt/dnf/pacman/brew
  install one-liners users can copy-paste, plus the env-var
  override hint, instead of a one-line "not found" log:

  ```
  [ERROR] qemu-system-x86_64 not found in $PATH or any known location.

    Install:
      Debian/Ubuntu:  sudo apt install qemu-system-x86 qemu-system-arm \
                                       ovmf qemu-efi-aarch64 \
                                       virtiofsd mtools dosfstools
      Fedora/RHEL:    sudo dnf install qemu-system-x86 ...
      Arch:           sudo pacman -S qemu-system-x86 ...
      macOS:          brew install qemu

    Or set QEMU_DIR=/path/to/your/qemu/install/bin
  ```

  Same shape for missing OVMF / AAVMF.

- **README install path no longer leads with `git clone`.** The
  binary `.deb`/`.rpm` is now the recommended consumer path;
  source builds are documented as the power-user option further
  down. Adds the new host-tools tarball/.deb to the install
  matrix.

## 0.2.8 — 2026-04-28

Two `scripts/run-qemu.sh` UX features for interactive UEFI app
testing: `-i`/`--interactive` to hand the host TTY to the guest,
and `--mount DIR` to expose a host directory as a virtiofs volume
the UEFI shell sees as `fsN:`. Together they enable a
"`./scripts/run-qemu.sh -i --mount ~/efi-apps`" loop where you
build apps on the host and run them straight from the UEFI shell
without rebuilding the disk image each iteration.

### Added

- **`run-qemu.sh --interactive` (`-i`)** — hands the host TTY to
  QEMU so keystrokes reach the guest. Disables the timeout, the
  CPU-spike sampler, and the ANSI-stripping post-filter (the guest
  may legitimately emit cursor moves and colors). Mutually
  exclusive with `--background` and `--screenshot`; composes
  cleanly with `--gdb` (separate TCP port) and with
  `--serial-log` (transcript captured via QEMU's chardev
  `logfile=`). Auto-prints the Ctrl-A C / Ctrl-A X escape hints
  on startup and installs an `EXIT`/`INT`/`TERM` trap that runs
  `stty sane` so an abnormal QEMU exit doesn't leave the parent
  shell in raw mode. Also skips the auto-`reset -s` in the
  generated `startup.nsh` so users land back at the UEFI shell
  after the app exits.
- **Bare-shell mode** — `run-qemu.sh -i` with no `.efi` argument
  boots OVMF straight into the UEFI Shell. Pairs with `--mount`
  for a "drop me into a UEFI shell with my host filesystem"
  workflow.
- **`run-qemu.sh --mount DIR[:TAG]`** — exposes a host directory
  to the guest as a virtiofs volume (default tag `hostfs`).
  Spawns `virtiofsd` as a child, wires the QEMU `vhost-user-fs-pci`
  device with a `memory-backend-file,share=on` over `/dev/shm`,
  and adds `map -r` to `startup.nsh` so the volume shows up as
  `fsN:` from the UEFI shell. Validates virtiofsd availability
  and `/dev/shm` writability up-front with actionable error
  messages (Fedora/Debian/Arch install hints; `VIRTIOFSD=` env
  override). Opportunistically stages a sibling
  `VirtioFsDxe.efi` from the EDK2 build alongside the firmware
  if found — works against OVMF builds that don't have the
  driver baked in. Composes with `--interactive`, `--gdb`,
  `--background`, and an `.efi` arg (host-fs available alongside
  the staged FAT). Trap kills virtiofsd on exit (foreground)
  or emits `VIRTIOFSD_PID=` for the caller (background). Both
  X64 and AARCH64 supported.
- **`test/integration/test-run-qemu-flags.sh`** — host-only
  argument-parsing tests for `run-qemu.sh` (syntax, `--help`,
  mutual-exclusion guards, missing-file guard, `--mount`
  validation).
- **Alternate-screen buffer in `--interactive`** — the script
  now emits `\e[?1049h` on stderr before exec'ing QEMU and
  `\e[?1049l` on exit. UEFI's Terminal driver uses absolute
  cursor positioning against an assumed 80x25 grid; against
  larger SSH PTYs (MacBook Terminal/iTerm/WSL) it produced
  artifacts where the shell prompt rendered mid-`ls` output.
  Alt-screen gives UEFI a fresh canvas with no scrollback
  interaction (same trick vim/less/htop use). Skipped when
  stderr isn't a TTY so escape sequences don't pollute log
  files. Pairs with the `mode <stty cols> <stty rows>` line
  the script already emits in startup.nsh.

## 0.2.7 — 2026-04-28

Polish release on top of 0.2.6 (cut earlier the same day). Two
real changes — a CI-side test fix and a new diagnostic in
`run-qemu.sh` — alongside a substantial documentation refresh
that came out of reviewing the CRT0/runtime conflation in the
docs corpus.

### Added

- **CPU-spike sidecar in `run-qemu.sh`** — samples QEMU's host
  CPU at 5 Hz after a firmware-boot warm-up window and prints a
  `WARN: CPU spike` line on stderr if the process sustains
  ≥1.5 cores for ≥2 s. Default-on, silent on healthy runs.
  Catches the orphan-QEMU-pegging-100%-CPU class of regression
  that bit us mid-session in 0.2.6's prep. Tunable via
  `--cpu-threshold N` / `--cpu-sustain SECS`; opt out with
  `--no-cpu-warn`. CI now smoke-tests it against
  `AxlTestCpuIdle.efi` to catch false-positives.
- **`docs/RELEASING.md`** — step-by-step release-cutting
  playbook (prereqs, `bump-version.sh`, CHANGELOG dating,
  branch-then-tag push order, watching workflows via
  `gh run watch`, recovery from a failed tag). Also surfaces in
  the Sphinx Guides toctree.

### Fixed

- **`/ttl-short` test was racing against UEFI's 1-second clock
  granularity.** The async-close speedup in 0.2.6 made both
  `/ttl-short` requests land in the same wall-clock second, so
  `now - timestamp_ms = 0` and the cache-hit path took it
  before the 150 ms TTL could expire. Bump TTL to 1500 ms and
  the inter-request sleep to 2 s so the test exercises a real
  expiry; comment on the constraint in the test source so the
  next person who tries to tune it down sees the reason.

### Documentation

- **Renamed `AXL-Runtime.md` → `AXL-Lifecycle.md`** and swept
  the corpus for the CRT0-vs-runtime conflation. CRT0 is the
  ~17-line entry stub; the AXL runtime is the lifecycle library
  in `src/runtime/`. The rename + sweep covers ~30 sites
  (design docs, module READMEs, header doc-comments, examples,
  ROADMAP, sphinx pages). The `git mv` keeps blame.
- **Audience framing for Linux systems C developers** — README,
  `AXL-Design.md`, `AXL-SDK-Design.md`, `AXL-Coding-Style.md`,
  and the `axl.h` / `axl-loop.h` umbrella headers now lead with
  the audience AXL targets (glibc / GLib / systemd / libcurl
  developers) rather than the inspiration. Includes an explicit
  GLib-to-AXL mapping table (`GMainLoop` → `AxlLoop`,
  `GHashTable` → `AxlHashTable`, etc.).
- **"How AXL avoids the EDK2 dependency"** — README and design
  docs now explain that `EFI_*` types are auto-generated from
  the published UEFI 2.x and PI 1.x specifications via
  `scripts/generate-uefi-headers.py` + `uefi-manifest.json5`.
  Spec drift is a manifest update + regeneration, not a vendor
  merge. Replaces the stale "EDK2 is invisible" phrasing that
  implied EDK2 was hidden rather than absent.
- **`AXL-Porting-Guide.md` picks up the v0.2.6 surface** —
  modern entry-point pattern (`int main(argc, argv)` with no
  `AXL_APP` macro), a new Step 1.5 covering `axl_net_auto_init`
  / `axl_driver_ensure` / `axl_tcp_close` lifetime for porters
  with networking or driver-needing apps, and the
  `run-qemu.sh --net --hostfwd` shape for testing. Adds seven
  EDK2→AXL mappings to the "What AXL Already Covers" table
  (net auto-init, driver self-load, signal, atexit, default
  loop, yield).
- **Sphinx version auto-syncs from `VERSION`.** `conf.py` reads
  the canonical file at build time; `index.rst` uses the
  `|release|` substitution. axl.aximcode.com's "Version" cell
  now updates automatically on every tagged release.
- **Sphinx full-width override** widens `.wy-nav-content` to
  1200 px so the docs site no longer leaves grey gutters on
  wide monitors.

### Changed

- **`run-qemu.sh` documents `--no-cpu-warn` / `--cpu-threshold`
  / `--cpu-sustain` flags** in `--help` and the file header.

## 0.2.6 — 2026-04-28

Networking-focused release. Headline is a TCP-close lifecycle rework
(heap-owned close token + async-finalize on the running loop) that
fixed three classes of bug at once: a UAF in `axl_tcp_close` that
corrupted nested-loop frames (the Reg A `/client-test` hang), a
busy-wait per active close that pegged the CPU during curl-storm
load, and a FIN drop on `Configure(NULL)` when the bounded close
wait expired before the firmware finished. Plus debugging tooling
(GDB-into-QEMU), CI gains (test-http and test-tcp-echo now run on
every push), debug info in release builds, refreshed audience
framing for Linux systems C developers, and several supporting
fixes that surfaced along the way.

### Added

- **GDB-into-QEMU debugging.** `scripts/run-qemu.sh --gdb [PORT]`
  exposes the QEMU GDB stub; `--debugcon FILE` captures OVMF's
  `Loading driver at 0x... NAME.efi` lines. `scripts/gdb-syms.py`
  consumes the debugcon log and emits `add-symbol-file` directives
  ready to paste into `gdb`. Documented end-to-end in
  `docs/DEBUGGING.md`. Used to bisect the close-event hangs and
  the CoreCheckEvent `#PF` (`docs/DEBUGGING.md § Worked example`).
- **`test/integration/test-tcp-echo.sh`** — minimal TCP-only
  integration test (15 sequential connect/echo/close probes against
  `sdk/examples/tcp-echo-server.efi`). Asserts every probe echoes,
  guest connect/disconnect counts match, and the host TCP table is
  clean after the storm — narrow signal for FIN-delivery and
  close-token regressions that test-http would only show as flake.
  Now part of the CI integration job.
- **`axl_net_ensure_drivers`** — drives auto-load of TCP4 / DNS4
  / IP4 service bindings under `NetInfo`, `Fetch`, `RfBrowse` so
  the tools work on bare-metal images that haven't pre-`connect`'d
  the network stack. Mirrors the `axl_driver_locate` pattern
  introduced for `MkRd` in v0.2.3.

### Fixed

- **`axl_tcp_close` no longer corrupts caller stack frames.** The
  close token is now heap-allocated; EDK2 holds its pointer past
  TIME_WAIT, and the previous stack-allocated token UAF'd whenever
  TIME_WAIT outlived the bounded wait. Manifested as the flaky
  `/client-test` handler hang ("Reg A") in test-http.
- **No CPU spin per active close.** Close registers its
  completion event on the caller's running event loop and finalizes
  asynchronously when SockConnClosed fires; the per-close
  `_axl_tcp_wait` is now a fallback used only outside a running
  loop (CLI tools, shutdown after `axl_loop_run` returned).
  test-http wall time fell ~94 s → ~21 s on the same hardware.
- **`Configure(NULL)` no longer drops the queued FIN** on active
  close. Configure-NULL runs from the close-event finalize callback
  after the firmware has fully completed Close, so `TcpFlushPcb`'s
  buffer flush has nothing left to send.
- **Synchronous `Receive()` errors and re-arm errors get delivered
  through the loop**, not silently swallowed. EDK2 returns
  `EFI_CONNECTION_FIN` synchronously when `SockNoMoreData` ran with
  an empty token list before the user re-armed; without delivery
  the server never observed EOF, never closed, and peers hung in
  FIN-WAIT-2.
- **Loop dispatch epilogue skips slot-reuse stomp.** A callback that
  removed its own source and registered a new one in the same slot
  used to have its new occupant deactivated by the dispatch
  epilogue. The dispatcher now snapshots `src->id` before the
  callback and skips epilogue if the slot was re-issued.
- **Loop event-array off-by-one.** `event_array[AXL_MAX_SOURCES + 2]`
  was sized for two sentinels but `axl_loop_next_event` appended a
  third (intrinsic keypress, added in 0.2.5). Wrote one slot past
  the stack array — surfaced under load as a stale `AxlEventHandle`
  the next iteration handed to `gBS->CheckEvent` (`#PF` in
  `CoreCheckEvent`). Fixed; size is now `+3`.
- **HTTP response cache FIFO eviction is now actually FIFO.**
  `axl_time_get_ms()` is 1-second resolution on UEFI, so many
  inserts within the same second tied on `timestamp_ms` and the
  tiebreak fell back to hash-bucket-walk order. Switched to a
  monotonic `cache_seq` counter; eviction is correct independent
  of clock granularity.
- **Serial Ctrl-C bridge** — `RegisterKeyNotify` is too sensitive
  on real hardware (fires on every keystroke at TPL_NOTIFY); use
  the Simple Text Input Ex `WaitForKeyEx` event under TPL_CALLBACK
  instead. Drops apparent CPU spin on serial-attached boards.

### Changed

- **`AXL_MAX_SOURCES` bumped 16 → 64.** The async-close shape needs
  ~one loop source per outstanding close ctx (held for ~TIME_WAIT
  seconds while SockConnClosed fires). 16 was tight even before —
  an http-server with the default 8 max-connections plus listener
  and per-conn cancellables would peak near the limit and fail
  `axl_loop_add_event` silently. Exhaustion now logs at error
  level instead of failing silently.
- **`axl_tcp_send` / `axl_tcp_recv` save/restore `sock->async_loop`**
  across their ephemeral wrapper loop. Previously each sync wrapper
  left `async_loop` pointing at the just-freed loop — a follow-up
  `axl_tcp_close` on the same sock would dereference freed memory
  while deciding sync-vs-async finalization.
- **CI integration job runs `test-http.sh` and `test-tcp-echo.sh`**
  on every push (was unit + tools + cpu-idle only). KVM
  acceleration is auto-enabled on GitHub-hosted runners via a
  one-shot `chmod 666 /dev/kvm` step.

### Library API

- **`axl_tcp_close` lifetime semantics** are now documented on the
  declaration in `axl/axl-tcp.h`. Callers must close TCP sockets
  before freeing the loop they were registered with; on the async
  path the AxlTcp pointer outlives the call until the firmware
  signals close-complete (treat as freed once the call returns).

### Examples / Tools

- **Examples now self-bootstrap networking** via `axl_net_auto_init`
  in `main` (echo-server, tcp-echo-server, echo-server-sync,
  http-server, net-check, fetch). Running any of them via plain
  `run-qemu.sh --net --hostfwd ...` Just Works — no custom nsh
  needed for `connect -r` / `ifconfig -s eth0 dhcp`. Tools
  (`fetch`, `netinfo`, `rfbrowse`, `mkrd`) already self-loaded
  their drivers; the examples now match.
- **Examples now check fallible-call returns**. `axl_loop_new`,
  `axl_*_accept_async`, `axl_http_server_add_route`, and
  `axl_socket_send` returns are checked in `echo-server.c`,
  `tcp-echo-server.c`, `http-server.c`, and `socket-demo.c` so the
  patterns developers crib from these files include the error
  paths, not just the happy path.
- **`tools/fetch`** now warns when more `-H` headers are passed
  than the static buffer pool holds (16). Previously truncated
  silently.

### Build

- **DWARF debug info in RELEASE builds** (Makefile + axl-cc).
  Both DEBUG and RELEASE now compile with `-g -gdwarf`. The `.efi`
  PE/COFF stays slim because objcopy still strips DWARF; the
  side-by-side `.so/.debug` carries it, and `pe-set-debug` points
  the PE debug-data directory at it. addr2line now works against
  any built artifact — a `#PF` reported by a user is resolvable
  against the binary they already have, without rebuilding with
  debug flags.

### Documentation

- **Audience framing.** README, AXL-Design, AXL-SDK-Design, and the
  `axl.h` / `axl-loop.h` umbrella headers now lead with the audience
  AXL is for — Linux systems C developers (glibc / GLib / systemd /
  libcurl) who need to ship a UEFI binary without learning EDK2.
  GLib-to-AXL mapping table (GMainLoop → AxlLoop, GHashTable →
  AxlHashTable, etc.) lives in AXL-Design.md.
- **How AXL avoids the EDK2 dependency** — README and design docs
  now explain that the `EFI_*` types are auto-generated from the
  published UEFI 2.x and PI 1.x specifications via
  `scripts/generate-uefi-headers.py` driven by
  `scripts/uefi-manifest.json5`. Spec drift is a manifest update,
  not a vendor merge.
- **Sphinx full-width override** — `docs/sphinx/_static/axl.css`
  widens `.wy-nav-content` to 1200 px so axl.aximcode.com no
  longer leaves grey gutters on wide monitors.

### Migration

- **`axl_tcp_close` returns immediately on the async path** when
  called from inside a running loop (typical: server connection
  teardown). The `AxlTcp *` pointer outlives the call by up to
  ~TIME_WAIT (~2 s) while the firmware completes the close.
  Existing callers that already null'd their `AxlTcp` pointer
  after `axl_tcp_close` work unchanged. Callers that read or use
  the pointer after close had a UAF in 0.2.5; those now read
  freed memory more visibly. The header doc on
  `include/axl/axl-tcp.h` describes the lifetime explicitly. No
  source changes required for the common case.

## 0.2.5 — 2026-04-25

Substantial release. Three headline buckets: a JSON-module rework
(reader/writer rename + writer rewrite + stats), `AxlRingBuf` push
statistics, and a stretch of CI/Ctrl-C plumbing that finally got QEMU
integration tests running in CI. Plus the typed numeric parsers and
miscellaneous API hardening that landed on top of v0.2.4.

### Added

- **Typed numeric string parsers.** `axl_str_to_u8/u16/u32/u64` and
  `axl_str_to_s8/s16/s32/s64` (in `<axl/axl-str.h>`) — the underlying
  parser is shared, the typed variants enforce range and sign. Replaces
  hand-rolled port parsers throughout `src/net/` and the
  `echo-server-sync` example.
- **`AxlJsonWriter`** — new orthogonal JSON writer with state machine.
  Backed by a caller-owned `AxlString` (auto-growing, no fixed-buffer
  guesses). Containers, keys, and atoms are independent calls; the
  writer handles comma placement, string escaping, and (optional)
  2-space-indent pretty mode (`AXL_JSON_WRITER_PRETTY` flag). Sticky
  error flag covers OOM and structural misuse; one check after
  `axl_json_writer_finish` is sufficient. Includes `kv_*` convenience
  pairs (`axl_json_kv_str/int/uint/bool/null/hex`) for the dominant
  key+atom shape, and `kv_strn` / `keyn` for non-NUL-terminated input.
- **`axl_json_write_token`** — parse → mutate → emit bridge. Splices a
  parsed token tree (object, array, or atom) into a writer's output
  verbatim, preserving `\uXXXX` escapes and other source representation.
- **`AxlRingBuf` push statistics.** New `pushes_total` and `pushes_lost`
  cumulative byte counters on every push path (`push`, `push_msg`,
  `push_elem`, `push_advance`), with accessors
  `axl_ring_buf_pushes_total / _lost`. Counts attempted bytes vs bytes
  invisible to the consumer (rejected in reject mode, displaced or
  input-dropped in overwrite mode). Reset on `axl_ring_buf_clear` and
  on init. Element-mode consumers divide by element size to get
  element counts.
- **`axl_diag_startup` / `axl_diag_probe_protocol`** — extracted the
  `-v` startup-diagnostics block that lived in tools into a public
  `<axl/axl-diag.h>` API. Tools that want a uniform "what's the
  firmware seeing" dump get it as a one-liner.
- **`axlk_http_read_request_line`** (kernel POC) — wraps the
  read-until-`\r\n\r\n` loop and delegates parsing to the public
  `axl_http_parse_request_line`. The three SoftBMC-shape kernel POC
  ports (hwinfo, bootconfig, reqlog) now share one implementation
  instead of duplicating ~70 LOC of byte-fiddling each.
- **`--log <path>` flag on `test/integration/test-axl.sh`** — captures
  the raw QEMU serial log to the given path. Equivalent to setting
  `TEST_KEEP_LOG`; just discoverable from `--help`.
- **Phase K10 (`axlk_offload`) added to the AXL-Kernel-Design.md phase
  plan.** AP compute pool design — out-of-band CPU work via
  `EFI_MP_SERVICES_PROTOCOL`, with cooperative blocking on completion
  via a new fd kind. Not started; design is staked.

### Changed

- **`AxlJsonCtx` → `AxlJsonReader`, `AxlJsonBuilder` → `AxlJsonWriter`.**
  Type rename for symmetry. Reader function signatures unchanged
  (parameter renamed `ctx` → `r` internally only). All callers
  migrated: tests, fuzz harness, `tools/rfbrowse`, `sdk/examples/json`,
  experiments. Old type names are gone — recompile against the new
  header.
- **`AxlJsonBuilder.overflow` field → `axl_json_writer_error()`
  accessor.** Struct fields are private now. The flag also broadened
  semantically: it's set on either AxlString OOM or structural misuse
  (key in array, value where key expected, mismatched begin/end, etc.).
- **`axl_json_pretty_print` → `axl_json_console_print`.** Renamed to
  make clear it's a *colored UEFI-console* pretty-printer (writes
  attribute-based color codes directly to the console), distinct from
  the writer's `AXL_JSON_WRITER_PRETTY` flag (buffer output, no color).
  rfbrowse keeps its colored Redfish-body output via the renamed call.
- **Three previously-`void` API sites now report failures.** Surfaced
  error returns where the caller had no way to know the operation
  silently failed. Caught by code review of the new typed-parser /
  config OOR work.
- **AxlConfig OOR rejection.** Config values that overflow the declared
  field width are now rejected with a clear warning and the parse
  fails, instead of being truncated silently.
- **AxlHashTable header preamble + per-constructor doc-comments.**
  Ownership semantics across the three constructors (`new_str` copies
  keys, `new` borrows both sides, `new_full` takes ownership when
  destroy callbacks are non-NULL) now spelled out at the top of the
  header and reinforced in each `_new_*` doc-comment. README adds an
  ownership matrix.
- **Three SoftBMC-shape kernel POC ports** (hwinfo, bootconfig, reqlog)
  rewritten to use `AxlJsonWriter` for endpoint JSON, `axl_strlcpy` in
  place of manual NUL-terminated copy loops, and the new
  `axlk_http_read_request_line` helper. axlk-reqlog-server's hand-rolled
  ring buffer replaced with `AxlRingBuf` (received/dropped derived
  from `pushes_total`/`pushes_lost`). On-the-wire output unchanged.
- **`axl_json_print_raw` removed** — it was a one-liner over
  `axl_printf("%.*s", ...)`. The single caller (rfbrowse `--raw` mode)
  updated to call `axl_printf` directly.
- **`AXL-Kernel-Design.md` moved into `experiments/axl-kernel/`** —
  co-located with the source it describes. README cross-refs updated.

### Fixed

- **Cooperative-Ctrl-C plumbing for serial / TCG QEMU.**
  - Backend bridges serial Ctrl-C to UEFI Shell `ExecutionBreak`.
  - `axl_loop_run` no longer busy-spins when there are no user sources
    (was pegging a CPU at 100% in tests with empty source sets).
  - test-yield-ctrlc fixes: FIFO open-order deadlock, two-socat split
    collapsed to one bidirectional pipeline, idle-marker + QEMU
    timeouts bumped to be tolerable on TCG.
- **`run-qemu.sh` interactive-tty silent failure** — script previously
  swallowed errors when stdin wasn't a TTY; now surfaces them and
  cleans up the serial log. Companion `--raw` flag plumbed through
  test-cpu-idle so failures appear in CI logs.
- **`run-qemu.sh` mtools-fallback dest path bug** — `mcopy` got the
  wrong destination when EDK2 mkimage was unavailable; rewritten to
  use `mcopy -s` correctly.
- **`u64_to_hex` truncation safety** — overflow path now writes a
  `"0x0"` placeholder instead of leaving the caller buffer
  uninitialized. Defensive against future buffer-size shrinks (current
  callers always size adequately).
- **Test-log noise:** AxlMem leak-dump test annotated with a
  clarifying printf so its expected-leak report (the AXL runtime's own
  argv/registry/atexit state) is no longer mistakable for a real leak.
  AxlData hash-table fixture leak silenced —
  `test_hash_insert_vs_replace` now reclaims its `axl_strdup`'d test
  keys after the table is freed.

### CI / Build

- **QEMU integration tests now run in CI.** `axl-common.sh` falls back
  to system `PATH` for QEMU binaries when none are pre-staged. `mkimage`
  build-dependency dropped — the mtools fallback covers the path.
  UEFIExtract installed from the LongSoft GitHub releases (correct
  lowercase binary name, A74 / x64_linux asset URL). `chmod 666
  /dev/kvm` so QEMU can use hardware acceleration; `axl-common.sh`
  also gracefully drops `-enable-kvm` when `/dev/kvm` isn't read/writable.

### Documentation

- **§ section anchors linkified** across Sphinx-rendered pages and the
  kernel design doc, so cross-references render as clickable links
  instead of plain text.
- **`src/data/README.md`** rewritten to fix existing-broken examples
  (parser used a wrong `get_string` signature; iterator example
  referenced types that don't exist) and added new JSON Writer +
  Round-Trip Transforms + Error Handling sections.
- **AxlRingBuf README** documents the new push-statistics API + the
  byte-counter unit semantics.

## 0.2.4 — 2026-04-24

### Added

- **`axl_driver_locate(name, out, out_size)`** — find a driver file
  on disk without loading it. Same search order as
  `axl_driver_ensure`: image's `drivers/<arch>/`, image's own
  directory, image's `drivers/`, then other volumes' `drivers/<arch>/`.
  Useful when the caller controls the LoadImage / StartImage
  lifecycle — for example, to set per-invocation load options
  between the two steps for a configurable DXE driver. axl-webfs's
  `mount` command uses this to find `axl-webfs-dxe.efi` regardless
  of which volume the user invoked the CLI from.

### Changed

- **`axl_driver_ensure` review followups.** Header doc example now
  shows the required `(const AxlGuid *)` cast that mkrd actually
  uses (the previous example wouldn't compile against the published
  signature). Added a trust-model paragraph: this function loads the
  first matching .efi off any mounted FAT volume at full firmware
  privilege, so don't pass attacker-controlled driver names. Internal:
  candidate-list dedup so the common "running tool lives at
  drivers/&lt;arch&gt;/" case doesn't double-stat the same path; comment
  on the EFI_GUID const-cast for future readers.

## 0.2.3 — 2026-04-24

### Added

- **`axl_driver_ensure(guid, name)`** — short-circuit on registered
  protocols, otherwise locate, load, and start a named DXE driver
  from `drivers/<arch>/<name>` on the running image's volume, the
  image's own directory, `drivers/<name>` at the volume root, or any
  other mounted FAT volume. Tools that depend on driver-provided
  protocols no longer need a `startup.nsh` that pre-loads them.
- **`tools/dmidecode`** — full SMBIOS record decoder with typed
  output for the common types and a hex/strings fallback for
  undecoded ones; companion to `sysinfo` for hardware introspection.
- **AxlSmbios** promoted to a top-level module
  (`include/axl/axl-smbios.h`) with typed accessors for Type 0/1/2/3/4
  /16/17, Type 38 (IPMI Device Info), and Type 42 (Host Interface),
  plus enumeration helpers (`axl_smbios_next`, `axl_smbios_version`),
  reentrant string access, and a UUID helper.

### Changed

- **`mkrd`** auto-loads `RamDiskDxe.efi` via `axl_driver_ensure`
  before dispatching modes. Prints
  `MkRd: RamDiskDxe.efi not found on any mounted volume.` when the
  driver isn't discoverable; previously the user saw the more
  cryptic `EFI_RAM_DISK_PROTOCOL not available` deep inside the
  create path.

### Fixed

- **Tool argv on Dell firmware.** `_axl_args_init` (in `axl-app.c`)
  no longer depends on `EFI_SHELL_PARAMETERS_PROTOCOL`. Dell's UEFI
  firmware doesn't publish that optional Shell-2.0 protocol for
  cross-volume invocations, which made every AXL tool see `argc=1`
  and miss all user arguments. The implementation now parses
  `EFI_LOADED_IMAGE_PROTOCOL.LoadOptions` (the spec-mandated
  primitive) directly, with a `_tokenize_load_options` helper that
  handles whitespace splitting and quoted arguments.

## 0.2.1 — 2026-04-23

### Legal / Compliance

- **Per-file SPDX headers.** Every `.c` / `.h` under `src/`,
  `include/`, and `tools/` now carries a two-line SPDX +
  copyright header:

  ```c
  /* SPDX-License-Identifier: Apache-2.0 */
  /* Copyright 2026 AximCode */
  ```

  Makes the license machine-discoverable by SBOM / compliance
  tooling (reuse.software, SPDX scanners, distro packagers).
  Auto-generated UEFI headers (`include/uefi/generated/*.h`)
  inherit the same header from the generator script.
  `docs/AXL-Coding-Style.md` documents the convention as the
  first block of every source file.

## 0.2.0 — 2026-04-23

Minor bump marks the **license change** (custom permissive →
Apache-2.0) and a round of distribution / compliance cleanup.
All prior releases (v0.1.0 through v0.1.4) have been removed
from the public release page; v0.2.0 is the new canonical
download. Existing copies remain valid under the terms they
were originally distributed under.

### Legal / Compliance

- **Relicensed to Apache-2.0.** The `LICENSE` file previously held a
  custom BSD-2-Clause-style license with a patent grant; replaced
  with the standard SPDX Apache-2.0 text. Legally similar
  (permissive, attribution, patent grant), standard-library
  recognizable, easier for downstream SBOM/compliance tooling.
  Previously distributed versions (v0.1.0 through v0.1.4) remain
  under the terms they were released under — Apache-2.0 applies
  from the next release forward.
- **Added `NOTICE` file** with copyright attribution and
  entity-clarification (AximCode is the trade name used by the sole
  author, copyright held personally, not by a registered entity).
- **Added `CONTRIBUTING.md`** with DCO sign-off requirement and a
  contributor-grants-relicensing clause. Prevents downstream
  contributions from constraining future licensing decisions.
- **Ship mbedtls LICENSE alongside binaries.** mbedtls is
  statically linked into `libaxl.a` and into the networking
  tools (`fetch`, `rfbrowse`); Apache-2.0 §4(a) requires
  the LICENSE be carried with any binary redistribution. The
  `.deb` / `.rpm` packages now install it at
  `/usr/share/doc/axl-sdk/third_party/mbedtls/LICENSE`, and
  each tools tarball carries it at `third_party/mbedtls/LICENSE`
  inside. A new top-level `THIRD_PARTY.md` documents the
  license election (we elect Apache-2.0 from mbedtls's
  Apache-2.0-OR-GPL-2.0-or-later dual-license).

## 0.1.4 — 2026-04-22

### Distribution

- **Collapsed `axl-sdk` and `axl-sdk-tls` into a single package.**
  The library now ships with mbedtls compiled in; `ar` only
  pulls mbedtls .o files into the final `.efi` when the app
  actually references `axl_tls_*` / `https://`. Apps that don't
  use TLS are unchanged in size; apps that do no longer need
  users to pick a different install. Users who want an even
  smaller `libaxl.a` can rebuild from source with `AXL_TLS=0`.
  Release assets drop from 4 packages to 2:
  `axl-sdk.deb` / `axl-sdk.rpm` only.
- **Tools tarballs now built with TLS.** `fetch` handles HTTPS;
  `rfbrowse` (Redfish) is fully functional. Tools that don't
  reference networking (mkrd, hexdump, find, grep, sysinfo)
  don't pull mbedtls in.
- **Dropped the dedicated `axl-sdk-source.tar.gz` asset.** Now
  that each release's tagged source tree is published to the
  public repo as a snapshot commit, GitHub's auto-generated
  **Source code (tar.gz)** archive is the canonical source
  download. `git clone https://github.com/aximcode/axl-sdk-releases.git`
  + `git checkout <tag>` also works.

## 0.1.3 — 2026-04-22

### Distribution

- **Binary + source packages now published at
  [aximcode/axl-sdk-releases](https://github.com/aximcode/axl-sdk-releases/releases).**
  The upstream development repo remains private; a dedicated
  public repo hosts the release artifacts so anyone can install
  without a GitHub account. Install URLs are version-agnostic via
  `/releases/latest/download/<file>`:

  ```bash
  curl -LO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/axl-sdk.deb
  sudo apt install ./axl-sdk.deb
  ```

  Each release publishes `SHA256SUMS` alongside the .deb / .rpm /
  source tarball / tool tarballs. Use the versioned URL pattern
  `.../releases/download/v<ver>/<file>` to pin a specific version.
- **Pre-built UEFI tool tarballs** (`axl-sdk-tools-x64.tar.gz`,
  `axl-sdk-tools-aa64.tar.gz`) for USB-stick UEFI-shell use —
  extract to a FAT stick, boot, run with `--help`.
- **Source snapshots on the public repo** so `git clone` yields
  the latest release's source and GitHub's auto **Source code**
  archives are functional (not just the release-repo README).
- **Filenames cleaned up**: host-arch labels dropped from .deb
  and .rpm filenames (`axl-sdk.deb` / `axl-sdk.rpm`, not
  `axl-sdk_amd64.deb` / `axl-sdk.x86_64.rpm`) so the release
  page's only arch labels refer to UEFI target arch (x64 / aa64
  in the tool tarballs).

### Changed (breaking, pre-1.0)

- **Sleep / wait APIs consolidated in `<axl/axl-wait.h>`.**
  Removed the public busy-wait surface (`axl_stall` from
  `axl-sys.h`, `axl_spin_sleep` / `axl_spin_msleep` / `axl_spin_usleep`
  from `axl-time.h`) — the spin family had zero callers, and the
  one `axl_stall` caller (`tools/mkrd.c`) was a bug. Sub-millisecond
  hardware timing (e.g. AxlIpmi KCS) still uses the backend-internal
  `axl_backend_stall` — not exposed to SDK consumers.

  The CPU-idle sleep family (`axl_sleep`, `axl_msleep`, `axl_usleep`)
  moved from `<axl/axl-time.h>` to `<axl/axl-wait.h>` and is now
  implemented as thin void-return wrappers over `axl_wait_ms(NULL, ...)`.
  Ctrl-C now returns from sleep early (matching POSIX intuition);
  it does not auto-terminate the app. `axl-time.h` now contains
  only `axl_time_format` and `axl_time_get_ms`.

  Consumers must include `<axl/axl-wait.h>` for sleep APIs instead
  of `<axl/axl-time.h>`. Sources that use `#include <axl.h>` are
  unaffected.

- **AxlEvent promoted to a first-class struct; AxlCompletion
  collapsed into it.** The old `AxlCompletion` (wrapping an
  EFI_EVENT) and the `AxlEvent` typedef (thin `void *` handle) were
  structurally identical. They're now one primitive: `AxlEvent` is
  an opaque struct with signal/wait/reset/is_set state, and
  `AxlCancellable` is a typed contract layered on top. Migration:
  - `AxlCompletion` → `AxlEvent`
  - `axl_completion_new/free/signal/reset/wait/wait_timeout` →
    `axl_event_new/free/signal/reset/wait/wait_timeout`
  - `<axl/axl-completion.h>` → `<axl/axl-event.h>`
  - `axl_event_create/signal/close` (raw-handle trinity in
    `axl-loop.h`) removed. Use `AxlEvent` and pass
    `axl_event_handle(e)` to `axl_loop_add_event` where a handle is
    required. The raw typedef is now `AxlEventHandle` (public, in
    `axl-event.h`) and still flows through `axl_loop_add_event` for
    firmware-owned events (TCP completion tokens, etc.).
  - New fast-check: `axl_event_is_set(e)` reads a local flag without
    driving the loop.

- **Sync primitives relocated.** `axl-cancellable.{h,c}`,
  `axl-wait.{h,c}`, and the new `axl-event.{h,c}` now live in
  `src/event/` (with matching `include/axl/axl-event.h`). `src/util/`
  is reserved for env/path/time/hexdump/sys-style helpers.

- **Arena moved.** `src/task/axl-arena.c` → `src/mem/axl-arena.c`.
  No API change. Arenas are allocators, not task/offload primitives.

### Added

- **AxlIpmi diagnostics (ported from uefi-ipmitool debugging).**
  Brings over the three hard-learned diagnostic features the
  uefi-ipmitool project accumulated debugging Dell iDRAC on Nvidia
  Grace Arm64:
  - **Chunked SDR reads.** `axl_ipmi_sdr_get()` now fetches records
    in the Linux-ipmitool style — 5-byte header first, then body
    in 23-byte chunks. Full-record reads (BytesToRead=0xFF) produce
    60+ byte IPMI responses that force multi-part SSIF reads,
    which hang the Nvidia UEFI I2C driver + Dell iDRAC combo
    (uefi-ipmitool commit 8c6acdb). Keeps every response in a
    single SSIF block. Public signature is unchanged; callers get
    the full record transparently.
  - **BMC reset wrappers.** `axl_ipmi_bmc_cold_reset()` (App 0x02)
    and `axl_ipmi_bmc_warm_reset()` (App 0x03) exposed as typed
    functions. Surfaced as `ipmi mc reset cold|warm` on the tool.
  - **`axl_ipmi_probe(AxlIpmiProbe *out)`** library helper + `ipmi
    probe` subcommand. Snapshots which IPMI-related firmware
    protocols are present (EDKII, Dell, AMI DXE/SMM, Intel SM,
    Microsoft Project Mu, SMBus HC, I2C Master, Dell iDRAC),
    decodes SMBIOS Type 38, counts I2C Master handles, and does an
    end-to-end Get Device ID through whatever auto-detect picks.
    Indispensable when `axl_ipmi_session_new()` can't find a working
    transport on an unknown platform — mirrors uefi-ipmitool's
    `probe` feature (commits eb90045 / 99c1dad) but uses the
    library's backend + SMBIOS helpers instead of raw LocateProtocol
    boilerplate.
  - **10 new unit tests** (bmc cold/warm reset + 6-check SDR
    chunked-read scenario). Ratchet 1198 → 1208.
- **AxlIpmi polish: test runbook, fuzz target, Sphinx page,
  design-doc family section.** Closes out Phase B1:
  - `test/integration/test-ipmi.sh` — documented manual-hardware
    runbook. Builds the tool, optionally stages it onto a FAT32
    disk, and prints the six-step pass criteria (BMC-backed).
  - `test/fuzz/ipmi_fuzz.c` — libFuzzer target that overrides
    `axl_ipmi_raw()` at link time to feed the fuzzer's bytes as
    IPMI responses. Drives every typed wrapper + all three
    format helpers under ASan. 50k runs green; 131 cov / 144 ft
    reached on a small seed corpus. Added to `test/fuzz/Makefile`
    alongside `url_fuzz` and `json_fuzz`.
  - `docs/sphinx/modules/ipmi.rst` + `index.rst` — AxlIpmi gets
    its own page on axl.aximcode.com, pulling in
    `src/ipmi/README.md` plus Doxygen reference for
    `axl-ipmi.h`.
  - `docs/AXL-Design.md` — new "Platform Access Modules" section
    establishing the shared shape (auto-detect session, raw +
    typed API, transport vtable, backend hooks, dogfood tool)
    that AxlSmbios + AxlIpmi share and that AxlAcpi / AxlPci /
    AxlSpd will inherit.
  - Refactor side effect: `src/ipmi/axl-ipmi-cmd.c` dropped its
    `axl-ipmi-internal.h` include (and therefore the transitive
    backend-header pull). The cmd wrappers only ever needed the
    public types + allocator, and this makes the fuzz build
    dramatically simpler.
- **AxlIpmi — phase 7 (format helpers + ROADMAP).** Adds
  `src/ipmi/axl-ipmi-format.c` with three string-table lookups:
  `axl_ipmi_completion_code_string` covers every standard
  completion code from IPMI v2.0 Table 5-2 plus OEM / command-
  specific ranges; `axl_ipmi_sensor_type_string` covers sensor
  types 0x01–0x2C from Table 42-3; `axl_ipmi_entity_id_string`
  covers the common entity IDs from Table 43-13 plus OEM ranges.
  Wired into `tools/ipmi sensor` (column-aligned output with typed
  sensor + entity names) and `tools/ipmi raw` (CC byte shown with
  its description). `docs/ROADMAP.md` marks Phase B1 complete and
  adds a new "Phase B3: Platform Access follow-on modules" section
  with scoped AxlAcpi / AxlPci / AxlSpd entries (consumer tool,
  backend deps, and effort estimate for each).
- **`tools/ipmi` — stripped-down ipmitool-equivalent.**
  New UEFI tool exercising AxlIpmi end-to-end from the command line.
  Subcommands:
  - `ipmi info` — device ID + detected transport
  - `ipmi chassis status` / `chassis power on|off|cycle|reset|diag|soft`
  - `ipmi sel list` — System Event Log dump
  - `ipmi sdr list` — SDR repository entries
  - `ipmi sensor` — all Full/Compact sensors with raw readings
  - `ipmi fru list` — FRU 0 raw bytes, 16-byte rows
  - `ipmi raw <netfn> <cmd> [<hex>...]` — raw passthrough

  Uses AxlConfig for argument parsing and `AXL_AUTOPTR(AxlIpmiSession)`
  for RAII session cleanup. Output mirrors `ipmitool`'s layout closely
  enough to feel familiar but doesn't try to cover the full feature
  set — formatted sensor/entity/event strings are deferred until the
  format helpers ship (phase 7). Builds on both x64 and aa64; 9 tools
  are now built (was 8). Real-hardware smoke coverage comes via the
  manual `test-ipmi.sh` script against a live BMC.
- **AxlIpmi — phase 5 (typed command wrappers + callback transport).**
  Ten typed wrappers covering the commands a tool needs for
  `info` / `chassis` / `sel` / `sdr` / `sensor` / `fru` subcommands:
  `axl_ipmi_get_device_id`, `axl_ipmi_get_chassis_status`,
  `axl_ipmi_chassis_control`, `axl_ipmi_sel_info`,
  `axl_ipmi_sel_get_entry`, `axl_ipmi_sdr_info`, `axl_ipmi_sdr_get`,
  `axl_ipmi_get_sensor_reading`, `axl_ipmi_fru_info`,
  `axl_ipmi_fru_read`. Each builds the request per IPMI v2.0 spec,
  verifies the completion code, and decodes the response into a
  public `AxlIpmi*` struct using little-endian byte extractors.

  New `axl_ipmi_session_new_with_callback(kind, callback,
  user_data)` lets callers plug in a send-raw function pointer.
  Primary use is unit testing (exercise the wrappers with canned
  responses, no BMC hardware required) but it also opens the door
  to pluggable out-of-process transports (IPMI-over-LAN, test
  rigs) without baking them into auto-detect.

  **43 new unit tests** in `test/unit/axl-test-ipmi.c` (1155 → 1198
  baseline). Every typed wrapper gets a happy-path test plus two
  negative-path assertions: non-zero completion code and truncated
  response both correctly fail. Ratchet bumped to 1198.
- **AxlIpmi — phase 4 (EDKII + Dell vendor protocol dispatchers).**
  Adds `src/ipmi/axl-ipmi-edkii.c` and `-dell.c`, both resolving
  their protocol via `gBS->LocateProtocol` and forwarding
  `axl_ipmi_raw()` through the firmware-provided function pointers.
  EDKII uses the standard MdeModulePkg `IPMI_PROTOCOL` shape; Dell's
  proprietary protocol returns response data without a completion
  code, so the dispatcher synthesizes `CC=0x00` at `resp[0]` and
  shifts the vendor bytes up — callers see the same layout
  (`[CC, data...]`) regardless of transport.

  Auto-detect priority in `axl_ipmi_session_new()` is now:

      1. EDKII IPMI_PROTOCOL        (firmware-mediated; preferred)
      2. Dell EFI_IPMI_TRANSPORT    (vendor; Dell HW only)
      3. SMBIOS Type 38             (KCS or SSIF from iface byte)
      4. x86 default KCS            (0x0CA2 / 0x0CA3 last resort)

  Firmware-mediated transports handle platform quirks and reach
  BMCs on buses we can't enumerate directly, so they always win
  over direct physical access when both are available. All four
  transports plug into the same vtable, so `axl_ipmi_raw()` calls
  are identical downstream regardless of which transport the
  auto-detect landed on.
- **AxlIpmi — phase 3 (SSIF transport).** Adds
  `src/ipmi/axl-ipmi-ssif.c`: SMBus-based framing per IPMI v2.0
  Section 12, on top of the `axl_backend_smbus_*` hooks (which
  already handle the `EFI_SMBUS_HC_PROTOCOL` → `EFI_I2C_MASTER_PROTOCOL`
  fallback). Transport layer owns the protocol's timing hazards:
  60 ms inter-command delay after every completed transaction,
  5-retry write loop at 60 ms intervals, 10-retry read loop with
  exponential backoff starting at 60 ms. Multi-part
  write/read framing handles requests / responses beyond the 32-byte
  SMBus block limit (reads reassemble blocks prefixed with the
  sequence byte until the `0xFF` end marker arrives). Auto-detect
  in `axl_ipmi_session_new()` now picks SSIF when SMBIOS Type 38
  reports interface type 4; the I2C slave address is shifted out
  of the SMBIOS-encoded wire address before handoff. Cross-compiles
  on both X64 and AARCH64; 1155 unit tests still pass on both.
- **AxlIpmi — phase 2 (skeleton + KCS transport).**
  New public header `<axl/axl-ipmi.h>` with an opaque session type
  (`AxlIpmiSession`), transport enum, and the lowest-level raw
  command entry point:
  ```c
  AXL_AUTOPTR(AxlIpmiSession) ipmi = axl_ipmi_session_new();
  uint8_t resp[16];
  size_t  resp_len = sizeof(resp);
  axl_ipmi_raw(ipmi, 0x06, 0x01, NULL, 0, resp, &resp_len);
  ```
  Auto-detects the best available transport (Phase 2 covers
  SMBIOS Type 38 → x86 default KCS at `0x0CA2`/`0x0CA3`; SSIF,
  EDKII and Dell vendor protocols follow in later phases). The
  KCS transport is a polled FSM ported from uefi-ipmitool on top
  of the new `axl_backend_io_read8/write8` hooks. Module lives
  in `src/ipmi/`; see `src/ipmi/README.md` for the layout and
  transport priority table. Public header is pulled into
  `<axl.h>` so consumer apps can `#include <axl.h>` as usual.
  Typed command wrappers, unit tests, and `tools/ipmi.c` land
  in subsequent phases.
- **UEFI generator: normalize trailing `[]` flex arrays to `[1]`.**
  `scripts/generate-uefi-headers.py` rewrites `Name[]` →
  `Name[1]` in extracted struct members, matching EDK2's
  convention and sidestepping GCC's rejection of `[]` as the sole
  named member of a struct. Removes the standing hand-edit in
  `include/uefi/generated/media.h` that kept getting clobbered on
  every regeneration.
- **AxlIpmi prep: UEFI protocol types + backend I/O hooks.** New
  SMBus / I2C protocol extractions in the manifest
  (`EFI_SMBUS_HC_PROTOCOL`, `EFI_I2C_MASTER_PROTOCOL`, and
  supporting enums/structs), plus hand-written additions to
  `include/uefi/axl-uefi-extra.h` for `IPMI_PROTOCOL` (EDKII),
  `DELL_IPMI_TRANSPORT` (vendor-proprietary), and
  `SMBIOS_TABLE_TYPE38` (DMTF SMBIOS spec). Adds four new backend
  hooks to `src/backend/axl-backend.h`:
  `axl_backend_io_read8/write8` (inline asm on x86, `-1` on aa64),
  `axl_backend_smbus_read_block/write_block` (SMBus with I2C
  fallback, matching uefi-ipmitool's dual-backend pattern).

## 0.1.2 (2026-04-18)

### Removed

- **`axl_args_*` API** (`include/axl/axl-args.h`, `src/util/axl-args.c`)
  merged into `AxlConfig`. There is now a single descriptor type,
  `AxlConfigDesc`, which drives both runtime configuration and
  command-line parsing. Callers that used `axl_args_parse(argc, argv, opts)`
  switch to:
  ```c
  AxlConfig *cfg = axl_config_new(descs, NULL, NULL);
  axl_config_parse_args(cfg, argc, argv);
  ```
  with an `AxlConfigDesc` table in place of the old `AxlOpt` array.
  `axl_config_get_bool` / `_get` / `_get_multi` / `_pos` replace the
  corresponding `axl_args_*` getters. All 8 in-tree tools, the
  `args-demo` example (merged into `config-demo`), and the unit
  tests were migrated in the same commit.

### Added

- **`scripts/build-packages.sh`** mirrors what the release workflow
  does for one TLS variant — stages the SDK via `install.sh`, runs
  `fpm` for `.deb` + `.rpm`, and smoke-tests the RPM by extracting
  it and invoking the installed `axl-cc` against `examples/hello.c`.
  Useful for smoke-testing a release before tagging and for
  producing packages on machines without CI access. Packages advertise
  `Provides: axl-sdk-devel` so `dnf install axl-sdk-devel` resolves
  correctly on Fedora/RHEL (matches the muscle-memory Fedora
  convention for dev-only packages like `gnu-efi-devel`).

### Changed

- **SDK layout polished for lintian/rpmlint compliance.** Files that
  previously sat bare at the top of `<prefix>/lib/` have moved to
  properly namespaced subdirectories, matching what distro packaging
  tools expect:
  - `lib/<arch>/` → `lib/axl/<arch>/` (per-target-arch libs and objs).
  - `lib/elf_*_efi.lds` → `lib/axl/elf_*_efi.lds` (linker scripts).
  - `lib/axl.cmake` → `lib/cmake/axl/axl-config.cmake` (now findable
    via `find_package(axl REQUIRED)`).
  - `lib/{version,build-date,backend}` → `share/axl/{...}` (SDK
    metadata; arch-independent plain text belongs under `share/`).

  The `lib/pkgconfig/axl.pc` path is unchanged. All paths remain
  relocatable — pkg-config uses `${pcfiledir}`, axl.cmake uses
  `${CMAKE_CURRENT_LIST_DIR}`, and axl-cc uses `$(dirname "$0")`.
  The embedded axl-cc wrapper and axl-config.cmake were updated to
  resolve their files from the new locations.

  **Consumer impact:** existing CMake projects that did
  `include(/path/to/sdk/lib/axl.cmake)` must switch to
  `find_package(axl REQUIRED)` (or update the explicit path to
  `/path/to/sdk/lib/cmake/axl/axl-config.cmake`). No API changes;
  `axl_add_app()` and every flag/option are unchanged.
- **Release artifacts are now `.deb` and `.rpm` packages, not tarballs.**
  `.github/workflows/release.yml` uses `fpm` against an `install.sh`
  staging tree to produce two Debian packages (`axl-sdk` and
  `axl-sdk-tls`) and two RPMs per release, each bundling both x64
  and aa64 UEFI target libraries. Consumers install with
  `sudo apt install ./axl-sdk_*.deb` or
  `sudo dnf install ./axl-sdk-*.rpm` — no tarball extraction, no
  manual PATH setup. The per-arch `axl-sdk-{x64,aa64}-linux.tar.gz`
  artifacts are no longer produced; a single `git archive`
  `axl-sdk-<ver>-source.tar.gz` is attached for source builds. See
  `packaging/postinst.sh` for the minimal after-install hint.
  Contributors and users without a matching distro package can still
  run `./scripts/install.sh --prefix /opt/axl-sdk` to stage the
  same FHS layout locally.
- **AUTO_FREE + steal_pointer applied to three internal parsers.**
  `axl_http_parse_request_line`, `axl_http_parse_headers`, and
  `axl_json_parse` now use `AXL_AUTO_FREE` on their heap scratch
  buffers and `axl_steal_pointer(&p)` to transfer ownership on
  success. Eliminates repeated `axl_free(...)` calls on error
  paths and closes two latent leaks: a silent OOM in
  `axl_http_parse_request_line` (a query-string `axl_strndup`
  failure would previously return success with `*query = NULL`,
  indistinguishable from "no query present"), and a leak in
  `axl_http_parse_headers` if `axl_hash_table_replace` rejected
  an entry (the name/value buffers were not freed on that path).
  All 1155 unit tests and 62 HTTP integration tests still pass.

### Added

- **JSON5-lite parsing in `scripts/generate-uefi-headers.py`.** The
  UEFI manifest now accepts `//` line comments, `/* */` block
  comments, and trailing commas, so entries can carry inline "why"
  notes and a file header instead of cramming them into a `_comment`
  array at the top. File renamed from `scripts/uefi-manifest.json`
  to `scripts/uefi-manifest.json5`; old `_comment` array lifted out
  into a real header comment block at the top of the file. The
  loader is a ~50-line preprocessor in the generator (`_strip_json5`)
  that walks the text string-aware and feeds the stripped result to
  stdlib `json`. No pip dependencies added. The JSON5-lite grammar
  covers only comments and trailing commas — single-quoted strings,
  unquoted keys, and numeric extensions are **not** supported. All
  six in-tree references (`CLAUDE.md`, `scripts/build.sh`, three
  `docs/*` files) updated to the new filename. Generator output is
  byte-identical to pre-rename.
- **`axl_steal_pointer()` in `<axl/axl-macros.h>`.** GLib-style
  ownership-transfer helper that returns the pointed-to value and
  NULLs the source in one step. Designed to pair with `AXL_AUTOPTR`
  on constructor success paths — `return axl_steal_pointer(&foo);`
  hands the resource to the caller while disarming the cleanup
  attribute. Type-preserving via `__typeof__`, so the result is
  assignment-compatible with the caller's variable. Applied to
  `axl_http_client_new`, `axl_http_server_new`, and `axl_url_parse`
  in this release, replacing ~30 lines of manual cascading cleanup
  code with straight-line early returns. As a side effect,
  `axl_url_parse` now correctly reports OOM if the query-string
  `strdup` fails — previously that case silently returned success
  with a NULL query, indistinguishable from "no query present".
- **Fuzz harness scaffold under `test/fuzz/`.** Standalone host-side
  libFuzzer build (`clang -fsanitize=fuzzer,address`) with two
  initial targets: `url_fuzz` covering `axl_url_parse` and
  `json_fuzz` covering `axl_json_parse`. Parser sources compile
  directly against a tiny libc-backed shim (`fuzz_shim.c`) rather
  than the freestanding library, so they run under AddressSanitizer.
  Not wired into the default `make` target or CI — fuzzing is opt-in,
  and a nightly job with crash artifact upload is tracked separately.
  Seed corpora live in `test/fuzz/{url,json}_corpus/`; see
  `test/fuzz/README.md` for build and run instructions.
- **OOM fault injection for testing error paths.**
  `axl_mem_fail_next_alloc(N)` in `<axl/axl-mem.h>` arms the Nth
  subsequent allocation to return NULL without touching the backend.
  Because `axl_calloc`, `axl_realloc`, `axl_strdup`, and `axl_memdup`
  all route through `axl_malloc_impl`, one hook catches every
  allocation path. 26 new unit tests use it to exercise the silent
  OOM paths added during the April 2026 logging work — 13
  allocator-primitive tests in `test/unit/axl-test-mem.c` and 13
  container tests in `test/unit/axl-test-data.c`. Unit-test baseline
  1129 → 1155. The hook is gated on `AXL_MEM_DEBUG` and compiles to
  a no-op in release. See `src/mem/README.md` for usage details.
- **clang-tidy static analysis in CI**
  (`.github/workflows/ci.yml`). The `.clang-tidy` config enables
  `bugprone-*` and `clang-analyzer-*` minus five documented noisy
  checks, with `WarningsAsErrors: '*'`. 11 pre-existing findings
  fixed (including two real null-deref bugs in `axl-list.c` caught
  by `clang-analyzer-core.NullDereference`).

### Changed

- **Unit test baseline**: 1162 → 1129 → 1155 across the Unreleased
  window. The `test_args` removal dropped 33 assertions when
  `axl_args_*` was unified into `AxlConfig`; the OOM injection work
  added 26 new tests. Net: -7 from the 0.1.1 baseline, but with
  stronger coverage of error paths. HTTP integration tests
  unchanged at 62.

### Fixed

- `test_args`/`axl_args_*` surface removed cleanly — no lingering
  references in docs, Sphinx, Makefile, or `include/axl.h`.
- Two real null-deref bugs in `src/data/axl-list.c` `merge_sorted` /
  `merge_sorted_data` helpers: `head.next->prev = NULL` could
  dereference NULL when both inputs to the merge were empty. Guard
  added; caught by `clang-analyzer-core.NullDereference`.

## 0.1.1 — 2026-04-13

### Added

- Compile-time version macros in `include/axl/axl-version.h`:
  `AXL_VERSION_MAJOR`, `AXL_VERSION_MINOR`, `AXL_VERSION_PATCH`,
  `AXL_VERSION_STRING`, `AXL_VERSION_NUMBER`, and the
  `AXL_VERSION_AT_LEAST(major, minor, patch)` convenience macro.
  Consumers can now gate features on a specific AXL version at
  compile time.
- `lib/pkgconfig/axl.pc` installed with the SDK so host toolchains
  that don't use `axl-cc` can `pkg-config --cflags --libs axl`
  instead of hard-coding `-Iinclude -laxl`.
- `scripts/bump-version.sh` helper that atomically updates
  `VERSION` and `include/axl/axl-version.h` so the two files never
  drift. The Makefile runs `check-version` on every build and
  hard-errors on mismatch.

### Fixed

- **HTTP server cache `max_entries` is now enforced** via FIFO
  eviction using the existing `CachedResponse.timestamp_ms` field.
  0.1.0 stored the limit but never applied it, so long-running
  servers grew the cache without bound.
- **`axl_http_server_set_route_ttl(path, ttl_ms)` honors the path
  argument.** A per-server `path → ttl_ms` map is consulted at
  cache-store time and falls back to the server-wide default on
  miss. 0.1.0 silently updated the default and discarded the path.
- **`axl_http_server_cache_invalidate(prefix)` honors the prefix
  argument.** Uses `axl_hash_table_foreach_remove` with a
  predicate that skips the `METHOD ` token in the cache key and
  compares the path portion. NULL or empty prefix still clears
  the whole cache. 0.1.0 cleared the whole cache regardless of
  prefix.
- Pre-existing leak of `key_dup` on OOM paths in
  `src/net/axl-http-route.c` fixed incidentally during the
  hash-table return-value refactor.

### Changed

- `axl_hash_table_insert` and `axl_hash_table_replace` now return
  a tri-state `int`: `1` if a new entry was added, `0` if an
  existing entry was replaced, `-1` on allocation failure. 0.1.0
  returned `int` but only signaled `0` / `-1`, so callers could
  not distinguish "replaced" from "inserted new." Most call sites
  in the tree ignored the return and need no update; the one
  exception (`src/net/axl-http-route.c`) was updated in place.
  GLib's `g_hash_table_insert` uses a bool for this distinction,
  but GLib aborts on OOM — AXL recovers, which is why we need
  three states instead of two.

### Logging

- `axl_error` / `axl_warning` calls added to ~26 silent OOM
  failure paths across `src/mem/`, `src/data/`, `src/io/`,
  `src/net/`, and `src/util/`. Every AXL allocation failure now
  surfaces with the function name, requested size, and caller
  file:line.
- `axl_calloc` gained a `count * size` integer-overflow guard
  with its own log line before the multiplication.

### Refactoring (no behavior change)

- `src/data/axl-digest.c` (645 lines) split into per-algorithm
  files: `axl-digest-md5.c`, `axl-digest-sha1.c`,
  `axl-digest-sha256.c`, plus a shared `axl-digest-internal.h`.
  Adding new algorithms is now a single-file add.
- `src/net/axl-net-util.c` (1080 lines) split into five focused
  files: `axl-net-ping.c`, `axl-net-resolve.c`,
  `axl-net-interfaces.c`, `axl-net-addr.c`, `axl-net-dhcp.c`.
- `src/net/axl-tcp.c` (1160 lines) split into `axl-tcp-sync.c`
  and `axl-tcp-async.c` with shared struct and helpers in
  `axl-tcp-internal.h`.

### Tooling

- `.clangd` config at the repo root silences
  `UnusedIncludes` / `MissingIncludes` diagnostics so the editor
  stops flagging intentional umbrella includes like
  `axl-backend.h`.
- `compile_commands.json` regenerated via `bear -- make tests tools`
  to cover all refactored files. Gitignored per-dev.

### Tests

- 18 new HTTP integration assertions covering the three cache
  policy fixes (eviction, per-route TTL, prefix invalidation).
  HTTP integration goes from 44 to 62 passed; unit tests
  unchanged at 1162 on X64 and AARCH64.

## 0.1.0 — 2026-04-06

Initial public release. Core library ported from UdkLib
(uefi-devkit), native UEFI backend (GCC + ld + objcopy), and the
first GitHub release workflow.

### Modules

- **AxlLog** — structured logging with domains, levels, handlers
  (console, ring, file).
- **AxlData** — hash table (CHAR8 + CHAR16), dynamic array, string
  utilities, JSON parser, radix tree, ring buffer, message digests.
- **AxlUtil** — file I/O, path manipulation, SMBIOS, hex dump,
  time, argument parsing, environment, system info, NVRAM.
- **AxlLoop** — event loop with timers, idle dispatch, Ctrl-C
  handling.
- **AxlTask** — AP worker pool, arena allocator, buffer pool.
- **AxlNet** — TCP / UDP sockets, HTTP server with routing and
  middleware, HTTP client, URL parser, TLS via mbedTLS (optional).
- **AxlGfx** — Graphics Output Protocol, bitmap font, primitives.
